# Сценарій демо (6–7 хв) — Home Security Controller

ТЗ: демонстрація 5–10 хв, 30 % оцінки. Кожна сцена закриває конкретний пункт ТЗ
або контрольне питання (у дужках). Таймінги орієнтовні. Термінал накладай на кадр
з платою, як у першій версії, і коментуй голосом або субтитрами.

## Перед записом (не в кадрі)

```sh
cmake --build cmake-build-buildroot --target deploy     # сервіс, libgpiod, час, cards.db
ssh root@<ip> 'date; /etc/init.d/S99security-daemon status; ls /dev/watchdog'
```
- `date` має показувати сьогоднішню дату, а не 1970.
- У базі має бути одна картка (наприклад, Arsen). Другу (Andriy) тримай для `--enroll`, третя — «чужа».
- Радар прикрий (папір/коробка), щоб тривога не спрацьовувала раніше, ніж треба.
- `mount --bind /data /root` більше **не потрібен**: усе лежить у `/usr/bin`, `/etc`, `/var/lib`.

---

## 1. Вступ — 0:00–0:20
Плата і периферія в кадрі. Одне речення: що це за система і які в ній інтерфейси:
SPI (RC522), I2C (SHT3x), GPIO (радар, buzzer, світлофор).

## 2. Власний образ і Linux-частина — 0:20–1:30  *(Boot process, Device Tree, Kernel modules)*
```sh
cat /etc/os-release | head -2        # NAME=Buildroot
cat /proc/1/comm                     # init  -> BusyBox init
uname -r; cat /proc/cmdline          # ядро Buildroot, root=/dev/mmcblk0p2
ls -l /dev/spidev0.0 /dev/i2c-1 /dev/gpiochip0 /dev/watchdog
i2cdetect -y 1                       # 44 -> SHT3x
```
На хості покажи `docs/config.txt`: `dtparam=spi=on`, `dtparam=i2c_arm=on` — це і є «необхідні
зміни DT». Скажи, що boot-ланцюжок такий: firmware Pi 5 → `kernel=Image` (Buildroot) → rootfs Buildroot.

## 3. Сервіс і автозапуск — 1:30–2:30  *(Systemd / init, «налаштувати запуск сервісу»)*
```sh
cat /etc/init.d/S99security-daemon | head -20   # коротко: env, watchdog, start-stop-daemon
reboot
# ... після завантаження, нічого не запускаючи вручну:
/etc/init.d/S99security-daemon status           # running (pid N)
tail -n 3 /var/lib/security_daemon/security.log # [STARTUP] cards=1 sensor=yes wdt=10s, з реальною датою
```

## 4. Контроль доступу — 2:30–3:50  *(контроль присутності, сигналізація)*
У другому вікні: `tail -f /var/lib/security_daemon/security.log` (або таргет `logs`).
1. **Своя картка** → зелений + 1 біп → відкрий радар, рух → жовтий → рух зник → червоний + біп.
   У журналі: `GRANTED` → `PASSAGE_START` → `PASSAGE_END`.
2. **Чужа картка** → 10 біпів, лишається червоний. `DENIED uid=…`.
3. **Рух без картки** понад 2 с → 10 біпів. `INTRUSION рух без картки`.
4. (опційно) Своя картка, але ніхто не пройшов 15 с → `UNLOCK_TIMEOUT`, червоний.

## 5. Адміністрування карток і SIGHUP — 3:50–5:00  *(Linux permissions, IPC/сигнали)*
```sh
/etc/init.d/S99security-daemon stop             # зчитувач потрібен для enroll
security_daemon --enroll "Andriy"               # піднести картку
security_daemon --list
/etc/init.d/S99security-daemon start            # Andriy -> GRANTED
security_daemon --remove <uid Andriy>           # сервіс НЕ зупиняємо
/etc/init.d/S99security-daemon reload           # SIGHUP -> DB_RELOAD cards=1
#   картка Andriy -> DENIED, без перезапуску сервісу
ls -l /etc/security_daemon/cards.db             # -rw------- root  (0600)
```

## 6. Сенсор середовища — 5:00–5:30  *(I2C)*
```sh
security_daemon --sensor                        # T/RH; потримай сенсор у руці й повтори — значення зростуть
grep SENSOR /var/lib/security_daemon/security.log | tail -2   # періодичні виміри від сервісу
```

## 7. Watchdog і самовідновлення — 5:30–6:30  *(Watchdog)*
```sh
/etc/init.d/S99security-daemon stop             # коректна зупинка: magic close, плата НЕ перезавантажується
/etc/init.d/S99security-daemon start
kill -9 $(cat /var/run/security_daemon.pid)     # «падіння» демона без magic close
# ~10 с -> апаратний watchdog перезавантажує плату -> S99 знову піднімає сервіс
/etc/init.d/S99security-daemon status
tail -n 4 /var/lib/security_daemon/security.log # перед новим STARTUP нема SHUTDOWN: процес упав
```
Пауза на перезавантаження — тут можна вирізати.

## 8. Підсумок — 6:30–6:50
Пройдись по таблиці відповідності ТЗ із `DESIGN.md` (розд. 12) і покажи репозиторій.

---

### Чек-лист після запису
- [ ] тривалість 5–10 хв, термінал читається
- [ ] у журналі в кадрі реальні дати, а не 1970
- [ ] нема `LD_LIBRARY_PATH`, `mount --bind`, `ubuntu-base`
- [ ] показано: автостарт після reboot, GRANTED/DENIED/INTRUSION, enroll/remove/reload, sensor, watchdog
