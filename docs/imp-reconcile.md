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
- `examples/wireless.uc`, `examples/wireless-device.uc`: wireless device and vif
  FSMs, retry and hotplug processing.
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

Stage 2 is implemented as an opt-in recovery mode. By default, the reconciler
still behaves as audit-only code and logs mismatches with `action=none`.

Build-time control:

```sh
cmake -DRECONCILE_ACTIONS=ON ...
```

When action mode is enabled:

- `IFS_DOWN` + `want_up` calls the existing `interface_set_up()` path;
- repeated soft actions are guarded by per-interface backoff and suppression;
- wireless check is coalesced through `netifd_ucode_check_network_enabled()`;
- all other mismatches still log only `action=none`.

When action mode is disabled:

- no interface setup is called by the reconciler;
- no wireless check is called by the reconciler;
- stage 1 audit behavior is preserved.

## Stage 3: stuck setup recovery

Implementation status: in progress.

Setup age is already tracked through `iface->setup_time`. The reconciler warns
about stuck setup in audit mode and can optionally restart only the affected
interface through the existing `interface_restart()` path.

This recovery is disabled by default and requires an explicit build option:

```sh
cmake -DRECONCILE_SETUP_RESTART=ON ...
```

Current stage 3 action:

- `IFS_SETUP` older than `REC_SETUP_RESTART_SEC` -> `interface_restart(iface)`;
- no direct proto manipulation;
- no device state change;
- no `service network restart`;
- no `service wpad restart`.

Safety guards:

- same desired-state gates as stage 2;
- no action during `IFS_TEARDOWN`;
- per-interface action backoff;
- failure limit;
- suppression window after repeated failures.

Recovery must remain opt-in until tested on target hardware.

## Stage 4: missing kernel object recovery

If an interface is `IFS_UP` but the kernel object is gone:

1. run wireless/network check path first if the interface is wireless-related;
2. wait a grace interval;
3. restart only the affected interface if it is still broken.

This replaces the practical effect of `service wpad restart` for the common case
where only one vif or interface was missed.

## Stage 5: optional escalation

Escalation is disabled by default.

Possible later actions:

- retry one wireless device through the existing wireless ucode handler;
- expose diagnostic counters through additive ubus status fields;
- add a debug-only ubus method to trigger reconcile manually.

Full `wpad` restart must not be a default netifd action.

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
- Do not change external netifd behavior by default.
