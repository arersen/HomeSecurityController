//
// Created by jambyl on 10/13/26.
//
#pragma once
#ifndef HOMESECURITYCONTROLLER_RADAR_H
#define HOMESECURITYCONTROLLER_RADAR_H

#include "gpio.h"
#include "etc.h"
#include "board_config.h"

e_gpio_status radar_init(gpio_t* radar, unsigned int offset);

void radar_deinit(gpio_t* radar);

int radar_read(gpio_t* radar);

void radar_run_diagnostic(gpio_t* radar, int timeout_sec);

#endif //HOMESECURITYCONTROLLER_RADAR_H