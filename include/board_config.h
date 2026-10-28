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

#define RLIGHT_GPIO_OFFSET 22
#define YLIGHT_GPIO_OFFSET 26
#define GLIGHT_GPIO_OFFSET 23

#define RC522_SPI_DEV         "/dev/spidev0.0"
#define RC522_RST_GPIO_OFFSET 25

/* I2C: SHT3x (температура+вологість). SDA=GPIO2(пін3), SCL=GPIO3(пін5), шина 1 */
#define I2C_BUS               "/dev/i2c-1"
#define SHT3X_I2C_ADDR        0x44   /* ADDR->GND; 0x45 якщо ADDR->VDD */


void check_pins_or_die(void);

#endif //HOMESECURITYCONTROLLER_BOARD_CONFIG_H
