//
// Created by jambyl on 10/28/26.
//
#ifndef HOMESECURITYCONTROLLER_I2C_H
#define HOMESECURITYCONTROLLER_I2C_H

#include <stddef.h>
#include <stdint.h>

/*
 * Тонка обгортка Linux I2C-master (/dev/i2c-N через ioctl I2C_SLAVE).
 * Одна шина — багато адрес: адреса задається на кожну операцію.
 * НЕ потокобезпечний (одна шина — з одного потоку).
 */

typedef struct {
	int fd;
} i2c_t;

int  i2c_open(i2c_t *bus, const char *path);   /* напр. "/dev/i2c-1" */
void i2c_close(i2c_t *bus);

/* 0 / -1. Повна передача або помилка. */
int  i2c_write(i2c_t *bus, uint8_t addr, const uint8_t *buf, size_t len);
int  i2c_read(i2c_t *bus, uint8_t addr, uint8_t *buf, size_t len);

#endif //HOMESECURITYCONTROLLER_I2C_H
