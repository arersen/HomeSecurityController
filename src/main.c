#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>

#include "gpio.h"
#include "buzzer.h"
#include "etc.h"
#include "radar.h"
#include "traffic_light.h"
#include "rc522.h"
#include "card_store.h"
#include "events.h"
#include "logger.h"

/* ---- Глобальні прапорці, якими керують сигнали ---- */
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload  = 0;   /* SIGHUP -> перечитати базу карток */

static void on_stop(int sig)   { (void)sig; g_running = 0; }
static void on_reload(int sig) { (void)sig; g_reload = 1; }

static void install_signals(void)
{
	struct sigaction stop = {0}, hup = {0};

	stop.sa_handler = on_stop;
	sigemptyset(&stop.sa_mask);     /* без SA_RESTART: sleep_ms переривається одразу */
	sigaction(SIGINT, &stop, NULL);
	sigaction(SIGTERM, &stop, NULL);

	hup.sa_handler = on_reload;
	sigemptyset(&hup.sa_mask);
	sigaction(SIGHUP, &hup, NULL);
}

__attribute__((constructor)) void setup(void) {
	printf("setup\n");
	check_pins_or_die();
}

/* Анімація світлофора у фоновому потоці (діагностика радара).
 * Потрібна як символ для radar_run_diagnostic (extern j, animation). */
int j = 1;
void *animation(void *args)
{
	(void)args;

	traffic_light_t tl;
	if (traffic_light_init(&tl, "/dev/gpiochip0") != GPIO_OK) {
		fprintf(stderr, "traffic light init failed\n");
		return NULL;
	}
	while (j) {
		traffic_light_set(&tl, TL_RED);    sleep_ms(100);
		traffic_light_set(&tl, TL_YELLOW); sleep_ms(100);
		traffic_light_set(&tl, TL_GREEN);  sleep_ms(100);
	}
	traffic_light_set(&tl, TL_OFF);
	traffic_light_deinit(&tl);
	return NULL;
}

/* Шлях до бази карток: змінна оточення або типовий. */
static const char *cards_db_path(void)
{
	const char *p = getenv("SECURITY_CARDS_DB");
	return (p && *p) ? p : "/root/cards.db";
}

