#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

#include "rc522.h"

/* ---------- Регістри MFRC522 ---------- */
enum {
	REG_COMMAND      = 0x01,
	REG_COM_IRQ      = 0x04,
	REG_DIV_IRQ      = 0x05,
	REG_ERROR        = 0x06,
	REG_STATUS2      = 0x08,
	REG_FIFO_DATA    = 0x09,
	REG_FIFO_LEVEL   = 0x0A,
	REG_CONTROL      = 0x0C,
	REG_BIT_FRAMING  = 0x0D,
	REG_COLL         = 0x0E,
	REG_MODE         = 0x11,
	REG_TX_MODE      = 0x12,
	REG_RX_MODE      = 0x13,
	REG_TX_CONTROL   = 0x14,
	REG_TX_ASK       = 0x15,
	REG_MOD_WIDTH    = 0x24,
	REG_RF_CFG       = 0x26,
	REG_T_MODE       = 0x2A,
	REG_T_PRESCALER  = 0x2B,
	REG_T_RELOAD_H   = 0x2C,
	REG_T_RELOAD_L   = 0x2D,
	REG_VERSION      = 0x37
};

/* ---------- Команди чипа ---------- */
enum {
	CMD_IDLE        = 0x00,
	CMD_TRANSCEIVE  = 0x0C,
	CMD_MF_AUTHENT  = 0x0E,
	CMD_SOFT_RESET  = 0x0F
};

/* ---------- Команди картки ---------- */
enum {
	PICC_REQA         = 0x26,
	PICC_WUPA         = 0x52,
	PICC_CASCADE_TAG  = 0x88,
	PICC_SEL_CL1      = 0x93,
	PICC_SEL_CL2      = 0x95,
	PICC_SEL_CL3      = 0x97,
	PICC_HLTA         = 0x50,
	PICC_MF_AUTH_A    = 0x60,
	PICC_MF_AUTH_B    = 0x61,
	PICC_MF_READ      = 0x30,
	PICC_MF_WRITE     = 0xA0,
	PICC_MF_DECREMENT = 0xC0,
	PICC_MF_INCREMENT = 0xC1,
	PICC_MF_RESTORE   = 0xC2,
	PICC_MF_TRANSFER  = 0xB0,
	PICC_UL_WRITE     = 0xA2
};

#define FIFO_SIZE              64
#define WAIT_MS                50      /* запас поверх апаратного таймера 25 мс */

#define COMMAND_POWER_DOWN     0x10
#define IRQ_TIMER              0x01
#define IRQ_IDLE               0x10
#define IRQ_RX                 0x20
#define ERR_COLL               0x08
#define ERR_FATAL              0x13    /* BufferOvfl | ParityErr | ProtocolErr */
#define STATUS2_CRYPTO1_ON     0x08
#define BIT_FRAMING_START_SEND 0x80
#define COLL_VALUES_AFTER_COLL 0x80
#define COLL_POS_NOT_VALID     0x20
#define MF_ACK                 0x0A

#define IO(x) do { if ((x) < 0) return RC522_ERR_IO; } while (0)

