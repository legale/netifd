# netifd reconcile loop implementation plan

## Verdict

Do not rewrite netifd as a new Kubernetes-like controller. Add a small internal
reconciler around the existing netifd FSMs.

The reconciler must not own configuration, must not change UCI, ubus, hotplug or
proto contracts, and must not restart services directly. It must compare the
state netifd already wants with the state that is actually present, then trigger
only existing netifd recovery paths.

## Goal

Make partial setup failures self-healing.

Target failure class:

- most interfaces are up, but one device or vif was missed;
- one wireless interface was not created or was lost;
- setup got stuck after a missed event;
- `service wpad restart` fixes the system because it forces a fresh creation and
  hotplug/check path.

The replacement netifd must remain externally compatible with stock netifd.
Existing OpenWrt users of UCI, ubus, hotplug scripts, proto scripts, wireless
ucode and init scripts must continue to work.

## Existing state to reuse

netifd already has most of the needed pieces:

- `interface.c`: interface FSM: `IFS_DOWN`, `IFS_SETUP`, `IFS_UP`,
  `IFS_TEARDOWN`.
- `device.c`: device ownership, claim/release, active users, present/link state.
- `proto.c`, `proto-ext.c`, `proto-shell.c`, `proto-static.c`: proto setup,
  teardown and notify paths.
- `ucode.c`: C to ucode bridge, including
  `netifd_ucode_check_network_enabled()` and hotplug event integration.
- `examples/wireless.uc`, `examples/wireless-device.uc`: reference wireless device
  and vif FSMs, retry and hotplug processing. These files are not always the
  runtime source installed on target devices; some OpenWrt trees ship the live
  wireless ucode from a separate package.
- `system-linux.c`: kernel state and hotplug/netlink input.

The reconciler must use these pieces instead of creating a second network
manager inside netifd.

## Minimal architecture

Add only two files:

- `reconcile.c`
- `reconcile.h`

Expose only small internal entry points:

```c
void reconcile_init(void);
void reconcile_schedule(enum rec_reason reason);
void reconcile_iface_schedule(struct interface *iface, enum rec_reason reason);
```

Call `reconcile_init()` from `main.c` after initial configuration loading.

Add schedule hooks only. Hooks must not perform recovery directly.

Good hook points:

- interface events: up, down, up-failed, link-up;
- device events: add, remove, link-up, link-down;
- manual interface actions: ifup, ifdown, restart;
- config reload completion;
- hotplug handling after device hotplug event;
- wireless enabled-state check path after `netifd_ucode_check_network_enabled()`.

The reconciler runs from one coalesced `uloop_timeout`. No threads. No blocking.
No direct service calls.

## Desired state source

Do not duplicate desired state.

The existing netifd objects are the desired state:

```c
want_up = iface->autostart &&
          iface->enabled &&
          iface->available &&
          iface->config_state == IFC_NORMAL &&
          !config_init;
```

Device desired state is derived from the existing interface and device ownership:

- `iface->main_dev.dev`
- `iface->l3_dev.dev`
- `device_user.claimed`
- `dev->present`
- `dev->active`
- `dev->link_active`

Wireless desired state should not be reimplemented in C in the first version.
Use the existing ucode side through `netifd_ucode_check_network_enabled()`.
The actual production patch must be applied to the wireless-device.uc that is
installed on the target, not blindly to `src/examples/wireless-device.uc`.

## Stage 1: audit-only reconciler

Goal: detect real mismatches without changing behavior.

The first commit must add a reconciler that only logs mismatches. It must not
call setup, teardown, restart, service restart, wireless reload or proto restart.

Checks:

- interface wants to be up but is not `IFS_UP`;
- interface is `IFS_UP`, but `iface->l3_dev.dev` is missing;
- interface is `IFS_UP`, but main device exists and its kernel ifindex is gone;
- device is claimed by an interface but `dev->present` is false;
- setup state age is suspiciously high;
- wireless check may be needed, but no recovery is executed.

Required safety gates:

- ignore disabled interfaces;
- ignore interfaces with `autostart == false`;
- ignore interfaces while `config_init` is active;
- ignore interfaces with `config_state != IFC_NORMAL`;
- do not act in `IFS_TEARDOWN`;
- do not change device state;
- do not call external scripts.

Log format must be compact and grep-friendly:

```text
reconcile: iface=lan want=up state=up dev=br-lan l3=(null) action=none reason=missing_l3
reconcile: iface=wan want=up state=setup age=45 action=none reason=setup_timeout
reconcile: iface=wifi0 want=up state=up dev=wlan0 ifindex=0 action=none reason=missing_ifindex
```

