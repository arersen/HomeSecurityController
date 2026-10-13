//
// Created by jambyl on 10/13/26.
//
#pragma once

#ifndef HOMESECURITYCONTROLLER_BOARD_CONFIG_H
#define HOMESECURITYCONTROLLER_BOARD_CONFIG_H

#include <stdlib.h>
#include <stdio.h>

#define BUZZER_GPIO_OFFSET 17
#define RADAR_GPIO_OFFSET 27
#define TEST_GPIO_COLLISION_OFFSET 17




void check_pins_or_die(void);

#endif //HOMESECURITYCONTROLLER_BOARD_CONFIG_H
