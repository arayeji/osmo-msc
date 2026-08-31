/* osmo-msc-cdrenc: sidecar that turns rotated OsmoMSC CSV into Huawei bA*.dat.
 *
 * Separate process from osmo-msc. Does not insert into SQL (that is OFCS).
 */

/* (C) 2026 by Osmocom contributors
 * SPDX-License-Identifier: AGPL-3.0+
 */

#define _GNU_SOURCE

#include "huawei_ber.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_COLS 80
#define MAX_ROUTES 32
#define LINE_MAX_LEN 4096

struct route {
	char prefix[32];
	char name[32];
};

struct cfg {
	char csv_dir[PATH_MAX];
	char csv_pattern[128];
	char output_dir[PATH_MAX];
	char done_dir[PATH_MAX];
	char entity[32];
	char file_prefix[16];
	char timezone[16];
	int poll_seconds;
	int seq;
	struct route routes[MAX_ROUTES];
	int n_routes;
};

struct row {
	char *key[MAX_COLS];
	char *val[MAX_COLS];
	int n;
};

static void cfg_defaults(struct cfg *c)
{
	memset(c, 0, sizeof(*c));
	snprintf(c->csv_dir, sizeof(c->csv_dir), "/var/log");
	snprintf(c->csv_pattern, sizeof(c->csv_pattern), "osmo-msc.cdr.*");
	snprintf(c->output_dir, sizeof(c->output_dir), "/var/lib/osmocom/huawei-cdr");
	snprintf(c->done_dir, sizeof(c->done_dir), "/var/log/osmo-msc-cdr-done");
	snprintf(c->entity, sizeof(c->entity), "0000");
	snprintf(c->file_prefix, sizeof(c->file_prefix), "bA");
	snprintf(c->timezone, sizeof(c->timezone), "2b0000");
	c->poll_seconds = 10;
	c->seq = 1;
}

static const char *row_get(const struct row *r, const char *key)
{
	int i;

	for (i = 0; i < r->n; i++) {
		if (strcasecmp(r->key[i], key) == 0)
			return r->val[i] ? r->val[i] : "";
	}
	return "";
}

static int row_int(const struct row *r, const char *key, int def)
{
	const char *s = row_get(r, key);
	char *end;
	long v;

	if (!s[0])
		return def;
	v = strtol(s, &end, 10);
	return end == s ? def : (int)v;
}

static void row_free(struct row *r)
{
	int i;

	for (i = 0; i < r->n; i++) {
		free(r->key[i]);
		free(r->val[i]);
	}
	memset(r, 0, sizeof(*r));
}

static int split_csv(char *line, char **out, int max)
{
	int n = 0;
	char *p = line;

	while (n < max) {
		out[n++] = p;
		p = strchr(p, ',');
		if (!p)
			break;
		*p++ = '\0';
	}
	return n;
}

static char *xstrdup(const char *s)
{
	char *p = strdup(s ? s : "");
	if (!p)
		abort();
	return p;
}

static int mkdir_p(const char *path)
{
	char tmp[PATH_MAX];
	size_t len;
	size_t i;

	if (!path || !path[0])
		return -1;
	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp);
	if (len && tmp[len - 1] == '/')
		tmp[len - 1] = '\0';
	for (i = 1; tmp[i]; i++) {
		if (tmp[i] != '/')
			continue;
		tmp[i] = '\0';
		if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
			return -1;
		tmp[i] = '/';
	}
	if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

static const char *route_for(const struct cfg *c, const char *number,
			     const char *fallback)
{
	int best = -1;
	int i;
	const char *name = "";

	if (!number)
		number = "";
	for (i = 0; i < c->n_routes; i++) {
		size_t pl = strlen(c->routes[i].prefix);
		if (strncmp(number, c->routes[i].prefix, pl) == 0 && (int)pl > best) {
			best = (int)pl;
			name = c->routes[i].name;
		}
	}
	if (best >= 0 && name[0])
		return name;
	return fallback ? fallback : "";
}

static void digit_entity(const struct cfg *c, const struct row *r, char *out, size_t outl)
{
	const char *src;
	size_t n = 0;

	if (c->entity[0] && strspn(c->entity, "0123456789") == strlen(c->entity)) {
		snprintf(out, outl, "%s", c->entity);
		return;
	}
	src = row_get(r, "recording_entity");
	for (; *src && n + 1 < outl; src++) {
		if (isdigit((unsigned char)*src))
			out[n++] = *src;
	}
	out[n] = '\0';
	if (!n)
		snprintf(out, outl, "0000");
}

