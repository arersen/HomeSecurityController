# Сценарій демо — Home Security Controller

ТЗ: демонстрація 5–10 хв, 30 % оцінки. Знімаєш без звуку, потім накладаєш текст і/або озвучку.
Кожен крок: **що робиш** → **команда** → **що видно** → **текст на відео** (його ж можна озвучити).
Сцени з позначкою *(опц.)* — якщо не влазиш у 10 хв, виріж їх першими. Паузи на reboot вирізай.

Кадр: плата з макеткою + термінал поверх (великий шрифт). Два вікна термінала:
**A** — команди, **B** — `tail -f /var/lib/security_daemon/security.log` (журнал наживо).

---

## 0. За кадром — лише фізичне й секрети

- Картки під рукою: **Arsen** (своя), **Andriy** (для enroll), **чужа**, **iPhone**.
- Радар прикрий папером/коробкою — відкриватимеш, коли треба рух.
- `~/.ssh/config` з `IdentityAgent none` — щоб у кадрі не вискакували вікна Gateway.
- Не показуй у кадрі вміст `~/.ssh`, `user-data`/`network-config` на boot-розділі (Wi‑Fi пароль).
- ⚠️ Під час запису **не висмикуй плату з розетки**: RTC без батарейки скинеться на 1970.
  Перезавантаження через `reboot` або watchdog час не збивають.

Усе інше (деплой, час, база, журнал) — **у кадрі**, сцена 2b.

---

## 1. Вступ — 0:00–0:20

**Що робиш:** плата і периферія в кадрі, повільно проведи камерою.

> **Текст:** Home Security Controller на Raspberry Pi 5. Контроль доступу за RFID-картками,
> радар присутності, сигналізація, журнал подій. Власний образ Linux (Buildroot), сервіс із автозапуском.
> Інтерфейси: SPI — зчитувач RC522, I2C — сенсор SHT3x, GPIO — радар, зумер, світлофор.

---

## 2. Збірка на хості — 0:20–0:50  *(Cross compile)*

**Вікно на хості (WSL/CLion):**
```sh
cmake --build cmake-build-buildroot -j
file cmake-build-buildroot/security_daemon
```
**Видно:** `ELF 64-bit LSB … ARM aarch64`.

> **Текст:** Застосунок на C збирається на ПК крос-компілятором із тулчейну Buildroot.
> Результат — бінарник під ARM64. Таргет `deploy` копіює його на плату й встановлює як сервіс.

---

## 3. Власний образ і boot — 0:50–2:00  *(Boot process, Device Tree, Kernel modules)*

**Вікно A (SSH на плату):**
```sh
cat /etc/os-release | head -2        # NAME=Buildroot, VERSION=2026.08…
cat /proc/1/comm                     # init
ls -l /sbin/init                     # -> ../bin/busybox
cat /proc/partitions                 # p1 32768, p2 122880
cat /proc/cmdline                    # … root=/dev/mmcblk0p2 rootfstype=ext4 …
uname -a                             # ядро Buildroot, aarch64
```
> **Текст:** Коренева ФС і ядро — Buildroot. PID 1 — BusyBox init.
> Розмітка картки — з `sdcard.img` Buildroot: розділ boot 32 МБ, rootfs 120 МБ.
> На boot-розділ поставлено dtb і config від Raspberry Pi OS Lite (з ними Pi 5 стабільно стартує),
> ядро — `Image` від Buildroot.
>
> Завантаження: EEPROM-bootloader Pi 5 → `config.txt` (`kernel=Image`) → ядро + Device Tree →
> `cmdline.txt` вказує корінь `/dev/mmcblk0p2` → BusyBox init.

