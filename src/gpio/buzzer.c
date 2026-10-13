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
    printf("buzzer_beep\n");
    gpio_write(buzzer, 1);
    sleep_ms(time);
    gpio_write(buzzer, 0);
}