Stage 1 is successful when it can run on a live AP without changing network
state and without producing constant false positives on a stable system.

Status: completed in audit-only mode. The implementation adds coalesced scheduling,
configuration/device/interface/hotplug/wireless triggers, setup-age tracking and
compact mismatch logs. No recovery action is executed in this stage.

## Stage 2: safe soft reconcile

Enable only low-risk actions.

Allowed actions:

- if an interface wants up and is `IFS_DOWN`, call the existing interface up
  path;
- coalesce and call `netifd_ucode_check_network_enabled()` after relevant
  network or hotplug events.

Forbidden actions:

- no direct `service wpad restart`;
- no direct hostapd/wpa_supplicant kill;
- no full network reload;
- no manual bridge/vlan/device creation outside existing handlers;
- no proto-specific private recovery.

Required guards before any action:

- per-interface action backoff;
- max failure limit;
- temporary suppression after repeated failures;
- compact logs for performed, suppressed and blocked actions.

Implementation status: completed.

Stage 2 is enabled by default. It can be disabled at build time or at runtime.

Build-time control:

```sh
cmake -DRECONCILE_ACTIONS=OFF ...
```

Runtime control:

```sh
uci set network.globals.reconcile_actions='0'
uci commit network
service network reload
```

When action mode is enabled:

- `IFS_DOWN` + `want_up` calls the existing `interface_set_up()` path;
- repeated soft actions are guarded by per-interface backoff and suppression;
- wireless check is coalesced through `netifd_ucode_check_network_enabled()`;
- all other mismatches still log only `action=none`.

When action mode is disabled:

- no interface setup is called by the reconciler;
- stage 1 audit behavior is preserved for generic interface actions.

## Stage 3: stuck setup recovery

Implementation status: completed.

Setup age is tracked through `iface->setup_time`. The reconciler warns about
stuck setup and can restart only the affected interface through the existing
`interface_restart()` path.

This recovery is enabled by default. It can be disabled at build time or at runtime:

```sh
cmake -DRECONCILE_SETUP_RESTART=OFF ...
uci set network.globals.reconcile_setup_restart='0'
uci commit network
service network reload
```

Stage 3 action:

- `IFS_SETUP` older than `REC_SETUP_RESTART_SEC` is treated as stuck;
- first stuck observation only logs `action=none reason=setup_confirm`;
- restart is executed only if the same setup instance is still stuck after
  `REC_SETUP_CONFIRM_SEC`;
- recovery uses `interface_restart(iface)` only;
- no direct proto manipulation;
- no device state change;
- no `service network restart`;
- no `service wpad restart`.

Safety guards:

- same desired-state gates as stage 2;
- no action during `IFS_TEARDOWN`;
- setup restart is tied to the current `setup_time` generation;
- per-interface action backoff;
- failure limit;
- suppression window after repeated failures;
- confirmation state is cleared once the interface leaves setup.

Expected logs:

```text
reconcile: iface=wan want=up state=setup dev=eth0 l3=(null) ifindex=0 age=61 action=none reason=setup_confirm trigger=periodic
reconcile: iface=wan want=up state=setup dev=eth0 l3=(null) ifindex=0 age=66 action=restart reason=setup_timeout trigger=periodic
```

Recovery is enabled by default, but remains guarded by setup generation, confirmation delay, backoff, fail limit and suppression.

## Stage 4: missing wireless vif recovery

Implementation status: wireless-specific recovery is implemented and hardened.

Problem solved by this stage: a radio has many configured wifi interfaces, the
radio setup reaches `up`, but one expected vif/vlan is missing. A full
`service wpad restart` usually fixes it because hostapd/wpa_supplicant and the
wireless handler recreate the missing interface. Netifd now does the same kind
of recovery at radio scope instead of service scope.

Current implementation:

- netifd exposes `device_ifindex(ifname)` to ucode;
- the wireless device object checks expected vif/vlan handler data;
- a missing handler result is logged as `missing_handler_data`;
- a reported ifname that is absent in the kernel is logged as
  `missing_kernel_ifname`;
- recovery calls the existing wireless `teardown()` path for the affected radio;
- the existing teardown callback calls `setup()` again;
- no direct `service wpad restart`;
- no direct process kill;
- no manual device/proto state manipulation.

The recovery is enabled by default through:

```sh
cmake -DRECONCILE_WIRELESS_RECOVER=ON ...
```

This option also enables periodic wireless checks from the reconciler.

Safety guards:

- only runs when the wireless device state is `up`;
- only runs for autostart wireless devices;
- skips vifs whose target network is known and disabled;
- treats sections without a known network as wanted, matching existing wireless behavior;
- grace window after radio reaches `up`;
- persistent-missing confirmation before recovery;
- per-radio backoff;
- failure limit;
- suppression window after repeated failures;
- no recovery while setup retry has already failed.

Expected log examples:

```text
reconcile: wireless=radio0 action=none reason=recover_grace age=2
reconcile: wireless=radio0 action=none reason=recover_confirm age=0 section=wifi1 ifname=wlan0-2
reconcile: wireless=radio0 action=teardown_setup reason=missing_handler_data section=wifi0 ifname=(null) count=1
reconcile: wireless=radio0 action=teardown_setup reason=missing_kernel_ifname section=wifi1 ifname=wlan0-2 count=1
reconcile: wireless=radio0 action=suppress reason=recover_backoff section=wifi1 ifname=wlan0-2
reconcile: wireless=radio0 action=blocked reason=recover_fail_limit section=wifi1 ifname=wlan0-2
```

This replaces the practical effect of `service wpad restart` for the common case
where only one vif or vlan was missed, while keeping the blast radius limited to
the affected wireless device.

Stage 4 completion criteria: completed.

- missing vif/vlan is detected from handler data and kernel ifindex;
- disabled vif/vlan does not trigger recovery;
- recovery waits after radio `up`;
- recovery requires the same missing object to remain missing across checks;
- repeated recovery is rate-limited and eventually suppressed;
- recovery uses only existing wireless teardown/setup path.

## Stage 5: runtime control and visibility

Implementation status: completed.

The reconciler now has runtime configuration under `network.globals`. Defaults are enabled so the improved netifd is self-healing immediately after installation. Runtime options can disable or tune recovery without rebuilding.

Runtime options:

```text
option reconcile_enabled '1'
option reconcile_actions '1'
option reconcile_setup_restart '1'
option reconcile_wireless_recover '1'
option reconcile_delay_sec '1'
option reconcile_period_sec '10'
option reconcile_setup_warn_sec '45'
option reconcile_setup_restart_sec '60'
option reconcile_setup_confirm_sec '5'
option reconcile_action_backoff_sec '15'
option reconcile_action_suppress_sec '60'
option reconcile_fail_limit '3'
```

Status is exposed through ubus:

```sh
ubus call network reconcile_status
```

Status fields include enabled flags, pending state, last trigger/action/reason, run count, wireless check count and all active timing/failure limits.

Build-time defaults are also enabled:

```sh
cmake -DRECONCILE_ACTIONS=ON -DRECONCILE_SETUP_RESTART=ON -DRECONCILE_WIRELESS_RECOVER=ON ...
```

Any default can still be compiled out:

```sh
cmake -DRECONCILE_ACTIONS=OFF -DRECONCILE_SETUP_RESTART=OFF -DRECONCILE_WIRELESS_RECOVER=OFF ...
```

Full `wpad` restart is still not a netifd action.


## Stage 8: deployment boundary and live wireless ucode patch

Status: completed.

The wireless recovery logic has one required non-C dependency: the live
`wireless-device.uc` used on the target must contain the missing vif recovery
check. In this tree, `examples/wireless-device.uc` is only a reference copy. On
platforms where wireless ucode is provided by another package, the same minimal
patch must be applied there.

Why this is necessary:

- C netifd can observe only devices that already exist or existed before;
- a wifi-iface that never produced handler data may be invisible to C;
- `wireless-device.uc` owns `data.vif`, `handler_data`, radio state and the
  existing teardown/setup handler path;
- therefore the missing handler data check belongs in live wireless ucode.

Required live `wireless-device.uc` changes are intentionally limited:

- fix the disabled-vif key bug: `disabled[wdev] = true` must become
  `disabled[name] = true`;
- add `wdev_recover_missing_interfaces()` and its small helper state;
- in `check()`, when `wdev_update_disabled_vifs()` reports no config change,
  call `wdev_recover_missing_interfaces(this)` before returning;
- in `wdev_mark_up()`, store `wdev.recover_up_time = time()`;
- in `wdev_reset()`, clear recovery state;
- do not add telemetry calls to live ucode unless needed.

Optional telemetry:

`netifd.reconcile_event()` may be called from live wireless ucode to populate
wireless counters in `ubus call network reconcile_status`. This is optional. If
it is not added, recovery still works and logread remains the source of wireless
recovery events.

Deployment check on target:

```sh
grep -R "missing_kernel_ifname\|wdev_recover_missing_interfaces\|device_ifindex" -n /lib/netifd /usr/lib /usr/share/ucode
```

