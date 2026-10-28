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
# результат:
#   output/images/Image        ядро (arm64)
#   output/images/*.dtb         device tree
#   output/host/                крос-тулчейн
#   output/target/ , rootfs.*   коренева ФС
```

### 1.3. Складання завантажувальної картки (Pi OS boot + ядро Buildroot)

Чистий `sdcard.img` від Buildroot на Raspberry Pi 5 **не завантажувався** (капризний
bootloader/firmware Pi 5). Тому за основу взято **Raspberry Pi OS Lite** — у нього
надійний boot на Pi 5 — а від Buildroot підставлено лише **ядро**:

1. Записати Raspberry Pi OS Lite на SD (Raspberry Pi Imager).
2. Підставити ядро Buildroot на boot-розділ:
   ```sh
   cp ~/ajax_project/buildroot/output/images/Image  /media/<user>/bootfs/
   ```
   У `config.txt` вказати це ядро (Pi 5 типово шукає `kernel_2712.img`):
   ```
   kernel=Image
   arm_64bit=1
   ```
3. Прибрати зайве від Pi OS Lite.
4. Додати параметри інтерфейсів у `config.txt` (див. крок 3).

> Коренева ФС лишається тією, що на картці. Перевірити, яка саме:
> ```sh
> cat /etc/os-release      # "Buildroot ..." -> корінь Buildroot; "Debian ..." -> Pi OS Lite
> cat /proc/1/comm         # init (BusyBox) -> Buildroot; systemd -> Pi OS
> ```
> Для захисту: мінімум — **власне ядро Buildroot** поверх firmware Pi; якщо корінь
> теж від Buildroot — це повністю власний образ (ядро + userland).

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

IP плати задається в CMake (`DEPLOY_HOST`, зараз `192.168.137.127`) або з ключем:
```sh
cmake --build cmake-build-buildroot --target deploy        # зібрати + залити (scp -O)
cmake --build cmake-build-buildroot --target run           # залити + запустити, вивід сюди
# або точковий IP:
cmake -DDEPLOY_HOST=<ip> -S . -B cmake-build-buildroot
```

Вручну:
```sh
scp -O cmake-build-buildroot/security_daemon root@<ip>:/usr/bin/
ssh root@<ip> /usr/bin/security_daemon
```

Як сервіс (автозапуск) — див. **`init/README.md`** (systemd unit або BusyBox S99-скрипт).

---

## 6. Запуск і перевірка (на платі)

```sh
security_daemon --sensor              # разове T/RH з SHT3x
security_daemon --enroll "Arsen"      # піднести картку -> у базу
security_daemon --list                # перелік дозволених карток
security_daemon                       # контроль доступу

# журнал подій:
tail -f /var/log/security_daemon.log          # BusyBox
journalctl -u security_daemon -f              # systemd
systemctl reload security_daemon              # SIGHUP -> перечитати картки
```

Сценарії для демонстрації: GRANTED (зелений+1 біп) → рух (жовтий) → прохід (червоний+біп);
невідома картка (10 біпів); рух без картки (тривога); `--enroll`/`--remove`;
`reload`; періодичний `[SENSOR]` у журналі.

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
