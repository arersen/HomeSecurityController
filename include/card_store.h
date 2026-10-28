//
// Created by jambyl on 10/28/26.
//
#ifndef HOMESECURITYCONTROLLER_CARD_STORE_H
#define HOMESECURITYCONTROLLER_CARD_STORE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Перелік дозволених карток (allowlist) із збереженням у файл.
 *
 * Формат файлу — текстовий, по рядку на картку:
 *     04:52:8A:B2:1C:3D:80   Arsen
 *     25:58:9F:E0            Guest card
 * Порожні рядки й рядки, що починаються з '#', ігноруються.
 *
 * Збереження атомарне (tmp + rename) з правами 0600, тож або старий файл,
 * або повністю новий — обірваного стану не буває.
 *
 * НЕ потокобезпечний.
 */

#define CARD_UID_MAX    10
#define CARD_NAME_MAX   48    /* включно з '\0' */
#define CARD_UID_STRLEN (CARD_UID_MAX * 3)   /* "AA:...:JJ" + '\0' */

typedef struct {
	uint8_t uid[CARD_UID_MAX];
	uint8_t uid_len;                 /* 4, 7 або 10 */
	char    name[CARD_NAME_MAX];
} card_entry_t;

typedef struct {
	card_entry_t *items;
	size_t        count;
	size_t        cap;
} card_store_t;

void   card_store_init(card_store_t *s);
void   card_store_free(card_store_t *s);

/*
 * Завантажити з файлу (перед цим вміст очищується).
 * Відсутній файл — не помилка: стор лишається порожнім, повертає 0.
 * Повертає 0 при успіху, -1 при помилці вводу/виводу чи пам'яті.
 * Биті рядки пропускаються з попередженням у stderr.
 */
int    card_store_load(card_store_t *s, const char *path);

/* Атомарний запис усього стору у файл. 0 / -1. */
int    card_store_save(const card_store_t *s, const char *path);

/* Ім'я власника картки або NULL, якщо UID не в списку. */
const char *card_store_find(const card_store_t *s, const uint8_t *uid, uint8_t uid_len);

/*
 * Додати картку. Повертає 0 (додано), 1 (такий UID уже є — ім'я оновлено),
 * -1 (помилка аргументів чи пам'яті). Файл НЕ пишеться — викличте save окремо.
 */
int    card_store_add(card_store_t *s, const uint8_t *uid, uint8_t uid_len, const char *name);

/* Прибрати картку. 0 (прибрано), 1 (не знайдено). Файл НЕ пишеться. */
int    card_store_remove(card_store_t *s, const uint8_t *uid, uint8_t uid_len);

/* ---- Хелпери UID ---- */

/* "AA:BB:CC:DD" або "AABBCCDD" -> байти. 0 при успіху, -1 при битому форматі. */
int    card_uid_parse(const char *str, uint8_t *uid, uint8_t *uid_len);

/* Байти -> "AA:BB:CC:DD". Повертає buf. */
char  *card_uid_format(const uint8_t *uid, uint8_t uid_len, char *buf, size_t len);

#endif //HOMESECURITYCONTROLLER_CARD_STORE_H
