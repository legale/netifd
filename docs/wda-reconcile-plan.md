# netifd reconcile hardening plan

## Контекст

Цель этой ветки - усилить существующий netifd, не меняя его внешние интерфейсы: UCI, ubus, protocol handlers, hotplug scripts, netifd-wireless.sh и поведение OpenWrt-обвязки должны остаться совместимыми.

Собственный netrec удалён из wda. На этом этапе не строим параллельный reconciler рядом с netifd. Вместо этого усиливаем сам netifd маленькими изменениями внутри текущей модели devices/interfaces/proto/system.

Главный дефект класса, который надо закрыть: transient failure в середине настройки не должен оставлять интерфейс в частично применённом состоянии навсегда. Например, route/add/address/neighbor может не примениться из-за неподготовленного устройства, link state, master/bridge race, netlink timing, reload/hotplug order. После того как устройство становится готовым, netifd должен сам повторить безопасные idempotent операции.

Внешнее поведение не меняем. Добавляем только внутренние retries, reconcile pass, диагностику и fail-closed guards.

## Что уже есть в netifd

Текущая архитектура уже близка к event driven reconcile, но не завершена:

- device state построен вокруг present/active/users/events;
- interface state построен вокруг IFS_DOWN/IFS_SETUP/IFS_UP/IFS_TEARDOWN;
- proto handlers сообщают IFPEV_UP/IFPEV_DOWN;
- bridge/bonding/vrf уже имеют retry timer для members;
- rtnetlink операции используют replace/create там, где это возможно.

Слабое место сейчас - IP state:

- interface_update_proto_addr() при ошибке system_add_address() ставит addr->failed=true, но общий retry отсутствует;
- __interface_update_route() при ошибке system_add_route() ставит route->failed=true, но retry зависит от будущего update/re-enable;
- interface_ip_set_route_enabled() при одинаковом route->enabled == enabled сразу выходит и не пытается исправить ранее failed route;
- interface_ip_set_enabled() при add address вообще не фиксирует ошибку в addr->failed;
- neighbor add аналогично может остаться failed без общего retry;
- system_rtnl_call() возвращает ошибку, но вызывающий код часто теряет контекст операции в логах.

Вывод: desired state уже есть в структурах interface_ip_settings, но нет дешёвого periodic/event-triggered pass, который повторно применяет failed или missing kernel state.

## Принцип

Не делать новый большой engine.

Использовать существующие структуры как desired state:

- iface->config_ip.addr/route/neighbor/prefix;
- iface->proto_ip.addr/route/neighbor/prefix;
- iface->host_routes;
- iface->l3_dev.dev;
- iface->state;
- device present/active/link events.

Добавить маленький internal reconcile слой только для повторного применения того, что уже считается enabled и desired.

Idempotent operations можно повторять:

- RTM_NEWADDR with NLM_F_CREATE | NLM_F_REPLACE;
- RTM_NEWROUTE with NLM_F_CREATE | NLM_F_REPLACE;
- RTM_NEWNEIGH with NLM_F_CREATE | NLM_F_REPLACE;
- bridge/bonding/vrf member retry уже использует тот же принцип.

Удаление не делать периодическим на первом этапе. Сначала только восстановление missing/failed desired state. Это снижает риск снести внешний state, который netifd не должен трогать.


## Прогресс в коде на 2026-06-10

Сделаны первые две серии hardening patches.

IP state reconcile:

- `00e5aa5 interface-ip: make IP apply failures explicit`;
- `d588bba interface-ip: retry enabled failed routes`;
- `fcf32a9 interface-ip: retry failed IP state`;
- `46095e9 interface-ip: schedule reconcile after device events`.

Фактически добавлено:

- единое правило `failed=true/false` для add address/route/neighbor;
- retry для `enabled && failed` routes, addresses, neighbors;
- retry для `host_routes`;
- global `ip_retry_timeout` в `interface-ip.c`;
- backoff 1s -> 2s -> 4s -> 8s -> 16s -> 30s max;
- порядок retry: address -> route -> neighbor;
- skip apply, если interface не `IFS_UP/IFS_SETUP`;
- skip apply, если нет `l3_dev`, device не present или ifindex invalid;
- schedule retry после `DEV_EVENT_UPDATE_IFINDEX`, `DEV_EVENT_UP`, `DEV_EVENT_AUTH_UP`, `DEV_EVENT_LINK_UP`, `IFPEV_UP`.

