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
	struct sigaction sa = {0};

	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

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
int main(void)
{


	rc522_t rfid;
	rc522_card_t card;
	char uid[RC522_UID_STR_LEN];

	if (rc522_init(&rfid, RC522_SPI_DEV, 0, "/dev/gpiochip0", RC522_RST_GPIO_OFFSET) != RC522_OK) {
		fprintf(stderr, "RC522 init failed\n");
		return 1;
	}
	printf("RC522: %s\n", rc522_version_name(rfid.version));

	while (g_running) {
		rc522_status_t st = rc522_read_card(&rfid, &card, 0);
		if (st == RC522_OK) {
			printf("Card %s (%s)\n", rc522_uid_to_str(&card, uid, sizeof(uid)),
				   rc522_card_type_name(card.type));
			rc522_halt(&rfid);   /* та сама картка не спрацює повторно, поки її не приберуть */
		} else if (st != RC522_ERR_NO_CARD) {
			fprintf(stderr, "RC522: %s\n", rc522_strerror(st));
		}
		sleep_ms(100);
	}
	rc522_deinit(&rfid);
	//test();

}