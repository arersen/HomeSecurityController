//
// Created by jambyl on 10/28/26.
//
#include "logger.h"

#include <stdio.h>
#include <string.h>

#define LOGGER_POP_TIMEOUT_MS 500

static void logger_write(logger_t *lg, const char *line)
{
	if (lg->file) {
		fprintf(lg->file, "%s\n", line);
		fflush(lg->file);
	}
	if (lg->echo_stdout) {
		fprintf(stdout, "%s\n", line);
		fflush(stdout);
	}
}

/* Якщо від минулого разу щось відкинули — зафіксувати це в журналі. */
static void logger_note_drops(logger_t *lg)
{
	unsigned long d = event_queue_take_dropped(lg->q);

	if (d) {
		char line[96];
		event_t ev;

		memset(&ev, 0, sizeof(ev));
		ev.type = EV_ERROR;
		clock_gettime(CLOCK_REALTIME, &ev.ts);
		snprintf(ev.detail, sizeof(ev.detail), "dropped=%lu (queue overflow)", d);
		event_format_line(&ev, line, sizeof(line));
		logger_write(lg, line);
	}
}

static void *logger_main(void *arg)
{
	logger_t *lg = arg;
	char line[192];
	event_t ev;

	for (;;) {
		int r = event_queue_pop(lg->q, &ev, LOGGER_POP_TIMEOUT_MS);

		if (r == 1) {
			event_format_line(&ev, line, sizeof(line));
			logger_write(lg, line);
		} else if (r == 0) {
			logger_note_drops(lg);   /* таймаут: періодично перевіряємо втрати */
		} else {
			break;                   /* черга закрита й порожня */
		}
	}
	logger_note_drops(lg);
	return NULL;
}

int logger_start(logger_t *lg, event_queue_t *q, const char *path, int echo_stdout)
{
	if (!lg || !q)
		return -1;

	lg->q = q;
	lg->file = NULL;
	lg->started = 0;
	lg->echo_stdout = echo_stdout;

	if (path && *path) {
		lg->file = fopen(path, "ae");   /* append + O_CLOEXEC */
		if (!lg->file) {
			fprintf(stderr, "logger: не вдалося відкрити '%s', пишу лише в stdout\n", path);
			lg->echo_stdout = 1;
		}
	}

	if (pthread_create(&lg->thread, NULL, logger_main, lg) != 0) {
		if (lg->file)
			fclose(lg->file);
		lg->file = NULL;
		return -1;
	}
	lg->started = 1;
	return 0;
}

void logger_stop(logger_t *lg)
{
	if (!lg || !lg->started)
		return;

	event_queue_shutdown(lg->q);   /* потік добере залишок і вийде */
	pthread_join(lg->thread, NULL);
	lg->started = 0;

	if (lg->file) {
		fclose(lg->file);
		lg->file = NULL;
	}
}