static void write_location(struct ber_buf *w, int tag, const struct row *r)
{
	int lac = row_int(r, "lac", 0);
	int ci = row_int(r, "ci", 0);
	char hex[8];
	uint8_t b[4];
	size_t n;
	struct ber_buf inner;

	if (!lac && !ci)
		return;
	ber_init(&inner);
	snprintf(hex, sizeof(hex), "%04x", lac & 0xffff);
	n = ber_hex_bytes(b, sizeof(b), hex);
	ber_ctx_prim(&inner, 0, b, n);
	snprintf(hex, sizeof(hex), "%04x", ci & 0xffff);
	n = ber_hex_bytes(b, sizeof(b), hex);
	ber_ctx_prim(&inner, 1, b, n);
	ber_wrap_ctx(w, tag, &inner);
	ber_free(&inner);
}

static void write_lu_location(struct ber_buf *w, int tag, const struct row *r)
{
	int lac = row_int(r, "lac", 0);
	int ci = row_int(r, "ci", 0);
	char hex[8];
	uint8_t b[4];
	size_t n;
	struct ber_buf inner;

	if (!lac && !ci)
		return;
	ber_init(&inner);
	snprintf(hex, sizeof(hex), "%04x", lac & 0xffff);
	n = ber_hex_bytes(b, sizeof(b), hex);
	ber_ctx_prim(&inner, 2, b, n);
	snprintf(hex, sizeof(hex), "%04x", ci & 0xffff);
	n = ber_hex_bytes(b, sizeof(b), hex);
	ber_ctx_prim(&inner, 3, b, n);
	ber_wrap_ctx(w, tag, &inner);
	ber_free(&inner);
}

static void write_route(struct ber_buf *w, int tag, const char *name)
{
	struct ber_buf inner;

	if (!name || !name[0])
		return;
	ber_init(&inner);
	ber_ctx_ascii(&inner, 1, name);
	ber_wrap_ctx(w, tag, &inner);
	ber_free(&inner);
}

