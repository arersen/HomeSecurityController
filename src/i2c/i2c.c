//
// Created by jambyl on 10/28/26.
//
#include "i2c.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

int i2c_open(i2c_t *bus, const char *path)
{
	if (!bus || !path)
		return -1;
	bus->fd = open(path, O_RDWR | O_CLOEXEC);
	return bus->fd < 0 ? -1 : 0;
}

void i2c_close(i2c_t *bus)
{
	if (bus && bus->fd >= 0) {
		close(bus->fd);
		bus->fd = -1;
	}
}

static int i2c_set_addr(i2c_t *bus, uint8_t addr)
{
	return ioctl(bus->fd, I2C_SLAVE, (unsigned long)addr) < 0 ? -1 : 0;
}

int i2c_write(i2c_t *bus, uint8_t addr, const uint8_t *buf, size_t len)
{
	if (!bus || bus->fd < 0 || (!buf && len))
		return -1;
	if (i2c_set_addr(bus, addr) != 0)
		return -1;
	return write(bus->fd, buf, len) == (ssize_t)len ? 0 : -1;
}

int i2c_read(i2c_t *bus, uint8_t addr, uint8_t *buf, size_t len)
{
	if (!bus || bus->fd < 0 || (!buf && len))
		return -1;
	if (i2c_set_addr(bus, addr) != 0)
		return -1;
	return read(bus->fd, buf, len) == (ssize_t)len ? 0 : -1;
}
