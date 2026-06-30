/*
 * OsmoMSC embedded HTTP/JSON API.
 *
 * (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <osmocom/core/msgb.h>
#include <osmocom/core/rate_ctr.h>
#include <osmocom/core/select.h>
#include <osmocom/core/socket.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/stat_item.h>
#include <osmocom/core/timer.h>
#include <osmocom/core/utils.h>
#include <osmocom/gsm/gsm23003.h>
#include <osmocom/gsm/protocol/gsm_04_08.h>
#include <osmocom/msc/debug.h>
#include <osmocom/msc/gsm_data.h>
#include <osmocom/msc/msc_a.h>
#include <osmocom/msc/msc_api.h>
#include <osmocom/msc/msub.h>
#include <osmocom/msc/neighbor_ident.h>
#include <osmocom/msc/ran_conn.h>
#include <osmocom/msc/ran_peer.h>
#include <osmocom/msc/sccp_ran.h>
#include <osmocom/msc/cell_id_list.h>
#include <osmocom/msc/transaction.h>
#include <osmocom/msc/vty.h>
#include <osmocom/sigtran/osmo_ss7.h>
#include <osmocom/sigtran/sccp_helpers.h>
#include <osmocom/vty/vty.h>
#include <osmocom/netif/stream.h>
#include <osmocom/vlr/vlr.h>

#define VSUB_USE_API "API"
#define MSC_API_MAX_REQUEST 8192
/* msgb_alloc() size is uint16_t; keep chunks below 64k. */
#define MSC_API_MSGB_CHUNK 60000
/* Reject oversized bulk JSON before send (PrettyNMS should use /api/stats). */
#define MSC_API_MAX_JSON_BYTES (256 * 1024)

struct msc_api_conn {
	struct osmo_stream_srv *srv;
	char *buf;
	size_t buf_len;
	bool closed;
};

static struct msc_api_state *g_msc_api;

static struct vlr_subscr *api_find_vsub(struct gsm_network *net, const char *id)
{
	struct vlr_subscr *vsub;

	if (!net || !net->vlr || !id || !id[0])
		return NULL;

	vsub = vlr_subscr_find_by_imsi(net->vlr, id, VSUB_USE_API);
	if (vsub)
		return vsub;

	return vlr_subscr_find_by_msisdn(net->vlr, id, VSUB_USE_API);
}

static char *json_escape(void *ctx, const char *str)
{
	const char *in;
	char *out, *o;
	size_t extra = 0;

	if (!str)
		return talloc_strdup(ctx, "");

	for (in = str; *in; in++) {
		switch (*in) {
		case '"':
		case '\\':
			extra++;
			break;
		default:
			if ((unsigned char)*in < 0x20)
				extra += 5;
			break;
		}
	}

	out = o = talloc_array(ctx, char, (in - str) + extra + 1);
	if (!out)
		return NULL;

	for (in = str; *in; in++) {
		switch (*in) {
		case '"':
		case '\\':
			*o++ = '\\';
			*o++ = *in;
			break;
		default:
			if ((unsigned char)*in < 0x20)
				o += sprintf(o, "\\u%04x", (unsigned char)*in);
			else
				*o++ = *in;
			break;
		}
	}
	*o = '\0';
	return out;
}

static int api_send_response(struct osmo_stream_srv *conn, int status,
			     const char *status_text, const char *json_body)
{
	const char *body = json_body ? json_body : "";
	size_t body_len = strlen(body);
	size_t hdr_len, off;
	char hdr[256];
	struct msgb *msg;

	hdr_len = snprintf(hdr, sizeof(hdr),
			   "HTTP/1.0 %d %s\r\n"
			   "Content-Type: application/json\r\n"
			   "Content-Length: %zu\r\n"
			   "Connection: close\r\n"
			   "\r\n",
			   status, status_text, body_len);
	if (hdr_len >= sizeof(hdr))
		return -EINVAL;

	msg = msgb_alloc(hdr_len + 1, "msc_api_http");
	if (!msg)
		return -ENOMEM;

	memcpy(msgb_put(msg, hdr_len), hdr, hdr_len);
	osmo_stream_srv_send(conn, msg);

	for (off = 0; off < body_len; off += MSC_API_MSGB_CHUNK) {
		size_t chunk = body_len - off;

		if (chunk > MSC_API_MSGB_CHUNK)
			chunk = MSC_API_MSGB_CHUNK;

		msg = msgb_alloc(chunk + 1, "msc_api_http");
		if (!msg)
			return -ENOMEM;

		memcpy(msgb_put(msg, chunk), body + off, chunk);
		osmo_stream_srv_send(conn, msg);
	}

	return 0;
}

static void api_reply_json(struct osmo_stream_srv *srv, char *json)
{
	if (json && strlen(json) > MSC_API_MAX_JSON_BYTES) {
		LOGP(DMSC, LOGL_NOTICE, "HTTP API response too large (%zu bytes)\n",
		     strlen(json));
		api_send_response(srv, 413, "Payload Too Large",
				  "{\"error\":\"response too large; use /api/stats for dashboard polling\"}");
		return;
	}
	api_send_response(srv, 200, "OK", json);
}

struct api_json_buf {
	void *ctx;
	char *data;
	size_t len;
	size_t cap;
};

static int api_json_buf_grow(struct api_json_buf *jb, size_t need)
{
	if (!jb->data)
		return -ENOMEM;
	if (jb->len + need + 1 <= jb->cap)
		return 0;
	while (jb->len + need + 1 > jb->cap)
		jb->cap *= 2;
	jb->data = talloc_realloc(jb->ctx, jb->data, char, jb->cap);
	if (!jb->data)
		return -ENOMEM;
	return 0;
}

static int api_json_buf_append(struct api_json_buf *jb, const char *str)
{
	size_t sl;

	if (!str)
		return -ENOMEM;
	sl = strlen(str);
	if (api_json_buf_grow(jb, sl) < 0)
		return -ENOMEM;
	memcpy(jb->data + jb->len, str, sl);
	jb->len += sl;
	jb->data[jb->len] = '\0';
	return 0;
}

static int api_json_buf_init(struct api_json_buf *jb, void *ctx, const char *start)
{
	jb->ctx = ctx;
	jb->cap = 4096;
	jb->len = 0;
	jb->data = talloc_size(ctx, jb->cap);
	if (!jb->data)
		return -ENOMEM;
	jb->data[0] = '\0';
	return api_json_buf_append(jb, start);
}

static int api_json_buf_append_va(struct api_json_buf *jb, const char *fmt, ...)
{
	char *tmp;
	va_list ap;
	int rc;

	va_start(ap, fmt);
	tmp = talloc_vasprintf(jb->ctx, fmt, ap);
	va_end(ap);
	if (!tmp)
		return -ENOMEM;
	rc = api_json_buf_append(jb, tmp);
	talloc_free(tmp);
	return rc;
}

static const char *api_header_value(const char *req, const char *name)
{
	const char *p, *line_end, *val;
	size_t name_len = strlen(name);

	for (p = req; (line_end = strstr(p, "\r\n")); p = line_end + 2) {
		if (line_end == p)
			break;
		if (line_end - p > (ptrdiff_t)name_len && strncasecmp(p, name, name_len) == 0
		    && p[name_len] == ':') {
			val = p + name_len + 1;
			while (*val == ' ' || *val == '\t')
				val++;
			return val;
		}
	}
	return NULL;
}

static char *api_header_value_trimmed(void *ctx, const char *req, const char *name)
{
	const char *val = api_header_value(req, name);
	char *out, *cr, *end;

	if (!val)
		return NULL;

	out = talloc_strdup(ctx, val);
	if (!out)
		return NULL;

	cr = strchr(out, '\r');
	if (cr)
		*cr = '\0';
	end = out + strlen(out);
	while (end > out && (end[-1] == ' ' || end[-1] == '\t'))
		*--end = '\0';
	return out;
}

