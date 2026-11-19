# Design Document — Home Security Controller (Raspberry Pi 5)

**Автор:** Arsen · **Репозиторій:** HomeSecurityController · **Платформа:** Raspberry Pi 5 (aarch64), власний образ Linux (Buildroot)

---

## 1. Мета та область

Контролер домашньої сигналізації з контролем доступу за RFID-перепустками.
Система керує проходом через точку доступу: ідентифікує власника картки,
відкриває/блокує прохід (індикація світлофором), фіксує рух радаром, подає
звукові сигнали (buzzer), веде журнал подій і моніторить середовище (T/RH).
Працює як фоновий сервіс на власному образі Linux, автозапуск при старті.

## 2. Вимоги (з ТЗ)

- Власний образ Linux; запуск як сервіс.
- Периферія на C, 2–3 різні інтерфейси.
- Функції: контроль присутності, журнал подій, сигналізація.
- Практична частина: daemon, event queue, logging.

## 3. Апаратна частина та інтерфейси

Задіяно **три** інтерфейси: **SPI** (RFID), **I2C** (сенсор), **GPIO** (дискретні лінії).

| Периферія | Інтерфейс | Пін (фіз.) | GPIO |
|---|---|---|---|
| RFID RC522 | SPI0 | SDA 24 / SCK 23 / MOSI 19 / MISO 21 / RST 22 | CE0/11/10/9/25 |
| SHT3x (T/RH) | I2C1 | SDA 3 / SCL 5 | GPIO2 / GPIO3 |
| Buzzer (через транзистор) | GPIO | 11 | GPIO17 |
| Радар RCWL-0516 | GPIO | 13 | GPIO27 |
| Світлофор R/G/Y | GPIO | 15 / 16 / 37 | GPIO22 / 23 / 26 |

**Сенсор середовища.** ТЗ пропонує BME680 або ENS160+AHT21; у проєкті використано **SHT3x** —
той самий інтерфейс (I2C) і та сама роль у системі (T/RH у журнал). BME680 сідає на ту саму
шину (адреса 0x76/0x77) і додав би тиск і якість повітря — див. розд. 11.

RC522 і SHT3x живляться від 3.3 V. GPIO керуються з userspace через **libgpiod v1**
(`/dev/gpiochip0` = контролер RP1 на Pi 5). Повна схема — у EasyEDA.

## 4. Архітектура ПЗ

Демон однопроцесний, з одним фоновим потоком-логером. Події від реального часу
(картка, рух, стани) кладуться в чергу й обробляються логером окремо, щоб
повільний файловий ввід/вивід не блокував реакцію на периферію.

```
   ┌─────────────── security_daemon ───────────────┐
   │  головний цикл (realtime, ~20 мс)              │
   │   RC522(SPI) ─ radar(GPIO) ─ SHT3x(I2C)        │
   │   світлофор(GPIO) ─ buzzer FSM(GPIO)           │
   │        │ push                                  │
   │        ▼                                       │
   │   [event_queue]  (кільц. буфер, mutex+cond)    │
   │        │ pop                                   │
   │        ▼                                       │
   │   [logger thread] ─► журнал (файл + stdout)    │
   └────────────────────────────────────────────────┘
```

**Модулі:**

| Модуль | Призначення |
|---|---|
| `spi/rc522` | Драйвер MFRC522: UID, MIFARE auth/read/write |
| `i2c/i2c`, `i2c/sht3x` | I2C-хелпер + драйвер SHT3x (CRC-8, конверсія) |
| `gpio/gpio` | Обгортка libgpiod |
| `gpio/buzzer` | Неблокуючий зумер-FSM |
| `gpio/radar` | Читання радара |
| `gpio/traffic_light` | Світлофор (R/Y/G) |
| `core/card_store` | Перепустки у файлі (load/save атомарно, enroll/remove) |
| `core/events` | Черга подій (producer/consumer) |
| `core/logger` | Потік-споживач → журнал |
| `core/watchdog` | Апаратний watchdog (`/dev/watchdog`): kick з головного циклу, magic close |
| `core/board_config` | Піни, шини, перевірка колізій |
| `main` | Автомат доступу + CLI |

## 5. Автомат станів контролю доступу

```
   LOCKED ──(дозволена картка)──► UNLOCKED ──(рух)──► PASSING ──(рух зник)──► LOCKED
   (червоний)   зелений + 1 біп   (зелений)  жовтий   (жовтий)  червоний + біп
     │
     ├─ невідома картка ──► 10 біпів (відмова), лишається LOCKED
     └─ рух без картки >2с ─► 10 біпів (тривога)
```

- **UNLOCKED** має таймаут (15 с): якщо ніхто не пройшов — знову LOCKED.
- **PASSING** завершується з антидребезгом (рух відсутній ≥1.2 с).
- Зумер неблокуючий (FSM із тіканням у головному циклі), тож під час тривоги
  радар і зчитувач опитуються без пауз.

## 6. Багатопотоковість та IPC

- **Черга подій** — кільцевий буфер під `pthread_mutex` + `pthread_cond`
  (condvar на `CLOCK_MONOTONIC`). Це внутрішньопроцесний IPC.
- `event_queue_push` неблокуючий; при переповненні подія відкидається,
  лічильник `dropped` фіксується логером.
- **Логер** — єдиний потік-споживач: `pop` з таймаутом, коректний `shutdown`
  (добирає залишок і виходить). Протестовано під ThreadSanitizer.

## 7. Журналювання

Кожна подія — рядок: `2026-10-28T14:00:41Z [GRANTED] uid=25:58:9F:E0 name="Guest"`.
Типи: STARTUP, GRANTED, DENIED, INTRUSION, PASSAGE_START/END, UNLOCK_TIMEOUT,
DB_RELOAD, SENSOR, ERROR. Призначення — файл (`/var/lib/security_daemon/security.log`) та
stdout (під systemd → journald; під BusyBox — файл).

