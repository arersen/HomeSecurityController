#ifndef RC522_H
#define RC522_H

/*
 * Драйвер MFRC522 (модуль RFID-RC522) через Linux spidev.
 *
 * Що вміє:
 *   - ініціалізація, апаратний (RST) і програмний reset, версія чипа
 *   - антена on/off, підсилення приймача, soft power-down
 *   - ISO 14443A: REQA/WUPA, anticollision + select (UID 4/7/10 байт), HLTA
 *   - MIFARE Classic (Mini/1K/4K): auth ключем A/B, read/write блоків,
 *     value-блоки (increment/decrement/restore/transfer), sector trailer
 *   - MIFARE Ultralight / NTAG2xx: read 4 сторінки, write сторінки
 *   - сирий transceive для будь-яких інших команд (NTAG GET_VERSION тощо)
 *
 * Не потокобезпечний: один rc522_t використовуйте з одного потоку.
 *
 * Безпека: UID легко клонується (magic-картки), а телефони з Apple/Google Pay
 * віддають випадковий UID при кожному піднесенні. Для реального контролю
 * доступу краще перевіряти секрет у секторі MIFARE під своїм ключем.
 *
 * Підключення до Raspberry Pi 5 (SPI0, /dev/spidev0.0):
 *   SDA(SS) -> пін 24 (GPIO8, CE0)    SCK  -> пін 23 (GPIO11)
 *   MOSI    -> пін 19 (GPIO10)        MISO -> пін 21 (GPIO9)
 *   RST     -> пін 22 (GPIO25)        IRQ  -> не підключати
 *   3.3V    -> пін 17 (НЕ 5V!)        GND  -> пін 20
 */

#include <stddef.h>
#include <stdint.h>
#include "gpio.h"

#define RC522_UID_MAX          10
#define RC522_UID_STR_LEN      (RC522_UID_MAX * 3)   /* "AA:BB:...:JJ" + '\0' */
#define RC522_MF_BLOCK_SIZE    16
#define RC522_MF_KEY_SIZE      6
#define RC522_UL_PAGE_SIZE     4
#define RC522_DEFAULT_SPI_HZ   1000000u   /* 1 МГц: надійно на бредборді, максимум чипа 10 МГц */
#define RC522_NO_RST           (-1)       /* rst_offset, якщо пін RST не підключено */

typedef enum {
	RC522_OK = 0,
	RC522_ERR_IO,          /* помилка SPI / GPIO */
	RC522_ERR_NO_CHIP,     /* чип не відповідає: перевір проводку та живлення */
	RC522_ERR_NO_CARD,     /* у полі немає картки */
	RC522_ERR_TIMEOUT,     /* картка не відповіла вчасно */
	RC522_ERR_COLLISION,   /* кілька карток одночасно */
	RC522_ERR_CRC,         /* зіпсований кадр (CRC / BCC) */
	RC522_ERR_PROTOCOL,    /* неочікувана відповідь, помилка парності тощо */
	RC522_ERR_NACK,        /* картка відхилила команду (немає доступу / неправильний блок) */
	RC522_ERR_AUTH,        /* неправильний ключ, після цього картку треба вибрати знову */
	RC522_ERR_BUFFER,      /* відповідь не влазить у буфер */
	RC522_ERR_INVALID,     /* неправильні аргументи */
	RC522_ERR_VALUE        /* блок не у форматі value-блоку */
} rc522_status_t;

typedef enum {
	RC522_CARD_UNKNOWN = 0,
	RC522_CARD_MIFARE_MINI,
	RC522_CARD_MIFARE_1K,
	RC522_CARD_MIFARE_4K,
	RC522_CARD_MIFARE_UL,      /* Ultralight, NTAG213/215/216 */
	RC522_CARD_MIFARE_PLUS,
	RC522_CARD_TNP3XXX,
	RC522_CARD_ISO_14443_4,    /* DESFire, банківські картки, телефони */
	RC522_CARD_ISO_18092       /* NFC peer-to-peer */
} rc522_card_type_t;