/* Шлях до журналу подій: змінна оточення або типовий. */
static const char *log_file_path(void)
{
	const char *p = getenv("SECURITY_LOG");
	return (p && *p) ? p : "/root/security.log";
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ============================================================
 *  Контроль доступу (неблокуючий кінцевий автомат)
 *
 *  LOCKED   — червоний. Відома картка -> зелений + короткий біп, UNLOCKED.
 *             Невідома картка -> 10 біпів (відмова).
 *             Рух без картки довше AC_INTRUSION_MS -> 10 біпів (тривога).
 *  UNLOCKED — зелений. Рух -> жовтий, PASSING. Таймаут -> знову LOCKED.
 *  PASSING  — жовтий, поки є рух. Рух зник (антидребезг) -> червоний + біп, LOCKED.
 *
 *  Головний цикл не блокується: зумер грає через buzzer_ctl_tick, тож радар і
 *  зчитувач опитуються навіть під час сигналізації. SIGHUP перечитує базу.
 * ============================================================ */

#define AC_LOOP_MS            20      /* період головного циклу */
#define AC_CARD_POLL_MS       120     /* як часто питати зчитувач у стані LOCKED */
#define AC_SHORT_BEEP_MS      60
#define AC_DENY_BEEPS         10
#define AC_BEEP_ON_MS         60
#define AC_BEEP_OFF_MS        90
#define AC_MOTION_CLEAR_MS    1200    /* рух має зникнути на стільки -> прохід завершено */
#define AC_UNLOCK_TIMEOUT_MS  15000   /* ніхто не пройшов -> знову блокуємо (0 = вимкнено) */
#define AC_INTRUSION_MS       2000    /* рух без картки довше цього -> тривога (0 = вимкнено) */

typedef enum { AC_LOCKED, AC_UNLOCKED, AC_PASSING } ac_state_t;

static void maybe_reload(card_store_t *store, const char *db, event_queue_t *q)
{
	char detail[EVENT_DETAIL_MAX];

	if (!g_reload)
		return;
	g_reload = 0;
	if (card_store_load(store, db) == 0) {
		snprintf(detail, sizeof(detail), "cards=%zu", store->count);
		event_queue_emit(q, EV_DB_RELOAD, NULL, NULL, detail);
	} else {
		event_queue_emit(q, EV_ERROR, NULL, NULL, "db reload failed");
	}
}

int access_control(rc522_t *rfid, card_store_t *store, const char *db, event_queue_t *q)
{
	gpio_t         buzzer_pin, radar;
	traffic_light_t tl;
	buzzer_ctl_t   buzzer;
	ac_state_t     state = AC_LOCKED;
	uint64_t       unlock_ts = 0, last_motion_ts = 0, intrusion_ts = 0, last_card_poll = 0;
	char           uid[RC522_UID_STR_LEN];

	if (buzzer_init(&buzzer_pin, BUZZER_GPIO_OFFSET) != GPIO_OK)
		return EXIT_FAILURE;
	if (radar_init(&radar, RADAR_GPIO_OFFSET) != GPIO_OK) {
		buzzer_deinit(&buzzer_pin);
		return EXIT_FAILURE;
	}
	if (traffic_light_init(&tl, "/dev/gpiochip0") != GPIO_OK) {
		fprintf(stderr, "traffic light init failed\n");
		radar_deinit(&radar);
		buzzer_deinit(&buzzer_pin);
		return EXIT_FAILURE;
	}
	buzzer_ctl_init(&buzzer, &buzzer_pin);

	traffic_light_set(&tl, TL_RED);
	{
		char detail[EVENT_DETAIL_MAX];
		snprintf(detail, sizeof(detail), "cards=%zu state=LOCKED", store->count);
		event_queue_emit(q, EV_STARTUP, NULL, NULL, detail);
	}

	while (g_running) {
		uint64_t now = now_ms();

		maybe_reload(store, db, q);
		buzzer_ctl_tick(&buzzer, now);

		switch (state) {
		case AC_LOCKED:
			/* зчитувач опитуємо не щотіка, щоб не навантажувати SPI */
			if (now - last_card_poll >= AC_CARD_POLL_MS) {
				rc522_card_t card;
				rc522_status_t st;

				last_card_poll = now;
				st = rc522_read_card(rfid, &card, 0);
				if (st == RC522_OK) {
					const char *who = card_store_find(store, card.uid, card.uid_len);

					intrusion_ts = 0;
					rc522_uid_to_str(&card, uid, sizeof(uid));
					if (who) {
						event_queue_emit(q, EV_ACCESS_GRANTED, uid, who, NULL);
						traffic_light_set(&tl, TL_GREEN);
						buzzer_ctl_start(&buzzer, 1, AC_SHORT_BEEP_MS, 0, now);
						unlock_ts = now;
						state = AC_UNLOCKED;
					} else {
						event_queue_emit(q, EV_ACCESS_DENIED, uid, NULL, NULL);
						buzzer_ctl_start(&buzzer, AC_DENY_BEEPS, AC_BEEP_ON_MS, AC_BEEP_OFF_MS, now);
					}
					rc522_halt(rfid);
				}
			}

			/* прохід без картки -> тривога (радар опитуємо щотіка) */
			if (state == AC_LOCKED && AC_INTRUSION_MS > 0) {
				if (radar_read(&radar) == 1) {
					if (intrusion_ts == 0) {
						intrusion_ts = now;
					} else if (now - intrusion_ts > AC_INTRUSION_MS) {
						event_queue_emit(q, EV_INTRUSION, NULL, NULL, "рух без картки");
						buzzer_ctl_start(&buzzer, AC_DENY_BEEPS, AC_BEEP_ON_MS, AC_BEEP_OFF_MS, now);
						intrusion_ts = now;   /* ре-арм, поки рух триває */
					}
				} else {
					intrusion_ts = 0;
				}
			}
			break;

		case AC_UNLOCKED:
			if (radar_read(&radar) == 1) {
				event_queue_emit(q, EV_PASSAGE_START, NULL, NULL, NULL);
				traffic_light_set(&tl, TL_YELLOW);
				last_motion_ts = now;
				state = AC_PASSING;
			} else if (AC_UNLOCK_TIMEOUT_MS > 0 && now - unlock_ts > AC_UNLOCK_TIMEOUT_MS) {
				event_queue_emit(q, EV_UNLOCK_TIMEOUT, NULL, NULL, NULL);
				traffic_light_set(&tl, TL_RED);
				buzzer_ctl_start(&buzzer, 1, AC_SHORT_BEEP_MS, 0, now);
				intrusion_ts = 0;
				state = AC_LOCKED;
			}
			break;

		case AC_PASSING:
			if (radar_read(&radar) == 1)
				last_motion_ts = now;
			if (now - last_motion_ts > AC_MOTION_CLEAR_MS) {
				event_queue_emit(q, EV_PASSAGE_END, NULL, NULL, NULL);
				traffic_light_set(&tl, TL_RED);
				buzzer_ctl_start(&buzzer, 1, AC_SHORT_BEEP_MS, 0, now);
				intrusion_ts = 0;
				last_card_poll = now;   /* не зчитувати залишок проходу як нову картку */
				state = AC_LOCKED;
			}
			break;
		}

		sleep_ms(AC_LOOP_MS);
	}

	buzzer_ctl_cancel(&buzzer);
	traffic_light_set(&tl, TL_RED);
	buzzer_deinit(&buzzer_pin);
	radar_deinit(&radar);
	traffic_light_deinit(&tl);
	return EXIT_SUCCESS;
}

/* ============================================================
 *  Адмін-команди: реєстрація / видалення / список карток
 * ============================================================ */

static int cmd_enroll(rc522_t *rfid, card_store_t *store, const char *db, const char *name)
{
	rc522_card_t card;
	char uid[RC522_UID_STR_LEN];
	int r;

	if (!name || !*name) {
		fprintf(stderr, "enroll: вкажи ім'я\n");
		return 1;
	}
	printf("Піднеси картку для реєстрації '%s'... (Ctrl+C — скасувати)\n", name);
	fflush(stdout);

	while (g_running) {
		if (rc522_read_card(rfid, &card, 1) == RC522_OK)
			break;
		sleep_ms(150);
	}
	if (!g_running)
		return 1;

	rc522_uid_to_str(&card, uid, sizeof(uid));
	rc522_halt(rfid);

	r = card_store_add(store, card.uid, card.uid_len, name);
	if (r < 0) {
		fprintf(stderr, "enroll: не вдалося додати картку\n");
		return 1;
	}
	if (card_store_save(store, db) != 0) {
		fprintf(stderr, "enroll: не вдалося зберегти базу '%s'\n", db);
		return 1;
	}
	printf(r == 1 ? "Оновлено: %s -> %s\n" : "Додано: %s -> %s\n", uid, name);
	return 0;
}

static int cmd_remove(card_store_t *store, const char *db, const char *uid_str)
{
	uint8_t uid[CARD_UID_MAX], uid_len;

	if (card_uid_parse(uid_str, uid, &uid_len) != 0) {
		fprintf(stderr, "remove: некоректний UID '%s' (очікую, напр., 25:58:9F:E0)\n", uid_str);
		return 1;
	}
	if (card_store_remove(store, uid, uid_len) != 0) {
		printf("Картки %s у базі немає\n", uid_str);
		return 0;
	}
	if (card_store_save(store, db) != 0) {
		fprintf(stderr, "remove: не вдалося зберегти базу '%s'\n", db);
		return 1;
	}
	printf("Видалено: %s\n", uid_str);
	return 0;
}

static void cmd_list(const card_store_t *store)
{
	char uid[CARD_UID_STRLEN];

	printf("Дозволені картки (%zu):\n", store->count);
	for (size_t i = 0; i < store->count; i++) {
		card_uid_format(store->items[i].uid, store->items[i].uid_len, uid, sizeof(uid));
		printf("  %-23s %s\n", uid, store->items[i].name);
	}
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Використання:\n"
		"  %s                 запуск контролю доступу\n"
		"  %s --enroll \"Ім'я\" зареєструвати піднесену картку\n"
		"  %s --remove <uid>  видалити картку (напр. 25:58:9F:E0)\n"
		"  %s --list          показати список карток\n"
		"  %s --copy          копіювати MIFARE (дані)\n"
		"База карток: $SECURITY_CARDS_DB або /root/cards.db. SIGHUP перечитує її.\n",
		prog, prog, prog, prog, prog);
}

