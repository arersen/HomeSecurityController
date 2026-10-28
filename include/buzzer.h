//
// Created by jambyl on 10/13/26.
//

#ifndef HOMESECURITYCONTROLLER_BUZZER_H
#define HOMESECURITYCONTROLLER_BUZZER_H


#include <stdint.h>
#include "gpio.h"
#include "etc.h"
#include "board_config.h"


/* ---- Низькорівневий зумер (блокуючий) ---- */
e_gpio_status buzzer_init(gpio_t* buzzer, unsigned int offset);
void          buzzer_deinit(gpio_t* buzzer);
void          buzzer_beep(gpio_t* buzzer, long time);   /* блокує на time мс */


/* ---- Неблокуючий контролер патернів ----
 *
 * Програє послідовність "count біпів по on_ms із паузами off_ms", не блокуючи
 * потік. Викликається з головного циклу: buzzer_ctl_tick(&b, now) раз за ітерацію,
 * а buzzer_ctl_start(...) лише ставить патерн і повертається миттєво.
 *
 * now — монотонний час у мілісекундах (той самий, що передається в tick).
 * НЕ потокобезпечний: один buzzer_ctl_t обслуговується з одного потоку.
 */
typedef struct {
	gpio_t  *pin;
	int      remaining;     /* скільки увімкнень лишилось відіграти */
	long     on_ms;
	long     off_ms;
	int      active;        /* 1 = лінія зараз під напругою */
	int      running;       /* 1 = патерн відтворюється */
	uint64_t next_toggle;   /* час наступного перемикання (мс) */
} buzzer_ctl_t;

/* Прив'язує контролер до вже ініціалізованого gpio_t (buzzer_init). */
void buzzer_ctl_init(buzzer_ctl_t *b, gpio_t *pin);

/* Поставити патерн. Якщо вже грає інший — замінює його. count<=0 нічого не робить. */
void buzzer_ctl_start(buzzer_ctl_t *b, int count, long on_ms, long off_ms, uint64_t now);

/* Негайно зупинити й погасити лінію. */
void buzzer_ctl_cancel(buzzer_ctl_t *b);

/* 1, якщо патерн ще грає. */
int  buzzer_ctl_busy(const buzzer_ctl_t *b);

/* Рушій: викликати щоітерації головного циклу. Перемикає лінію за часом. */
void buzzer_ctl_tick(buzzer_ctl_t *b, uint64_t now);

#endif //HOMESECURITYCONTROLLER_BUZZER_H
