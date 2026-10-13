
#include <stdio.h>
#include "gpio.h"
int main(){
	gpio_t buzzer;
	if (gpio_init(&buzzer, "/dev/gpiochip4", 17, GPIO_DIR_OUTPUT, "buzzer") != GPIO_OK) {
		printf("Error\n\n\n\n");
		return 1;
	}
	gpio_write(&buzzer, 1);
	printf("Test\n\n\n\n");
	return 0;

}

