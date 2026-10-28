#include "buzzer.h"
#include <stdio.h>


e_gpio_status buzzer_init(gpio_t* buzzer, unsigned int offset) {
    printf("buzzer_init\n");

    e_gpio_status gpio_init_status = gpio_init(buzzer, "/dev/gpiochip0", offset, GPIO_DIR_OUTPUT, "buzzer");
    if (gpio_init_status != GPIO_OK) {
        fprintf(stderr, "Error: gpio_init failed\n");
    }

    return gpio_init_status;
}


void buzzer_deinit(gpio_t* buzzer) {
    gpio_write(buzzer, 0);
    gpio_deinit(buzzer);

}


void buzzer_beep(gpio_t* buzzer, long time) {
    gpio_write(buzzer, 1);
    sleep_ms(time);
    gpio_write(buzzer, 0);
}


/* ---- Неблокуючий контролер ---- */

void buzzer_ctl_init(buzzer_ctl_t *b, gpio_t *pin)
{
	b->pin = pin;
	b->remaining = 0;
	b->on_ms = 0;
	b->off_ms = 0;
	b->active = 0;
	b->running = 0;
	b->next_toggle = 0;
}

/* Увімкнути лінію й запланувати вимкнення через on_ms. */
static void buzzer_ctl_begin_pulse(buzzer_ctl_t *b, uint64_t now)
{
	gpio_write(b->pin, 1);
	b->active = 1;
	b->next_toggle = now + (uint64_t)b->on_ms;
}

void buzzer_ctl_start(buzzer_ctl_t *b, int count, long on_ms, long off_ms, uint64_t now)
{
	if (count <= 0 || on_ms <= 0) {
		buzzer_ctl_cancel(b);
		return;
	}
	b->remaining = count;
	b->on_ms = on_ms;
	b->off_ms = off_ms;
	b->running = 1;
	b->remaining--;                 /* цей імпульс рахуємо одразу */
	buzzer_ctl_begin_pulse(b, now);
}

void buzzer_ctl_cancel(buzzer_ctl_t *b)
{
	if (b->active || b->running)
		gpio_write(b->pin, 0);
	b->active = 0;
	b->running = 0;
	b->remaining = 0;
}

int buzzer_ctl_busy(const buzzer_ctl_t *b)
{
	return b->running;
}

void buzzer_ctl_tick(buzzer_ctl_t *b, uint64_t now)
{
	if (!b->running)
		return;
	if (now < b->next_toggle)
		return;

	if (b->active) {
		/* кінець імпульсу: гасимо лінію */
		gpio_write(b->pin, 0);
		b->active = 0;
		if (b->remaining > 0) {
			b->next_toggle = now + (uint64_t)b->off_ms;   /* пауза перед наступним */
		} else {
			b->running = 0;                               /* патерн завершено */
		}
	} else {
		/* кінець паузи: наступний імпульс */
		b->remaining--;
		buzzer_ctl_begin_pulse(b, now);
	}
}
