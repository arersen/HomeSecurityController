# Home Security Controller (Raspberry Pi 5)

Контролер домашньої сигналізації на власному образі Linux (Buildroot).
Демон контролю доступу: RFID-перепустки, контроль присутності радаром,
звукова сигналізація, індикація світлофором, моніторинг середовища та
журнал подій.

## Інтерфейси / периферія
- **SPI** — RFID RC522 (перепустки, MIFARE)
- **I2C** — SHT3x (температура / вологість)
- **GPIO** — радар RCWL (присутність), buzzer (сигналізація), світлофор (індикація)

## Архітектура
```
радар / RFID / стани ─push─▶ [event_queue] ─pop─▶ [logger thread] ─▶ журнал (файл/journald)
      (realtime loop)          (mutex+cond)         (повільний I/O окремо)
```
Автомат станів доступу: `LOCKED → (картка) → UNLOCKED → (рух) → PASSING → LOCKED`.
Неблокуючий зумер (FSM), перепустки у файлі з гарячим перечитуванням по `SIGHUP`.

## Команди
```
security_daemon                 контроль доступу (сервіс)
security_daemon --enroll "Ім'я" зареєструвати піднесену картку
security_daemon --remove <uid>  видалити картку
security_daemon --list          перелік карток
security_daemon --sensor        разове читання SHT3x
security_daemon --copy          копіювання даних MIFARE
```
База карток: `$SECURITY_CARDS_DB` або `/etc/security_daemon/cards.db` (0600).
Журнал: `$SECURITY_LOG` або `/var/lib/security_daemon/security.log`.
Апаратний watchdog: `SECURITY_WATCHDOG=/dev/watchdog` (вмикає init-скрипт).

## Структура
```
include/        заголовки
src/core/       events (черга), logger, card_store, watchdog
src/gpio/        gpio, buzzer, radar, traffic_light
src/i2c/         i2c, sht3x
src/spi/         rc522
init/            systemd unit + BusyBox init-скрипт (запуск сервісу)
scripts/         deploy.sh (встановлення на плату як сервіс)
docs/            DESIGN.md, BUILD.md, DEMO.md (сценарій демо), config.txt (зміни DT), схема (PDF)
```

## Збірка та запуск
Повні інструкції — **[docs/BUILD.md](docs/BUILD.md)**. Запуск як сервіс — **[init/README.md](init/README.md)**.

Коротко:
```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<buildroot>/output/host/share/buildroot/toolchainfile.cmake
cmake --build build --target deploy   # зібрати, встановити як сервіс, синхронізувати час, запустити
cmake --build build --target logs     # журнал подій з плати в реальному часі
cmake --build build --target run      # те саме, але демон у терміналі (налагодження)
```
