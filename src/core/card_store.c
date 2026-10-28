//
// Created by jambyl on 10/28/26.
//
#include "card_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* ---------- Хелпери UID ---------- */

static int hex_nibble(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

int card_uid_parse(const char *str, uint8_t *uid, uint8_t *uid_len)
{
	size_t n = 0;

	if (!str || !uid || !uid_len)
		return -1;

	while (*str && n < CARD_UID_MAX) {
		int hi, lo;

		while (*str == ':' || *str == ' ' || *str == '\t')
			str++;
		if (!*str)
			break;

		hi = hex_nibble((unsigned char)*str++);
		lo = (*str) ? hex_nibble((unsigned char)*str++) : -1;
		if (hi < 0 || lo < 0)
			return -1;
		uid[n++] = (uint8_t)((hi << 4) | lo);
	}

	/* не лишилось зайвих значущих символів */
	while (*str == ':' || *str == ' ' || *str == '\t')
		str++;
	if (*str != '\0')
		return -1;
	if (n != 4 && n != 7 && n != 10)
		return -1;

	*uid_len = (uint8_t)n;
	return 0;
}

char *card_uid_format(const uint8_t *uid, uint8_t uid_len, char *buf, size_t len)
{
	size_t pos = 0;

	if (!buf || len == 0)
		return buf;
	buf[0] = '\0';
	if (!uid)
		return buf;

	for (uint8_t i = 0; i < uid_len && i < CARD_UID_MAX; i++) {
		int w = snprintf(buf + pos, len - pos, i ? ":%02X" : "%02X", uid[i]);
		if (w < 0 || (size_t)w >= len - pos)
			break;
		pos += (size_t)w;
	}
	return buf;
}

/* ---------- Внутрішнє ---------- */

static int uid_eq(const card_entry_t *e, const uint8_t *uid, uint8_t len)
{
	return e->uid_len == len && memcmp(e->uid, uid, len) == 0;
}

static card_entry_t *find_mut(card_store_t *s, const uint8_t *uid, uint8_t len)
{
	for (size_t i = 0; i < s->count; i++)
		if (uid_eq(&s->items[i], uid, len))
			return &s->items[i];
	return NULL;
}

static int ensure_cap(card_store_t *s, size_t need)
{
	size_t cap = s->cap ? s->cap : 8;
	card_entry_t *p;

	if (need <= s->cap)
		return 0;
	while (cap < need)
		cap *= 2;
	p = realloc(s->items, cap * sizeof(*p));
	if (!p)
		return -1;
	s->items = p;
	s->cap = cap;
	return 0;
}

static void sanitize_name(const char *in, char *out)
{
	size_t j = 0;

	if (!in)
		in = "";
	for (size_t i = 0; in[i] && j < CARD_NAME_MAX - 1; i++) {
		unsigned char c = (unsigned char)in[i];
		out[j++] = (c < 0x20) ? ' ' : (char)c;   /* без керівних символів */
	}
	out[j] = '\0';
	/* обрізати хвостові пробіли */
	while (j > 0 && out[j - 1] == ' ')
		out[--j] = '\0';
}

/* ---------- API ---------- */

void card_store_init(card_store_t *s)
{
	s->items = NULL;
	s->count = 0;
	s->cap = 0;
}

void card_store_free(card_store_t *s)
{
	free(s->items);
	s->items = NULL;
	s->count = 0;
	s->cap = 0;
}

const char *card_store_find(const card_store_t *s, const uint8_t *uid, uint8_t uid_len)
{
	if (!s || !uid)
		return NULL;
	for (size_t i = 0; i < s->count; i++)
		if (uid_eq(&s->items[i], uid, uid_len))
			return s->items[i].name;
	return NULL;
}

int card_store_add(card_store_t *s, const uint8_t *uid, uint8_t uid_len, const char *name)
{
	card_entry_t *e;

	if (!s || !uid || (uid_len != 4 && uid_len != 7 && uid_len != 10))
		return -1;

	e = find_mut(s, uid, uid_len);
	if (e) {
		sanitize_name(name, e->name);   /* оновлюємо ім'я */
		return 1;
	}
	if (ensure_cap(s, s->count + 1) != 0)
		return -1;

	e = &s->items[s->count++];
	memset(e, 0, sizeof(*e));
	memcpy(e->uid, uid, uid_len);
	e->uid_len = uid_len;
	sanitize_name(name, e->name);
	return 0;
}

int card_store_remove(card_store_t *s, const uint8_t *uid, uint8_t uid_len)
{
	if (!s || !uid)
		return 1;
	for (size_t i = 0; i < s->count; i++) {
		if (uid_eq(&s->items[i], uid, uid_len)) {
			s->items[i] = s->items[s->count - 1];   /* swap-remove, порядок не важливий */
			s->count--;
			return 0;
		}
	}
	return 1;
}

int card_store_load(card_store_t *s, const char *path)
{
	FILE *f;
	char *line = NULL;
	size_t cap = 0;
	ssize_t len;
	long lineno = 0;

	if (!s || !path)
		return -1;

	s->count = 0;   /* повне перезавантаження */

	f = fopen(path, "re");
	if (!f)
		return (errno == ENOENT) ? 0 : -1;   /* немає файлу — порожній список */

	while ((len = getline(&line, &cap, f)) != -1) {
		char *p = line, *uid_tok, *name;
		uint8_t uid[CARD_UID_MAX], uid_len;

		lineno++;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0' || *p == '\n' || *p == '#')
			continue;

		/* перший токен — UID, решта рядка — ім'я */
		uid_tok = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '\n')
			p++;
		if (*p)
			*p++ = '\0';

		while (*p == ' ' || *p == '\t')
			p++;
		name = p;
		p = name + strlen(name);
		while (p > name && (p[-1] == '\n' || p[-1] == '\r' || p[-1] == ' ' || p[-1] == '\t'))
			*--p = '\0';

		if (card_uid_parse(uid_tok, uid, &uid_len) != 0) {
			fprintf(stderr, "[card_store] %s:%ld: некоректний UID '%s', пропускаю\n",
			        path, lineno, uid_tok);
			continue;
		}
		if (card_store_add(s, uid, uid_len, name) < 0) {
			free(line);
			fclose(f);
			return -1;
		}
	}

	free(line);
	fclose(f);
	return 0;
}

int card_store_save(const card_store_t *s, const char *path)
{
	char tmp[1024];
	FILE *f;
	int fd, rc = -1;

	if (!s || !path)
		return -1;
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return -1;

	f = fopen(tmp, "we");
	if (!f)
		return -1;

	/* права 0600 ще до запису вмісту */
	fd = fileno(f);
	if (fd >= 0)
		(void)fchmod(fd, 0600);

	if (fprintf(f, "# HomeSecurityController authorized cards\n"
	               "# <uid hex>  <name>\n") < 0)
		goto out;

	for (size_t i = 0; i < s->count; i++) {
		char ustr[CARD_UID_STRLEN];
		card_uid_format(s->items[i].uid, s->items[i].uid_len, ustr, sizeof(ustr));
		if (fprintf(f, "%-23s %s\n", ustr, s->items[i].name) < 0)
			goto out;
	}

	if (fflush(f) != 0)
		goto out;
	if (fd >= 0)
		(void)fsync(fd);   /* дані на диск до rename */
	rc = 0;

out:
	if (fclose(f) != 0)
		rc = -1;
	if (rc == 0) {
		if (rename(tmp, path) != 0)   /* атомарна підміна */
			rc = -1;
	}
	if (rc != 0)
		unlink(tmp);
	return rc;
}