/* Підсилення приймача: більше = далі читає, але чутливіше до шуму */
typedef enum {
	RC522_GAIN_18DB = 0x00,
	RC522_GAIN_23DB = 0x10,
	RC522_GAIN_33DB = 0x40,    /* значення після reset */
	RC522_GAIN_38DB = 0x50,
	RC522_GAIN_43DB = 0x60,
	RC522_GAIN_48DB = 0x70
} rc522_gain_t;

typedef enum {
	RC522_KEY_A,
	RC522_KEY_B
} rc522_key_type_t;

typedef struct {
	int      fd;
	uint32_t speed_hz;
	int      has_rst;
	gpio_t   rst;
	uint8_t  version;          /* VersionReg, заповнюється в rc522_init */
} rc522_t;

typedef struct {
	uint8_t           uid[RC522_UID_MAX];
	uint8_t           uid_len;   /* 4, 7 або 10 */
	uint8_t           atqa[2];
	uint8_t           sak;
	rc522_card_type_t type;
} rc522_card_t;

/* Заводський ключ MIFARE: FF FF FF FF FF FF */
extern const uint8_t RC522_KEY_DEFAULT[RC522_MF_KEY_SIZE];

/* ---------- Життєвий цикл ---------- */

/*
 * spidev:     "/dev/spidev0.0"
 * speed_hz:   0 = RC522_DEFAULT_SPI_HZ
 * gpiochip:   "/dev/gpiochip0" (можна NULL, якщо rst_offset = RC522_NO_RST)
 * rst_offset: номер GPIO для RST або RC522_NO_RST
 * Вмикає антену. При помилці все звільняє сам.
 */
rc522_status_t rc522_init(rc522_t *dev, const char *spidev, uint32_t speed_hz,
                          const char *gpiochip, int rst_offset);
void           rc522_deinit(rc522_t *dev);   /* антена off, RST = 0, закриває spidev */
rc522_status_t rc522_soft_reset(rc522_t *dev);   /* після нього треба знову rc522_init */
rc522_status_t rc522_get_version(rc522_t *dev, uint8_t *version);

/* ---------- Антена й живлення ---------- */

rc522_status_t rc522_antenna_on(rc522_t *dev);
rc522_status_t rc522_antenna_off(rc522_t *dev);
rc522_status_t rc522_set_gain(rc522_t *dev, rc522_gain_t gain);
rc522_status_t rc522_get_gain(rc522_t *dev, rc522_gain_t *gain);
rc522_status_t rc522_power_down(rc522_t *dev);   /* soft power-down, регістри зберігаються */
rc522_status_t rc522_power_up(rc522_t *dev);

/* ---------- Виявлення карток (ISO 14443A) ---------- */

/*
 * Найпростіший шлях: REQA/WUPA + anticollision + select.
 * wake_halted = 0: лише нові картки (REQA). Картка після rc522_halt не
 *                  спрацює знову, поки її не приберуть з поля.
 * wake_halted = 1: будить і "приспані" картки (WUPA).
 * Повертає RC522_ERR_NO_CARD, якщо картки немає.
 */
rc522_status_t rc522_read_card(rc522_t *dev, rc522_card_t *card, int wake_halted);

int            rc522_card_present(rc522_t *dev);   /* 1/0, через REQA */
rc522_status_t rc522_request(rc522_t *dev, int wake_halted, uint8_t atqa[2]);   /* atqa може бути NULL */
rc522_status_t rc522_select(rc522_t *dev, rc522_card_t *card);
rc522_status_t rc522_halt(rc522_t *dev);   /* HLTA + вимикає MIFARE crypto */

/* ---------- MIFARE Classic ---------- */

/*
 * Перед read/write/value потрібна auth на будь-який блок того самого сектора.
 * Після помилки auth картка "засинає": викличте rc522_read_card(dev, &card, 1).
 */
rc522_status_t rc522_mifare_auth(rc522_t *dev, const rc522_card_t *card,
                                 rc522_key_type_t key_type, uint8_t block,
                                 const uint8_t key[RC522_MF_KEY_SIZE]);
rc522_status_t rc522_mifare_stop_crypto(rc522_t *dev);

rc522_status_t rc522_mifare_read(rc522_t *dev, uint8_t block, uint8_t out[RC522_MF_BLOCK_SIZE]);

