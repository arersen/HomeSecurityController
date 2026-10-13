#include "gpio.h"

e_gpio_status gpio_init(gpio_t *pin, const char *chipname,
						unsigned int offset, e_gpio_dir dir,
						const char *consumer)
{
	const char *name = consumer ? consumer : "security_daemon";
	int ret;

	pin->chip = NULL;
	pin->line = NULL;
	pin->offset = offset;

	pin->chip = gpiod_chip_open(chipname);
	if (!pin->chip) {
		return GPIO_CHIP_FAIL;
	}

	pin->line = gpiod_chip_get_line(pin->chip, offset);
	if (!pin->line) {
		gpiod_chip_close(pin->chip);
		pin->chip = NULL;
		return GPIO_LINE_FAIL;
	}

	if (dir == GPIO_DIR_OUTPUT) {
		ret = gpiod_line_request_output(pin->line, name, 0);
	} else {
		ret = gpiod_line_request_input(pin->line, name);
	}

	if (ret < 0) {
		gpiod_chip_close(pin->chip);
		pin->chip = NULL;
		pin->line = NULL;
		return GPIO_LINE_FAIL;
	}

	return GPIO_OK;
}

e_gpio_status gpio_write(gpio_t *pin, int value)
{
	if (gpiod_line_set_value(pin->line, value ? 1 : 0) < 0) {
		return GPIO_FAIL;
	}
	return GPIO_OK;
}

int gpio_read(gpio_t *pin)
{
	return gpiod_line_get_value(pin->line);   /* 0, 1 або -1 */
}

void gpio_deinit(gpio_t *pin)
{
	if (pin->line) {
		gpiod_line_release(pin->line);
		pin->line = NULL;
	}
	if (pin->chip) {
		gpiod_chip_close(pin->chip);
		pin->chip = NULL;
	}
}