/*
 * OsmoMSC embedded HTTP/JSON API.
 *
 * (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <osmocom/core/msgb.h>
#include <osmocom/core/select.h>
#include <osmocom/core/socket.h>
#include <osmocom/core/utils.h>
#include <osmocom/gsm/gsm23003.h>
#include <osmocom/gsm/protocol/gsm_04_08.h>
#include <osmocom/msc/debug.h>
#include <osmocom/msc/gsm_data.h>
#include <osmocom/msc/msc_a.h>
#include <osmocom/msc/msc_api.h>
#include <osmocom/msc/msub.h>
#include <osmocom/msc/transaction.h>
#include <osmocom/msc/vty.h>
#include <osmocom/vty/vty.h>
#include <osmocom/netif/stream.h>
#include <osmocom/vlr/vlr.h>

#define VSUB_USE_API "API"
#define MSC_API_MAX_REQUEST 8192

struct msc_api_conn {
	struct osmo_stream_srv *srv;
	char *buf;
	size_t buf_len;
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

static void api_append_msgb_str(struct msgb *msg, const char *str)
{
	size_t len = strlen(str);
	memcpy(msgb_put(msg, len), str, len);
}

static int api_send_response(struct osmo_stream_srv *conn, int status,
			     const char *status_text, const char *json_body)
{
	struct msgb *msg;
	const char *body = json_body ? json_body : "";
	size_t body_len = strlen(body);
	char hdr[256];

	snprintf(hdr, sizeof(hdr),
		 "HTTP/1.0 %d %s\r\n"
		 "Content-Type: application/json\r\n"
		 "Content-Length: %zu\r\n"
		 "Connection: close\r\n"
		 "\r\n",
		 status, status_text, body_len);

	msg = msgb_alloc(strlen(hdr) + body_len + 1, "msc_api_http");
	if (!msg)
		return -ENOMEM;

	api_append_msgb_str(msg, hdr);
	api_append_msgb_str(msg, body);
	osmo_stream_srv_send(conn, msg);
	return 0;
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

static char *api_json_subscribers_online(void *ctx, struct gsm_network *net)
{
	struct vlr_subscr *vsub;
	char *json;
	bool first = true;

	json = talloc_strdup(ctx, "{\"subscribers\":[");
	if (!json || !net || !net->vlr)
		return talloc_asprintf(ctx, "{\"subscribers\":[]}");

	llist_for_each_entry(vsub, &net->vlr->subscribers, list) {
		struct msc_a *msc_a;
		char *imsi, *msisdn, *ran;
		char *entry;

		if (!vsub->lu_complete)
			continue;

		msc_a = msc_a_for_vsub(vsub, true);
		imsi = json_escape(ctx, vsub->imsi);
		msisdn = json_escape(ctx, vsub->msisdn);
		ran = json_escape(ctx, osmo_rat_type_name(vsub->cs.attached_via_ran));

		entry = talloc_asprintf(ctx,
			"%s{\"imsi\":\"%s\",\"msisdn\":\"%s\",\"tmsi\":\"%08X\","
			"\"lac\":%u,\"ran\":\"%s\",\"connected\":%s}",
			first ? "" : ",",
			imsi, msisdn,
			vsub->tmsi != GSM_RESERVED_TMSI ? vsub->tmsi : 0,
			vsub->cgi.lai.lac, ran,
			msc_a ? "true" : "false");
		json = talloc_asprintf_append(json, "%s", entry ? entry : "");
		first = false;
	}

	json = talloc_asprintf_append(json, "]}");
	return json;
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

	json = talloc_asprintf_append(json, "}");
	vlr_subscr_put(vsub, VSUB_USE_API);
	return json;
}

static char *api_json_active_calls(void *ctx, struct gsm_network *net)
{
	struct gsm_trans *trans;
	char *json;
	bool first = true;

	json = talloc_strdup(ctx, "{\"calls\":[");
	if (!json)
		return NULL;

	llist_for_each_entry(trans, &net->trans_list, entry) {
		const char *imsi = "";
		const char *msisdn = "";
		char *eimsi, *emsisdn, *state;
		char *entry;

		if (trans->type != TRANS_CC)
			continue;
		if (trans->cc.state != GSM_CSTATE_ACTIVE)
			continue;

		if (trans->vsub) {
			imsi = trans->vsub->imsi;
			msisdn = trans->vsub->msisdn;
		}

		eimsi = json_escape(ctx, imsi);
		emsisdn = json_escape(ctx, msisdn);
		state = json_escape(ctx, gsm48_cc_state_name(trans->cc.state));

		entry = talloc_asprintf(ctx,
			"%s{\"callref\":\"0x%08x\",\"imsi\":\"%s\",\"msisdn\":\"%s\","
			"\"direction\":\"%s\",\"state\":\"%s\",\"transaction_id\":%u}",
			first ? "" : ",",
			trans->callref, eimsi, emsisdn,
			(trans->transaction_id & 0x08) ? "MO" : "MT",
			state, trans->transaction_id);
		json = talloc_asprintf_append(json, "%s", entry ? entry : "");
		first = false;
	}

	return talloc_asprintf_append(json, "]}");
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

static void api_handle_request(struct msc_api_conn *conn)
{
	void *ctx = conn;
	struct msc_api_state *api = g_msc_api;
	struct gsm_network *net = api->net;
	char method[16], path[256];
	char *auth, *api_token;
	char *json = NULL;
	int rc = 0;

	api_parse_request(conn->buf, method, sizeof(method), path, sizeof(path));
	auth = api_header_value_trimmed(ctx, conn->buf, "Authorization");
	api_token = api_header_value_trimmed(ctx, conn->buf, "X-Api-Token");

	if (!api_token_valid(api, auth, api_token)) {
		api_send_response(conn->srv, 401, "Unauthorized",
				  "{\"error\":\"invalid or missing API token\"}");
		return;
	}

	if (!strcmp(method, "GET") && !strcmp(path, "/api/subscribers/online")) {
		json = api_json_subscribers_online(ctx, net);
		api_send_response(conn->srv, 200, "OK", json);
		return;
	}

	if (!strcmp(method, "GET") && !strcmp(path, "/api/calls/active")) {
		json = api_json_active_calls(ctx, net);
		api_send_response(conn->srv, 200, "OK", json);
		return;
	}

	if (!strcmp(method, "GET") && !strncmp(path, "/api/subscribers/", 17)) {
		const char *rest = path + 17;
		const char *suffix = "/detail";
		char id[64];

		if (strlen(rest) <= strlen(suffix) || strcmp(rest + strlen(rest) - strlen(suffix), suffix))
			goto not_found;

		osmo_strlcpy(id, rest, OSMO_MIN(sizeof(id), strlen(rest) - strlen(suffix) + 1));
		if (!api_valid_subscriber_id(id)) {
			api_send_response(conn->srv, 400, "Bad Request",
					  "{\"error\":\"invalid subscriber identifier\"}");
			return;
		}

		json = api_json_subscriber_detail(ctx, net, id);
		if (!json) {
			api_send_response(conn->srv, 404, "Not Found",
					  "{\"error\":\"subscriber not found\"}");
			return;
		}
		api_send_response(conn->srv, 200, "OK", json);
		return;
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

static int api_conn_read_cb(struct osmo_stream_srv *srv, int res, struct msgb *msg)
{
	struct msc_api_conn *conn = osmo_stream_srv_get_data(srv);
	const unsigned char *data;
	size_t data_len;

	if (res <= 0 || !msg) {
		api_conn_detach(srv);
		return res;
	}

	data = msgb_data(msg);
	data_len = msgb_length(msg);

	if (conn->buf_len + data_len > MSC_API_MAX_REQUEST) {
		api_send_response(srv, 413, "Payload Too Large",
				  "{\"error\":\"request too large\"}");
		api_conn_detach(srv);
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
	api_conn_detach(srv);
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