**Device Tree** — на хості покажи `docs/config.txt` (або на платі `grep dtparam` у boot-розділі):
```sh
mkdir -p /mnt/boot && mount /dev/mmcblk0p1 /mnt/boot
grep -E "dtparam=(spi|i2c_arm)|kernel=" /mnt/boot/config.txt
umount /mnt/boot
```
```sh
ls -l /dev/spidev0.0 /dev/i2c-1 /dev/gpiochip0 /dev/watchdog
ls /sys/bus/spi/drivers/ ; ls /sys/class/i2c-dev/
i2cdetect -y 1                       # 44 -> SHT3x
```
> **Текст:** Зміни Device Tree — лише `dtparam=spi=on` і `dtparam=i2c_arm=on`: bootloader вмикає
> вже наявні вузли SPI0 та I2C1. Ядро піднімає драйвери spidev та i2c-dev → з'являються
> `/dev/spidev0.0` і `/dev/i2c-1`. Власний overlay не потрібен — периферія стандартна.
> SHT3x відповідає на шині I2C за адресою 0x44.

---

## 4. Сервіс і автозапуск — 2:00–3:00  *(Systemd / init, запуск сервісу)*

```sh
grep -v '^#' /etc/inittab | grep .   # ::sysinit:/etc/init.d/rcS …
ls /etc/init.d/                      # S01… S40network … S99security-daemon
sed -n '/^start()/,/^}/p' /etc/init.d/S99security-daemon
```
> **Текст:** BusyBox init читає `/etc/inittab` і запускає `rcS`, а той — усі скрипти `S??*`
> за алфавітом. `S99security-daemon` стартує останнім, після мережі й драйверів.
> `start-stop-daemon -b -m` відправляє демон у фон і створює pidfile.

```sh
reboot
# … (виріж очікування) … SSH знову:
/etc/init.d/S99security-daemon status
tail -n 3 /var/lib/security_daemon/security.log
```
**Видно:** `running (pid N)`, `[STARTUP] cards=1 sensor=yes wdt=10s state=LOCKED` з сьогоднішньою датою.

> **Текст:** Після перезавантаження сервіс піднявся сам. У журналі — STARTUP: одна картка в базі,
> сенсор знайдено, апаратний watchdog увімкнено (10 с).

*(опц., 10 с)* Що тримає демон:
```sh
ls -l /proc/$(cat /var/run/security_daemon.pid)/fd
```
> **Текст:** Демон тримає відкритими spidev, gpiochip, watchdog і файл журналу.
> stdout — у /dev/null, stderr — у файл: у демона немає термінала.

---

## 5. Контроль доступу — 3:00–4:40  *(контроль присутності, сигналізація)*

Тепер у кадрі **вікно B** (журнал) + плата.

**5.1 Своя картка → прохід**
Приклади **Arsen** → відкрий радар, проведи рукою → прибери руку.
**Видно:** зелений + 1 біп → жовтий → червоний + біп. У журналі `GRANTED uid=… name="Arsen"` →
`PASSAGE_START` → `PASSAGE_END`.
> **Текст:** Дозволена картка — зелений і короткий сигнал. Радар фіксує рух — жовтий, прохід.
> Руху немає 1.2 с — прохід завершено, знову червоний (замкнено).

**5.2 Чужа картка**
Приклади **чужу**. **Видно:** 10 біпів, лишається червоний. `DENIED uid=…`.
> **Текст:** Невідома картка — відмова: 10 сигналів, прохід лишається закритим.

**5.3 Рух без картки (вторгнення)**
Відкрий радар і рухайся > 2 с. **Видно:** 10 біпів. `INTRUSION рух без картки`.
> **Текст:** Рух без картки довше 2 секунд — тривога.

**5.4** *(опц.)* **Відкрили, але ніхто не пройшов**
Приклади Arsen, радар закритий, чекай 15 с. **Видно:** червоний + біп. `UNLOCK_TIMEOUT`.
> **Текст:** Якщо після відкриття ніхто не пройшов за 15 с — прохід знову блокується.

**5.5** *(опц.)* **iPhone**
Приклади iPhone двічі. **Видно:** двічі `DENIED`, **різні** UID, що починаються з `08:`.
> **Текст:** Телефон щоразу дає випадковий UID (префікс 08 за ISO 14443-3), тому за UID його не
> зареєструвати. Це відоме обмеження автентифікації за UID; надійніший шлях — перевірка секрету
> в секторі MIFARE під власним ключем.

---

## 6. Адміністрування карток, права, SIGHUP — 4:40–6:00  *(Linux permissions, сигнали)*

