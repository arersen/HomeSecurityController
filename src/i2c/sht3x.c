//
// Created by jambyl on 10/28/26.
//
#include "sht3x.h"
#include "etc.h"   /* sleep_ms */

#include <string.h>

/* Команди (big-endian 16 біт) */
enum {
	CMD_MEAS_HIGH   = 0x2400,   /* single-shot, high repeatability, clock stretch off */
	CMD_SOFT_RESET  = 0x30A2
};

uint8_t sht3x_crc8(const uint8_t *data, size_t len)
{
	uint8_t crc = 0xFF;

	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
	}
	return crc;
}

int sht3x_parse(const uint8_t raw[6], float *temp_c, float *humidity)
{
	uint16_t rt, rh;

	if (!raw)
		return -1;
	if (sht3x_crc8(&raw[0], 2) != raw[2])
		return -1;
	if (sht3x_crc8(&raw[3], 2) != raw[5])
		return -1;

	rt = (uint16_t)((raw[0] << 8) | raw[1]);
	rh = (uint16_t)((raw[3] << 8) | raw[4]);

	if (temp_c)
		*temp_c = -45.0f + 175.0f * (float)rt / 65535.0f;
	if (humidity) {
		float h = 100.0f * (float)rh / 65535.0f;
		if (h < 0.0f)   h = 0.0f;
		if (h > 100.0f) h = 100.0f;
		*humidity = h;
	}
	return 0;
}

static int sht3x_cmd(sht3x_t *s, uint16_t cmd)
{
	uint8_t b[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
	return i2c_write(&s->bus, s->addr, b, sizeof(b));
}

int sht3x_init(sht3x_t *s, const char *i2c_path, uint8_t addr)
{
	if (!s || !i2c_path)
		return -1;

	memset(s, 0, sizeof(*s));
	s->bus.fd = -1;
	s->addr = addr ? addr : SHT3X_ADDR_DEFAULT;

	if (i2c_open(&s->bus, i2c_path) != 0)
		return -1;
	if (sht3x_cmd(s, CMD_SOFT_RESET) != 0) {   /* перевіряє і наявність датчика (ACK) */
		i2c_close(&s->bus);
		return -1;
	}
	sleep_ms(2);   /* reset <= 1.5 мс */
	s->ready = 1;
	return 0;
}

void sht3x_deinit(sht3x_t *s)
{
	if (!s)
		return;
	i2c_close(&s->bus);
	s->ready = 0;
}

int sht3x_read(sht3x_t *s, float *temp_c, float *humidity)
{
	uint8_t raw[6];

	if (!s || !s->ready)
		return -1;
	if (sht3x_cmd(s, CMD_MEAS_HIGH) != 0)
		return -1;
	sleep_ms(20);   /* high repeatability: макс. ~15 мс */
	if (i2c_read(&s->bus, s->addr, raw, sizeof(raw)) != 0)
		return -1;
	return sht3x_parse(raw, temp_c, humidity);
}