static bool api_token_valid(const struct msc_api_state *api, const char *auth_hdr,
			    const char *api_token_hdr)
{
	const char *prefix = "Bearer ";
	const char *token = NULL;
	bool ok;

	if (api_token_hdr && api_token_hdr[0])
		token = api_token_hdr;
	else if (auth_hdr && strncasecmp(auth_hdr, prefix, strlen(prefix)) == 0)
		token = auth_hdr + strlen(prefix);

	if (!token || !api->cfg.token[0])
		return false;

	ok = strcmp(token, api->cfg.token) == 0;
	return ok;
}

static void api_parse_request(const char *req, char *method, size_t method_len,
			      char *path, size_t path_len)
{
	const char *sp1, *sp2, *line_end;
	size_t mlen, plen;

	method[0] = path[0] = '\0';
	line_end = strstr(req, "\r\n");
	if (!line_end)
		return;

	sp1 = strchr(req, ' ');
	if (!sp1 || sp1 >= line_end)
		return;
	sp2 = strchr(sp1 + 1, ' ');
	if (!sp2 || sp2 >= line_end)
		return;

	mlen = sp1 - req;
	if (mlen >= method_len)
		mlen = method_len - 1;
	memcpy(method, req, mlen);
	method[mlen] = '\0';

	plen = sp2 - (sp1 + 1);
	if (plen >= path_len)
		plen = path_len - 1;
	memcpy(path, sp1 + 1, plen);
	path[plen] = '\0';
}

static void api_path_strip_query(char *path, char *query, size_t query_len)
{
	char *q = strchr(path, '?');

	query[0] = '\0';
	if (q) {
		osmo_strlcpy(query, q + 1, query_len);
		*q = '\0';
	}
}

static bool api_query_get(const char *query, const char *key, char *val, size_t val_len)
{
	const char *p;
	size_t key_len = strlen(key);

	if (!query || !query[0])
		return false;

	for (p = query; *p;) {
		const char *amp = strchr(p, '&');
		const char *eq = strchr(p, '=');
		size_t seg_len = amp ? (size_t)(amp - p) : strlen(p);

		if (eq && eq < p + seg_len && (size_t)(eq - p) == key_len
		    && strncmp(p, key, key_len) == 0) {
			const char *v = eq + 1;
			size_t vlen = seg_len - key_len - 1;

			if (vlen >= val_len)
				vlen = val_len - 1;
			memcpy(val, v, vlen);
			val[vlen] = '\0';
			return val[0] != '\0';
		}
		p = amp ? amp + 1 : p + strlen(p);
	}
	return false;
}

static bool api_imsi_matches_vsub(const struct vlr_subscr *vsub, const char *filter)
{
	if (!filter || !filter[0])
		return true;
	if (strcmp(vsub->imsi, filter) == 0)
		return true;
	if (vsub->msisdn[0] && strcmp(vsub->msisdn, filter) == 0)
		return true;
	return false;
}

static bool api_valid_subscriber_id(const char *id)
{
	const char *p;

	if (!id || !id[0])
		return false;
	for (p = id; *p; p++) {
		if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z')
		    || (*p >= 'a' && *p <= 'z') || *p == '+')
			continue;
		return false;
	}
	return true;
}

static bool api_subscriber_path_id(const char *rest, const char *suffix, char *id, size_t id_len)
{
	size_t suffix_len = strlen(suffix);
	size_t rest_len = strlen(rest);

	if (rest_len <= suffix_len + 1 || rest[rest_len - suffix_len - 1] != '/')
		return false;
	if (strcmp(rest + rest_len - suffix_len, suffix) != 0)
		return false;

	osmo_strlcpy(id, rest, OSMO_MIN(id_len, rest_len - suffix_len));
	return api_valid_subscriber_id(id);
}

static bool api_subscriber_is_online(struct gsm_network *net, const char *id)
{
	struct vlr_subscr *vsub;
	bool online;

	vsub = api_find_vsub(net, id);
	if (!vsub)
		return false;
	online = vsub->lu_complete != 0;
	vlr_subscr_put(vsub, VSUB_USE_API);
	return online;
}

static char *api_json_count(void *ctx, unsigned int count)
{
	return talloc_asprintf(ctx, "{\"count\":%u}", count);
}

/* Append configured periodic LU expiry (T3212 CS / T3312 PS) and remaining time. */
static void api_json_append_lu_expiry(void *ctx, char **json, const struct vlr_subscr *vsub)
{
	unsigned long timer_sec = vlr_timer_secs(vsub->vlr, 3212, 3312);
	const char *timer_name = vlr_is_cs(vsub->vlr) ? "T3212" : "T3312";
	struct timespec now;
	long long expires_in = -1;

	if (timer_sec && vsub->expire_lu != VLR_SUBSCRIBER_NO_EXPIRATION
	    && osmo_clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
		if (vsub->expire_lu > (time_t)now.tv_sec)
			expires_in = vsub->expire_lu - now.tv_sec;
		else
			expires_in = 0;
	}

	*json = talloc_asprintf_append(*json,
				       ",\"lu_timer\":\"%s\",\"lu_timer_sec\":%lu",
				       timer_name, timer_sec);
	if (expires_in >= 0)
		*json = talloc_asprintf_append(*json, ",\"lu_expires_in_sec\":%lld", expires_in);
	else
		*json = talloc_asprintf_append(*json, ",\"lu_expires_in_sec\":null");
}

static void api_json_buf_append_lu_expiry(struct api_json_buf *jb, const struct vlr_subscr *vsub)
{
	unsigned long timer_sec = vlr_timer_secs(vsub->vlr, 3212, 3312);
	const char *timer_name = vlr_is_cs(vsub->vlr) ? "T3212" : "T3312";
	struct timespec now;
	long long expires_in = -1;

	if (timer_sec && vsub->expire_lu != VLR_SUBSCRIBER_NO_EXPIRATION
	    && osmo_clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
		if (vsub->expire_lu > (time_t)now.tv_sec)
			expires_in = vsub->expire_lu - now.tv_sec;
		else
			expires_in = 0;
	}

	api_json_buf_append_va(jb, ",\"lu_timer\":\"%s\",\"lu_timer_sec\":%lu",
			      timer_name, timer_sec);
	if (expires_in >= 0)
		api_json_buf_append_va(jb, ",\"lu_expires_in_sec\":%lld", expires_in);
	else
		api_json_buf_append(jb, ",\"lu_expires_in_sec\":null");
}

static unsigned int api_count_subscribers_online(struct gsm_network *net, const char *filter_imsi)
{
	struct vlr_subscr *vsub;
	unsigned int count = 0;

	if (!net || !net->vlr)
		return 0;

	llist_for_each_entry(vsub, &net->vlr->subscribers, list) {
		if (!vsub->lu_complete)
			continue;
		if (!api_imsi_matches_vsub(vsub, filter_imsi))
			continue;
		count++;
	}
	return count;
}

static unsigned int api_count_active_calls(struct gsm_network *net, const char *filter_imsi)
{
	struct gsm_trans *trans;
	unsigned int count = 0;

	if (!net)
		return 0;

	llist_for_each_entry(trans, &net->trans_list, entry) {
		if (trans->type != TRANS_CC || trans->cc.state != GSM_CSTATE_ACTIVE)
			continue;
		if (filter_imsi && filter_imsi[0]) {
			if (!trans->vsub || !api_imsi_matches_vsub(trans->vsub, filter_imsi))
				continue;
		}
		count++;
	}
	return count;
}

