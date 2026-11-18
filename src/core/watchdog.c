//
// Апаратний watchdog (Linux watchdog API, /dev/watchdog).
//
#include "watchdog.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/watchdog.h>

const char *watchdog_path_from_env(void)
{
	const char *p = getenv("SECURITY_WATCHDOG");

	if (!p || !*p || strcmp(p, "0") == 0)
		return NULL;
	if (strcmp(p, "1") == 0)
		return "/dev/watchdog";
	return p;
}

int watchdog_open(watchdog_t *w, const char *dev, int timeout_s)
{
	int t;

	w->fd = -1;
	w->timeout_s = 0;
	if (!dev)
		return -1;

	/* відкриття вже запускає таймер */
	w->fd = open(dev, O_WRONLY | O_CLOEXEC);
	if (w->fd < 0) {
		fprintf(stderr, "watchdog: не вдалося відкрити %s: %s\n", dev, strerror(errno));
		return -1;
	}
	if (timeout_s > 0) {
		t = timeout_s;
		if (ioctl(w->fd, WDIOC_SETTIMEOUT, &t) != 0)
			fprintf(stderr, "watchdog: SETTIMEOUT %d не підтримано: %s\n", timeout_s, strerror(errno));
	}
	if (ioctl(w->fd, WDIOC_GETTIMEOUT, &t) == 0)
		w->timeout_s = t;
	watchdog_kick(w);
	return 0;
}

void watchdog_kick(watchdog_t *w)
{
	if (w && w->fd >= 0)
		ioctl(w->fd, WDIOC_KEEPALIVE, 0);
}

void watchdog_close(watchdog_t *w)
{
	if (!w || w->fd < 0)
		return;
	/* 'V' = magic close: драйвер зупиняє таймер (якщо ядро не NOWAYOUT) */
	if (write(w->fd, "V", 1) != 1)
		fprintf(stderr, "watchdog: magic close не вдався: %s\n", strerror(errno));
	close(w->fd);
	w->fd = -1;
}
