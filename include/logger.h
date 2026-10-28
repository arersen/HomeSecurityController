//
// Created by jambyl on 10/28/26.
//
#ifndef HOMESECURITYCONTROLLER_LOGGER_H
#define HOMESECURITYCONTROLLER_LOGGER_H

#include <stdio.h>
#include <pthread.h>

#include "events.h"

/*
 * Логер — єдиний споживач черги подій. Працює в окремому потоці: забирає
 * події з event_queue_t і пише їх у журнал (файл + stdout). Так файловий
 * ввід/вивід не блокує цикл контролю доступу.
 */

typedef struct {
	event_queue_t *q;
	FILE          *file;        /* NULL, якщо без файлу */
	pthread_t      thread;
	int            started;
	int            echo_stdout; /* 1 = дублювати в stdout */
} logger_t;

/*
 * Запустити потік логера.
 *  path        — файл журналу (дозаписується). NULL — тільки stdout.
 *  echo_stdout — також друкувати у stdout.
 * 0 / -1.
 */
int  logger_start(logger_t *lg, event_queue_t *q, const char *path, int echo_stdout);

/* Коректно зупинити: закрити чергу, добрати залишок, приєднати потік, закрити файл. */
void logger_stop(logger_t *lg);

#endif //HOMESECURITYCONTROLLER_LOGGER_H
