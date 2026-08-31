#include "huawei_ber.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void ber_grow(struct ber_buf *b, size_t add)
{
	size_t need = b->len + add;
	size_t cap = b->cap ? b->cap : 256;
	uint8_t *p;

	if (need <= b->cap)
		return;
	while (cap < need)
		cap *= 2;
	p = realloc(b->data, cap);
	if (!p)
		abort();
	b->data = p;
	b->cap = cap;
}

void ber_init(struct ber_buf *b)
{
	memset(b, 0, sizeof(*b));
}

void ber_free(struct ber_buf *b)
{
	free(b->data);
	memset(b, 0, sizeof(*b));
}

void ber_reset(struct ber_buf *b)
{
	b->len = 0;
}

void ber_raw(struct ber_buf *b, const uint8_t *data, size_t n)
{
	if (!data || !n)
		return;
	ber_grow(b, n);
	memcpy(b->data + b->len, data, n);
	b->len += n;
}

void ber_tag(struct ber_buf *b, int tag_class, bool constructed, int tag)
{
	uint8_t first = (uint8_t)((tag_class << 6) | (constructed ? 0x20 : 0));
	uint8_t stack[8];
	int n = 0;

	if (tag < 31) {
		uint8_t v = (uint8_t)(first | tag);
		ber_raw(b, &v, 1);
		return;
	}
	first |= 0x1f;
	ber_raw(b, &first, 1);
	stack[n++] = (uint8_t)(tag & 0x7f);
	tag >>= 7;
	while (tag > 0) {
		stack[n++] = (uint8_t)((tag & 0x7f) | 0x80);
		tag >>= 7;
	}
	while (n > 0) {
		n--;
		ber_raw(b, &stack[n], 1);
	}
}

void ber_length(struct ber_buf *b, size_t len)
{
	if (len < 128) {
		uint8_t v = (uint8_t)len;
		ber_raw(b, &v, 1);
		return;
	}
	uint8_t tmp[8];
	int n = 0;
	size_t t = len;
	while (t) {
		tmp[n++] = (uint8_t)(t & 0xff);
		t >>= 8;
	}
	uint8_t hdr = (uint8_t)(0x80 | n);
	ber_raw(b, &hdr, 1);
	while (n > 0) {
		n--;
		ber_raw(b, &tmp[n], 1);
	}
}

void ber_ctx_prim(struct ber_buf *b, int tag, const uint8_t *data, size_t n)
{
	if (!data || !n)
		return;
	ber_tag(b, 2, false, tag);
	ber_length(b, n);
	ber_raw(b, data, n);
}

static size_t encode_int(uint8_t *out, int value)
{
	uint8_t tmp[5];
	int n = 0;
	unsigned int u = (unsigned int)value;

	if (value == 0) {
		out[0] = 0;
		return 1;
	}
	while (u) {
		tmp[n++] = (uint8_t)(u & 0xff);
		u >>= 8;
	}
	if (tmp[n - 1] & 0x80)
		tmp[n++] = 0;
	for (int i = 0; i < n; i++)
		out[i] = tmp[n - 1 - i];
	return (size_t)n;
}

void ber_ctx_int(struct ber_buf *b, int tag, int value)
{
	uint8_t tmp[8];
	size_t n = encode_int(tmp, value);
	ber_ctx_prim(b, tag, tmp, n);
}

void ber_wrap_ctx(struct ber_buf *outer, int tag, const struct ber_buf *inner)
{
	if (!inner->len)
		return;
	ber_tag(outer, 2, true, tag);
	ber_length(outer, inner->len);
	ber_raw(outer, inner->data, inner->len);
}

static int nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return 15;
}

size_t ber_tbcd_digits(uint8_t *out, size_t outl, const char *digits)
{
	char hex[64];
	size_t n = 0;

	if (!digits || !digits[0])
		return 0;
	for (const char *p = digits; *p && n + 1 < sizeof(hex); p++) {
		if (*p >= '0' && *p <= '9')
			hex[n++] = *p;
		else if (*p == '*')
			hex[n++] = 'a';
		else if (*p == '#')
			hex[n++] = 'b';
	}
	if (n % 2)
		hex[n++] = 'f';
	if (n / 2 > outl)
		return 0;
	for (size_t i = 0; i < n / 2; i++) {
		int a = nibble(hex[i * 2]);
		int b = nibble(hex[i * 2 + 1]);
		out[i] = (uint8_t)((b << 4) | a);
	}
	return n / 2;
}