static void fill_record(struct ber_buf *w, const struct cfg *c, const struct row *r, int type)
{
	char entity[32];
	const char *incoming = row_get(r, "incoming_route");
	const char *outgoing = route_for(c, row_get(r, "called_number"),
					 row_get(r, "outgoing_route"));
	const char *tz = c->timezone;

	ber_ctx_int(w, 0, type);
	digit_entity(c, r, entity, sizeof(entity));

	switch (type) {
	case 0:
		ber_ctx_tbcd(w, 1, row_get(r, "served_imsi"));
		ber_ctx_tbcd(w, 2, row_get(r, "served_imei"));
		ber_ctx_isdn(w, 3, row_get(r, "served_msisdn"));
		ber_ctx_isdn(w, 4, row_get(r, "calling_number"));
		ber_ctx_isdn(w, 5, row_get(r, "called_number"));
		ber_ctx_isdn(w, 6, row_get(r, "translated_number"));
		ber_ctx_isdn(w, 7, row_get(r, "connected_number"));
		ber_ctx_isdn(w, 9, entity);
		write_route(w, 10, incoming);
		write_route(w, 11, outgoing);
		write_location(w, 12, r);
		ber_ctx_time_csv(w, 22, row_get(r, "seizure_time"), tz);
		ber_ctx_time_csv(w, 23, row_get(r, "answer_time"), tz);
		ber_ctx_time_csv(w, 24, row_get(r, "release_time"), tz);
		ber_ctx_int(w, 25, row_int(r, "call_duration", 0));
		ber_ctx_int(w, 30, row_int(r, "cause_for_term", 0));
		ber_ctx_callref(w, 32, row_get(r, "call_reference"));
		ber_ctx_callref(w, 38, row_get(r, "network_call_reference"));
		ber_ctx_isdn(w, 39, entity);
		ber_ctx_int(w, 61, row_int(r, "system_type", 0));
		ber_ctx_ascii(w, 188, row_get(r, "global_area_id"));
		ber_ctx_tbcd(w, 192, row_get(r, "first_mccmnc"));
		ber_ctx_tbcd(w, 194, row_get(r, "last_mccmnc"));
		ber_ctx_time_csv(w, 201, row_get(r, "setup_time"), tz);
		ber_ctx_time_csv(w, 202, row_get(r, "alerting_time"), tz);
		ber_ctx_int(w, 232, row_int(r, "record_number", 0));
		break;
	case 1:
		ber_ctx_tbcd(w, 1, row_get(r, "served_imsi"));
		ber_ctx_tbcd(w, 2, row_get(r, "served_imei"));
		ber_ctx_isdn(w, 3, row_get(r, "served_msisdn"));
		ber_ctx_isdn(w, 4, row_get(r, "calling_number"));
		ber_ctx_isdn(w, 5, row_get(r, "connected_number"));
		ber_ctx_isdn(w, 6, entity);
		write_route(w, 7, incoming);
		write_route(w, 8, outgoing);
		write_location(w, 9, r);
		ber_ctx_time_csv(w, 19, row_get(r, "seizure_time"), tz);
		ber_ctx_time_csv(w, 20, row_get(r, "answer_time"), tz);
		ber_ctx_time_csv(w, 21, row_get(r, "release_time"), tz);
		ber_ctx_int(w, 22, row_int(r, "call_duration", 0));
		ber_ctx_int(w, 27, row_int(r, "cause_for_term", 0));
		ber_ctx_callref(w, 29, row_get(r, "call_reference"));
		ber_ctx_callref(w, 33, row_get(r, "network_call_reference"));
		ber_ctx_isdn(w, 34, entity);
		ber_ctx_int(w, 46, row_int(r, "system_type", 0));
		ber_ctx_ascii(w, 188, row_get(r, "global_area_id"));
		ber_ctx_tbcd(w, 192, row_get(r, "first_mccmnc"));
		ber_ctx_tbcd(w, 194, row_get(r, "last_mccmnc"));
		ber_ctx_time_csv(w, 203, row_get(r, "setup_time"), tz);
		ber_ctx_time_csv(w, 204, row_get(r, "alerting_time"), tz);
		ber_ctx_isdn(w, 205, row_get(r, "called_number"));
		ber_ctx_int(w, 232, row_int(r, "record_number", 0));
		break;
	case 6:
		ber_ctx_tbcd(w, 1, row_get(r, "served_imsi"));
		ber_ctx_tbcd(w, 2, row_get(r, "served_imei"));
		ber_ctx_isdn(w, 3, row_get(r, "served_msisdn"));
		ber_ctx_isdn(w, 5, row_get(r, "service_centre"));
		ber_ctx_isdn(w, 6, entity);
		write_location(w, 7, r);
		ber_ctx_time_csv(w, 9, row_get(r, "origination_time"), tz);
		ber_ctx_isdn(w, 12, row_get(r, "destination_number"));
		ber_ctx_int(w, 14, row_int(r, "system_type", 0));
		ber_ctx_ascii(w, 188, row_get(r, "global_area_id"));
		ber_ctx_tbcd(w, 192, row_get(r, "first_mccmnc"));
		ber_ctx_callref(w, 201, row_get(r, "call_reference"));
		ber_ctx_int(w, 232, row_int(r, "record_number", 0));
		break;
	case 7:
		ber_ctx_isdn(w, 1, row_get(r, "service_centre"));
		ber_ctx_tbcd(w, 2, row_get(r, "served_imsi"));
		ber_ctx_tbcd(w, 3, row_get(r, "served_imei"));
		ber_ctx_isdn(w, 4, row_get(r, "served_msisdn"));
		ber_ctx_isdn(w, 6, entity);
		write_location(w, 7, r);
		ber_ctx_time_csv(w, 8, row_get(r, "delivery_time"), tz);
		ber_ctx_int(w, 11, row_int(r, "system_type", 0));
		ber_ctx_ascii(w, 188, row_get(r, "global_area_id"));
		ber_ctx_tbcd(w, 192, row_get(r, "first_mccmnc"));
		ber_ctx_isdn(w, 201, row_get(r, "origination"));
		ber_ctx_callref(w, 202, row_get(r, "call_reference"));
		ber_ctx_int(w, 232, row_int(r, "record_number", 0));
		break;
	case 10:
		ber_ctx_tbcd(w, 1, row_get(r, "served_imsi"));
		ber_ctx_tbcd(w, 2, row_get(r, "served_imei"));
		ber_ctx_isdn(w, 3, row_get(r, "served_msisdn"));
		ber_ctx_isdn(w, 5, entity);
		write_location(w, 6, r);
		ber_ctx_time_csv(w, 10, row_get(r, "seizure_time"), tz);
		ber_ctx_callref(w, 13, row_get(r, "call_reference"));
		ber_ctx_int(w, 232, row_int(r, "record_number", 0));
		break;
	case 13:
		ber_ctx_tbcd(w, 1, row_get(r, "served_imsi"));
		ber_ctx_isdn(w, 2, row_get(r, "served_msisdn"));
		ber_ctx_isdn(w, 3, entity);
		write_lu_location(w, 5, r);
		ber_ctx_time_csv(w, 7, row_get(r, "seizure_time"), tz);
		ber_ctx_callref(w, 12, row_get(r, "call_reference"));
		ber_ctx_int(w, 232, row_int(r, "record_number", 0));
		break;
	default:
		break;
	}
}

