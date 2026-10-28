//
// Created by jambyl on 10/28/26.
//
#include "events.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Внутрішнє ---------- */

static void copy_field(char *dst, size_t dstsz, const char *src)
{
	if (!src) {
		dst[0] = '\0';
		return;
	}
	snprintf(dst, dstsz, "%s", src);
}

static void abstime_after(struct timespec *ts, int timeout_ms)
{
	clock_gettime(CLOCK_MONOTONIC, ts);
	ts->tv_sec  += timeout_ms / 1000;
	ts->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec++;
		ts->tv_nsec -= 1000000000L;
	}
}

/* ---------- Життєвий цикл ---------- */

int event_queue_init(event_queue_t *q, size_t capacity)
{
	pthread_condattr_t cattr;

	if (!q || capacity == 0)
		return -1;

	memset(q, 0, sizeof(*q));
	q->buf = calloc(capacity, sizeof(*q->buf));
	if (!q->buf)
		return -1;
	q->cap = capacity;

	if (pthread_mutex_init(&q->mtx, NULL) != 0) {
		free(q->buf);
		q->buf = NULL;
		return -1;
	}

	/* condvar на монотонному годиннику — стійко до стрибків системного часу */
	pthread_condattr_init(&cattr);
	pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
	if (pthread_cond_init(&q->not_empty, &cattr) != 0) {
		pthread_condattr_destroy(&cattr);
		pthread_mutex_destroy(&q->mtx);
		free(q->buf);
		q->buf = NULL;
		return -1;
	}
	pthread_condattr_destroy(&cattr);
	return 0;
}

void event_queue_destroy(event_queue_t *q)
{
	if (!q || !q->buf)
		return;
	pthread_cond_destroy(&q->not_empty);
	pthread_mutex_destroy(&q->mtx);
	free(q->buf);
	q->buf = NULL;
	q->cap = q->count = q->head = q->tail = 0;
}

/* ---------- Producer ---------- */

int event_queue_push(event_queue_t *q, const event_t *ev)
{
	if (!q || !ev)
		return -1;

	pthread_mutex_lock(&q->mtx);

	if (q->shutdown || q->count == q->cap) {
		q->dropped++;                 /* переповнення або вже зупиняємось */
		pthread_mutex_unlock(&q->mtx);
		return -1;
	}

	q->buf[q->tail] = *ev;
	q->tail = (q->tail + 1) % q->cap;
	q->count++;

	pthread_cond_signal(&q->not_empty);
	pthread_mutex_unlock(&q->mtx);
	return 0;
}

int event_queue_emit(event_queue_t *q, event_type_t type,
                     const char *uid, const char *name, const char *detail)
{
	event_t ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	clock_gettime(CLOCK_REALTIME, &ev.ts);
	copy_field(ev.uid, sizeof(ev.uid), uid);
	copy_field(ev.name, sizeof(ev.name), name);
	copy_field(ev.detail, sizeof(ev.detail), detail);
	return event_queue_push(q, &ev);
}

/* ---------- Consumer ---------- */

int event_queue_pop(event_queue_t *q, event_t *out, int timeout_ms)
{
	struct timespec deadline;
	int rc = 0;

	if (!q || !out)
		return -1;

	pthread_mutex_lock(&q->mtx);
	abstime_after(&deadline, timeout_ms);

	while (q->count == 0) {
		if (q->shutdown) {
			pthread_mutex_unlock(&q->mtx);
			return -1;                /* закрито й порожньо */
		}
		if (pthread_cond_timedwait(&q->not_empty, &q->mtx, &deadline) != 0) {
			/* таймаут (або помилка) — ще раз перевіримо умову циклом */
			if (q->count == 0) {
				int sh = q->shutdown;
				pthread_mutex_unlock(&q->mtx);
				return sh ? -1 : 0;
			}
			break;
		}
	}

	*out = q->buf[q->head];
	q->head = (q->head + 1) % q->cap;
	q->count--;
	rc = 1;

	pthread_mutex_unlock(&q->mtx);
	return rc;
}

void event_queue_shutdown(event_queue_t *q)
{
	if (!q)
		return;
	pthread_mutex_lock(&q->mtx);
	q->shutdown = 1;
	pthread_cond_broadcast(&q->not_empty);
	pthread_mutex_unlock(&q->mtx);
}

unsigned long event_queue_take_dropped(event_queue_t *q)
{
	unsigned long d;

	if (!q)
		return 0;
	pthread_mutex_lock(&q->mtx);
	d = q->dropped;
	q->dropped = 0;
	pthread_mutex_unlock(&q->mtx);
	return d;
}

/* ---------- Форматування ---------- */

const char *event_type_name(event_type_t t)
{
	switch (t) {
	case EV_STARTUP:        return "STARTUP";
	case EV_SHUTDOWN:       return "SHUTDOWN";
	case EV_ACCESS_GRANTED: return "GRANTED";
	case EV_ACCESS_DENIED:  return "DENIED";
	case EV_INTRUSION:      return "INTRUSION";
	case EV_PASSAGE_START:  return "PASSAGE_START";
	case EV_PASSAGE_END:    return "PASSAGE_END";
	case EV_UNLOCK_TIMEOUT: return "UNLOCK_TIMEOUT";
	case EV_DB_RELOAD:      return "DB_RELOAD";
	case EV_SENSOR:         return "SENSOR";
	case EV_ERROR:          return "ERROR";
	case EV_TYPE_COUNT:     break;
	}
	return "UNKNOWN";
}

char *event_format_line(const event_t *ev, char *buf, size_t len)
{
	char tbuf[32];
	struct tm tm;
	time_t t;
	int n;

	if (!buf || len == 0)
		return buf;
	if (!ev) {
		buf[0] = '\0';
		return buf;
	}

	t = ev->ts.tv_sec;
	gmtime_r(&t, &tm);
	strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", &tm);

	n = snprintf(buf, len, "%s [%-13s]", tbuf, event_type_name(ev->type));
	if (n < 0 || (size_t)n >= len)
		return buf;

	if (ev->uid[0])
		n += snprintf(buf + n, (n < (int)len) ? len - (size_t)n : 0, " uid=%s", ev->uid);
	if (n >= 0 && (size_t)n < len && ev->name[0])
		n += snprintf(buf + n, len - (size_t)n, " name=\"%s\"", ev->name);
	if (n >= 0 && (size_t)n < len && ev->detail[0])
		snprintf(buf + n, len - (size_t)n, " %s", ev->detail);

	return buf;
}