size_t ber_tbcd_number(uint8_t *out, size_t outl, const char *digits, uint8_t ton_npi)
{
	uint8_t tmp[32];
	size_t n = ber_tbcd_digits(tmp, sizeof(tmp), digits);

	if (!n || n + 1 > outl)
		return 0;
	out[0] = ton_npi;
	memcpy(out + 1, tmp, n);
	return n + 1;
}

size_t ber_hex_bytes(uint8_t *out, size_t outl, const char *hex)
{
	char buf[128];
	size_t n;

	if (!hex || !hex[0])
		return 0;
	n = strlen(hex);
	if (n % 2) {
		if (n + 2 > sizeof(buf))
			return 0;
		buf[0] = '0';
		memcpy(buf + 1, hex, n + 1);
		hex = buf;
		n++;
	}
	if (n / 2 > outl)
		return 0;
	for (size_t i = 0; i < n / 2; i++)
		out[i] = (uint8_t)((nibble(hex[i * 2]) << 4) | nibble(hex[i * 2 + 1]));
	return n / 2;
}

bool ber_parse_csv_time(const char *s, struct tm *out)
{
	int y, mo, d, h, mi, se;

	if (!s || strlen(s) < 14 || !out)
		return false;
	if (sscanf(s, "%4d%2d%2d%2d%2d%2d", &y, &mo, &d, &h, &mi, &se) != 6)
		return false;
	memset(out, 0, sizeof(*out));
	out->tm_year = y - 1900;
	out->tm_mon = mo - 1;
	out->tm_mday = d;
	out->tm_hour = h;
	out->tm_min = mi;
	out->tm_sec = se;
	return true;
}

size_t ber_timestamp(uint8_t *out, size_t outl, const struct tm *utc, const char *tz_hex)
{
	char hex[32];

	if (!utc)
		return 0;
	snprintf(hex, sizeof(hex), "%02d%02d%02d%02d%02d%02d%s",
		 (utc->tm_year + 1900) % 100, utc->tm_mon + 1, utc->tm_mday,
		 utc->tm_hour, utc->tm_min, utc->tm_sec,
		 tz_hex && tz_hex[0] ? tz_hex : "2b0000");
	return ber_hex_bytes(out, outl, hex);
}

void ber_ctx_tbcd(struct ber_buf *b, int tag, const char *digits)
{
	uint8_t tmp[32];
	size_t n = ber_tbcd_digits(tmp, sizeof(tmp), digits);
	ber_ctx_prim(b, tag, tmp, n);
}

void ber_ctx_isdn(struct ber_buf *b, int tag, const char *digits)
{
	uint8_t tmp[33];
	size_t n = ber_tbcd_number(tmp, sizeof(tmp), digits, 0x91);
	ber_ctx_prim(b, tag, tmp, n);
}

void ber_ctx_time_csv(struct ber_buf *b, int tag, const char *yyyymmddhhmmss,
		      const char *tz_hex)
{
	struct tm tm;
	uint8_t tmp[16];
	size_t n;

	if (!ber_parse_csv_time(yyyymmddhhmmss, &tm))
		return;
	n = ber_timestamp(tmp, sizeof(tmp), &tm, tz_hex);
	ber_ctx_prim(b, tag, tmp, n);
}

void ber_ctx_callref(struct ber_buf *b, int tag, const char *hex_or_num)
{
	const char *s = hex_or_num;
	char hex[32];
	unsigned long v = 0;
	uint8_t tmp[16];
	size_t n;
	char *end = NULL;

	if (!s || !s[0])
		return;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	v = strtoul(s, &end, 16);
	if (end == s || *end)
		v = strtoul(s, &end, 10);
	if (end == s)
		return;
	snprintf(hex, sizeof(hex), "%lx", v);
	n = ber_hex_bytes(tmp, sizeof(tmp), hex);
	ber_ctx_prim(b, tag, tmp, n);
}

void ber_ctx_ascii(struct ber_buf *b, int tag, const char *text)
{
	if (!text || !text[0])
		return;
	ber_ctx_prim(b, tag, (const uint8_t *)text, strlen(text));
}