static void note_time(const char *s, time_t *first, time_t *last, bool is_first)
{
	struct tm tm;
	time_t t;

	if (!ber_parse_csv_time(s, &tm))
		return;
	t = timegm(&tm);
	if (t == (time_t)-1)
		return;
	if (is_first && (*first == 0 || t < *first))
		*first = t;
	if (!is_first && t > *last)
		*last = t;
}

static int encode_file(struct cfg *c, const char *csv_path)
{
	FILE *fp;
	char header_line[LINE_MAX_LEN];
	char line[LINE_MAX_LEN];
	char *hdr[MAX_COLS];
	int nh = 0;
	struct ber_buf recs, file, body, header, trailer, rec, rec_body;
	time_t first = 0, last = 0;
	int nrows = 0;
	char entity[32] = "0000";
	char out_name[PATH_MAX];
	char out_path[PATH_MAX];
	time_t now = time(NULL);
	struct tm now_tm, first_tm, last_tm;
	FILE *out;

	fp = fopen(csv_path, "r");
	if (!fp) {
		fprintf(stderr, "cannot open %s: %s\n", csv_path, strerror(errno));
		return -1;
	}
	if (!fgets(header_line, sizeof(header_line), fp)) {
		fclose(fp);
		fprintf(stderr, "%s: empty\n", csv_path);
		return -1;
	}
	header_line[strcspn(header_line, "\r\n")] = '\0';
	nh = split_csv(header_line, hdr, MAX_COLS);

	ber_init(&recs);
	while (fgets(line, sizeof(line), fp)) {
		char *cols[MAX_COLS];
		int nc, i;
		struct row row;
		int type;

		line[strcspn(line, "\r\n")] = '\0';
		if (!line[0])
			continue;
		nc = split_csv(line, cols, MAX_COLS);
		memset(&row, 0, sizeof(row));
		for (i = 0; i < nh && i < nc && i < MAX_COLS; i++) {
			row.key[row.n] = xstrdup(hdr[i]);
			row.val[row.n] = xstrdup(cols[i]);
			row.n++;
		}
		type = row_int(&row, "record_type", 0);
		note_time(row_get(&row, "seizure_time"), &first, &last, true);
		note_time(row_get(&row, "release_time"), &first, &last, false);
		note_time(row_get(&row, "origination_time"), &first, &last, true);
		note_time(row_get(&row, "delivery_time"), &first, &last, false);
		if (nrows == 0)
			digit_entity(c, &row, entity, sizeof(entity));

		ber_init(&rec_body);
		fill_record(&rec_body, c, &row, type);
		ber_init(&rec);
		ber_wrap_ctx(&rec, type, &rec_body);
		ber_raw(&recs, rec.data, rec.len);
		ber_free(&rec);
		ber_free(&rec_body);
		row_free(&row);
		nrows++;
	}
	fclose(fp);

	if (!nrows) {
		ber_free(&recs);
		fprintf(stderr, "%s: no data rows\n", csv_path);
		return -1;
	}
	if (!first)
		first = now;
	if (!last)
		last = first;

	gmtime_r(&now, &now_tm);
	gmtime_r(&first, &first_tm);
	gmtime_r(&last, &last_tm);

	ber_init(&header);
	{
		uint8_t ts[16];
		size_t n = ber_timestamp(ts, sizeof(ts), &now_tm, c->timezone);
		ber_ctx_prim(&header, 0, ts, n);
		ber_ctx_isdn(&header, 1, entity);
	}
	ber_init(&trailer);
	{
		uint8_t ts[16];
		size_t n = ber_timestamp(ts, sizeof(ts), &now_tm, c->timezone);
		ber_ctx_prim(&trailer, 0, ts, n);
		ber_ctx_isdn(&trailer, 1, entity);
		n = ber_timestamp(ts, sizeof(ts), &first_tm, c->timezone);
		ber_ctx_prim(&trailer, 2, ts, n);
		n = ber_timestamp(ts, sizeof(ts), &last_tm, c->timezone);
		ber_ctx_prim(&trailer, 3, ts, n);
		ber_ctx_int(&trailer, 4, nrows);
	}

	ber_init(&body);
	ber_wrap_ctx(&body, 0, &header);
	ber_wrap_ctx(&body, 1, &recs);
	ber_wrap_ctx(&body, 2, &trailer);

	ber_init(&file);
	ber_tag(&file, 0, true, 16); /* SEQUENCE */
	ber_length(&file, body.len);
	ber_raw(&file, body.data, body.len);

	if (mkdir_p(c->output_dir) < 0) {
		fprintf(stderr, "mkdir %s: %s\n", c->output_dir, strerror(errno));
		ber_free(&file);
		ber_free(&body);
		ber_free(&header);
		ber_free(&trailer);
		ber_free(&recs);
		return -1;
	}
	snprintf(out_name, sizeof(out_name), "%s%02d%02d%02d%02d%02d%04d.dat",
		 c->file_prefix,
		 (now_tm.tm_year + 1900) % 100, now_tm.tm_mon + 1, now_tm.tm_mday,
		 now_tm.tm_hour, now_tm.tm_min, c->seq);
	c->seq++;
	snprintf(out_path, sizeof(out_path), "%s/%s", c->output_dir, out_name);

	out = fopen(out_path, "wb");
	if (!out) {
		fprintf(stderr, "write %s: %s\n", out_path, strerror(errno));
		ber_free(&file);
		ber_free(&body);
		ber_free(&header);
		ber_free(&trailer);
		ber_free(&recs);
		return -1;
	}
	if (fwrite(file.data, 1, file.len, out) != file.len)
		fprintf(stderr, "short write %s\n", out_path);
	fclose(out);
	printf("wrote %s (%d records) from %s\n", out_path, nrows, csv_path);

	ber_free(&file);
	ber_free(&body);
	ber_free(&header);
	ber_free(&trailer);
	ber_free(&recs);
	return 0;
}

