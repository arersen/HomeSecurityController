//
// Апаратний watchdog (Linux watchdog API, /dev/watchdog).
//
#ifndef HOMESECURITYCONTROLLER_WATCHDOG_H
#define HOMESECURITYCONTROLLER_WATCHDOG_H

/*
 * Ідея: демон відкриває /dev/watchdog і періодично «годує» його з головного
 * циклу. Якщо цикл завис або процес упав (не встиг зробити magic close) —
 * таймер у SoC (на Pi 5: драйвер bcm2835-wdt) перезавантажує плату, а
 * init-скрипт S99 знову піднімає сервіс. Самовідновлення без людини.
 *
 * Коректна зупинка (SIGTERM/SIGINT) пише в пристрій 'V' (magic close),
 * тому `S99security-daemon stop` плату НЕ перезавантажує.
 * Увага: SIGKILL (kill -9) = «падіння» → reboot через timeout секунд.
 */

typedef struct {
	int fd;          /* -1 = вимкнено */
	int timeout_s;   /* фактичний таймаут, який погодив драйвер */
} watchdog_t;

/* Шлях пристрою зі змінної SECURITY_WATCHDOG:
 *   не задана / "" / "0"  -> NULL (watchdog вимкнено, зручно для налагодження)
 *   "1"                   -> "/dev/watchdog"
 *   інше                  -> як є (напр. "/dev/watchdog0") */
const char *watchdog_path_from_env(void);

/* 0 — відкрито, -1 — помилка (w->fd = -1). timeout_s <= 0 — не чіпати таймаут. */
int  watchdog_open(watchdog_t *w, const char *dev, int timeout_s);
void watchdog_kick(watchdog_t *w);
/* magic close: вимкнути таймер і закрити пристрій */
void watchdog_close(watchdog_t *w);

#endif //HOMESECURITYCONTROLLER_WATCHDOG_H