Эта серия закрывает класс ошибок: interface уже поднят, но IP address/route/neighbor не применился из-за transient ordering/device/link race. Теперь такая ошибка не должна оставаться permanent до reload.

External proto reconcile:

- `5c07422 proto-ext: log external proto setup failures`;
- `b39a40f proto-ext: retry failed external proto setup`;
- `cb755ea proto-ext: retry unresolved host dependencies`.

Фактически добавлено:

- явные логи external proto setup/teardown lifecycle;
- логи script exit/proto command exit;
- логи setup_failed notify/checkup timeout;
- логи link-up failure для missing device и device claim failure;
- per-proto `setup_retry_timeout`;
- per-proto `host_dep_retry_timeout`;
- backoff 1s -> 2s -> 4s -> 8s -> 16s -> 30s max;
- setup retry для script failed, proto command failed, link-up device missing, device claim failed, external setup failed, setup checkup timeout, setup start failed;
- reset retry после успешного external proto up;
- retry unresolved host dependencies, актуально для WireGuard `endpoint_host`.

Эта серия закрывает более ранний симптом с `wg1`, когда полностью корректный WireGuard config иногда не доходил до рабочего состояния. Если причина была transient setup/device/host dependency race, теперь netifd должен повторить setup или dependency lookup сам. Если причина постоянная, например нет wireguard module/tool или битый config, будет bounded retry с понятными логами.

Что уже неактуально в старом плане:

- Commit 3, 4, 5, 6, 7, 8 из плана фактически покрыты текущими IP-state patches, хотя нумерация коммитов в git другая.
- Дополнительный отдельный IFPEV_UP reapply уже покрыт через `interface_ip_retry_schedule()` после `IFPEV_UP`.
- Для WireGuard-like проблем добавлена отдельная proto-ext серия, которой в первом плане ещё не было.

Что ещё не сделано:

- current-state verification через netlink dump;
- reconcile missing state, если kernel state был успешно применён, а потом удалён внешним компонентом;
- counters в ubus/status/debug;
- унификация diagnostics для bridge/bonding/vrf retry;
- host-side build в этом окружении, потому что нет OpenWrt deps `libubox`, `ubus`, `uci`, `ucode`, `udebug`, `blobmsg_json`.

## Правила безопасности

1. Reconcile не должен вызывать proto setup/teardown.
2. Reconcile не должен менять UCI/ubus API.
3. Reconcile не должен создавать новые protocol states.
4. Reconcile не должен применять IP state, если iface не IFS_UP/IFS_SETUP.
5. Reconcile не должен применять IP state, если l3_dev отсутствует или !dev->present.
6. Reconcile не должен применять route через dev без valid ifindex.
7. Reconcile не должен трогать DEVADDR_EXTERNAL.
8. Reconcile не должен удалять unknown kernel state в первой серии.
9. Reconcile должен иметь bounded retry/backoff, чтобы ошибка не заливала лог.
10. Все новые операции должны логировать iface, dev, op, af, errno/libnl ret, retry count.

## Commit 1: docs: add netifd reconcile hardening plan

Файл:

- docs/wda-reconcile-plan.md

Суть:

- зафиксировать направление работ;
- явно записать, что netrec не возвращаем;
- цель - hardening netifd без внешних API changes.

Проверка:

- git diff --check

## Commit 2: system-linux: add rtnl operation diagnostics

Файлы:

- system-linux.c
- system-log.h, если нужно

Суть:

Добавить локальные helpers для логирования ошибок rtnetlink операций без изменения public system_* API.

Минимально:

- static int system_rtnl_call_op(struct nl_msg *msg, const char *op, const char *ifname)
- оставить system_rtnl_call() как wrapper или перевести только нужные места;
- для route/address/neighbor логировать только failure;
- success не логировать, чтобы не шуметь.

Пример логики:

- add address failed: op=addr_add if=br-lan af=inet ret=-NLE_* errno-like
- add route failed: op=route_add if=br-lan dst=0.0.0.0/0 table=main metric=...

Почему сначала лог:

- перед retry надо видеть реальные причины;
- без этого reconcile может маскировать первопричину.

Проверка:

- cmake --build build, если build уже создан;
- или cmake -B build && cmake --build build.

## Commit 3: interface-ip: make failed state explicit and consistent

Файлы:

- interface-ip.c
- interface-ip.h, если потребуется helper declaration

Суть:

Сделать единое правило:

- system_add_address() failure -> addr->failed = true;
- successful add address -> addr->failed = false;
- system_add_route() failure -> route->failed = true;
- successful add route -> route->failed = false;
- system_add_neighbor() failure -> neighbor->failed = true;
- successful add neighbor -> neighbor->failed = false.

Сейчас это сделано не везде. Например interface_ip_set_enabled() не фиксирует add address failure.

Prevention:

- failed flag становится единственным триггером retry;
- нельзя потерять ошибку в enable path.

Detection:

- логировать transition ok -> failed и failed -> ok.

Проверка:

- build;
- static grep: все system_add_address/route/neighbor в interface-ip.c должны выставлять failed deterministically.

## Commit 4: interface-ip: allow retry of enabled failed routes

Файлы:

- interface-ip.c

Суть:

Сейчас interface_ip_set_route_enabled() делает early return при route->enabled == enabled. Для failed route это закрывает путь к восстановлению.

Изменить правило:

- если enabled=true и route->enabled=true и route->failed=true, повторить add route;
- если add успешен, clear failed;
- если add снова упал, оставить failed и не менять enabled.

То же правило потом применить к addr/neighbor, но маленькими коммитами.

Почему отдельно:

- route failure - самый важный симптом: интерфейс UP, IP есть, но route не добавился.

Проверка:

- build;
- локальный unit пока отсутствует, поэтому минимум compile + code inspection;
- позже добавить host test вокруг fake system ops, если получится без большого refactor.

## Commit 5: interface-ip: add bounded retry timer for failed IP state

Файлы:

- interface-ip.c
- interface-ip.h
- interface.h, если нужен флаг в struct interface

Суть:

Добавить один небольшой retry timer на interface или global IP retry timer.

Предпочтение: global static uloop_timeout в interface-ip.c, который проходит по interfaces и трогает только failed enabled state.

Причина:

- меньше state в struct interface;
- меньше изменений ABI внутри netifd;
- проще отключать, проще debug.

Алгоритм:

- при add addr/route/neighbor failure вызвать interface_ip_retry_schedule(iface);
- timer через 1000 ms;
- pass по всем interfaces;
- skip если iface state не IFS_UP/IFS_SETUP;
- skip если !iface->l3_dev.dev || !dev->present;
- retry only enabled && failed entries;
- если остались failures, schedule again с bounded backoff.

Backoff:

- 1s, 2s, 5s, 10s, 30s max;
- on success reset to 1s;
- лог на первом fail, потом rate-limited.

Не делать:

- не вызывать interface_set_up();
- не вызывать proto renew;
- не удалять routes;
- не перезапускать device.

## Commit 6: interface-ip: retry failed addresses and neighbors

Файлы:

- interface-ip.c

Суть:

Расширить retry pass:

- enabled && failed addr -> system_add_address(); on success clear failed and re-apply subnet route/policy rules if needed;
- enabled && failed neighbor -> system_add_neighbor(); on success clear failed.

Осторожно:

- addr retry должен быть до route retry;
- route retry после addr retry;
- neighbor retry после route retry.

Причина порядка:

- kernel route может зависеть от address/link/master state;
- address является базой для source policy/subnet route.

## Commit 7: interface-ip: trigger reconcile on l3 device events

Файлы:

- interface.c
- interface-ip.c/h

Суть:

Когда l3/main device получает DEV_EVENT_UP, DEV_EVENT_LINK_UP, DEV_EVENT_AUTH_UP, вызвать schedule IP retry/reconcile.

Только schedule, не делать apply прямо из callback.

Причина:

- event order может быть грязным;
- отложенный timer схлопывает несколько событий;
- меньше риска reentrancy.

## Commit 8: interface-ip: add cheap desired IP reapply pass on IFPEV_UP

Файлы:

- interface.c
- interface-ip.c/h

Суть:

После proto сообщает IFPEV_UP и interface становится IFS_UP, запланировать reconcile pass.

Задача:

- закрыть ситуацию, когда proto выставил addr/routes до полной готовности dev/master;
- дать один гарантированный post-up reapply.

Не менять порядок старого setup. Только дополнительный deferred pass.

## Commit 9: device: add transparent present/active mismatch logs

Файлы:

- device.c
- bridge.c/bonding.c/vrf.c, если нужно

Суть:

Добавить диагностические WARN/NOTICE только на подозрительные состояния:

- device present but ifindex invalid;
- device active but system_if_check() не подтверждает kernel link;
- bridge/bond/vrf member failed retry count grows;
- stale master detach path срабатывает.

