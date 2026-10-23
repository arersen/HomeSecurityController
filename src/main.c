#include <stdio.h>
#include <signal.h>
#include "gpio.h"
#include "buzzer.h"
#include "etc.h"
#include "radar.h"
#include "traffic_light.h"
#include <pthread.h>
static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
	(void)sig;
	g_running = 0;
}

static void install_signals(void)
{
	struct sigaction sa = {0};
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);     /* без SA_RESTART, щоб sleep_ms переривався одразу */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
}


__attribute__((constructor)) void setup(void) {
	printf("setup\n");
	check_pins_or_die();
}

int j = 1;
void animation(void* args) {
	time_t start = time(NULL);

	traffic_light_t tl;
	if (traffic_light_init(&tl, "/dev/gpiochip0") != GPIO_OK) {
		fprintf(stderr, "traffic light init failed\n");
		return;
	}


	while (j) {
		traffic_light_set(&tl, TL_RED);
		sleep_ms(100);
		traffic_light_set(&tl, TL_YELLOW);
		sleep_ms(100);
		traffic_light_set(&tl, TL_GREEN);
		sleep_ms(100);
	}

	traffic_light_set(&tl, TL_OFF);
	traffic_light_deinit(&tl);
}
int test(void) {
	int counter = 0;
	gpio_t buzzer;
	gpio_t radar;

	install_signals();

	if (buzzer_init(&buzzer, BUZZER_GPIO_OFFSET) != GPIO_OK) {
		return EXIT_FAILURE;
	}

	if (radar_init(&radar, RADAR_GPIO_OFFSET) != GPIO_OK) {
		buzzer_deinit(&buzzer);
		return EXIT_FAILURE;
	}
	radar_run_diagnostic(&radar, 10);
	traffic_light_t tl;
	if (traffic_light_init(&tl, "/dev/gpiochip0") != GPIO_OK) {
		fprintf(stderr, "traffic light init failed\n");
		return 1;
	}

	traffic_light_set(&tl, TL_RED);


	printf("Security daemon is ACTIVE. Press Ctrl+C to stop.\n");
	fflush(stdout);

	int last_radar_state = 0;
	while (g_running) {
		int current_state = radar_read(&radar);

		if (current_state == 1 && last_radar_state == 0) {
			printf("[%d] -> INTRUDER DETECTED! Sounding alarm!\n", counter++);
			buzzer_beep(&buzzer, 1000);
		} else if (current_state == 0 && last_radar_state == 1) {
			printf("-> Area clear.\n");
			buzzer_beep(&buzzer, 250);
		}

		last_radar_state = current_state;

		sleep_ms(100);
	}

	printf("\nShutting down safely...\n");
	buzzer_deinit(&buzzer);
	radar_deinit(&radar);
	traffic_light_deinit(&tl);
	return EXIT_SUCCESS;
}
#include "rc522.h"
#include <string.h>
#include <time.h>

/* ============================================================
 *  Контроль доступу (кінцевий автомат на 3 стани)
 *
 *  LOCKED   — червоний. Чекаємо картку.
 *             картка в списку  -> зелений + 1 короткий біп, UNLOCKED
 *             невідома картка   -> лишається червоний + 10 біпів (відмова)
 *  UNLOCKED — зелений. Радар побачив рух -> жовтий, PASSING.
 *             (якщо ніхто не пройшов за AC_UNLOCK_TIMEOUT_MS -> знову LOCKED)
 *  PASSING  — жовтий, поки радар бачить рух. Рух зник (антидребезг
 *             AC_MOTION_CLEAR_MS) -> червоний + 1 короткий біп, LOCKED.
 * ============================================================ */

/* --- Список дозволених карток. Додавай свої UID сюди. --- */
typedef struct {
	uint8_t     uid[RC522_UID_MAX];
	uint8_t     len;
	const char *name;
} authorized_card_t;

static const authorized_card_t g_allowlist[] = {
	{ { 0x07, 0x84, 0x80, 0x33 }, 4, "Arsen" },
	{ { 0x25, 0x58, 0x9F, 0xE0 }, 4, "Guest" },
};

static const char *authorize(const rc522_card_t *card)
{
	for (size_t i = 0; i < sizeof(g_allowlist) / sizeof(g_allowlist[0]); i++)
		if (rc522_uid_match(card, g_allowlist[i].uid, g_allowlist[i].len))
			return g_allowlist[i].name;
	return NULL;
}

/* --- Параметри поведінки --- */
#define AC_SHORT_BEEP_MS      60
#define AC_DENY_BEEPS         10
#define AC_BEEP_GAP_MS        90
#define AC_POLL_MS            50
#define AC_MOTION_CLEAR_MS    1200    /* рух має зникнути на стільки, щоб вважати прохід завершеним */
#define AC_UNLOCK_TIMEOUT_MS  15000   /* ніхто не пройшов за цей час -> знову блокуємо (0 = вимкнено) */

static uint64_t ac_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void beep_short(gpio_t *buzzer)
{
	buzzer_beep(buzzer, AC_SHORT_BEEP_MS);
}