/* ============================================================
 *  Безпечне копіювання MIFARE Classic 1K: тільки блоки даних.
 *  НЕ пише block 0 і НЕ пише sector trailer, тому картку неможливо
 *  заблокувати. Ключі й UID не копіюються.
 * ============================================================ */

static int block_is_copyable(uint8_t block)
{
	return block != 0 && !rc522_mifare_is_trailer(block);
}

static int wait_for_card(rc522_t *rfid, rc522_card_t *card, const char *prompt)
{
	printf("%s", prompt);
	fflush(stdout);
	while (g_running) {
		if (rc522_read_card(rfid, card, 1) == RC522_OK) {
			char uid[RC522_UID_STR_LEN];
			printf(" %s (%s)\n", rc522_uid_to_str(card, uid, sizeof(uid)),
			       rc522_card_type_name(card->type));
			return 1;
		}
		sleep_ms(150);
	}
	printf("\n");
	return 0;
}

static int read_source(rc522_t *rfid, rc522_card_t *card, uint8_t dst[47][16])
{
	int idx = 0;

	for (uint8_t sector = 0; sector < 16; sector++) {
		uint8_t first = (uint8_t)(sector * 4);

		if (rc522_mifare_auth(rfid, card, RC522_KEY_A, first, RC522_KEY_DEFAULT) != RC522_OK) {
			printf("Sector %u: не відкривається заводським ключем, копіювання скасовано\n", sector);
			return -1;
		}
		for (uint8_t b = first; b < first + 4; b++) {
			if (!block_is_copyable(b))
				continue;
			if (rc522_mifare_read(rfid, b, dst[idx]) != RC522_OK) {
				printf("Block %u: помилка читання\n", b);
				return -1;
			}
			idx++;
		}
	}
	return 0;
}

