# netifd reconcile runtime checks

Defaults are enabled. The improved netifd starts self-healing immediately after
installation.

Check global reconcile status:

```sh
ubus call network reconcile_status
```

Disable all reconcile actions at runtime:

```sh
uci set network.globals.reconcile_enabled='0'
uci commit network
service network reload
```

Keep audit and wireless checks enabled, but disable generic interface ifup:

```sh
uci set network.globals.reconcile_actions='0'
uci commit network
service network reload
```

Disable stuck setup restart only:

```sh
uci set network.globals.reconcile_setup_restart='0'
uci commit network
service network reload
```

Disable wireless missing-vif recovery checks:

```sh
uci set network.globals.reconcile_wireless_recover='0'
uci commit network
service network reload
```

Tune delays:

```sh
uci set network.globals.reconcile_period_sec='10'
uci set network.globals.reconcile_setup_restart_sec='60'
uci set network.globals.reconcile_setup_confirm_sec='5'
uci set network.globals.reconcile_action_backoff_sec='15'
uci set network.globals.reconcile_action_suppress_sec='60'
uci set network.globals.reconcile_fail_limit='3'
uci commit network
service network reload
```

Test missing wireless interface recovery:

```sh
logread -f | grep -E 'reconcile|awlan0_42|wireless|netifd'
```

In another shell:

```sh
iw dev awlan0_42 del
```

Expected log sequence:

```text
reconcile: wireless=radio... action=none reason=recover_confirm ... ifname=awlan0_42
reconcile: wireless=radio... action=teardown_setup reason=missing_kernel_ifname ... ifname=awlan0_42
```

No reconcile path runs `service wpad restart`.
