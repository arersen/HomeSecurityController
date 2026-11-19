# Build instructions — Home Security Controller (Raspberry Pi 5)

Контролер домашньої сигналізації: RFID-доступ (RC522/SPI), контроль присутності
(радар RCWL/GPIO), сигналізація (buzzer), індикація (світлофор), середовище
(SHT3x/I2C), журнал подій через чергу + окремий потік-логер.

> Позначки `<...>` заміни на свої значення (defconfig, пристрій SD, IP плати).

---

## 0. Передумови (хост, Ubuntu/WSL)

```sh
sudo apt install build-essential git bc bison flex libssl-dev \
                 cmake file rsync wget unzip
```

Структура (як у цьому проєкті):
```
~/ajax_project/
├── buildroot/                     # образ Linux
└── HomeSecurityController/        # цей застосунок
```

---

## 1. Образ Linux (Buildroot)

### 1.1. Конфігурація
```sh
cd ~/ajax_project/buildroot
make <your_defconfig>              # напр. raspberrypi5_defconfig
make menuconfig
```

Увімкнути (обов'язково):
- **Target packages → Libraries → Hardware handling → `libgpiod`** — застосунок лінкується `-lgpiod`.
- init-система під сервіс (див. `init/README.md`): **BusyBox init** (типово) або **systemd**
  (`System configuration → Init system`).

Бажано:
- **Networking applications → `dropbear`** або `openssh` — для деплою по SSH.
- **Hardware handling → `i2c-tools`** — `i2cdetect`/`i2cget` для діагностики I2C.

Ядро має містити `spidev` (CONFIG_SPI_SPIDEV) та `i2c-dev` (CONFIG_I2C_CHARDEV) —
у RPi-ядрі вони є; активуються через `config.txt` (крок 3).

### 1.2. Збірка
```sh
make -j"$(nproc)"
# результат (output/images/):
#   Image          ядро (arm64) — лише ядро, rootfs у ньому НЕМА
#   *.dtb          device tree
#   rootfs.ext4    образ кореневої ФС (зібраний з output/target/)
#   boot.vfat      образ FAT-розділу
#   sdcard.img     уся картка: p1 = boot.vfat, p2 = rootfs.ext4 (genimage)
# output/host/     крос-тулчейн
```

### 1.3. Складання завантажувальної картки (розмітка й rootfs Buildroot + boot-файли Pi OS Lite)

Чистий `sdcard.img` від Buildroot на Raspberry Pi 5 **не завантажувався** з власними
boot-файлами. Тому розмітку й кореневу ФС взято від Buildroot, а **вміст FAT-розділу**
замінено файлами з Raspberry Pi OS Lite (dtb/overlays/config, з якими Pi 5 стабільно
стартує), і на нього покладено ядро Buildroot:

1. Записати `output/images/sdcard.img` на SD-картку:
   ```sh
   sudo dd if=output/images/sdcard.img of=/dev/sdX bs=4M conv=fsync status=progress
   ```
   Отримуємо: **p1** — FAT 32 МБ (boot), **p2** — ext4 120 МБ (**rootfs Buildroot**).
2. Взяти boot-файли Raspberry Pi OS Lite (образ, записаний Raspberry Pi Imager):
   `bcm2712*.dtb`, `overlays/`, `config.txt`, `cmdline.txt` (+ `start.elf`, `fixup*.dat`,
   `bootcode.bin` — Pi 5 їх не використовує, лишились при копіюванні).
3. Замінити ними вміст p1 картки Buildroot. Ядро Pi OS (`kernel_2712.img`, `kernel8.img`,
   initramfs) — **прибрати**.
4. Покласти ядро Buildroot і вказати його в `config.txt` (Pi 5 типово шукає `kernel_2712.img`):
   ```sh
   cp output/images/Image /media/<user>/<p1>/
   ```
   ```
   arm_64bit=1
   [pi5]
   kernel=Image
   ```
5. У `cmdline.txt` корінь вказати на розділ Buildroot (у Pi OS там `root=PARTUUID=…`):
   ```
   console=tty1 root=/dev/mmcblk0p2 rootfstype=ext4 fsck.repair=yes rootwait
   ```
6. Додати параметри інтерфейсів у `config.txt` (крок 3).

Додатково на картці створено розділ **p3** `data` (ext4, решта місця). Для роботи демона
він не потрібен: база карток і журнал лежать на p2 (`/etc`, `/var/lib`).

> **Перевірено на платі** (19.11.2026):
>
> | Що | Значення | Висновок |
> |---|---|---|
> | `/proc/partitions` | p1 = 32 МБ, p2 = 120 МБ | розмітка `sdcard.img` Buildroot (у Pi OS Lite p1 = 512 МБ, p2 на всю картку) |
> | `blkid` p1 | без `LABEL` | FAT створено genimage, а не Pi OS (там `bootfs`) |
> | `/etc/os-release` | `NAME=Buildroot`, `2026.08` | rootfs — Buildroot |
> | `/proc/1/comm`, `/sbin/init` | `init` → `../bin/busybox` | init — BusyBox; `apt`/`systemctl` відсутні |
> | `/proc/cmdline` | `root=/dev/mmcblk0p2 rootfstype=ext4` | корінь — p2 |
> | p1: `issue.txt` | `Raspberry Pi reference 2026-09-15, pi-gen` | boot-файли — з Pi OS Lite |
> | p1: `Image` | дата збірки Buildroot | ядро — Buildroot |
>
> Тобто **ядро і весь userland — Buildroot**, розмітка — Buildroot; від Raspberry Pi OS Lite
> лише dtb/overlays/config на FAT-розділі. Bootloader Pi 5 живе в EEPROM плати.
> Наслідок для сервісу: init — BusyBox, тож запуск ставиться скриптом
> `init/S99security-daemon` (а не systemd-юнітом).

---

## 2. Тулчейн для застосунку

Buildroot зібрав крос-тулчейн і CMake toolchain-файл:
```
~/ajax_project/buildroot/output/host/bin/aarch64-linux-gcc
~/ajax_project/buildroot/output/host/share/buildroot/toolchainfile.cmake
```

---

## 3. Увімкнення інтерфейсів (зміни Device Tree)

Скопіювати рядки з `docs/config.txt` у `config.txt` на boot-розділі образу
(або в `board/.../config.txt` overlay Buildroot, щоб вони потрапляли в кожен образ):

```
dtparam=spi=on        # /dev/spidev0.0  (RC522)
dtparam=i2c_arm=on    # /dev/i2c-1      (SHT3x)
```

Також у `config.txt` на платі: `dtoverlay=uart0-pi5` (UART0 на GPIO14/15 — консоль для
налагодження). `dtparam=i2s=on` і `dtparam=audio=on` лишились від Pi OS, проєкту не потрібні.

Перевірка на платі:
```sh
ls /dev/spidev0.0 /dev/i2c-1          # мають існувати
i2cdetect -y 1                        # SHT3x має відгукнутись на 0x44
```

---

## 4. Крос-компіляція застосунку

```sh
cd ~/ajax_project/HomeSecurityController
cmake -S . -B cmake-build-buildroot \
      -DCMAKE_TOOLCHAIN_FILE=~/ajax_project/buildroot/output/host/share/buildroot/toolchainfile.cmake
cmake --build cmake-build-buildroot -j

file cmake-build-buildroot/security_daemon   # має бути ELF ... ARM aarch64
```

> У CLion той самий toolchain-файл задається в **Settings → CMake → CMake options**.

---

## 5. Деплой і запуск

IP плати задається в CMake (`DEPLOY_HOST`, зараз `192.168.137.127`) або з ключем.
Уся логіка — у `scripts/deploy.sh` (потрібен SSH-доступ root по ключу):

```sh
cmake --build cmake-build-buildroot --target deploy   # встановити як сервіс і запустити
cmake --build cmake-build-buildroot --target logs     # журнал подій з плати (tail -f)
cmake --build cmake-build-buildroot --target run      # демон у цьому терміналі (налагодження)
# або точковий IP:
cmake -DDEPLOY_HOST=<ip> -S . -B cmake-build-buildroot
```

Що робить `deploy`:
1. зупиняє сервіс; кладе `/usr/bin/security_daemon` і `/etc/init.d/S99security-daemon` (автозапуск);
2. копіює libgpiod із sysroot Buildroot у `/usr/lib` — запуск без `LD_LIBRARY_PATH`
   (вимкнути: `-DDEPLOY_LIBGPIOD=OFF`, якщо libgpiod уже є в rootfs);
3. виставляє на платі час із хоста й пише його в RTC — у журналі реальні дати, а не 1970;
4. створює `/etc/security_daemon/cards.db` (0600; переносить стару `/root/cards.db`, якщо є)
   і `/var/lib/security_daemon/`; стартує сервіс і показує `status`.

Вручну те саме: `sh scripts/deploy.sh root@<ip> cmake-build-buildroot/security_daemon [libgpiod.so.X.Y.Z]`.

Як сервіс (автозапуск) — див. **`init/README.md`** (systemd unit або BusyBox S99-скрипт).

---

## 6. Запуск і перевірка (на платі)

```sh
security_daemon --sensor              # разове T/RH з SHT3x
/etc/init.d/S99security-daemon stop   # --enroll/--copy потребують зчитувача: спершу зупинити сервіс
security_daemon --enroll "Arsen"      # піднести картку -> у базу
/etc/init.d/S99security-daemon start
security_daemon --list                # перелік дозволених карток
security_daemon                       # контроль доступу

# журнал подій:
tail -f /var/lib/security_daemon/security.log  # BusyBox
/etc/init.d/S99security-daemon reload        # SIGHUP -> перечитати картки (BusyBox)
journalctl -u security_daemon -f              # systemd
systemctl reload security_daemon              # SIGHUP -> перечитати картки
```

Сценарії для демонстрації: GRANTED (зелений+1 біп) → рух (жовтий) → прохід (червоний+біп);
невідома картка (10 біпів); рух без картки (тривога); `--enroll`/`--remove`;
`reload`; періодичний `[SENSOR]` у журналі; автостарт після reboot; watchdog.
Покроковий сценарій із таймінгами — **`docs/DEMO.md`**.

---

## 6a. Образ прошивки для здачі (GitHub Release)

```sh
# boot-розділ (FAT) + rootfs Buildroot:
#   Image, bcm2712-rpi-5-b.dtb, config.txt, cmdline.txt, overlays/  +  output/images/rootfs.ext4
# або весь SD одним файлом (на Linux):  sudo dd if=/dev/sdX of=hsc-pi5.img bs=4M status=progress
xz -T0 hsc-pi5.img
```

> **Перед публікацією** прибери з boot-розділу `user-data`, `network-config` і `meta-data`
> (лишились від Raspberry Pi Imager): там Wi‑Fi пароль і хеш пароля користувача.
> Buildroot їх не використовує. Для образу всієї картки врахуй, що p3 `data` займає ~30 ГБ —
> або зменш/прибери його, або публікуй лише p1 + p2.

---

## 7. Схема підключення (зведено)

| Периферія | Сигнал | Пін (фіз.) | GPIO |
|---|---|---|---|
| RC522 (SPI) | SDA/CE0 | 24 | GPIO8 |
|  | SCK | 23 | GPIO11 |
|  | MOSI | 19 | GPIO10 |
|  | MISO | 21 | GPIO9 |
|  | RST | 22 | GPIO25 |
|  | VCC / GND | 17 / 20 | 3.3V |
| SHT3x (I2C) | SDA | 3 | GPIO2 |
|  | SCL | 5 | GPIO3 |
|  | VIN / GND | 1 / 6 | 3.3V |
| Buzzer | сигнал (через транзистор) | 11 | GPIO17 |
| Радар RCWL | OUT | 13 | GPIO27 |
| Світлофор | Red | 15 | GPIO22 |
|  | Green | 16 | GPIO23 |
|  | Yellow | 37 | GPIO26 |

RC522 і SHT3x живити **тільки від 3.3V**. Детальна схема — у EasyEDA.
