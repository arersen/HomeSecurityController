//
// Created by jambyl on 10/28/26.
//
#ifndef HOMESECURITYCONTROLLER_EVENTS_H
#define HOMESECURITYCONTROLLER_EVENTS_H

#include <pthread.h>
#include <stddef.h>
#include <time.h>

#include "card_store.h"   /* CARD_UID_STRLEN, CARD_NAME_MAX */

/*
 * Черга подій (producer/consumer) для демона.
 *
 * Робочі потоки (цикл контролю доступу) кладуть події неблокуюче, окремий
 * потік-логер їх забирає й пише в журнал. Так повільний файловий ввід/вивід
 * не стопорить реального часу реакції на картку/рух.
 *
 * Реалізація: обмежений кільцевий буфер під mutex + condition variable
 * (внутрішньопроцесний IPC). При переповненні подія відкидається, а лічильник
 * dropped зростає — логер потім це фіксує.
 */

typedef enum {
	EV_STARTUP = 0,
	EV_SHUTDOWN,
	EV_ACCESS_GRANTED,
	EV_ACCESS_DENIED,
	EV_INTRUSION,
	EV_PASSAGE_START,
	EV_PASSAGE_END,
	EV_UNLOCK_TIMEOUT,
	EV_DB_RELOAD,
	EV_SENSOR,
	EV_ERROR,
	EV_TYPE_COUNT
} event_type_t;

#define EVENT_DETAIL_MAX 64

typedef struct {
	event_type_t    type;
	struct timespec ts;                       /* CLOCK_REALTIME моменту події */
	char            uid[CARD_UID_STRLEN];     /* "" якщо нема */
	char            name[CARD_NAME_MAX];      /* "" якщо нема */
	char            detail[EVENT_DETAIL_MAX]; /* "" якщо нема */
} event_t;

typedef struct {
	event_t        *buf;
	size_t          cap;
	size_t          head;      /* індекс читання */
	size_t          tail;      /* індекс запису */
	size_t          count;
	unsigned long   dropped;   /* скільки відкинуто через переповнення */
	int             shutdown;
	pthread_mutex_t mtx;
	pthread_cond_t  not_empty;
} event_queue_t;

/* 0 / -1. capacity має бути > 0. */
int  event_queue_init(event_queue_t *q, size_t capacity);
void event_queue_destroy(event_queue_t *q);

/* Неблокуючий producer. 0 — покладено, -1 — відкинуто (черга повна/закрита). */
int  event_queue_push(event_queue_t *q, const event_t *ev);

/* Зручний варіант: зібрати подію (час проставляється сам) і покласти.
 * uid/name/detail можуть бути NULL. */
int  event_queue_emit(event_queue_t *q, event_type_t type,
                      const char *uid, const char *name, const char *detail);

/* Consumer: чекати до timeout_ms на подію.
 * 1 — отримано, 0 — таймаут, -1 — черга закрита й порожня. */
int  event_queue_pop(event_queue_t *q, event_t *out, int timeout_ms);

/* Розбудити споживача, щоб він добрав залишок і вийшов. */
void event_queue_shutdown(event_queue_t *q);

/* Прочитати й обнулити лічильник відкинутих подій. */
unsigned long event_queue_take_dropped(event_queue_t *q);

/* ---- Форматування ---- */
const char *event_type_name(event_type_t t);
/* Один рядок журналу без '\n'. Повертає buf. */
char       *event_format_line(const event_t *ev, char *buf, size_t len);

#endif //HOMESECURITYCONTROLLER_EVENTS_H
