# Запуск як сервіс

Демон працює у foreground і сам ловить `SIGTERM`/`SIGINT` (коректна зупинка, гасить GPIO)
та `SIGHUP` (перечитати базу карток). Тому ні systemd, ні BusyBox-обгортці не потрібен
окремий «daemonize» у коді.

## Який init у образі?

```sh
cat /proc/1/comm      # "systemd"  -> розділ A
                      # "init"     -> BusyBox init -> розділ B
```

## Спільна підготовка (на цілі)

```sh
install -m 0755 security_daemon /usr/bin/security_daemon
mkdir -p /etc/security_daemon
touch    /etc/security_daemon/cards.db      # або скопіюй наявну базу
mkdir -p /var/log
```

Шляхи бази й журналу задаються змінними оточення (їх виставляють unit/скрипт):
`SECURITY_CARDS_DB` (типово `/root/cards.db`), `SECURITY_LOG` (типово `/root/security.log`).

---

## A. systemd

```sh
install -m 0644 security_daemon.service /usr/lib/systemd/system/security_daemon.service
systemctl daemon-reload
systemctl enable --now security_daemon        # автозапуск + старт зараз

systemctl status security_daemon
journalctl -u security_daemon -f              # журнал подій у реальному часі
systemctl reload security_daemon              # SIGHUP -> перечитати картки
```

У Buildroot (rootfs overlay, `BR2_ROOTFS_OVERLAY`):

```
overlay/usr/lib/systemd/system/security_daemon.service
overlay/etc/systemd/system/multi-user.target.wants/security_daemon.service   # symlink -> ../../../../usr/lib/systemd/system/security_daemon.service
```
Symlink у `multi-user.target.wants` вмикає сервіс без `systemctl enable` на цілі.

---

## B. BusyBox init

```sh
install -m 0755 S99security-daemon /etc/init.d/S99security-daemon

/etc/init.d/S99security-daemon start
/etc/init.d/S99security-daemon reload          # SIGHUP -> перечитати картки
/etc/init.d/S99security-daemon stop
tail -f /var/log/security_daemon.log           # журнал подій (journald тут немає)
```

Автозапуск: BusyBox `rcS` сам виконує всі `/etc/init.d/S??*` по порядку, тож
`S99` стартує останнім (після монтування ФС і піднятих драйверів).

У Buildroot (rootfs overlay):
```
overlay/etc/init.d/S99security-daemon          # з правами 0755
```

---

## Перевірка пам'яток (для захисту)

- `systemctl` / `/etc/init.d` — пункт «Налаштувати запуск сервісу» та питання *Systemd*.
- `Restart=on-failure` + `StartLimitBurst` — поведінка при збої.
- `ExecReload`/`reload` + `SIGHUP` — гаряче перечитування без простою.
- журнал: `journalctl -u security_daemon` (systemd) або `/var/log/security_daemon.log` (BusyBox).
