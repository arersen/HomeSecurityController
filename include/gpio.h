#ifndef GPIO_H
#define GPIO_H

#include <gpiod.h>

typedef enum {
	GPIO_OK,
	GPIO_FAIL,
	GPIO_CHIP_FAIL,
	GPIO_LINE_FAIL
} e_gpio_status;

typedef enum {
	GPIO_DIR_INPUT,
	GPIO_DIR_OUTPUT
} e_gpio_dir;

typedef struct {
	struct gpiod_chip *chip;
	struct gpiod_line *line;
	unsigned int offset;
} gpio_t;

e_gpio_status gpio_init(gpio_t *pin, const char *chipname,
						unsigned int offset, e_gpio_dir dir,
						const char *consumer);
e_gpio_status gpio_write(gpio_t *pin, int value);
int           gpio_read(gpio_t *pin);   /* 0/1, або -1 при помилці */
void          gpio_deinit(gpio_t *pin);

#endif