```sh
/etc/init.d/S99security-daemon stop      # зчитувач тримає сервіс — звільняємо
security_daemon --enroll "Andriy"        # піднести картку Andriy
security_daemon --list
/etc/init.d/S99security-daemon start
```
Приклади Andriy → `GRANTED name="Andriy"`.
> **Текст:** Реєстрація нової картки. Сервіс тримає лінію RST зчитувача, тому на час `--enroll`
> його зупиняємо. Після старту картка Andriy проходить.

```sh
security_daemon --remove <uid Andriy>    # сервіс НЕ зупиняємо — це лише файл бази
/etc/init.d/S99security-daemon reload    # SIGHUP
```
**Видно:** у журналі `DB_RELOAD cards=1`. Приклади Andriy → `DENIED`.
> **Текст:** Видалення картки змінює лише файл бази. Команда `reload` надсилає сервісу SIGHUP —
> він перечитує базу без перезапуску. Andriy більше не проходить.

```sh
ls -ld /etc/security_daemon ; ls -l /etc/security_daemon/cards.db
```
**Видно:** `drwx------ root` і `-rw------- root`.
> **Текст:** База перепусток доступна лише root: тека 0700, файл 0600. Запис атомарний
> (тимчасовий файл + rename) — збій живлення не залишить обрізаної бази.

---

## 7. Сенсор середовища — 6:00–6:30  *(I2C)*

```sh
security_daemon --sensor                 # T/RH
# потримай SHT3x пальцями 10–15 с
security_daemon --sensor                 # значення зросли
grep SENSOR /var/lib/security_daemon/security.log | tail -2
```
> **Текст:** SHT3x по I2C: температура й вологість, з перевіркою CRC. Сервіс сам пише виміри
> в журнал щохвилини.

---

## 8. Watchdog і самовідновлення — 6:30–7:40  *(Watchdog)*

```sh
/etc/init.d/S99security-daemon stop      # плата НЕ перезавантажується
/etc/init.d/S99security-daemon start
```
> **Текст:** Коректна зупинка: демон отримує SIGTERM і робить magic close watchdog — перезавантаження немає.

```sh
kill -9 $(cat /var/run/security_daemon.pid)
# ~10 с — плата перезавантажується (виріж очікування)
/etc/init.d/S99security-daemon status
tail -n 4 /var/lib/security_daemon/security.log
```
**Видно:** новий `STARTUP`, а перед ним **нема** `SHUTDOWN` — процес «упав».
> **Текст:** Симулюємо падіння: `kill -9` не дає демону зробити magic close. Апаратний таймер
> у SoC не отримує «kick» і через 10 с перезавантажує плату. BusyBox init сам упалий сервіс не
> перезапускає — це робить watchdog, а S99 піднімає сервіс знову. Те саме стається при зависанні
> головного циклу.

---

## 9. *(опц.)* Журнал на моніторі — 7:40–8:00

Якщо до Pi підключено HDMI-монітор:
```sh
tail -f /var/lib/security_daemon/security.log > /dev/tty1 &
```
Приклади Arsen — рядок з'являється на моніторі.
> **Текст:** Журнал — звичайний файл; його можна вивести на будь-який термінал, наприклад на консоль HDMI.

---

## 10. Підсумок — 8:00–8:20

Покажи репозиторій (структура `src/`, `docs/`) і таблицю відповідності ТЗ із `docs/DESIGN.md` (розд. 12).
> **Текст:** Усі вимоги ТЗ: власний образ, сервіс з автозапуском, три інтерфейси, контроль
> присутності, журнал подій через чергу і окремий потік-логер, сигналізація, watchdog.
> Документація — у `docs/`: DESIGN, BUILD, схема, зміни Device Tree.

---

### Чек-лист після запису
- [ ] тривалість 5–10 хв (опц. сцени виріжи, якщо довше), паузи на reboot вирізані
- [ ] у журналі в кадрі сьогоднішні дати, не 1970
- [ ] термінал читається; не видно паролів/`user-data`
- [ ] показано: збірка, Buildroot/boot, DT, автостарт після reboot, GRANTED/PASSAGE/DENIED/INTRUSION,
      enroll/remove/reload, права 0600, sensor, watchdog (stop vs kill -9)
- [ ] у тексті ніде немає «U-Boot»