static int move_done(const struct cfg *c, const char *path)
{
	char dest[PATH_MAX];
	const char *base = strrchr(path, '/');

	base = base ? base + 1 : path;
	if (mkdir_p(c->done_dir) < 0) {
		fprintf(stderr, "mkdir %s: %s\n", c->done_dir, strerror(errno));
		return -1;
	}
	snprintf(dest, sizeof(dest), "%s/%s", c->done_dir, base);
	if (rename(path, dest) < 0) {
		fprintf(stderr, "move %s -> %s: %s\n", path, dest, strerror(errno));
		return -1;
	}
	return 0;
}

static int process_dir(struct cfg *c)
{
	DIR *d;
	struct dirent *de;
	int n = 0;

	d = opendir(c->csv_dir);
	if (!d) {
		fprintf(stderr, "watch %s: %s\n", c->csv_dir, strerror(errno));
		return -1;
	}
	while ((de = readdir(d))) {
		char path[PATH_MAX];

		if (de->d_name[0] == '.')
			continue;
		if (fnmatch(c->csv_pattern, de->d_name, 0) != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s", c->csv_dir, de->d_name);
		if (encode_file(c, path) == 0) {
			move_done(c, path);
			n++;
		}
	}
	closedir(d);
	return n;
}

static int load_config(struct cfg *c, const char *path)
{
	FILE *fp;
	char line[512];

	fp = fopen(path, "r");
	if (!fp) {
		fprintf(stderr, "config %s: %s\n", path, strerror(errno));
		return -1;
	}
	while (fgets(line, sizeof(line), fp)) {
		char key[64], val[PATH_MAX], extra[64];
		int nf;

		line[strcspn(line, "\r\n")] = '\0';
		if (!line[0] || line[0] == '#')
			continue;
		nf = sscanf(line, "%63s %4095s %63s", key, val, extra);
		if (nf < 2)
			continue;
		if (!strcmp(key, "csv-dir"))
			snprintf(c->csv_dir, sizeof(c->csv_dir), "%s", val);
		else if (!strcmp(key, "csv-pattern"))
			snprintf(c->csv_pattern, sizeof(c->csv_pattern), "%s", val);
		else if (!strcmp(key, "output-dir"))
			snprintf(c->output_dir, sizeof(c->output_dir), "%s", val);
		else if (!strcmp(key, "done-dir"))
			snprintf(c->done_dir, sizeof(c->done_dir), "%s", val);
		else if (!strcmp(key, "recording-entity"))
			snprintf(c->entity, sizeof(c->entity), "%s", val);
		else if (!strcmp(key, "file-prefix"))
			snprintf(c->file_prefix, sizeof(c->file_prefix), "%s", val);
		else if (!strcmp(key, "timezone"))
			snprintf(c->timezone, sizeof(c->timezone), "%s", val);
		else if (!strcmp(key, "poll-seconds"))
			c->poll_seconds = atoi(val);
		else if (!strcmp(key, "route") && nf >= 3 && c->n_routes < MAX_ROUTES) {
			snprintf(c->routes[c->n_routes].prefix,
				 sizeof(c->routes[0].prefix), "%s", val);
			snprintf(c->routes[c->n_routes].name,
				 sizeof(c->routes[0].name), "%s", extra);
			c->n_routes++;
		}
	}
	fclose(fp);
	return 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [--config FILE] [--once CSV] [--csv-dir DIR] [--output-dir DIR]\n"
		"          [--done-dir DIR] [--entity DIGITS] [--watch]\n"
		"\n"
		"Separate process beside osmo-msc. Reads rotated CSV tickets and writes\n"
		"Huawei CallEventDataFile BER (bA*.dat). Does not touch the OFCS SQL path.\n"
		"The live osmo-msc.cdr file is ignored; only names matching csv-pattern\n"
		"(default osmo-msc.cdr.*) are encoded.\n",
		argv0);
}

