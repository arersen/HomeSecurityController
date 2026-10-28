//
// Created by jambyl on 10/28/26.
//
#ifndef HOMESECURITYCONTROLLER_SHT3X_H
#define HOMESECURITYCONTROLLER_SHT3X_H

#include <stddef.h>
#include <stdint.h>
#include "i2c.h"

/*
 * Драйвер Sensirion SHT3x (SHT30/31/35) — температура + вологість по I2C.
 * Режим single-shot, висока повторюваність, без clock stretching
 * (найнадійніше для Linux i2c-dev).
 *
 * Адреса: 0x44 (ADDR->GND, типово на цій платі) або 0x45 (ADDR->VDD).
 */

#define SHT3X_ADDR_DEFAULT 0x44

typedef struct {
	i2c_t   bus;
	uint8_t addr;
	int     ready;
} sht3x_t;

/* i2c_path напр. "/dev/i2c-1"; addr 0 => SHT3X_ADDR_DEFAULT. 0/-1. */
int  sht3x_init(sht3x_t *s, const char *i2c_path, uint8_t addr);
void sht3x_deinit(sht3x_t *s);

/* Одне вимірювання. temp_c/humidity можуть бути NULL. 0 / -1 (I2C або CRC). */
int  sht3x_read(sht3x_t *s, float *temp_c, float *humidity);

/* ---- Чисті функції (тестовані без заліза) ---- */
uint8_t sht3x_crc8(const uint8_t *data, size_t len);   /* poly 0x31, init 0xFF */
/* raw[6] = T_msb,T_lsb,T_crc, RH_msb,RH_lsb,RH_crc. 0 / -1 (CRC). */
int     sht3x_parse(const uint8_t raw[6], float *temp_c, float *humidity);

#endif //HOMESECURITYCONTROLLER_SHT3X_H
