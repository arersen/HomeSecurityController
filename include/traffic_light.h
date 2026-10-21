//
// Created by jambyl on 10/21/26.
//

#ifndef HOMESECURITYCONTROLLER_TRAFFIC_LIGHT_H
#define HOMESECURITYCONTROLLER_TRAFFIC_LIGHT_H
#include "gpio.h"

typedef enum {
    TL_OFF,
    TL_RED,
    TL_YELLOW,
    TL_GREEN
} e_tl_state;

typedef struct {
    gpio_t red;
    gpio_t yellow;
    gpio_t green;
} traffic_light_t;

e_gpio_status traffic_light_init(traffic_light_t *tl, const char *chipname);
e_gpio_status traffic_light_set(traffic_light_t *tl, e_tl_state state);
void          traffic_light_deinit(traffic_light_t *tl);
#endif //HOMESECURITYCONTROLLER_TRAFFIC_LIGHT_H
