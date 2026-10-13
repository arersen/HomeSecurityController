#include <stdio.h>
#include <signal.h>
#include <time.h>
#include "gpio.h"

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
	(void)sig;
	g_running = 0;
}

static void sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
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

	if (gpio_init(&buzzer, "/dev/gpiochip4", 17, GPIO_DIR_OUTPUT, "buzzer") != GPIO_OK) {
		fprintf(stderr, "Error: gpio_init failed\n");
		return 1;
	}

	printf("Buzzer blinking, Ctrl+C to stop\n");
	fflush(stdout);

	while (g_running) {
		gpio_write(&buzzer, 1);
		sleep_ms(200);
		gpio_write(&buzzer, 0);
		sleep_ms(200);
	}

	gpio_write(&buzzer, 0);
	gpio_deinit(&buzzer);
	printf("Stopped\n");
	return 0;
}