Не логировать нормальные частые события.

Цель:

- на реальном AP быстро понять, почему reconcile повторяется.

## Commit 10: bridge/bonding/vrf: unify member retry diagnostics

Файлы:

- bridge.c
- bonding.c
- vrf.c

Суть:

Не менять семантику. Добавить одинаковый лог:

- master name;
- member name;
- operation;
- ret;
- retry count.

Уже есть retry timers, но диагностика слабая.

## Commit 11: interface-ip: add reconcile counters to status/debug only

Файлы:

- interface-ip.c
- ubus.c, если нужен вывод

Суть:

Добавить внутренние counters:

- retry_scheduled;
- retry_passes;
- addr_retry_ok/fail;
- route_retry_ok/fail;
- neigh_retry_ok/fail.

Экспортировать только в status/debug, не менять основной API.

Если риск большой - оставить только logs без ubus export.

## Commit 12: optional current-state verification for routes

Файлы:

- system-linux.c
- interface-ip.c

Суть:

Добавить проверку presence route/address через netlink dump только для failed entries или по debug command.

Не делать full diff loop сразу.

Причина:

- add replace обычно idempotent и дешевле, чем dump/compare;
- full current-state reconcile сложнее и опаснее.

Когда нужно:

- если repeated RTM_NEWROUTE success, но route реально отсутствует;
- если kernel принимает команду, но другой компонент удаляет state.

## Commit 13: optional config knob for hardening mode

Файлы:

- config.c
- netifd.h/interface-ip.c

Суть:

По умолчанию hardening включён, потому что внешнее API не меняется.

Но можно добавить runtime/internal option только если нужен escape hatch:

- retry disabled;
- retry interval max;
- log verbosity.

На первом этапе не добавлять UCI option, чтобы не расширять внешний интерфейс.

## Что не делать в этой серии

- не переписывать netifd в полноценный Kubernetes-style controller;
- не добавлять отдельный daemon/thread;
- не менять UCI schema;
- не менять ubus method names/arguments;
- не менять protocol handler contract;
- не трогать hostapd/mac80211 scripts;
- не удалять unknown kernel routes/address;
- не делать full desired/current diff для всего kernel state;
- не запускать shell/ip/nft commands из netifd;
- не добавлять sleeps.

## Минимальный acceptance

Статус на 2026-06-10:

- route/address/neighbor add failure больше не должен становиться permanent без reload - сделано;
- повторный apply после device/link/proto-up событий - сделано;
- retry bounded/backoff - сделано;
- external proto setup retry для WireGuard-like failures - сделано;
- host dependency retry для endpoint_host - сделано;
- existing OpenWrt UCI/ubus/scripts/proto contracts не менялись;
- wda не линкует netrec;
- build в текущем контейнере не подтверждён из-за отсутствующих OpenWrt deps.

## Практический порядок работ дальше

Сделанный слой не превращать в большой state engine.

Следующие безопасные шаги:

1. Собрать в OpenWrt SDK и проверить runtime на AP.
2. Проверить `wg1` сценарий: `ifstatus wg1`, `ip link show wg1`, `ip addr show dev wg1`, `ip route show table all`.
3. По логам понять, какой path реально лечит проблему: proto setup retry, host dependency retry или IP-state retry.
4. Добавить только недостающие diagnostics, если логов недостаточно.
5. Потом можно думать о counters/status-debug.
6. Current-state verification через netlink dump делать только после живого подтверждения, что add-retry недостаточен.

## Риски

1. Повторный RTM_NEWROUTE может скрыть ошибку ordering, но это осознанный tradeoff ради self-healing.
2. Если внешний компонент намеренно удаляет route, retry может вернуть её. На первом этапе retry только для entries, которые netifd сам считает enabled и failed. Full missing-state restore не включать без отдельного решения.
3. Логи могут стать шумными на нестабильном link. Нужен rate-limit/backoff.
4. Нельзя повторять delete path периодически: риск удалить чужой state выше пользы.
5. Нельзя вызывать proto setup из reconcile: это может сломать protocol handler FSM.

## Первый кодовый шаг после плана

Начать с маленького коммита в interface-ip.c:

- сделать failed flag consistent для route/address/neighbor;
- добавить логи на add failure;
- не добавлять retry timer в том же коммите.

Это даст прозрачность и подготовит следующий commit без изменения поведения.