int main(int argc, char **argv)
{
	struct cfg cfg;
	const char *once = NULL;
	bool watch = false;
	int opt;
	static struct option longopts[] = {
		{ "config", required_argument, NULL, 'c' },
		{ "once", required_argument, NULL, '1' },
		{ "csv-dir", required_argument, NULL, 'i' },
		{ "output-dir", required_argument, NULL, 'o' },
		{ "done-dir", required_argument, NULL, 'd' },
		{ "entity", required_argument, NULL, 'e' },
		{ "watch", no_argument, NULL, 'w' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	cfg_defaults(&cfg);
	while ((opt = getopt_long(argc, argv, "c:1:i:o:d:e:wh", longopts, NULL)) != -1) {
		switch (opt) {
		case 'c':
			if (load_config(&cfg, optarg) < 0)
				return 1;
			break;
		case '1':
			once = optarg;
			break;
		case 'i':
			snprintf(cfg.csv_dir, sizeof(cfg.csv_dir), "%s", optarg);
			break;
		case 'o':
			snprintf(cfg.output_dir, sizeof(cfg.output_dir), "%s", optarg);
			break;
		case 'd':
			snprintf(cfg.done_dir, sizeof(cfg.done_dir), "%s", optarg);
			break;
		case 'e':
			snprintf(cfg.entity, sizeof(cfg.entity), "%s", optarg);
			break;
		case 'w':
			watch = true;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	if (once) {
		return encode_file(&cfg, once) == 0 ? 0 : 1;
	}

	if (!watch && optind < argc)
		once = argv[optind];
	if (once)
		return encode_file(&cfg, once) == 0 ? 0 : 1;

	watch = true;
	printf("osmo-msc-cdrenc watching %s/%s -> %s\n",
	       cfg.csv_dir, cfg.csv_pattern, cfg.output_dir);
	for (;;) {
		process_dir(&cfg);
		sleep(cfg.poll_seconds > 0 ? cfg.poll_seconds : 10);
	}
	return 0;
}