static void beep_times(gpio_t *buzzer, int n)
{
	for (int i = 0; i < n && g_running; i++) {
		buzzer_beep(buzzer, AC_SHORT_BEEP_MS);
		if (i + 1 < n)
			sleep_ms(AC_BEEP_GAP_MS);
	}
}

typedef enum { AC_LOCKED, AC_UNLOCKED, AC_PASSING } ac_state_t;

int access_control(rc522_t *rfid)
{
	gpio_t buzzer, radar;
	traffic_light_t tl;
	ac_state_t state = AC_LOCKED;
	uint64_t unlock_ts = 0, last_motion_ts = 0;
	char uid[RC522_UID_STR_LEN];

	if (buzzer_init(&buzzer, BUZZER_GPIO_OFFSET) != GPIO_OK)
		return EXIT_FAILURE;
	if (radar_init(&radar, RADAR_GPIO_OFFSET) != GPIO_OK) {
		buzzer_deinit(&buzzer);
		return EXIT_FAILURE;
	}
	if (traffic_light_init(&tl, "/dev/gpiochip0") != GPIO_OK) {
		fprintf(stderr, "traffic light init failed\n");
		radar_deinit(&radar);
		buzzer_deinit(&buzzer);
		return EXIT_FAILURE;
	}

	traffic_light_set(&tl, TL_RED);
	printf("Access control ACTIVE (LOCKED). Ctrl+C to stop.\n");
	fflush(stdout);

	while (g_running) {
		switch (state) {
		case AC_LOCKED: {
			rc522_card_t card;
			rc522_status_t st = rc522_read_card(rfid, &card, 0);

			if (st == RC522_OK) {
				const char *who = authorize(&card);

				rc522_uid_to_str(&card, uid, sizeof(uid));
				if (who) {
					printf("[ACCESS] GRANTED: %s (%s) -> UNLOCKED\n", who, uid);
					traffic_light_set(&tl, TL_GREEN);
					beep_short(&buzzer);
					unlock_ts = ac_now_ms();
					state = AC_UNLOCKED;
				} else {
					printf("[ACCESS] DENIED: unknown card %s\n", uid);
					beep_times(&buzzer, AC_DENY_BEEPS);   /* лишаємось LOCKED */
				}
				rc522_halt(rfid);
			} else if (st != RC522_ERR_NO_CARD) {
				/* перехідні помилки (картку сіпнули на краю поля) ігноруємо */
			}
			sleep_ms(AC_POLL_MS);
			break;
		}

		case AC_UNLOCKED: {
			if (radar_read(&radar) == 1) {
				printf("[ACCESS] passage started -> PASSING\n");
				traffic_light_set(&tl, TL_YELLOW);
				last_motion_ts = ac_now_ms();
				state = AC_PASSING;
			} else if (AC_UNLOCK_TIMEOUT_MS > 0 &&
			           ac_now_ms() - unlock_ts > AC_UNLOCK_TIMEOUT_MS) {
				printf("[ACCESS] nobody passed in time -> LOCKED\n");
				traffic_light_set(&tl, TL_RED);
				beep_short(&buzzer);
				state = AC_LOCKED;
			}
			sleep_ms(AC_POLL_MS);
			break;
		}

		case AC_PASSING: {
			if (radar_read(&radar) == 1)
				last_motion_ts = ac_now_ms();

			if (ac_now_ms() - last_motion_ts > AC_MOTION_CLEAR_MS) {
				printf("[ACCESS] passage complete -> LOCKED\n");
				traffic_light_set(&tl, TL_RED);
				beep_short(&buzzer);   /* короткий біп = знову заблоковано, чекаємо наступне розблокування */
				state = AC_LOCKED;
			}
			sleep_ms(AC_POLL_MS);
			break;
		}
		}
	}

	printf("\nShutting down...\n");
	traffic_light_set(&tl, TL_RED);
	buzzer_deinit(&buzzer);
	radar_deinit(&radar);
	traffic_light_deinit(&tl);
	return EXIT_SUCCESS;
}

/* ============================================================
 *  Безпечне копіювання MIFARE Classic 1K: тільки блоки даних.
 *  НЕ пише block 0 (UID виробника) і НЕ пише sector trailer,
 *  тому картку неможливо заблокувати. Ключі й UID не копіюються.
 * ============================================================ */

#define COPY_DATA_SIZE 752   /* 1K мінус block0 і 16 трейлерів = 47 блоків * 16 */

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
	rc522_t rfid;
	int rc;

	install_signals();

	rc522_status_t rc522_init_status = rc522_init(&rfid, RC522_SPI_DEV, 0,
	                                               "/dev/gpiochip0", RC522_RST_GPIO_OFFSET);
	if (rc522_init_status != RC522_OK) {
		fprintf(stderr, "RC522 init failed, code: %d (%s)\n",
		        rc522_init_status, rc522_strerror(rc522_init_status));
		return 1;
	}
	printf("RC522: %s\n", rc522_version_name(rfid.version));

	if (argc > 1 && strcmp(argv[1], "--copy") == 0) {
		copy_tool(&rfid);
		rc522_deinit(&rfid);
		return 0;
	}

	rc = access_control(&rfid);

	rc522_deinit(&rfid);
	return rc;
}