- Журнал саме в `/var/lib`, бо у Buildroot `/var/log` — symlink на `/tmp` (tmpfs), і журнал
  безпеки зникав би після перезавантаження.
- **Час.** У Pi 5 без батарейки RTC і без мережі годинник стартує з 1970-01-01. Тому `deploy`
  виставляє час із хоста й пише його в RTC (`hwclock -w`), а init-скрипт при старті читає RTC
  (`hwclock -s`) і, якщо є мережа, підтягує NTP (BusyBox `ntpd -q`).

## 8. Перепустки та безпека

- Список дозволених UID — у файлі (`/etc/security_daemon/cards.db`), атомарний
  запис (tmp+rename), права `0600`, тека `0700` (лише root). Команди `--enroll`/`--remove`/`--list`.
- **Гаряче перечитування** по `SIGHUP` — без перезапуску сервісу.
- Обмеження (чесно): UID MIFARE клонується «magic»-картками, а телефони дають
  випадковий UID. Для реального доступу надійніше перевіряти секрет у секторі
  MIFARE під власним ключем — драйвер RC522 це підтримує (auth A/B).

## 9. Linux-частина

- **Власний образ**: ядро + userland **Buildroot** 2026.08 (`NAME=Buildroot`, init — BusyBox).
- **Картка**: записано `sdcard.img` Buildroot — розмітка genimage: p1 FAT 32 МБ (boot),
  p2 ext4 120 МБ (**rootfs Buildroot**, `root=/dev/mmcblk0p2`). Додатково p3 `data`.
- **Boot**: з власними boot-файлами Buildroot Pi 5 не стартував, тому **вміст FAT-розділу**
  замінено файлами з Raspberry Pi OS Lite (`bcm2712*.dtb`, `overlays/`, `config.txt`), ядро
  Pi OS прибрано, покладено `Image` від Buildroot (`kernel=Image` у `config.txt`),
  `cmdline.txt` вказує на p2. Ланцюжок: EEPROM-bootloader Pi 5 → `config.txt` → `Image`
  (Buildroot) + `bcm2712-rpi-5-b.dtb` → rootfs Buildroot → BusyBox init.
  Перевірка — `docs/BUILD.md`, розд. 1.3.
- **Зміни Device Tree**: `dtparam=spi=on`, `dtparam=i2c_arm=on` (вмикають
  `/dev/spidev0.0` та `/dev/i2c-1`). Власний `.dtbo` не потрібен — периферія стандартна.
- **Сервіс**: init = BusyBox → `init/S99security-daemon` (автозапуск через `rcS`;
  `start|stop|restart|reload|status`). Для systemd-образу є еквівалентний `security_daemon.service`.
- **Watchdog** (`core/watchdog`): демон відкриває `/dev/watchdog` (драйвер `bcm2835-wdt`, таймаут
  10 с) і годує його раз на секунду з головного циклу. Якщо цикл завис або процес упав —
  таймер SoC перезавантажує плату, і `S99` знову піднімає сервіс (самовідновлення).
  Коректна зупинка (`SIGTERM`) робить magic close (`'V'`), тож `stop` плату не перезавантажує.
  Вмикається змінною `SECURITY_WATCHDOG` (її ставить init-скрипт), тому ручний запуск для
  налагодження watchdog не чіпає. Подія `STARTUP` показує `wdt=10s`.

## 10. Збірка та розгортання

Крос-компіляція CMake із Buildroot-тулчейном. Деплой — `scripts/deploy.sh` (таргет `deploy`):
бінарник у `/usr/bin`, init-скрипт у `/etc/init.d`, libgpiod із sysroot у `/usr/lib`
(без `LD_LIBRARY_PATH`), синхронізація часу, старт сервісу. `run` — те саме, але демон у
терміналі; `logs` — `tail -f` журналу. Детально — `docs/BUILD.md`.

## 11. Обмеження та подальший розвиток

- Аутентифікація за UID (див. розд. 8) — перехід на перевірку сектора під ключем.
- Винести конфіг (піни, інтервали) у файл замість `board_config.h`.
- BME680/ENS160 замість SHT3x дало б ще й якість повітря/тиск.
- Boot-файли Pi 5 (dtb/overlays/config) брати з пакета `rpi-firmware` Buildroot через
  post-image скрипт, щоб `sdcard.img` вантажився без ручної заміни FAT-розділу.

## 12. Відповідність ТЗ

| Вимога | Реалізація |
|---|---|
| Власний образ Linux | ядро + rootfs Buildroot (розмітка `sdcard.img`), dtb/config на FAT — з Pi OS Lite (розд. 9) |
| Запуск як сервіс | BusyBox init-скрипт / systemd-юніт |
| 2–3 інтерфейси | SPI + I2C + GPIO (розд. 3) |
| Контроль присутності | радар + автомат станів (розд. 5) |
| Журнал подій | event queue + logger (розд. 6–7) |
| Сигналізація | buzzer-FSM: відмова, тривога (розд. 5) |
| daemon / event queue / logging | розд. 4, 6, 7 |
| Watchdog (контр. питання) | `/dev/watchdog` + magic close (розд. 9) |

**Контрольні питання** (готовність до захисту): Systemd / BusyBox init, SPI, Device
Tree (dtparam), GPIO (libgpiod/RP1), IPC (mutex+condvar черга), Cross compile
(Buildroot toolchain), Linux permissions (cards.db 0600), Boot process (EEPROM Pi 5
→ config.txt → kernel Buildroot → rootfs Buildroot), Kernel modules (spidev, i2c-dev), Watchdog (розд. 9).