static int write_dest(rc522_t *rfid, rc522_card_t *card, uint8_t src[47][16])
{
	int idx = 0, written = 0;

	for (uint8_t sector = 0; sector < 16; sector++) {
		uint8_t first = (uint8_t)(sector * 4);

		if (rc522_mifare_auth(rfid, card, RC522_KEY_A, first, RC522_KEY_DEFAULT) != RC522_OK) {
			printf("Sector %u: ціль не відкривається заводським ключем, пропускаю\n", sector);
			idx += 3;
			continue;
		}
		for (uint8_t b = first; b < first + 4; b++) {
			uint8_t check[16];

			if (!block_is_copyable(b))
				continue;
			if (rc522_mifare_write(rfid, b, src[idx]) != RC522_OK) {
				printf("Block %u: помилка запису\n", b);
			} else if (rc522_mifare_read(rfid, b, check) != RC522_OK ||
			           memcmp(check, src[idx], 16) != 0) {
				printf("Block %u: перевірка не збіглася\n", b);
			} else {
				written++;
			}
			idx++;
		}
	}
	return written;
}

static void copy_tool(rc522_t *rfid)
{
	static uint8_t data[47][16];
	rc522_card_t src, dst;

	printf("\n=== Копіювання MIFARE (тільки дані, UID і ключі не чіпаються) ===\n");

	if (!wait_for_card(rfid, &src, "Піднеси ДЖЕРЕЛО..."))
		return;
	if (src.type != RC522_CARD_MIFARE_1K) {
		printf("Поки підтримується лише MIFARE Classic 1K\n");
		return;
	}
	if (read_source(rfid, &src, data) != 0)
		return;
	rc522_halt(rfid);
	printf("Прочитано 47 блоків даних. Прибери джерело.\n");
	sleep_ms(1500);

	if (!wait_for_card(rfid, &dst, "Піднеси ЦІЛЬ (буде перезаписано)..."))
		return;
	if (dst.type != RC522_CARD_MIFARE_1K) {
		printf("Ціль має бути MIFARE Classic 1K\n");
		return;
	}
	if (rc522_uid_equal(&src, &dst)) {
		printf("Це та сама картка, нічого копіювати\n");
		return;
	}

	int n = write_dest(rfid, &dst, data);
	rc522_halt(rfid);
	printf("Готово: записано й перевірено %d / 47 блоків\n\n", n);
}