Expected result: the live wireless ucode path must contain the recovery function
and must call `netifd.device_ifindex()`. If the grep is empty, the target is
running old wireless ucode and Stage 4 cannot recover a vif that never produced
valid handler data.

Minimal recovery test on target:

```sh
logread -f | grep -E 'reconcile|wireless|hostapd'
```

Then inject the failure, for example:

```sh
j='{"iface":"cwlan1_60"}'; ubus call hostapd config_remove "$j"
j='{"iface":"cwlan0_60"}'; ubus call hostapd config_remove "$j"
```

Expected recovery: the affected radio is torn down and set up again through the
existing wireless handler path. `service wpad restart` must not be required.

## Prevention and detection

Prevention:

- one desired-state source: existing netifd config/runtime objects;
- one action path: existing interface/device/proto/wireless mechanisms;
- one event loop: `uloop_timeout` only;
- per-interface backoff;
- no direct service restart;
- no duplicate wireless FSM in C.

Detection:

- log every mismatch before action;
- log every suppressed action and reason;
- count seen/fixed/failed/suppressed events;
- keep last reason and last action timestamp per interface if action mode is
  enabled.

## Risks

- restart storm if action mode has no backoff;
- false mismatch during normal setup;
- conflict with existing wireless ucode retry logic;
- long-running proto handlers may look stuck;
- external devices may not be safe to bounce;
- aggressive recovery may hide real driver or config bugs.

## Tests

Basic compatibility:

```sh
ubus list network
ubus call network reload
ifup lan
ifdown lan
ifstatus lan
wifi reload
service network reload
```

Audit mode:

```sh
logread -f | grep reconcile
```

Broken wireless vif test:

```sh
iw dev wlan0 del
logread -f | grep reconcile
```

Expected in audit mode: only mismatch logs, no recovery.

Expected in later action mode: targeted wireless/interface recovery without full
`service wpad restart`.

## Non-goals

- Do not replace UCI.
- Do not replace ubus API.
- Do not replace proto scripts.
- Do not rewrite wireless ucode in C.
- Do not add a second config database.
- Keep all external ubus/UCI/proto contracts backward compatible.

## First hardware test status

The first hardware test should focus on Stage 4 only.

Recommended build defaults for this test:

```sh
cmake -DRECONCILE_WIRELESS_RECOVER=ON \
      -DRECONCILE_ACTIONS=OFF \
      -DRECONCILE_SETUP_RESTART=OFF ...
```

Expected behavior:

- netifd periodically calls the existing wireless `check_interfaces()` path;
- if one expected wireless vif/vlan is missing after radio `up`, the problem is
  confirmed across checks before recovery;
- recovery is limited to the affected wireless device teardown/setup path;
- generic interface recovery and stuck setup restart stay disabled;
- no `service wpad restart`, no process kill, no full network reload.

Useful runtime check:

```sh
logread -f | grep 'reconcile:'
```

Expected recovery log:

```text
reconcile: wireless=radio0 action=teardown_setup reason=missing_kernel_ifname section=wifi1 ifname=wlan0-2 count=1
```

If the radio is still settling, expected non-action logs are:

```text
reconcile: wireless=radio0 action=none reason=recover_grace age=2
reconcile: wireless=radio0 action=none reason=recover_confirm age=0 section=wifi1 ifname=wlan0-2
```

The test is successful if a missed wifi interface is recreated without running
`service wpad restart` and without repeated recovery loops.

## Stage 6: production visibility

Status: completed.

The reconciler now exposes runtime counters in:

```sh
ubus call network reconcile_status
```

The status output includes:

- total reconcile runs;
- total reconcile events;
- total recovery actions;
- ifup/restart/restart-failed counters;
- suppress/blocked/confirm counters;
- wireless check count;
- wireless recovery event counters;
- last wireless radio/action/reason/section/ifname/count.

Wireless ucode can report recovery events to C through:

```ucode
netifd.reconcile_event({
	radio: wdev.name,
	action: "teardown_setup",
	reason: "missing_kernel_ifname",
	section: missing.section,
	ifname: missing.ifname,
	count: wdev.recover_fail_cnt,
});
```

This keeps logread useful, but makes post-test diagnosis possible without
parsing logs.

## Hardware test result

The first real hardware recovery test passed. The tested failure injection was
removing both interfaces from hostapd config:

```sh
j='{"iface":"cwlan1_60"}'; ssh root@10.11.11.100 "ubus call hostapd config_remove '$j'"
j='{"iface":"cwlan0_60"}'; ssh root@10.11.11.100 "ubus call hostapd config_remove '$j'"
```

Expected and observed result: reconcile recovered the missing wireless runtime
state without running `service wpad restart`.
