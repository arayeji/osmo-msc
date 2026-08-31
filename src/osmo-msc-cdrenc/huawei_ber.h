/* Minimal BER writer for Huawei CallEventDataFile (same tags OFCS decodes). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

struct ber_buf {
	uint8_t *data;
	size_t len;
	size_t cap;
};

void ber_init(struct ber_buf *b);
void ber_free(struct ber_buf *b);
void ber_reset(struct ber_buf *b);

void ber_tag(struct ber_buf *b, int tag_class, bool constructed, int tag);
void ber_length(struct ber_buf *b, size_t len);
void ber_raw(struct ber_buf *b, const uint8_t *data, size_t n);

void ber_ctx_prim(struct ber_buf *b, int tag, const uint8_t *data, size_t n);
void ber_ctx_int(struct ber_buf *b, int tag, int value);
void ber_wrap_ctx(struct ber_buf *outer, int tag, const struct ber_buf *inner);

size_t ber_tbcd_digits(uint8_t *out, size_t outl, const char *digits);
size_t ber_tbcd_number(uint8_t *out, size_t outl, const char *digits, uint8_t ton_npi);
size_t ber_timestamp(uint8_t *out, size_t outl, const struct tm *utc, const char *tz_hex);
size_t ber_hex_bytes(uint8_t *out, size_t outl, const char *hex);
void ber_ctx_tbcd(struct ber_buf *b, int tag, const char *digits);
void ber_ctx_isdn(struct ber_buf *b, int tag, const char *digits);
void ber_ctx_time_csv(struct ber_buf *b, int tag, const char *yyyymmddhhmmss,
		      const char *tz_hex);
void ber_ctx_callref(struct ber_buf *b, int tag, const char *hex_or_num);
void ber_ctx_ascii(struct ber_buf *b, int tag, const char *text);
bool ber_parse_csv_time(const char *s, struct tm *out);