int main(int argc, char** argv)
{
	const char   *db = cards_db_path();
	card_store_t  store;
	rc522_t       rfid;
	rc522_status_t rc522_st;
	int rc;

	install_signals();
	card_store_init(&store);
	if (card_store_load(&store, db) != 0)
		fprintf(stderr, "Увага: не вдалося прочитати базу карток '%s'\n", db);

	/* команди, яким не потрібен зчитувач */
	if (argc > 1 && strcmp(argv[1], "--list") == 0) {
		cmd_list(&store);
		card_store_free(&store);
		return 0;
	}
	if (argc > 2 && strcmp(argv[1], "--remove") == 0) {
		rc = cmd_remove(&store, db, argv[2]);
		card_store_free(&store);
		return rc;
	}
	if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
		usage(argv[0]);
		card_store_free(&store);
		return 0;
	}

	/* далі потрібен RC522 */
	rc522_st = rc522_init(&rfid, RC522_SPI_DEV, 0, "/dev/gpiochip0", RC522_RST_GPIO_OFFSET);
	if (rc522_st != RC522_OK) {
		fprintf(stderr, "RC522 init failed, code: %d (%s)\n", rc522_st, rc522_strerror(rc522_st));
		card_store_free(&store);
		return 1;
	}
	printf("RC522: %s\n", rc522_version_name(rfid.version));

	if (argc > 2 && strcmp(argv[1], "--enroll") == 0) {
		rc = cmd_enroll(&rfid, &store, db, argv[2]);
	} else if (argc > 1 && strcmp(argv[1], "--copy") == 0) {
		copy_tool(&rfid);
		rc = 0;
	} else if (argc > 1) {
		usage(argv[0]);
		rc = 2;
	} else {
		/* контроль доступу: події йдуть у чергу, логер пише їх у журнал */
		event_queue_t q;
		logger_t      lg;

		if (event_queue_init(&q, 256) != 0) {
			fprintf(stderr, "не вдалося створити чергу подій\n");
			rc522_deinit(&rfid);
			card_store_free(&store);
			return 1;
		}
		if (logger_start(&lg, &q, log_file_path(), 1) != 0) {
			fprintf(stderr, "не вдалося запустити логер\n");
			event_queue_destroy(&q);
			rc522_deinit(&rfid);
			card_store_free(&store);
			return 1;
		}

		rc = access_control(&rfid, &store, db, &q);

		event_queue_emit(&q, EV_SHUTDOWN, NULL, NULL, NULL);
		logger_stop(&lg);           /* добере залишок черги й зупиниться */
		event_queue_destroy(&q);
	}

	rc522_deinit(&rfid);
	card_store_free(&store);
	return rc;
}
