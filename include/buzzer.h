//
// Created by jambyl on 10/13/26.
//

#ifndef HOMESECURITYCONTROLLER_BUZZER_H
#define HOMESECURITYCONTROLLER_BUZZER_H


#include "gpio.h"
#include "etc.h"
#include "board_config.h"


e_gpio_status buzzer_init(gpio_t* buzzer, unsigned int offset);
void buzzer_deinit(gpio_t* buzzer);
void buzzer_beep(gpio_t* buzzer, long time);

#endif //HOMESECURITYCONTROLLER_BUZZER_H