/*
 * Увага: блок 0 (виробника) на звичайних картках не пишеться, а запис
 * неправильного sector trailer назавжди блокує сектор. Перевіряйте
 * rc522_mifare_is_trailer() перед записом.
 */
rc522_status_t rc522_mifare_write(rc522_t *dev, uint8_t block, const uint8_t data[RC522_MF_BLOCK_SIZE]);

/* Value-блоки: value, ~value, value, addr, ~addr, addr, ~addr */
rc522_status_t rc522_mifare_value_set(rc522_t *dev, uint8_t block, int32_t value);
rc522_status_t rc522_mifare_value_get(rc522_t *dev, uint8_t block, int32_t *value);

/* Пишуть у внутрішній буфер картки; на блок потрапляє лише після transfer */
rc522_status_t rc522_mifare_increment(rc522_t *dev, uint8_t block, uint32_t delta);
rc522_status_t rc522_mifare_decrement(rc522_t *dev, uint8_t block, uint32_t delta);
rc522_status_t rc522_mifare_restore(rc522_t *dev, uint8_t block);
rc522_status_t rc522_mifare_transfer(rc522_t *dev, uint8_t block);
/* increment/decrement + transfer в той самий блок */
rc522_status_t rc522_mifare_value_add(rc522_t *dev, uint8_t block, int32_t delta);

/* Номер сектора / блок трейлера, з урахуванням 4K (сектори 32..39 по 16 блоків) */
uint8_t rc522_mifare_sector_of(uint8_t block);
uint8_t rc522_mifare_trailer_of(uint8_t sector);
int     rc522_mifare_is_trailer(uint8_t block);

/*
 * Біти доступу (байти 6..8 трейлера). g0..g2 це блоки даних, g3 це трейлер,
 * кожен у форматі 0bC1C2C3. Заводські: g0=g1=g2=0, g3=1 -> FF 07 80.
 */
void rc522_mifare_make_access_bits(uint8_t out[3], uint8_t g0, uint8_t g1, uint8_t g2, uint8_t g3);
/* Повний трейлер: key_a | access bits | 0x69 | key_b */
void rc522_mifare_make_trailer(uint8_t out[RC522_MF_BLOCK_SIZE],
                               const uint8_t key_a[RC522_MF_KEY_SIZE],
                               const uint8_t key_b[RC522_MF_KEY_SIZE],
                               uint8_t g0, uint8_t g1, uint8_t g2, uint8_t g3);

/* ---------- MIFARE Ultralight / NTAG ---------- */

rc522_status_t rc522_ul_read(rc522_t *dev, uint8_t page, uint8_t out[16]);   /* 4 сторінки підряд */
rc522_status_t rc522_ul_write(rc522_t *dev, uint8_t page, const uint8_t data[RC522_UL_PAGE_SIZE]);

/* ---------- Низький рівень ---------- */

/*
 * Довільний кадр до картки.
 * back:       може бути NULL, якщо відповідь не потрібна
 * back_len:   на вході розмір back, на виході скільки прийнято (без CRC при with_crc)
 * valid_bits: на вході скільки біт останнього байта передавати (0 = всі),
 *             на виході скільки біт останнього прийнятого байта валідні. Може бути NULL.
 * with_crc:   додати CRC_A до кадру, перевірити та відрізати CRC у відповіді
 */
rc522_status_t rc522_transceive(rc522_t *dev, const uint8_t *send, uint8_t send_len,
                                uint8_t *back, uint8_t *back_len, uint8_t *valid_bits,
                                int with_crc);
void           rc522_crc_a(const uint8_t *data, size_t len, uint8_t out[2]);
rc522_status_t rc522_read_reg(rc522_t *dev, uint8_t reg, uint8_t *val);
rc522_status_t rc522_write_reg(rc522_t *dev, uint8_t reg, uint8_t val);

/* ---------- Утиліти ---------- */

const char *rc522_strerror(rc522_status_t st);
const char *rc522_card_type_name(rc522_card_type_t type);
const char *rc522_version_name(uint8_t version);
char       *rc522_uid_to_str(const rc522_card_t *card, char *buf, size_t len);
int         rc522_uid_equal(const rc522_card_t *a, const rc522_card_t *b);
int         rc522_uid_match(const rc522_card_t *card, const uint8_t *uid, uint8_t uid_len);

#endif