const uint8_t RC522_KEY_DEFAULT[RC522_MF_KEY_SIZE] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ---------- Час ---------- */

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void rc522_sleep_ms(unsigned ms)
{
	struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

/* ---------- SPI ---------- */

static int spi_xfer(rc522_t *dev, const uint8_t *tx, uint8_t *rx, size_t len)
{
	struct spi_ioc_transfer tr;

	memset(&tr, 0, sizeof(tr));
	tr.tx_buf = (__u64)(uintptr_t)tx;
	tr.rx_buf = (__u64)(uintptr_t)rx;
	tr.len = (__u32)len;
	tr.speed_hz = dev->speed_hz;
	tr.bits_per_word = 8;

	return ioctl(dev->fd, SPI_IOC_MESSAGE(1), &tr) < 0 ? -1 : 0;
}

/* Адреса: біт 7 = читання, біти 6..1 = регістр, біт 0 = 0 */
static uint8_t addr_wr(uint8_t reg) { return (uint8_t)((reg << 1) & 0x7E); }
static uint8_t addr_rd(uint8_t reg) { return (uint8_t)(addr_wr(reg) | 0x80); }

static int reg_write(rc522_t *dev, uint8_t reg, uint8_t val)
{
	uint8_t tx[2] = { addr_wr(reg), val };
	uint8_t rx[2];
	return spi_xfer(dev, tx, rx, sizeof(tx));
}

static int reg_read(rc522_t *dev, uint8_t reg, uint8_t *val)
{
	uint8_t tx[2] = { addr_rd(reg), 0x00 };
	uint8_t rx[2] = { 0 };

	if (spi_xfer(dev, tx, rx, sizeof(tx)) < 0)
		return -1;
	*val = rx[1];
	return 0;
}

static int reg_set_bits(rc522_t *dev, uint8_t reg, uint8_t mask)
{
	uint8_t v;
	if (reg_read(dev, reg, &v) < 0)
		return -1;
	return reg_write(dev, reg, (uint8_t)(v | mask));
}

static int reg_clear_bits(rc522_t *dev, uint8_t reg, uint8_t mask)
{
	uint8_t v;
	if (reg_read(dev, reg, &v) < 0)
		return -1;
	return reg_write(dev, reg, (uint8_t)(v & ~mask));
}

static int fifo_write(rc522_t *dev, const uint8_t *data, uint8_t len)
{
	uint8_t tx[FIFO_SIZE + 1];
	uint8_t rx[FIFO_SIZE + 1];

	if (len > FIFO_SIZE)
		return -1;
	tx[0] = addr_wr(REG_FIFO_DATA);
	memcpy(&tx[1], data, len);
	return spi_xfer(dev, tx, rx, (size_t)len + 1);
}

/*
 * Пакетне читання FIFO. Якщо rx_align != 0, молодші rx_align біт першого
 * байта лишаються з buf (це біти, які ми самі передали при anticollision).
 */
static int fifo_read(rc522_t *dev, uint8_t *buf, uint8_t len, uint8_t rx_align)
{
	uint8_t tx[FIFO_SIZE + 1];
	uint8_t rx[FIFO_SIZE + 1];

	if (len == 0)
		return 0;
	if (len > FIFO_SIZE)
		return -1;

	memset(tx, addr_rd(REG_FIFO_DATA), len);
	tx[len] = 0x00;
	if (spi_xfer(dev, tx, rx, (size_t)len + 1) < 0)
		return -1;

	if (rx_align) {
		uint8_t mask = (uint8_t)(0xFF << rx_align);
		buf[0] = (uint8_t)((buf[0] & ~mask) | (rx[1] & mask));
		memcpy(&buf[1], &rx[2], (size_t)len - 1);
	} else {
		memcpy(buf, &rx[1], len);
	}
	return 0;
}

/* ---------- CRC_A (ISO 14443-3, програмно) ---------- */

void rc522_crc_a(const uint8_t *data, size_t len, uint8_t out[2])
{
	uint16_t crc = 0x6363;

	for (size_t i = 0; i < len; i++) {
		uint8_t b = (uint8_t)(data[i] ^ (uint8_t)(crc & 0xFF));
		b = (uint8_t)(b ^ (uint8_t)(b << 4));
		crc = (uint16_t)((crc >> 8) ^ ((uint16_t)b << 8) ^ ((uint16_t)b << 3) ^ ((uint16_t)b >> 4));
	}
	out[0] = (uint8_t)(crc & 0xFF);
	out[1] = (uint8_t)(crc >> 8);
}

/* ---------- Обмін з карткою ---------- */

static rc522_status_t communicate(rc522_t *dev, uint8_t command, uint8_t wait_irq,
                                  const uint8_t *send, uint8_t send_len,
                                  uint8_t *back, uint8_t *back_len,
                                  uint8_t *valid_bits, uint8_t rx_align, int check_crc)
{
	uint8_t tx_last_bits = valid_bits ? (uint8_t)(*valid_bits & 0x07) : 0;
	uint8_t irq = 0, err = 0, level = 0, vb = 0;
	uint64_t deadline;

	IO(reg_write(dev, REG_COMMAND, CMD_IDLE));
	IO(reg_write(dev, REG_COM_IRQ, 0x7F));          /* скинути всі прапорці переривань */
	IO(reg_write(dev, REG_FIFO_LEVEL, 0x80));       /* очистити FIFO */
	IO(fifo_write(dev, send, send_len));
	IO(reg_write(dev, REG_BIT_FRAMING, (uint8_t)((rx_align << 4) | tx_last_bits)));
	IO(reg_write(dev, REG_COMMAND, command));
	if (command == CMD_TRANSCEIVE)
		IO(reg_set_bits(dev, REG_BIT_FRAMING, BIT_FRAMING_START_SEND));

	deadline = now_ms() + WAIT_MS;
	for (;;) {
		IO(reg_read(dev, REG_COM_IRQ, &irq));
		if (irq & wait_irq)
			break;
		if (irq & IRQ_TIMER)
			return RC522_ERR_TIMEOUT;   /* картка мовчить */
		if (now_ms() > deadline)
			return RC522_ERR_TIMEOUT;
	}

	IO(reg_read(dev, REG_ERROR, &err));
	if (err & ERR_FATAL)
		return RC522_ERR_PROTOCOL;

	if (back && back_len) {
		IO(reg_read(dev, REG_FIFO_LEVEL, &level));
		level &= 0x7F;
		if (level > *back_len)
			return RC522_ERR_BUFFER;
		*back_len = level;
		IO(fifo_read(dev, back, level, rx_align));
		IO(reg_read(dev, REG_CONTROL, &vb));
		vb &= 0x07;
		if (valid_bits)
			*valid_bits = vb;
	}

	/* Колізію повертаємо після читання FIFO: anticollision потрібні прийняті біти */
	if (err & ERR_COLL)
		return RC522_ERR_COLLISION;

	if (back && back_len && check_crc) {
		uint8_t crc[2];

		if (*back_len == 1 && vb == 4)
			return RC522_ERR_NACK;     /* 4-бітна відповідь MIFARE = NAK */
		if (*back_len < 2 || vb != 0)
			return RC522_ERR_CRC;
		rc522_crc_a(back, (size_t)(*back_len - 2), crc);
		if (back[*back_len - 2] != crc[0] || back[*back_len - 1] != crc[1])
			return RC522_ERR_CRC;
	}
	return RC522_OK;
}

rc522_status_t rc522_transceive(rc522_t *dev, const uint8_t *send, uint8_t send_len,
                                uint8_t *back, uint8_t *back_len, uint8_t *valid_bits,
                                int with_crc)
{
	uint8_t buf[FIFO_SIZE];
	uint8_t rx[FIFO_SIZE];
	uint8_t rx_len = sizeof(rx);
	rc522_status_t st;

	if (!dev || !send || send_len == 0 || send_len > FIFO_SIZE)
		return RC522_ERR_INVALID;
	if (back && !back_len)
		return RC522_ERR_INVALID;

	if (!with_crc)
		return communicate(dev, CMD_TRANSCEIVE, IRQ_RX | IRQ_IDLE, send, send_len,
		                   back, back_len, valid_bits, 0, 0);

	if (send_len > FIFO_SIZE - 2)
		return RC522_ERR_INVALID;
	memcpy(buf, send, send_len);
	rc522_crc_a(buf, send_len, &buf[send_len]);

	/* Приймаємо у свій буфер, щоб back_len рахувався без 2 байт CRC */
	st = communicate(dev, CMD_TRANSCEIVE, IRQ_RX | IRQ_IDLE, buf, (uint8_t)(send_len + 2),
	                 back ? rx : NULL, back ? &rx_len : NULL, valid_bits, 0, back != NULL);
	if (st != RC522_OK || !back)
		return st;

	rx_len = (uint8_t)(rx_len - 2);   /* відрізаємо CRC */
	if (rx_len > *back_len)
		return RC522_ERR_BUFFER;
	memcpy(back, rx, rx_len);
	*back_len = rx_len;
	return RC522_OK;
}

rc522_status_t rc522_read_reg(rc522_t *dev, uint8_t reg, uint8_t *val)
{
	if (!dev || !val || reg > 0x3F)
		return RC522_ERR_INVALID;
	IO(reg_read(dev, reg, val));
	return RC522_OK;
}

rc522_status_t rc522_write_reg(rc522_t *dev, uint8_t reg, uint8_t val)
{
	if (!dev || reg > 0x3F)
		return RC522_ERR_INVALID;
	IO(reg_write(dev, reg, val));
	return RC522_OK;
}

/* ---------- Життєвий цикл ---------- */

rc522_status_t rc522_soft_reset(rc522_t *dev)
{
	uint8_t cmd;

	if (!dev)
		return RC522_ERR_INVALID;
	IO(reg_write(dev, REG_COMMAND, CMD_SOFT_RESET));

	/* Генератор стартує ~40 мкс, але дешеві модулі бувають повільніші */
	rc522_sleep_ms(50);
	for (int i = 0; i < 4; i++) {
		IO(reg_read(dev, REG_COMMAND, &cmd));
		if (!(cmd & COMMAND_POWER_DOWN))
			return RC522_OK;
		rc522_sleep_ms(50);
	}
	return RC522_ERR_TIMEOUT;
}

static rc522_status_t configure(rc522_t *dev)
{
	IO(reg_write(dev, REG_TX_MODE, 0x00));       /* 106 кбіт/с, CRC рахуємо програмно */
	IO(reg_write(dev, REG_RX_MODE, 0x00));
	IO(reg_write(dev, REG_MOD_WIDTH, 0x26));
	IO(reg_write(dev, REG_T_MODE, 0x80));        /* TAuto: таймер стартує після передачі */
	IO(reg_write(dev, REG_T_PRESCALER, 0xA9));   /* 13.56 МГц / (2*169+1) = 40 кГц -> 25 мкс */
	IO(reg_write(dev, REG_T_RELOAD_H, 0x03));    /* 1000 * 25 мкс = 25 мс таймаут */
	IO(reg_write(dev, REG_T_RELOAD_L, 0xE8));
	IO(reg_write(dev, REG_TX_ASK, 0x40));        /* 100% ASK */
	IO(reg_write(dev, REG_MODE, 0x3D));          /* CRC preset 0x6363 */
	return rc522_antenna_on(dev);
}

rc522_status_t rc522_init(rc522_t *dev, const char *spidev, uint32_t speed_hz,
                          const char *gpiochip, int rst_offset)
{
	uint8_t mode = SPI_MODE_0;
	uint8_t bits = 8;
	rc522_status_t st;

	if (!dev || !spidev)
		return RC522_ERR_INVALID;

	memset(dev, 0, sizeof(*dev));
	dev->fd = -1;
	dev->speed_hz = speed_hz ? speed_hz : RC522_DEFAULT_SPI_HZ;

	dev->fd = open(spidev, O_RDWR | O_CLOEXEC);
	if (dev->fd < 0)
		return RC522_ERR_IO;

	if (ioctl(dev->fd, SPI_IOC_WR_MODE, &mode) < 0 ||
	    ioctl(dev->fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
	    ioctl(dev->fd, SPI_IOC_WR_MAX_SPEED_HZ, &dev->speed_hz) < 0) {
		st = RC522_ERR_IO;
		goto fail;
	}

	if (rst_offset >= 0) {
		if (!gpiochip) {
			st = RC522_ERR_INVALID;
			goto fail;
		}
		/* gpio_init ставить 0: чип у hard power-down, потім відпускаємо RST */
		if (gpio_init(&dev->rst, gpiochip, (unsigned int)rst_offset,
		              GPIO_DIR_OUTPUT, "rc522_rst") != GPIO_OK) {
			st = RC522_ERR_IO;
			goto fail;
		}
		dev->has_rst = 1;
		rc522_sleep_ms(1);
		if (gpio_write(&dev->rst, 1) != GPIO_OK) {
			st = RC522_ERR_IO;
			goto fail;
		}
		rc522_sleep_ms(50);
	}

	st = rc522_soft_reset(dev);
	if (st == RC522_ERR_TIMEOUT)
		st = RC522_ERR_NO_CHIP;   /* MISO висить у 1: чипа не видно */
	if (st != RC522_OK)
		goto fail;

	if (reg_read(dev, REG_VERSION, &dev->version) < 0) {
		st = RC522_ERR_IO;
		goto fail;
	}
	if (dev->version == 0x00 || dev->version == 0xFF) {
		st = RC522_ERR_NO_CHIP;
		goto fail;
	}

	st = configure(dev);
	if (st != RC522_OK)
		goto fail;
	return RC522_OK;

fail:
	rc522_deinit(dev);
	return st;
}

void rc522_deinit(rc522_t *dev)
{
	if (!dev)
		return;
	if (dev->fd >= 0) {
		(void)rc522_antenna_off(dev);
		close(dev->fd);
	}
	dev->fd = -1;
	if (dev->has_rst) {
		gpio_write(&dev->rst, 0);   /* hard power-down */
		gpio_deinit(&dev->rst);
		dev->has_rst = 0;
	}
}

rc522_status_t rc522_get_version(rc522_t *dev, uint8_t *version)
{
	return rc522_read_reg(dev, REG_VERSION, version);
}

/* ---------- Антена й живлення ---------- */

rc522_status_t rc522_antenna_on(rc522_t *dev)
{
	uint8_t v;

	if (!dev)
		return RC522_ERR_INVALID;
	IO(reg_read(dev, REG_TX_CONTROL, &v));
	if ((v & 0x03) != 0x03)
		IO(reg_write(dev, REG_TX_CONTROL, (uint8_t)(v | 0x03)));
	return RC522_OK;
}

rc522_status_t rc522_antenna_off(rc522_t *dev)
{
	if (!dev)
		return RC522_ERR_INVALID;
	IO(reg_clear_bits(dev, REG_TX_CONTROL, 0x03));
	return RC522_OK;
}

rc522_status_t rc522_set_gain(rc522_t *dev, rc522_gain_t gain)
{
	uint8_t v;

	if (!dev)
		return RC522_ERR_INVALID;
	IO(reg_read(dev, REG_RF_CFG, &v));
	v = (uint8_t)((v & ~0x70) | ((uint8_t)gain & 0x70));
	IO(reg_write(dev, REG_RF_CFG, v));
	return RC522_OK;
}

rc522_status_t rc522_get_gain(rc522_t *dev, rc522_gain_t *gain)
{
	uint8_t v;

	if (!dev || !gain)
		return RC522_ERR_INVALID;
	IO(reg_read(dev, REG_RF_CFG, &v));
	v &= 0x70;
	if (v == 0x20) v = RC522_GAIN_18DB;   /* 0x20 і 0x30 дублюють 18 і 23 дБ */
	if (v == 0x30) v = RC522_GAIN_23DB;
	*gain = (rc522_gain_t)v;
	return RC522_OK;
}

rc522_status_t rc522_power_down(rc522_t *dev)
{
	if (!dev)
		return RC522_ERR_INVALID;
	IO(reg_set_bits(dev, REG_COMMAND, COMMAND_POWER_DOWN));
	return RC522_OK;
}

rc522_status_t rc522_power_up(rc522_t *dev)
{
	uint8_t cmd;
	uint64_t deadline;

	if (!dev)
		return RC522_ERR_INVALID;
	IO(reg_clear_bits(dev, REG_COMMAND, COMMAND_POWER_DOWN));

	deadline = now_ms() + 100;
	do {
		IO(reg_read(dev, REG_COMMAND, &cmd));
		if (!(cmd & COMMAND_POWER_DOWN))
			return RC522_OK;
		rc522_sleep_ms(1);
	} while (now_ms() < deadline);
	return RC522_ERR_TIMEOUT;
}

/* ---------- Виявлення карток ---------- */

static rc522_card_type_t type_from_sak(uint8_t sak)
{
	switch (sak & 0x7F) {
	case 0x09: return RC522_CARD_MIFARE_MINI;
	case 0x08: return RC522_CARD_MIFARE_1K;
	case 0x18: return RC522_CARD_MIFARE_4K;
	case 0x00: return RC522_CARD_MIFARE_UL;
	case 0x10:
	case 0x11: return RC522_CARD_MIFARE_PLUS;
	case 0x01: return RC522_CARD_TNP3XXX;
	case 0x20: return RC522_CARD_ISO_14443_4;
	case 0x40: return RC522_CARD_ISO_18092;
	default:   return RC522_CARD_UNKNOWN;
	}
}

rc522_status_t rc522_request(rc522_t *dev, int wake_halted, uint8_t atqa[2])
{
	uint8_t cmd = wake_halted ? PICC_WUPA : PICC_REQA;
	uint8_t back[2];
	uint8_t back_len = sizeof(back);
	uint8_t vb = 7;   /* короткий кадр: 7 біт */
	rc522_status_t st;

	if (!dev)
		return RC522_ERR_INVALID;

	/* Якщо лишилась MIFARE-сесія, REQA піде зашифрованим і картка не відповість */
	IO(reg_clear_bits(dev, REG_STATUS2, STATUS2_CRYPTO1_ON));
	IO(reg_write(dev, REG_TX_MODE, 0x00));
	IO(reg_write(dev, REG_RX_MODE, 0x00));
	IO(reg_write(dev, REG_MOD_WIDTH, 0x26));
	IO(reg_clear_bits(dev, REG_COLL, COLL_VALUES_AFTER_COLL));

	st = communicate(dev, CMD_TRANSCEIVE, IRQ_RX | IRQ_IDLE, &cmd, 1,
	                 back, &back_len, &vb, 0, 0);
	if (st == RC522_ERR_TIMEOUT)
		return RC522_ERR_NO_CARD;
	if (st != RC522_OK)
		return st;   /* RC522_ERR_COLLISION = кілька карток, але вони є */
	if (back_len != 2 || vb != 0)
		return RC522_ERR_PROTOCOL;

	if (atqa) {
		atqa[0] = back[0];
		atqa[1] = back[1];
	}
	return RC522_OK;
}

int rc522_card_present(rc522_t *dev)
{
	rc522_status_t st = rc522_request(dev, 0, NULL);
	return st == RC522_OK || st == RC522_ERR_COLLISION;
}

/*
 * Anticollision + select по рівнях каскаду (ISO 14443-3, розділ 6.5.3).
 * buf: [SEL][NVB][UID0..UID3 або CT,UID0..UID2][BCC][CRC0][CRC1]
 */
rc522_status_t rc522_select(rc522_t *dev, rc522_card_t *card)
{
	static const uint8_t sel_cmd[3] = { PICC_SEL_CL1, PICC_SEL_CL2, PICC_SEL_CL3 };
	uint8_t uid_pos = 0;

	if (!dev || !card)
		return RC522_ERR_INVALID;

	memset(card->uid, 0, sizeof(card->uid));
	card->uid_len = 0;
	card->sak = 0;
	card->type = RC522_CARD_UNKNOWN;

	IO(reg_clear_bits(dev, REG_COLL, COLL_VALUES_AFTER_COLL));

	for (int level = 0; level < 3; level++) {
		uint8_t buf[9];
		unsigned known_bits = 0;   /* скільки біт UID цього рівня вже відомо */
		uint8_t sak = 0;
		int selected = 0;

		memset(buf, 0, sizeof(buf));
		buf[0] = sel_cmd[level];

		/* Кожна колізія додає щонайменше 1 біт, тож 33 ітерацій точно досить */
		for (int iter = 0; iter < 33 && !selected; iter++) {
			rc522_status_t st;

			if (known_bits >= 32) {
				/* SELECT: повний UID рівня + BCC + CRC, у відповідь SAK */
				uint8_t back[3];
				uint8_t back_len = sizeof(back);
				uint8_t vb = 0;

				buf[1] = 0x70;
				buf[6] = (uint8_t)(buf[2] ^ buf[3] ^ buf[4] ^ buf[5]);
				rc522_crc_a(buf, 7, &buf[7]);

				st = communicate(dev, CMD_TRANSCEIVE, IRQ_RX | IRQ_IDLE, buf, 9,
				                 back, &back_len, &vb, 0, 1);
				if (st != RC522_OK)
					return st;
				if (back_len != 3)
					return RC522_ERR_PROTOCOL;
				sak = back[0];
				selected = 1;
			} else {
				/* ANTICOLLISION: шлемо відомі біти, картка дошле решту UID + BCC */
				uint8_t tx_last = (uint8_t)(known_bits % 8);
				uint8_t idx = (uint8_t)(2 + known_bits / 8);     /* повних байт з SEL і NVB */
				uint8_t send_len = (uint8_t)(idx + (tx_last ? 1 : 0));
				uint8_t back_len = (uint8_t)(sizeof(buf) - idx);
				uint8_t vb = tx_last;

				buf[1] = (uint8_t)((idx << 4) | tx_last);   /* NVB */

				/* Відповідь лягає поверх buf[idx], молодші біти лишаються наші */
				st = communicate(dev, CMD_TRANSCEIVE, IRQ_RX | IRQ_IDLE, buf, send_len,
				                 &buf[idx], &back_len, &vb, tx_last, 0);

				if (st == RC522_ERR_COLLISION) {
					uint8_t coll;
					unsigned pos;

					IO(reg_read(dev, REG_COLL, &coll));
					if (coll & COLL_POS_NOT_VALID)
						return RC522_ERR_COLLISION;
					pos = coll & 0x1F;
					if (pos == 0)
						pos = 32;
					if (pos <= known_bits)
						return RC522_ERR_PROTOCOL;

					/* Обираємо гілку з бітом 1 у місці колізії і йдемо далі */
					known_bits = pos;
					buf[2 + (known_bits - 1) / 8] |= (uint8_t)(1u << ((known_bits - 1) % 8));
					continue;
				}
				if (st != RC522_OK)
					return st;
				if ((uint8_t)(buf[2] ^ buf[3] ^ buf[4] ^ buf[5]) != buf[6])
					return RC522_ERR_CRC;   /* зіпсований BCC */
				known_bits = 32;
			}
		}

		if (!selected)
			return RC522_ERR_PROTOCOL;

		if (sak & 0x04) {
			/* UID не завершено: buf[2] = cascade tag, беремо 3 байти й ідемо на наступний рівень */
			if (buf[2] != PICC_CASCADE_TAG || level == 2)
				return RC522_ERR_PROTOCOL;
			memcpy(&card->uid[uid_pos], &buf[3], 3);
			uid_pos = (uint8_t)(uid_pos + 3);
		} else {
			memcpy(&card->uid[uid_pos], &buf[2], 4);
			uid_pos = (uint8_t)(uid_pos + 4);
			card->uid_len = uid_pos;
			card->sak = sak;
			card->type = type_from_sak(sak);
			return RC522_OK;
		}
	}
	return RC522_ERR_PROTOCOL;
}

rc522_status_t rc522_read_card(rc522_t *dev, rc522_card_t *card, int wake_halted)
{
	rc522_status_t st;

	if (!dev || !card)
		return RC522_ERR_INVALID;

	memset(card, 0, sizeof(*card));
	st = rc522_request(dev, wake_halted, card->atqa);
	if (st != RC522_OK && st != RC522_ERR_COLLISION)
		return st;
	return rc522_select(dev, card);
}

rc522_status_t rc522_halt(rc522_t *dev)
{
	uint8_t buf[4] = { PICC_HLTA, 0x00, 0, 0 };
	rc522_status_t st;

	if (!dev)
		return RC522_ERR_INVALID;

	rc522_crc_a(buf, 2, &buf[2]);
	st = communicate(dev, CMD_TRANSCEIVE, IRQ_RX | IRQ_IDLE, buf, sizeof(buf),
	                 NULL, NULL, NULL, 0, 0);

	IO(reg_clear_bits(dev, REG_STATUS2, STATUS2_CRYPTO1_ON));

	if (st == RC522_ERR_TIMEOUT)
		return RC522_OK;           /* мовчання = картка заснула */
	if (st == RC522_OK)
		return RC522_ERR_PROTOCOL; /* на HLTA картка не має відповідати */
	return st;
}

/* ---------- MIFARE Classic ---------- */

/* Кадр + CRC, у відповідь очікуємо 4-бітний ACK. accept_timeout: мовчання = успіх */
static rc522_status_t mifare_tx(rc522_t *dev, const uint8_t *data, uint8_t len, int accept_timeout)
{
	uint8_t buf[RC522_MF_BLOCK_SIZE + 2];
	uint8_t back[2];
	uint8_t back_len = sizeof(back);
	uint8_t vb = 0;
	rc522_status_t st;

	if (len > RC522_MF_BLOCK_SIZE)
		return RC522_ERR_INVALID;

	memcpy(buf, data, len);
	rc522_crc_a(buf, len, &buf[len]);

	st = communicate(dev, CMD_TRANSCEIVE, IRQ_RX | IRQ_IDLE, buf, (uint8_t)(len + 2),
	                 back, &back_len, &vb, 0, 0);
	if (accept_timeout && st == RC522_ERR_TIMEOUT)
		return RC522_OK;
	if (st != RC522_OK)
		return st;
	if (back_len != 1 || vb != 4)
		return RC522_ERR_PROTOCOL;
	if ((back[0] & 0x0F) != MF_ACK)
		return RC522_ERR_NACK;
	return RC522_OK;
}

rc522_status_t rc522_mifare_auth(rc522_t *dev, const rc522_card_t *card,
                                 rc522_key_type_t key_type, uint8_t block,
                                 const uint8_t key[RC522_MF_KEY_SIZE])
{
	uint8_t buf[12];
	uint8_t s2;
	rc522_status_t st;

	if (!dev || !card || !key || card->uid_len < 4)
		return RC522_ERR_INVALID;

	buf[0] = key_type == RC522_KEY_B ? PICC_MF_AUTH_B : PICC_MF_AUTH_A;
	buf[1] = block;
	memcpy(&buf[2], key, RC522_MF_KEY_SIZE);
	memcpy(&buf[8], &card->uid[card->uid_len - 4], 4);   /* останні 4 байти UID */

	st = communicate(dev, CMD_MF_AUTHENT, IRQ_IDLE, buf, sizeof(buf),
	                 NULL, NULL, NULL, 0, 0);
	if (st == RC522_ERR_TIMEOUT)
		return RC522_ERR_AUTH;
	if (st != RC522_OK)
		return st;

	IO(reg_read(dev, REG_STATUS2, &s2));
	return (s2 & STATUS2_CRYPTO1_ON) ? RC522_OK : RC522_ERR_AUTH;
}

rc522_status_t rc522_mifare_stop_crypto(rc522_t *dev)
{
	if (!dev)
		return RC522_ERR_INVALID;
	IO(reg_clear_bits(dev, REG_STATUS2, STATUS2_CRYPTO1_ON));
	return RC522_OK;
}

rc522_status_t rc522_mifare_read(rc522_t *dev, uint8_t block, uint8_t out[RC522_MF_BLOCK_SIZE])
{
	uint8_t cmd[4] = { PICC_MF_READ, block, 0, 0 };
	uint8_t back[RC522_MF_BLOCK_SIZE + 2];
	uint8_t back_len = sizeof(back);
	uint8_t vb = 0;
	rc522_status_t st;

	if (!dev || !out)
		return RC522_ERR_INVALID;

	rc522_crc_a(cmd, 2, &cmd[2]);
	st = communicate(dev, CMD_TRANSCEIVE, IRQ_RX | IRQ_IDLE, cmd, sizeof(cmd),
	                 back, &back_len, &vb, 0, 1);
	if (st != RC522_OK)
		return st;
	if (back_len != sizeof(back))
		return RC522_ERR_PROTOCOL;

	memcpy(out, back, RC522_MF_BLOCK_SIZE);
	return RC522_OK;
}

rc522_status_t rc522_mifare_write(rc522_t *dev, uint8_t block, const uint8_t data[RC522_MF_BLOCK_SIZE])
{
	uint8_t cmd[2] = { PICC_MF_WRITE, block };
	rc522_status_t st;

	if (!dev || !data)
		return RC522_ERR_INVALID;

	st = mifare_tx(dev, cmd, sizeof(cmd), 0);
	if (st != RC522_OK)
		return st;
	return mifare_tx(dev, data, RC522_MF_BLOCK_SIZE, 0);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

rc522_status_t rc522_mifare_value_set(rc522_t *dev, uint8_t block, int32_t value)
{
	uint8_t b[RC522_MF_BLOCK_SIZE];

	put_le32(&b[0], (uint32_t)value);
	put_le32(&b[4], ~(uint32_t)value);
	put_le32(&b[8], (uint32_t)value);
	b[12] = block;
	b[13] = (uint8_t)~block;
	b[14] = block;
	b[15] = (uint8_t)~block;
	return rc522_mifare_write(dev, block, b);
}

rc522_status_t rc522_mifare_value_get(rc522_t *dev, uint8_t block, int32_t *value)
{
	uint8_t b[RC522_MF_BLOCK_SIZE];
	uint32_t v;
	rc522_status_t st;

	if (!value)
		return RC522_ERR_INVALID;

	st = rc522_mifare_read(dev, block, b);
	if (st != RC522_OK)
		return st;

	v = get_le32(&b[0]);
	if (get_le32(&b[4]) != ~v || get_le32(&b[8]) != v)
		return RC522_ERR_VALUE;
	if (b[12] != b[14] || b[13] != b[15] || (uint8_t)~b[12] != b[13])
		return RC522_ERR_VALUE;

	memcpy(value, &v, sizeof(*value));
	return RC522_OK;
}

/* increment / decrement / restore: команда -> ACK, операнд -> картка мовчить */
static rc522_status_t mifare_two_step(rc522_t *dev, uint8_t cmd, uint8_t block, uint32_t operand)
{
	uint8_t c[2] = { cmd, block };
	uint8_t v[4];
	rc522_status_t st;

	if (!dev)
		return RC522_ERR_INVALID;

	st = mifare_tx(dev, c, sizeof(c), 0);
	if (st != RC522_OK)
		return st;
	put_le32(v, operand);
	return mifare_tx(dev, v, sizeof(v), 1);
}

rc522_status_t rc522_mifare_increment(rc522_t *dev, uint8_t block, uint32_t delta)
{
	return mifare_two_step(dev, PICC_MF_INCREMENT, block, delta);
}

rc522_status_t rc522_mifare_decrement(rc522_t *dev, uint8_t block, uint32_t delta)
{
	return mifare_two_step(dev, PICC_MF_DECREMENT, block, delta);
}

rc522_status_t rc522_mifare_restore(rc522_t *dev, uint8_t block)
{
	return mifare_two_step(dev, PICC_MF_RESTORE, block, 0);
}

rc522_status_t rc522_mifare_transfer(rc522_t *dev, uint8_t block)
{
	uint8_t c[2] = { PICC_MF_TRANSFER, block };

	if (!dev)
		return RC522_ERR_INVALID;
	return mifare_tx(dev, c, sizeof(c), 0);
}

rc522_status_t rc522_mifare_value_add(rc522_t *dev, uint8_t block, int32_t delta)
{
	rc522_status_t st;

	if (delta >= 0)
		st = rc522_mifare_increment(dev, block, (uint32_t)delta);
	else
		st = rc522_mifare_decrement(dev, block, 0u - (uint32_t)delta);
	if (st != RC522_OK)
		return st;
	return rc522_mifare_transfer(dev, block);
}

uint8_t rc522_mifare_sector_of(uint8_t block)
{
	return block < 128 ? (uint8_t)(block / 4) : (uint8_t)(32 + (block - 128) / 16);
}

uint8_t rc522_mifare_trailer_of(uint8_t sector)
{
	return sector < 32 ? (uint8_t)(sector * 4 + 3) : (uint8_t)(128 + (sector - 32) * 16 + 15);
}

int rc522_mifare_is_trailer(uint8_t block)
{
	return block == rc522_mifare_trailer_of(rc522_mifare_sector_of(block));
}

void rc522_mifare_make_access_bits(uint8_t out[3], uint8_t g0, uint8_t g1, uint8_t g2, uint8_t g3)
{
	uint8_t c1 = (uint8_t)(((g3 & 4) << 1) | ((g2 & 4) << 0) | ((g1 & 4) >> 1) | ((g0 & 4) >> 2));
	uint8_t c2 = (uint8_t)(((g3 & 2) << 2) | ((g2 & 2) << 1) | ((g1 & 2) << 0) | ((g0 & 2) >> 1));
	uint8_t c3 = (uint8_t)(((g3 & 1) << 3) | ((g2 & 1) << 2) | ((g1 & 1) << 1) | ((g0 & 1) << 0));

	out[0] = (uint8_t)(((~c2 & 0x0F) << 4) | (~c1 & 0x0F));
	out[1] = (uint8_t)((c1 << 4) | (~c3 & 0x0F));
	out[2] = (uint8_t)((c3 << 4) | c2);
}

void rc522_mifare_make_trailer(uint8_t out[RC522_MF_BLOCK_SIZE],
                               const uint8_t key_a[RC522_MF_KEY_SIZE],
                               const uint8_t key_b[RC522_MF_KEY_SIZE],
                               uint8_t g0, uint8_t g1, uint8_t g2, uint8_t g3)
{
	memcpy(&out[0], key_a, RC522_MF_KEY_SIZE);
	rc522_mifare_make_access_bits(&out[6], g0, g1, g2, g3);
	out[9] = 0x69;
	memcpy(&out[10], key_b, RC522_MF_KEY_SIZE);
}

/* ---------- MIFARE Ultralight / NTAG ---------- */

rc522_status_t rc522_ul_read(rc522_t *dev, uint8_t page, uint8_t out[16])
{
	return rc522_mifare_read(dev, page, out);   /* та сама команда READ 0x30 */
}

rc522_status_t rc522_ul_write(rc522_t *dev, uint8_t page, const uint8_t data[RC522_UL_PAGE_SIZE])
{
	uint8_t c[6] = { PICC_UL_WRITE, page, 0, 0, 0, 0 };

	if (!dev || !data)
		return RC522_ERR_INVALID;
	memcpy(&c[2], data, RC522_UL_PAGE_SIZE);
	return mifare_tx(dev, c, sizeof(c), 0);
}

/* ---------- Утиліти ---------- */

const char *rc522_strerror(rc522_status_t st)
{
	switch (st) {
	case RC522_OK:            return "OK";
	case RC522_ERR_IO:        return "SPI/GPIO I/O error";
	case RC522_ERR_NO_CHIP:   return "RC522 not responding (check wiring)";
	case RC522_ERR_NO_CARD:   return "no card";
	case RC522_ERR_TIMEOUT:   return "card timeout";
	case RC522_ERR_COLLISION: return "collision (several cards)";
	case RC522_ERR_CRC:       return "CRC/BCC error";
	case RC522_ERR_PROTOCOL:  return "protocol error";
	case RC522_ERR_NACK:      return "card NACK";
	case RC522_ERR_AUTH:      return "authentication failed";
	case RC522_ERR_BUFFER:    return "buffer too small";
	case RC522_ERR_INVALID:   return "invalid argument";
	case RC522_ERR_VALUE:     return "not a value block";
	}
	return "unknown error";
}

const char *rc522_card_type_name(rc522_card_type_t type)
{
	switch (type) {
	case RC522_CARD_MIFARE_MINI:  return "MIFARE Mini";
	case RC522_CARD_MIFARE_1K:    return "MIFARE Classic 1K";
	case RC522_CARD_MIFARE_4K:    return "MIFARE Classic 4K";
	case RC522_CARD_MIFARE_UL:    return "MIFARE Ultralight / NTAG";
	case RC522_CARD_MIFARE_PLUS:  return "MIFARE Plus";
	case RC522_CARD_TNP3XXX:      return "TNP3XXX";
	case RC522_CARD_ISO_14443_4:  return "ISO 14443-4";
	case RC522_CARD_ISO_18092:    return "ISO 18092 (NFC)";
	case RC522_CARD_UNKNOWN:      break;
	}
	return "unknown";
}

const char *rc522_version_name(uint8_t version)
{
	switch (version) {
	case 0x91: return "MFRC522 v1.0";
	case 0x92: return "MFRC522 v2.0";
	case 0x88: return "FM17522 (clone)";
	case 0x89: return "FM17522E (clone)";
	case 0xB2: return "FM17522_1 (clone)";
	case 0x12: return "counterfeit chip";
	default:   return "unknown";
	}
}

char *rc522_uid_to_str(const rc522_card_t *card, char *buf, size_t len)
{
	size_t pos = 0;

	if (!buf || len == 0)
		return buf;
	buf[0] = '\0';
	if (!card)
		return buf;

	for (uint8_t i = 0; i < card->uid_len && i < RC522_UID_MAX; i++) {
		int n = snprintf(buf + pos, len - pos, i ? ":%02X" : "%02X", card->uid[i]);
		if (n < 0 || (size_t)n >= len - pos)
			break;
		pos += (size_t)n;
	}
	return buf;
}

int rc522_uid_equal(const rc522_card_t *a, const rc522_card_t *b)
{
	if (!a || !b || a->uid_len != b->uid_len)
		return 0;
	return memcmp(a->uid, b->uid, a->uid_len) == 0;
}

int rc522_uid_match(const rc522_card_t *card, const uint8_t *uid, uint8_t uid_len)
{
	if (!card || !uid || card->uid_len != uid_len)
		return 0;
	return memcmp(card->uid, uid, uid_len) == 0;
}