static unsigned int api_count_links(struct gsm_network *net)
{
	struct ran_peer *rp;
	const struct neighbor_ident_entry *nie;
	unsigned int count = 0;

	if (!net)
		return 0;

	if (net->a.sri) {
		llist_for_each_entry(rp, &net->a.sri->ran_peers, entry)
			count++;
	}
	if (net->iu.sri) {
		llist_for_each_entry(rp, &net->iu.sri->ran_peers, entry)
			count++;
	}
	llist_for_each_entry(nie, &net->neighbor_ident_list, entry)
		count++;
	return count;
}

static uint64_t api_rate_ctr_current(const struct rate_ctr_group *ctrg, const char *name)
{
	const struct rate_ctr *ctr;

	if (!ctrg || !name)
		return 0;
	ctr = rate_ctr_get_by_name(ctrg, name);
	return ctr ? ctr->current : 0;
}

static int32_t api_stat_item_value(const struct osmo_stat_item_group *statg,
				   const char *name)
{
	const struct osmo_stat_item *item;

	if (!statg || !name)
		return 0;
	item = osmo_stat_item_get_by_name(statg, name);
	return item ? osmo_stat_item_get_last(item) : 0;
}

static char *api_iso8601_utc(void *ctx)
{
	time_t now = time(NULL);
	struct tm tm;
	char *buf;

	if (now == (time_t)-1 || !gmtime_r(&now, &tm))
		return talloc_strdup(ctx, "");

	buf = talloc_array(ctx, char, 32);
	if (!buf)
		return talloc_strdup(ctx, "");
	strftime(buf, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
	return buf;
}

static struct osmo_ss7_asp *api_find_asp_by_name(const char *name)
{
	struct llist_head *lh;
	struct osmo_ss7_instance *inst;
	struct osmo_ss7_asp *asp;

	if (!name || !name[0])
		return NULL;

	llist_for_each(lh, &osmo_ss7_instances) {
		inst = osmo_ss7_instances_llist_entry(lh);
		asp = osmo_ss7_asp_find_by_name(inst, name);
		if (asp)
			return asp;
	}

	return NULL;
}

static struct osmo_ss7_as *api_find_as_by_name(const char *name)
{
	struct llist_head *lh;
	struct osmo_ss7_instance *inst;
	struct osmo_ss7_as *as;

	if (!name || !name[0])
		return NULL;

	llist_for_each(lh, &osmo_ss7_instances) {
		inst = osmo_ss7_instances_llist_entry(lh);
		as = osmo_ss7_as_find_by_name(inst, name);
		if (as)
			return as;
	}

	return NULL;
}

struct api_sigtran_collect {
	void *ctx;
	char *asps_json;
	char *as_json;
	bool first_asp;
	bool first_as;
	unsigned int asp_up;
	uint64_t msu_rx;
	uint64_t msu_tx;
	uint64_t msu_discarded;
};

static bool api_ctrg_has_counter(const struct rate_ctr_group *ctrg, const char *name)
{
	if (!ctrg || !name)
		return false;
	return rate_ctr_get_by_name(ctrg, name) != NULL;
}

static int api_sigtran_collect_group(struct rate_ctr_group *ctrg, void *data)
{
	struct api_sigtran_collect *col = data;
	const char *name;
	uint64_t rx, tx, discarded;
	struct osmo_ss7_asp *asp;
	bool up;

	if (!ctrg || !ctrg->name)
		return 0;

	name = ctrg->name;

	if (api_ctrg_has_counter(ctrg, "rx:packets:total")) {
		asp = api_find_asp_by_name(name);
		if (!asp)
			return 0;

		rx = api_rate_ctr_current(ctrg, "rx:packets:total");
		tx = api_rate_ctr_current(ctrg, "tx:packets:total");
		up = osmo_ss7_asp_active(asp);

		if (rx > 0 || tx > 0)
			col->asp_up++;

		col->asps_json = talloc_asprintf_append(col->asps_json,
			"%s{\"name\":\"%s\","
			"\"rx_packets\":%llu,"
			"\"tx_packets\":%llu,"
			"\"up\":%s}",
			col->first_asp ? "" : ",",
			json_escape(col->ctx, name),
			(unsigned long long)rx,
			(unsigned long long)tx,
			up ? "true" : "false");
		col->first_asp = false;
		return 0;
	}

	if (api_ctrg_has_counter(ctrg, "rx:msu:total")) {
		if (!api_find_as_by_name(name))
			return 0;

		rx = api_rate_ctr_current(ctrg, "rx:msu:total");
		tx = api_rate_ctr_current(ctrg, "tx:msu:total");
		discarded = api_rate_ctr_current(ctrg, "rx:msu:discarded");
		if (!discarded)
			discarded = api_rate_ctr_current(ctrg, "rx:packets:unknown");

		col->msu_rx += rx;
		col->msu_tx += tx;
		col->msu_discarded += discarded;

		col->as_json = talloc_asprintf_append(col->as_json,
			"%s{\"name\":\"%s\","
			"\"msu_rx\":%llu,"
			"\"msu_tx\":%llu,"
			"\"msu_discarded\":%llu}",
			col->first_as ? "" : ",",
			json_escape(col->ctx, name),
			(unsigned long long)rx,
			(unsigned long long)tx,
			(unsigned long long)discarded);
		col->first_as = false;
	}

	return 0;
}

static char *api_json_stats(void *ctx, struct gsm_network *net)
{
	struct rate_ctr_group *sms_ctrg;
	struct osmo_stat_item_group *sms_statg;
	struct api_sigtran_collect sig;
	char *json, *ts;
	int32_t active_calls = 0;
	int32_t active_nc_ss = 0;
	int32_t ran_peers_active = 0;
	int32_t ran_peers_total = 0;
	int32_t vlr_subscribers = 0;
	int32_t sms_pending = 0;
	uint64_t sms_mt_attempted = 0;
	uint64_t sms_mt_failed_paging = 0;
	uint64_t sms_mt_failed_nomem = 0;
	uint64_t calls_lu_success = 0;
	uint64_t calls_mo_setup = 0;
	uint64_t calls_reached_active = 0;

	if (!net)
		return talloc_strdup(ctx, "{}");

	ts = api_iso8601_utc(ctx);

	if (net->statg) {
		active_calls = osmo_stat_item_get_last(
			osmo_stat_item_group_get_item(net->statg, MSC_STAT_ACTIVE_CALLS));
		active_nc_ss = osmo_stat_item_get_last(
			osmo_stat_item_group_get_item(net->statg, MSC_STAT_ACTIVE_NC_SS));
		ran_peers_active = osmo_stat_item_get_last(
			osmo_stat_item_group_get_item(net->statg, MSC_STAT_RAN_PEERS_ACTIVE));
		ran_peers_total = osmo_stat_item_get_last(
			osmo_stat_item_group_get_item(net->statg, MSC_STAT_RAN_PEERS_TOTAL));
	}

	if (net->vlr && net->vlr->statg)
		vlr_subscribers = api_stat_item_value(net->vlr->statg, "subscribers");

	sms_statg = osmo_stat_item_get_group_by_name_idxname("sms_queue", NULL);
	if (sms_statg)
		sms_pending = api_stat_item_value(sms_statg, "ram:pending");

	if (net->msc_ctrs) {
		calls_lu_success = rate_ctr_group_get_ctr(net->msc_ctrs,
			MSC_CTR_LOC_UPDATE_COMPLETED)->current;
		calls_mo_setup = rate_ctr_group_get_ctr(net->msc_ctrs,
			MSC_CTR_CALL_MO_SETUP)->current;
		calls_reached_active = rate_ctr_group_get_ctr(net->msc_ctrs,
			MSC_CTR_CALL_ACTIVE)->current;
		sms_mt_attempted = rate_ctr_group_get_ctr(net->msc_ctrs,
			MSC_CTR_SMS_DELIVERED)->current;
		sms_mt_failed_nomem = rate_ctr_group_get_ctr(net->msc_ctrs,
			MSC_CTR_SMS_RP_ERR_MEM)->current;
	}

	sms_ctrg = rate_ctr_get_group_by_name_idx("sms_queue", 0);
	if (sms_ctrg)
		sms_mt_failed_paging = api_rate_ctr_current(sms_ctrg, "deliver:paging_timeout");

	sig = (struct api_sigtran_collect) {
		.ctx = ctx,
		.asps_json = talloc_strdup(ctx, "["),
		.as_json = talloc_strdup(ctx, "["),
		.first_asp = true,
		.first_as = true,
	};
	rate_ctr_for_each_group(api_sigtran_collect_group, &sig);
	sig.asps_json = talloc_asprintf_append(sig.asps_json, "]");
	sig.as_json = talloc_asprintf_append(sig.as_json, "]");

	json = talloc_asprintf(ctx,
		"{\"timestamp\":\"%s\","
		"\"active_calls\":%d,"
		"\"online_subscribers\":%u,"
		"\"sms_pending_queue\":%d,"
		"\"vlr\":{\"subscribers\":%d},"
		"\"network\":{"
		"\"active_ran_peers\":%d,"
		"\"total_ran_peers_seen\":%d,"
		"\"active_ss_ussd_sessions\":%d},"
		"\"sigtran\":{"
		"\"asp_up\":%u,"
		"\"msu_discarded\":%llu,"
		"\"msu_rx\":%llu,"
		"\"msu_tx\":%llu,"
		"\"asps\":%s,"
		"\"application_servers\":%s},"
		"\"sms\":{"
		"\"mt_delivery_attempted\":%llu,"
		"\"mt_delivery_failed_paging\":%llu,"
		"\"mt_delivery_failed_no_memory\":%llu},"
		"\"calls\":{"
		"\"lu_success\":%llu,"
		"\"mo_setup\":%llu,"
		"\"reached_active\":%llu}}",
		json_escape(ctx, ts),
		active_calls,
		api_count_subscribers_online(net, NULL),
		sms_pending,
		vlr_subscribers,
		ran_peers_active,
		ran_peers_total,
		active_nc_ss,
		sig.asp_up,
		(unsigned long long)sig.msu_discarded,
		(unsigned long long)sig.msu_rx,
		(unsigned long long)sig.msu_tx,
		sig.asps_json, sig.as_json,
		(unsigned long long)sms_mt_attempted,
		(unsigned long long)sms_mt_failed_paging,
		(unsigned long long)sms_mt_failed_nomem,
		(unsigned long long)calls_lu_success,
		(unsigned long long)calls_mo_setup,
		(unsigned long long)calls_reached_active);

	return json;
}

static char *api_json_subscribers_online(void *ctx, struct gsm_network *net, const char *filter_imsi)
{
	struct vlr_subscr *vsub;
	struct api_json_buf jb;
	bool first = true;

	if (!net || !net->vlr)
		return talloc_asprintf(ctx, "{\"subscribers\":[]}");
	if (api_json_buf_init(&jb, ctx, "{\"subscribers\":[") < 0)
		return NULL;

	llist_for_each_entry(vsub, &net->vlr->subscribers, list) {
		struct msc_a *msc_a;
		char *imsi, *msisdn, *ran;

		if (!vsub->lu_complete)
			continue;
		if (!api_imsi_matches_vsub(vsub, filter_imsi))
			continue;

		msc_a = msc_a_for_vsub(vsub, true);
		imsi = json_escape(ctx, vsub->imsi);
		msisdn = json_escape(ctx, vsub->msisdn);
		ran = json_escape(ctx, osmo_rat_type_name(vsub->cs.attached_via_ran));

		if (api_json_buf_append_va(&jb,
			"%s{\"imsi\":\"%s\",\"msisdn\":\"%s\",\"tmsi\":\"%08X\","
			"\"lac\":%u,\"ran\":\"%s\",\"state\":\"online\",\"connected\":%s",
			first ? "" : ",",
			imsi, msisdn,
			vsub->tmsi != GSM_RESERVED_TMSI ? vsub->tmsi : 0,
			vsub->cgi.lai.lac, ran,
			msc_a ? "true" : "false") < 0)
			return NULL;
		api_json_buf_append_lu_expiry(&jb, vsub);
		if (api_json_buf_append(&jb, "}") < 0)
			return NULL;
		first = false;
	}

	if (api_json_buf_append(&jb, "]}") < 0)
		return NULL;
	return jb.data;
}

static char *api_json_subscriber_detail(void *ctx, struct gsm_network *net, const char *id)
{
	struct vlr_subscr *vsub;
	struct msc_a *msc_a;
	struct gsm_trans *trans;
	unsigned int active_calls = 0;
	char *imsi, *msisdn, *imei, *ran, *conn_state = NULL;
	char *json;

	vsub = api_find_vsub(net, id);
	if (!vsub)
		return NULL;

	msc_a = msc_a_for_vsub(vsub, true);
	llist_for_each_entry(trans, &net->trans_list, entry) {
		if (trans->vsub != vsub)
			continue;
		if (trans->type == TRANS_CC && trans->cc.state == GSM_CSTATE_ACTIVE)
			active_calls++;
	}

	imsi = json_escape(ctx, vsub->imsi);
	msisdn = json_escape(ctx, vsub->msisdn[0] ? vsub->msisdn : "");
	imei = json_escape(ctx, vsub->imei[0] ? vsub->imei : "");
	ran = json_escape(ctx, osmo_rat_type_name(vsub->cs.attached_via_ran));
	if (msc_a)
		conn_state = json_escape(ctx, osmo_fsm_inst_state_name(msc_a->c.fi));

	json = talloc_asprintf(ctx,
		"{\"imsi\":\"%s\",\"msisdn\":\"%s\",\"imei\":\"%s\","
		"\"tmsi\":\"%08X\",\"lac\":%u,\"cell_id\":%u,\"ran\":\"%s\","
		"\"lu_complete\":%s,\"connected\":%s,\"paging\":%s,"
		"\"active_calls\":%u",
		imsi, msisdn, imei,
		vsub->tmsi != GSM_RESERVED_TMSI ? vsub->tmsi : 0,
		vsub->cgi.lai.lac, vsub->cgi.cell_identity, ran,
		vsub->lu_complete ? "true" : "false",
		msc_a ? "true" : "false",
		vsub->cs.is_paging ? "true" : "false",
		active_calls);

	if (msc_a) {
		json = talloc_asprintf_append(json,
			",\"conn_state\":\"%s\",\"conn_lac\":%u,\"conn_cell_id\":%u",
			conn_state, msc_a->via_cell.lai.lac, msc_a->via_cell.cell_identity);
	}

	api_json_append_lu_expiry(ctx, &json, vsub);
	json = talloc_asprintf_append(json, "}");
	vlr_subscr_put(vsub, VSUB_USE_API);
	return json;
}

static char *api_json_active_calls(void *ctx, struct gsm_network *net, const char *filter_imsi)
{
	struct gsm_trans *trans;
	struct api_json_buf jb;
	bool first = true;

	if (api_json_buf_init(&jb, ctx, "{\"calls\":[") < 0)
		return NULL;

	llist_for_each_entry(trans, &net->trans_list, entry) {
		const char *imsi = "";
		const char *msisdn = "";
		char *eimsi, *emsisdn;

		if (trans->type != TRANS_CC)
			continue;
		if (trans->cc.state != GSM_CSTATE_ACTIVE)
			continue;
		if (filter_imsi && filter_imsi[0]) {
			if (!trans->vsub || !api_imsi_matches_vsub(trans->vsub, filter_imsi))
				continue;
		}

		if (trans->vsub) {
			imsi = trans->vsub->imsi;
			msisdn = trans->vsub->msisdn;
		}

		eimsi = json_escape(ctx, imsi);
		emsisdn = json_escape(ctx, msisdn);

		if (api_json_buf_append_va(&jb,
			"%s{\"callref\":\"0x%08x\",\"imsi\":\"%s\",\"msisdn\":\"%s\","
			"\"direction\":\"%s\",\"state\":\"active\",\"transaction_id\":%u}",
			first ? "" : ",",
			trans->callref, eimsi, emsisdn,
			(trans->transaction_id & 0x08) ? "MO" : "MT",
			trans->transaction_id) < 0)
			return NULL;
		first = false;
	}

	if (api_json_buf_append(&jb, "]}") < 0)
		return NULL;
	return jb.data;
}

static unsigned int api_ran_peer_conn_count(const struct ran_peer *rp)
{
	struct ran_conn *conn;
	unsigned int count = 0;

	ran_peer_for_each_ran_conn(conn, rp)
		count++;
	return count;
}

static void api_json_append_ran_peers(void *ctx, char **json, bool *first,
				      struct sccp_ran_inst *sri)
{
	struct ran_peer *rp;

	if (!sri || !sri->sccp || !sri->ran)
		return;

	llist_for_each_entry(rp, &sri->ran_peers, entry) {
		const char *addr = osmo_sccp_inst_addr_name(sri->sccp, &rp->peer_addr);
		char *eaddr = json_escape(ctx, addr);
		char *state = json_escape(ctx, osmo_fsm_inst_state_name(rp->fi));
		char *ran = json_escape(ctx, osmo_rat_type_name(sri->ran->type));
		char *entry;

		entry = talloc_asprintf(ctx,
			"%s{\"type\":\"ran\",\"ran\":\"%s\",\"address\":\"%s\","
			"\"state\":\"%s\",\"connections\":%u,\"osmux\":%s}",
			*first ? "" : ",",
			ran, eaddr, state, api_ran_peer_conn_count(rp),
			rp->remote_supports_osmux ? "true" : "false");
		*json = talloc_asprintf_append(*json, "%s", entry ? entry : "");
		*first = false;
	}
}

static const char *api_neighbor_type_name(enum msc_neighbor_type type)
{
	switch (type) {
	case MSC_NEIGHBOR_TYPE_LOCAL_RAN_PEER:
		return "local_ran_peer";
	case MSC_NEIGHBOR_TYPE_REMOTE_MSC:
		return "remote_msc";
	default:
		return "unknown";
	}
}

static char *api_json_neighbor_target(void *ctx, const struct neighbor_ident_addr *addr)
{
	char tmp[65];

	switch (addr->type) {
	case MSC_NEIGHBOR_TYPE_LOCAL_RAN_PEER:
		return json_escape(ctx, addr->local_ran_peer_pc_str);
	case MSC_NEIGHBOR_TYPE_REMOTE_MSC:
		osmo_strlcpy(tmp, addr->remote_msc_ipa_name.buf, sizeof(tmp));
		if (addr->remote_msc_ipa_name.len < sizeof(tmp))
			tmp[addr->remote_msc_ipa_name.len] = '\0';
		return json_escape(ctx, tmp);
	default:
		return json_escape(ctx, "");
	}
}

static char *api_json_cell_id(void *ctx, const struct gsm0808_cell_id *cid)
{
	char buf[128];

	switch (cid->id_discr) {
	case CELL_IDENT_LAC:
		snprintf(buf, sizeof(buf), "lac:%u", cid->id.lac);
		break;
	case CELL_IDENT_LAC_AND_CI:
		snprintf(buf, sizeof(buf), "lac-ci:%u:%u",
			 cid->id.lac_and_ci.lac, cid->id.lac_and_ci.ci);
		break;
	case CELL_IDENT_WHOLE_GLOBAL:
		snprintf(buf, sizeof(buf), "cgi:%s-%s:%u:%u",
			 osmo_mcc_name(cid->id.global.lai.plmn.mcc),
			 osmo_mnc_name(cid->id.global.lai.plmn.mnc,
				       cid->id.global.lai.plmn.mnc_3_digits),
			 cid->id.global.lai.lac,
			 cid->id.global.cell_identity);
		break;
	default:
		return json_escape(ctx, "unknown");
	}

	return json_escape(ctx, buf);
}

static void api_json_append_neighbors(void *ctx, char **json, bool *first,
				      struct gsm_network *net)
{
	const struct neighbor_ident_entry *nie;
	struct cell_id_list_entry *cie;

	llist_for_each_entry(nie, &net->neighbor_ident_list, entry) {
		char *ran = json_escape(ctx, osmo_rat_type_name(nie->addr.ran_type));
		char *target_type = json_escape(ctx, api_neighbor_type_name(nie->addr.type));
		char *target = api_json_neighbor_target(ctx, &nie->addr);
		char *cells = talloc_strdup(ctx, "[");
		bool cell_first = true;
		char *entry;

		llist_for_each_entry(cie, &nie->cell_ids, entry) {
			char *cell = api_json_cell_id(ctx, &cie->cell_id);
			cells = talloc_asprintf_append(cells, "%s\"%s\"",
						       cell_first ? "" : ",", cell);
			cell_first = false;
		}
		cells = talloc_asprintf_append(cells, "]");

		entry = talloc_asprintf(ctx,
			"%s{\"type\":\"neighbor\",\"ran\":\"%s\",\"target_type\":\"%s\","
			"\"target\":\"%s\",\"cells\":%s}",
			*first ? "" : ",",
			ran, target_type, target, cells);
		*json = talloc_asprintf_append(*json, "%s", entry ? entry : "");
		*first = false;
	}
}

static char *api_json_links(void *ctx, struct gsm_network *net)
{
	char *json;
	bool first = true;
	char *gsup_host;
	char *ipa_name = NULL;

	json = talloc_strdup(ctx, "{\"links\":[");
	if (!json)
		return NULL;

	if (net->a.sri)
		api_json_append_ran_peers(ctx, &json, &first, net->a.sri);
	if (net->iu.sri)
		api_json_append_ran_peers(ctx, &json, &first, net->iu.sri);
	api_json_append_neighbors(ctx, &json, &first, net);

	gsup_host = json_escape(ctx, net->gsup_server_addr_str ? net->gsup_server_addr_str : "");
	if (net->msc_ipa_name && net->msc_ipa_name[0])
		ipa_name = json_escape(ctx, net->msc_ipa_name);

	json = talloc_asprintf_append(json,
		"],\"services\":{\"gsup_hlr\":{\"host\":\"%s\",\"port\":%u}",
		gsup_host, net->gsup_server_port);
	if (ipa_name)
		json = talloc_asprintf_append(json, ",\"msc_ipa_name\":\"%s\"", ipa_name);
	json = talloc_asprintf_append(json, "}}");
	return json;
}

static int api_disconnect_subscriber(struct gsm_network *net, const char *id)
{
	struct vlr_subscr *vsub;
	struct msc_a *msc_a;

	vsub = api_find_vsub(net, id);
	if (!vsub)
		return -ENOENT;

	LOGP(DMSC, LOGL_NOTICE, "API disconnecting subscriber %s\n", vlr_subscr_name(vsub));
	msc_a = msc_a_for_vsub(vsub, false);
	if (msc_a)
		msc_a_release_cn(msc_a);
	if (vlr_subscr_expire(vsub))
		LOGP(DMSC, LOGL_NOTICE, "API released subscriber %s\n", vlr_subscr_name(vsub));

	vlr_subscr_put(vsub, VSUB_USE_API);
	return 0;
}

static int api_disconnect_call(struct gsm_network *net, const char *callref_str)
{
	struct gsm_trans *trans;
	uint32_t callref;
	char *endptr;

	callref = strtoul(callref_str, &endptr, 0);
	if (endptr == callref_str || *endptr != '\0')
		return -EINVAL;

	trans = trans_find_by_callref(net, TRANS_CC, callref);
	if (!trans)
		return -ENOENT;

	LOGP(DMSC, LOGL_NOTICE, "API disconnecting call callref=0x%08x\n", callref);
	trans_free(trans);
	return 0;
}

/* ---- per-IMSI debug trace ----
 *
 * Mirrors the `logging filter imsi IMSI` VTY command, but driven over the API:
 * a dedicated file log target is created per IMSI, set to DEBUG, and pinned to
 * the subscriber via the VLR filter. The daemon's filter_fn() (msc_main.c) then
 * routes only that subscriber's log lines into the file. This is the OsmoMSC
 * equivalent of the Open5GS MME/SGW-C/SMF IMSI trace.
 */

#define VSUB_USE_API_TRACE "API-trace"

static bool api_valid_imsi(const char *imsi)
{
	const char *p;
	size_t n = 0;

	if (!imsi || !imsi[0])
		return false;
	for (p = imsi; *p; p++, n++) {
		if (*p < '0' || *p > '9')
			return false;
	}
	return n >= 5 && n <= 15;
}

static struct msc_api_trace *api_trace_find(struct msc_api_state *api, const char *imsi)
{
	struct msc_api_trace *t;

	llist_for_each_entry(t, &api->traces, entry) {
		if (!strcmp(t->imsi, imsi))
			return t;
	}
	return NULL;
}

static char *api_json_trace(void *ctx, const struct msc_api_trace *t, const char *status)
{
	char *imsi = json_escape(ctx, t->imsi);

	return talloc_asprintf(ctx,
		"{\"status\":\"%s\",\"imsi\":\"%s\",\"output\":\"journal\",\"level\":\"debug\"}",
		status, imsi);
}

/* Returns 0 on success (and *out_trace set), negative errno otherwise. */
static int api_trace_enable(struct msc_api_state *api, const char *imsi,
			    struct msc_api_trace **out_trace)
{
	struct gsm_network *net = api->net;
	struct vlr_subscr *vsub;
	struct msc_api_trace *t;
	struct log_target *tgt;

	t = api_trace_find(api, imsi);
	if (t) {
		/* idempotent: trace already running for this IMSI */
		*out_trace = t;
		return 0;
	}

	/* the VLR filter pins to a live subscriber object (pointer identity),
	 * so the subscriber must already be known to the VLR. */
	vsub = vlr_subscr_find_by_imsi(net->vlr, imsi, VSUB_USE_API_TRACE);
	if (!vsub)
		return -ENOENT;

	t = talloc_zero(api, struct msc_api_trace);
	if (!t) {
		vlr_subscr_put(vsub, VSUB_USE_API_TRACE);
		return -ENOMEM;
	}
	osmo_strlcpy(t->imsi, imsi, sizeof(t->imsi));

	/* emit to stderr so the daemon's journald unit captures the lines */
	tgt = log_target_create_stderr();
	if (!tgt) {
		vlr_subscr_put(vsub, VSUB_USE_API_TRACE);
		talloc_free(t);
		return -EIO;
	}

	log_set_log_level(tgt, LOGL_DEBUG);
	log_set_use_color(tgt, 0);
	log_set_print_category(tgt, 1);
	log_set_print_category_hex(tgt, 0);
	log_set_print_level(tgt, 1);
	log_set_print_extended_timestamp(tgt, 1);
	/* pin to this subscriber; the daemon filter_fn() does the matching */
	log_set_filter_vlr_subscr(tgt, vsub);
	log_add_target(tgt);

	t->target = tgt;
	llist_add_tail(&t->entry, &api->traces);

	/* log_set_filter_vlr_subscr took its own ref; drop ours */
	vlr_subscr_put(vsub, VSUB_USE_API_TRACE);

	LOGP(DMSC, LOGL_NOTICE, "API enabled IMSI debug trace for %s (-> journal)\n", imsi);
	*out_trace = t;
	return 0;
}

static int api_trace_disable(struct msc_api_state *api, const char *imsi)
{
	struct msc_api_trace *t = api_trace_find(api, imsi);

	if (!t)
		return -ENOENT;

	llist_del(&t->entry);
	if (t->target) {
		log_set_filter_vlr_subscr(t->target, NULL);
		log_target_destroy(t->target);
	}
	LOGP(DMSC, LOGL_NOTICE, "API disabled IMSI debug trace for %s\n", imsi);
	talloc_free(t);
	return 0;
}

static char *api_json_trace_list(void *ctx, struct msc_api_state *api)
{
	struct msc_api_trace *t;
	char *json = talloc_strdup(ctx, "{\"traces\":[");
	bool first = true;

	if (!json)
		return NULL;

	llist_for_each_entry(t, &api->traces, entry) {
		char *e = api_json_trace(ctx, t, "active");

		json = talloc_asprintf_append(json, "%s%s", first ? "" : ",",
					      e ? e : "");
		first = false;
	}
	return talloc_asprintf_append(json, "]}");
}

/* Handle /api/trace[...]. Returns true if the path was a trace route. */
static bool api_handle_trace(struct msc_api_conn *conn, struct msc_api_state *api,
			     const char *method, const char *path)
{
	void *ctx = conn;
	char *json;
	int rc;

	if (!strcmp(method, "GET") && !strcmp(path, "/api/trace")) {
		json = api_json_trace_list(ctx, api);
		api_send_response(conn->srv, 200, "OK", json);
		return true;
	}

	if (strncmp(path, "/api/trace/", 11) != 0)
		return false;

	const char *imsi = path + 11;
	struct msc_api_trace *t;

	if (!api_valid_imsi(imsi)) {
		api_send_response(conn->srv, 400, "Bad Request",
				  "{\"error\":\"invalid IMSI\"}");
		return true;
	}

	if (!strcmp(method, "POST") || !strcmp(method, "PUT")) {
		rc = api_trace_enable(api, imsi, &t);
		if (rc == -ENOENT) {
			api_send_response(conn->srv, 404, "Not Found",
					  "{\"error\":\"subscriber not known to VLR; "
					  "trace can only be attached to an attached subscriber\"}");
			return true;
		}
		if (rc < 0) {
			api_send_response(conn->srv, 500, "Internal Server Error",
					  "{\"error\":\"failed to enable trace\"}");
			return true;
		}
		json = api_json_trace(ctx, t, "enabled");
		api_send_response(conn->srv, 200, "OK", json);
		return true;
	}

	if (!strcmp(method, "DELETE")) {
		rc = api_trace_disable(api, imsi);
		if (rc == -ENOENT) {
			api_send_response(conn->srv, 404, "Not Found",
					  "{\"error\":\"no active trace for this IMSI\"}");
			return true;
		}
		json = talloc_asprintf(ctx, "{\"status\":\"disabled\",\"imsi\":\"%s\"}",
				       json_escape(ctx, imsi));
		api_send_response(conn->srv, 200, "OK", json);
		return true;
	}

	if (!strcmp(method, "GET")) {
		t = api_trace_find(api, imsi);
		if (!t) {
			api_send_response(conn->srv, 404, "Not Found",
					  "{\"error\":\"no active trace for this IMSI\"}");
			return true;
		}
		json = api_json_trace(ctx, t, "active");
		api_send_response(conn->srv, 200, "OK", json);
		return true;
	}

	return false;
}

static void api_handle_request(struct msc_api_conn *conn)
{
	void *ctx = conn;
	struct msc_api_state *api = g_msc_api;
	struct gsm_network *net = api->net;
	char method[16], path[256], query[256];
	char query_imsi[64] = "";
	const char *filter_imsi = NULL;
	char *auth, *api_token;
	char *json = NULL;
	int rc = 0;

	api_parse_request(conn->buf, method, sizeof(method), path, sizeof(path));
	api_path_strip_query(path, query, sizeof(query));
	if (api_query_get(query, "imsi", query_imsi, sizeof(query_imsi)))
		filter_imsi = query_imsi;

	auth = api_header_value_trimmed(ctx, conn->buf, "Authorization");
	api_token = api_header_value_trimmed(ctx, conn->buf, "X-Api-Token");

	if (!api_token_valid(api, auth, api_token)) {
		api_send_response(conn->srv, 401, "Unauthorized",
				  "{\"error\":\"invalid or missing API token\"}");
		return;
	}

	if (!strcmp(method, "GET") && !strcmp(path, "/api/stats")) {
		json = api_json_stats(ctx, net);
		api_send_response(conn->srv, 200, "OK", json);
		return;
	}

	if (!strcmp(method, "GET") && !strcmp(path, "/api/subscribers/online/count")) {
		json = api_json_count(ctx, api_count_subscribers_online(net, filter_imsi));
		api_send_response(conn->srv, 200, "OK", json);
		return;
	}

	if (!strcmp(method, "GET") && !strcmp(path, "/api/subscribers/online")) {
		json = api_json_subscribers_online(ctx, net, filter_imsi);
		api_reply_json(conn->srv, json);
		return;
	}

	if (!strcmp(method, "GET") && !strcmp(path, "/api/calls/active/count")) {
		json = api_json_count(ctx, api_count_active_calls(net, filter_imsi));
		api_send_response(conn->srv, 200, "OK", json);
		return;
	}

	if (!strcmp(method, "GET") && !strcmp(path, "/api/calls/active")) {
		json = api_json_active_calls(ctx, net, filter_imsi);
		api_reply_json(conn->srv, json);
		return;
	}

	if (api_handle_trace(conn, api, method, path))
		return;

	if (!strcmp(method, "GET") && !strcmp(path, "/api/links/count")) {
		json = api_json_count(ctx, api_count_links(net));
		api_send_response(conn->srv, 200, "OK", json);
		return;
	}

	if (!strcmp(method, "GET") && !strcmp(path, "/api/links")) {
		json = api_json_links(ctx, net);
		api_reply_json(conn->srv, json);
		return;
	}

	if (!strcmp(method, "GET") && !strncmp(path, "/api/subscribers/", 17)) {
		const char *rest = path + 17;
		char id[64];

		if (api_subscriber_path_id(rest, "/calls/active/count", id, sizeof(id))) {
			json = api_json_count(ctx, api_count_active_calls(net, id));
			api_send_response(conn->srv, 200, "OK", json);
			return;
		}
		if (api_subscriber_path_id(rest, "/calls/active", id, sizeof(id))) {
			json = api_json_active_calls(ctx, net, id);
			api_reply_json(conn->srv, json);
			return;
		}
		if (api_subscriber_path_id(rest, "/online/count", id, sizeof(id))) {
			json = api_json_count(ctx, api_subscriber_is_online(net, id) ? 1 : 0);
			api_send_response(conn->srv, 200, "OK", json);
			return;
		}
		if (api_subscriber_path_id(rest, "/online", id, sizeof(id))) {
			json = api_json_subscribers_online(ctx, net, id);
			api_reply_json(conn->srv, json);
			return;
		}
		if (api_subscriber_path_id(rest, "/detail/count", id, sizeof(id))) {
			json = api_json_count(ctx, api_subscriber_is_online(net, id) ? 1 : 0);
			api_send_response(conn->srv, 200, "OK", json);
			return;
		}
		if (api_subscriber_path_id(rest, "/detail", id, sizeof(id))) {
			json = api_json_subscriber_detail(ctx, net, id);
			if (!json) {
				api_send_response(conn->srv, 404, "Not Found",
						  "{\"error\":\"subscriber not found\"}");
				return;
			}
			api_send_response(conn->srv, 200, "OK", json);
			return;
		}
	}

	if (!strcmp(method, "DELETE") && !strncmp(path, "/api/subscribers/", 17)) {
		const char *id = path + 17;

		if (!api_valid_subscriber_id(id)) {
			api_send_response(conn->srv, 400, "Bad Request",
					  "{\"error\":\"invalid subscriber identifier\"}");
			return;
		}

		rc = api_disconnect_subscriber(net, id);
		if (rc == -ENOENT) {
			api_send_response(conn->srv, 404, "Not Found",
					  "{\"error\":\"subscriber not found\"}");
			return;
		}
		json = talloc_asprintf(ctx, "{\"status\":\"disconnected\",\"id\":\"%s\"}",
				       json_escape(ctx, id));
		api_send_response(conn->srv, 200, "OK", json);
		return;
	}

	if (!strcmp(method, "DELETE") && !strncmp(path, "/api/calls/", 11)) {
		const char *rest = path + 11;
		const char *suffix = "/disconnect";
		char callref[32];

		if (strlen(rest) <= strlen(suffix) || strcmp(rest + strlen(rest) - strlen(suffix), suffix))
			goto not_found;

		osmo_strlcpy(callref, rest, OSMO_MIN(sizeof(callref), strlen(rest) - strlen(suffix) + 1));
		rc = api_disconnect_call(net, callref);
		if (rc == -ENOENT) {
			api_send_response(conn->srv, 404, "Not Found",
					  "{\"error\":\"call not found\"}");
			return;
		}
		if (rc == -EINVAL) {
			api_send_response(conn->srv, 400, "Bad Request",
					  "{\"error\":\"invalid callref\"}");
			return;
		}
		json = talloc_asprintf(ctx, "{\"status\":\"disconnected\",\"callref\":\"%s\"}",
				       json_escape(ctx, callref));
		api_send_response(conn->srv, 200, "OK", json);
		return;
	}

not_found:
	api_send_response(conn->srv, 404, "Not Found", "{\"error\":\"not found\"}");
}

static void api_conn_free(struct msc_api_conn *conn)
{
	if (!conn)
		return;
	talloc_free(conn->buf);
	talloc_free(conn);
}

static void api_conn_detach(struct osmo_stream_srv *srv)
{
	struct msc_api_conn *conn = osmo_stream_srv_get_data(srv);

	if (!conn)
		return;
	osmo_stream_srv_set_data(srv, NULL);
	api_conn_free(conn);
}

static void api_conn_close(struct osmo_stream_srv *srv)
{
	struct msc_api_conn *conn;

	if (!srv)
		return;

	conn = osmo_stream_srv_get_data(srv);
	if (conn) {
		if (conn->closed)
			return;
		conn->closed = true;
		osmo_stream_srv_set_data(srv, NULL);
		api_conn_free(conn);
	}
	osmo_stream_srv_set_flush_and_destroy(srv);
}

static int api_conn_read_cb(struct osmo_stream_srv *srv, int res, struct msgb *msg)
{
	struct msc_api_conn *conn = osmo_stream_srv_get_data(srv);
	const unsigned char *data;
	size_t data_len;

	if (res <= 0 || !msg) {
		api_conn_close(srv);
		return res;
	}

	if (!conn) {
		msgb_free(msg);
		api_conn_close(srv);
		return -1;
	}

	data = msgb_data(msg);
	data_len = msgb_length(msg);

	if (conn->buf_len + data_len > MSC_API_MAX_REQUEST) {
		api_send_response(srv, 413, "Payload Too Large",
				  "{\"error\":\"request too large\"}");
		api_conn_close(srv);
		msgb_free(msg);
		return -1;
	}

	conn->buf = talloc_realloc(conn, conn->buf, char, conn->buf_len + data_len + 1);
	memcpy(conn->buf + conn->buf_len, data, data_len);
	conn->buf_len += data_len;
	conn->buf[conn->buf_len] = '\0';
	msgb_free(msg);

	if (!strstr(conn->buf, "\r\n\r\n"))
		return 0;

	api_handle_request(conn);
	api_conn_close(srv);
	return 0;
}

static int api_conn_closed_cb(struct osmo_stream_srv *srv)
{
	api_conn_detach(srv);
	return 0;
}

static int api_accept_cb(struct osmo_stream_srv_link *link, int fd)
{
	struct msc_api_state *api = osmo_stream_srv_link_get_data(link);
	struct msc_api_conn *conn;
	struct osmo_stream_srv *srv;

	conn = talloc_zero(api, struct msc_api_conn);
	if (!conn)
		return -ENOMEM;

	srv = osmo_stream_srv_create2(api, link, fd, conn);
	if (!srv) {
		talloc_free(conn);
		return -ENOMEM;
	}

	conn->srv = srv;
	osmo_stream_srv_set_name(srv, "msc-api");
	osmo_stream_srv_set_read_cb(srv, api_conn_read_cb);
	osmo_stream_srv_set_closed_cb(srv, api_conn_closed_cb);
	osmo_stream_srv_set_data(srv, conn);
	return 0;
}

struct msc_api_state *msc_api_alloc(void *ctx, struct gsm_network *net)
{
	struct msc_api_state *api;

	api = talloc_zero(ctx, struct msc_api_state);
	if (!api)
		return NULL;

	api->net = net;
	api->cfg.port = MSC_API_PORT_DEFAULT;
	INIT_LLIST_HEAD(&api->traces);
	osmo_strlcpy(api->cfg.bind_addr, MSC_API_BIND_DEFAULT, sizeof(api->cfg.bind_addr));
	api->srv_link = osmo_stream_srv_link_create(api);
	if (!api->srv_link) {
		talloc_free(api);
		return NULL;
	}

	osmo_stream_srv_link_set_nodelay(api->srv_link, true);
	osmo_stream_srv_link_set_addr(api->srv_link, api->cfg.bind_addr);
	osmo_stream_srv_link_set_port(api->srv_link, api->cfg.port);
	osmo_stream_srv_link_set_data(api->srv_link, api);
	osmo_stream_srv_link_set_accept_cb(api->srv_link, api_accept_cb);

	g_msc_api = api;
	net->api = api;
	return api;
}

bool msc_api_configured(const struct msc_api_state *api)
{
	return api && api->cfg.token[0] != '\0';
}

void msc_api_close(struct msc_api_state *api)
{
	if (!api || !api->srv_link)
		return;
	osmo_stream_srv_link_close(api->srv_link);
}

int msc_api_open(struct msc_api_state *api)
{
	struct osmo_fd *ofd;
	int rc;

	if (!api)
		return -EINVAL;

	msc_api_close(api);

	if (!msc_api_configured(api)) {
		LOGP(DMSC, LOGL_INFO, "HTTP API disabled (no token configured)\n");
		return 0;
	}

	osmo_stream_srv_link_set_addr(api->srv_link, api->cfg.bind_addr);
	osmo_stream_srv_link_set_port(api->srv_link, api->cfg.port);

	rc = osmo_stream_srv_link_open(api->srv_link);
	if (rc < 0) {
		LOGP(DMSC, LOGL_ERROR, "HTTP API socket cannot be opened: %s\n", strerror(errno));
		return -EINVAL;
	}

	ofd = osmo_stream_srv_link_get_ofd(api->srv_link);
	LOGP(DMSC, LOGL_NOTICE, "HTTP API listening on %s (token auth required)\n",
	     osmo_sock_get_name2(ofd->fd));
	return 0;
}

/* ---- VTY configuration ---- */

struct cmd_node cfg_api_node = {
	CFG_API_NODE,
	"%s(config-api)# ",
	1,
};

int msc_api_config_write(struct vty *vty)
{
	struct msc_api_state *api = g_msc_api;

	if (!api || !msc_api_configured(api))
		return CMD_SUCCESS;

	vty_out(vty, " api%s", VTY_NEWLINE);
	vty_out(vty, "  bind-ip %s%s", api->cfg.bind_addr, VTY_NEWLINE);
	if (api->cfg.port != MSC_API_PORT_DEFAULT)
		vty_out(vty, "  port %u%s", api->cfg.port, VTY_NEWLINE);
	vty_out(vty, "  token %s%s", api->cfg.token, VTY_NEWLINE);
	return CMD_SUCCESS;
}

DEFUN(cfg_msc_api, cfg_msc_api_cmd,
      "api",
      "Configure the embedded HTTP/JSON API\n")
{
	vty->node = CFG_API_NODE;
	return CMD_SUCCESS;
}

DEFUN(cfg_api_bind_ip, cfg_api_bind_ip_cmd,
      "bind-ip A.B.C.D",
      "Set the HTTP API bind address\n"
      "IP address to bind\n")
{
	struct msc_api_state *api = g_msc_api;
	int rc;

	osmo_strlcpy(api->cfg.bind_addr, argv[0], sizeof(api->cfg.bind_addr));
	if (vty->type != VTY_FILE) {
		rc = msc_api_open(api);
		if (rc < 0)
			return CMD_WARNING;
	}
	return CMD_SUCCESS;
}

DEFUN(cfg_api_port, cfg_api_port_cmd,
      "port <1-65535>",
      "Set the HTTP API TCP port\n"
      "TCP port number\n")
{
	struct msc_api_state *api = g_msc_api;
	int rc;

	api->cfg.port = atoi(argv[0]);
	if (vty->type != VTY_FILE) {
		rc = msc_api_open(api);
		if (rc < 0)
			return CMD_WARNING;
	}
	return CMD_SUCCESS;
}

DEFUN(cfg_api_token, cfg_api_token_cmd,
      "token .LINE",
      "Set the HTTP API access token (required to enable the API)\n"
      "Shared secret token sent as 'Authorization: Bearer TOKEN' or 'X-Api-Token: TOKEN'\n")
{
	struct msc_api_state *api = g_msc_api;
	int rc;

	osmo_strlcpy(api->cfg.token, argv[0], sizeof(api->cfg.token));
	if (vty->type != VTY_FILE) {
		rc = msc_api_open(api);
		if (rc < 0)
			return CMD_WARNING;
	}
	return CMD_SUCCESS;
}

DEFUN(cfg_api_no_token, cfg_api_no_token_cmd,
      "no token",
      NO_STR
      "Disable the HTTP API by removing the access token\n")
{
	struct msc_api_state *api = g_msc_api;

	api->cfg.token[0] = '\0';
	if (vty->type != VTY_FILE)
		msc_api_close(api);
	return CMD_SUCCESS;
}

void msc_api_vty_init(void)
{
	install_element(MSC_NODE, &cfg_msc_api_cmd);
	install_node(&cfg_api_node, NULL);
	install_element(CFG_API_NODE, &cfg_api_bind_ip_cmd);
	install_element(CFG_API_NODE, &cfg_api_port_cmd);
	install_element(CFG_API_NODE, &cfg_api_token_cmd);
	install_element(CFG_API_NODE, &cfg_api_no_token_cmd);
}
