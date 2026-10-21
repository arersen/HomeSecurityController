#include <stdio.h>
#include <signal.h>
#include "gpio.h"
#include "buzzer.h"
#include "etc.h"
#include "radar.h"
#include "traffic_light.h"
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

	traffic_light_t tl;
	if (traffic_light_init(&tl, "/dev/gpiochip0") != GPIO_OK) {
		fprintf(stderr, "traffic light init failed\n");
		return 1;
	}

	traffic_light_set(&tl, TL_RED);
	radar_run_diagnostic(&radar, 10);

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
int main(void)
{
	test();

}