#include <stdio.h>
#include <signal.h>
#include "gpio.h"
#include "buzzer.h"
#include "etc.h"
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

int main(void)
{
	gpio_t buzzer;

	struct sigaction sa = {0};
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	e_gpio_status buzzer_init_status = buzzer_init(&buzzer, BUZZER_GPIO_OFFSET);
	if (buzzer_init_status != GPIO_OK) {
		printf("buzzer_init failed\n");
		return buzzer_init_status;
	}

	printf("Buzzer blinking, Ctrl+C to stop\n");
	fflush(stdout);

	while (g_running) {
		buzzer_beep(&buzzer, 500);
		sleep_ms(500);
	}


	buzzer_deinit(&buzzer);
	printf("Stopped\n");
	return 0;
}