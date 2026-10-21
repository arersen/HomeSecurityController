//
// Created by jambyl on 10/21/26.
//
#include <string.h>
#include "traffic_light.h"
#include "board_config.h"

e_gpio_status traffic_light_init(traffic_light_t *tl, const char *chipname)
{
    e_gpio_status st;

    memset(tl, 0, sizeof(*tl));

    st = gpio_init(&tl->red, chipname, RLIGHT_GPIO_OFFSET, GPIO_DIR_OUTPUT, "tl_red");
    if (st != GPIO_OK) goto fail;

    st = gpio_init(&tl->yellow, chipname, YLIGHT_GPIO_OFFSET, GPIO_DIR_OUTPUT, "tl_yellow");
    if (st != GPIO_OK) goto fail;

    st = gpio_init(&tl->green, chipname, GLIGHT_GPIO_OFFSET, GPIO_DIR_OUTPUT, "tl_green");
    if (st != GPIO_OK) goto fail;

    return GPIO_OK;

    fail:
        traffic_light_deinit(tl);
    return st;
}

e_gpio_status traffic_light_set(traffic_light_t *tl, e_tl_state state)
{
    if (gpio_write(&tl->red,    state == TL_RED)    != GPIO_OK) return GPIO_FAIL;
    if (gpio_write(&tl->yellow, state == TL_YELLOW) != GPIO_OK) return GPIO_FAIL;
    if (gpio_write(&tl->green,  state == TL_GREEN)  != GPIO_OK) return GPIO_FAIL;
    return GPIO_OK;
}

void traffic_light_deinit(traffic_light_t *tl)
{
    if (tl->red.line)    gpio_write(&tl->red, 0);
    if (tl->yellow.line) gpio_write(&tl->yellow, 0);
    if (tl->green.line)  gpio_write(&tl->green, 0);

    gpio_deinit(&tl->red);
    gpio_deinit(&tl->yellow);
    gpio_deinit(&tl->green);
}