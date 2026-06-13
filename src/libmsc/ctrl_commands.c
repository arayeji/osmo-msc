/*
 * (C) 2014 by Holger Hans Peter Freyther
 * (C) 2014 by sysmocom s.f.m.c. GmbH
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <osmocom/ctrl/control_cmd.h>
#include <osmocom/core/utils.h>
#include <osmocom/gsm/gsm23003.h>
#include <osmocom/gsm/protocol/gsm_04_08.h>
#include <osmocom/msc/gsm_data.h>
#include <osmocom/msc/gsm_subscriber.h>
#include <osmocom/msc/db.h>
#include <osmocom/msc/debug.h>
#include <osmocom/msc/msub.h>
#include <osmocom/msc/msc_a.h>
#include <osmocom/msc/transaction.h>
#include <osmocom/vlr/vlr.h>

#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define VSUB_USE_CTRL "CTRL"

static struct gsm_network *msc_ctrl_net = NULL;

static struct vlr_subscr *ctrl_find_vsub(const char *id)
{
	struct vlr_subscr *vsub;

	if (!msc_ctrl_net || !msc_ctrl_net->vlr || !id || !id[0])
		return NULL;

	vsub = vlr_subscr_find_by_imsi(msc_ctrl_net->vlr, id, VSUB_USE_CTRL);
	if (vsub)
		return vsub;

	return vlr_subscr_find_by_msisdn(msc_ctrl_net->vlr, id, VSUB_USE_CTRL);
}

static const char *ctrl_variable_suffix(const char *variable, const char *prefix)
{
	size_t prefix_len;

	if (!variable || !prefix)
		return NULL;

	prefix_len = strlen(prefix);
	if (strncmp(variable, prefix, prefix_len) != 0)
		return NULL;
	if (variable[prefix_len] != '.')
		return NULL;
	if (!variable[prefix_len + 1])
		return NULL;

	return variable + prefix_len + 1;
}

static void ctrl_append_kv(struct ctrl_cmd *cmd, const char *key, const char *fmt, ...)
{
	va_list ap;
	char *line;

	va_start(ap, fmt);
	line = talloc_vasprintf(cmd, fmt, ap);
	va_end(ap);

	if (!line)
		return;

	cmd->reply = talloc_asprintf_append(cmd->reply, "%s=%s\n", key, line);
	talloc_free(line);
}

static int get_subscriber_list(struct ctrl_cmd *cmd, void *d)
{
	struct vlr_subscr *vsub;

	if (!msc_ctrl_net) {
		cmd->reply = "MSC CTRL commands not initialized";
		return CTRL_CMD_ERROR;
	}

	if (!msc_ctrl_net->vlr) {
		cmd->reply = "VLR not initialized";
		return CTRL_CMD_ERROR;
	}

	cmd->reply = talloc_strdup(cmd, "");

	llist_for_each_entry(vsub, &msc_ctrl_net->vlr->subscribers, list) {
		/* Do not list subscribers that aren't successfully attached. */
		if (!vsub->lu_complete)
			continue;
		cmd->reply = talloc_asprintf_append(cmd->reply, "%s,%s\n",
						    vsub->imsi, vsub->msisdn);
	}
	return CTRL_CMD_REPLY;
}
CTRL_CMD_DEFINE_RO(subscriber_list, "subscriber-list-active-v1");

static int get_subscriber_list_v2(struct ctrl_cmd *cmd, void *d)
{
	struct vlr_subscr *vsub;

	if (!msc_ctrl_net) {
		cmd->reply = "MSC CTRL commands not initialized";
		return CTRL_CMD_ERROR;
	}

	if (!msc_ctrl_net->vlr) {
		cmd->reply = "VLR not initialized";
		return CTRL_CMD_ERROR;
	}

	cmd->reply = talloc_strdup(cmd, "");

	llist_for_each_entry(vsub, &msc_ctrl_net->vlr->subscribers, list) {
		struct msc_a *msc_a;
		bool connected;

		if (!vsub->lu_complete)
			continue;

		msc_a = msc_a_for_vsub(vsub, true);
		connected = msc_a != NULL;

		cmd->reply = talloc_asprintf_append(cmd->reply,
			"%s,%s,%08X,%u,%s,%u\n",
			vsub->imsi,
			vsub->msisdn,
			vsub->tmsi != GSM_RESERVED_TMSI ? vsub->tmsi : 0,
			vsub->cgi.lai.lac,
			osmo_rat_type_name(vsub->cs.attached_via_ran),
			connected ? 1 : 0);
	}
	return CTRL_CMD_REPLY;
}
CTRL_CMD_DEFINE_RO(subscriber_list_v2, "subscriber-list-active-v2");

static int get_subscriber_detail(struct ctrl_cmd *cmd, void *d)
{
	const char *id;
	struct vlr_subscr *vsub;
	struct msc_a *msc_a;
	struct gsm_trans *trans;
	unsigned int active_calls = 0;

	if (!msc_ctrl_net) {
		cmd->reply = "MSC CTRL commands not initialized";
		return CTRL_CMD_ERROR;
	}

	id = ctrl_variable_suffix(cmd->variable, "subscriber-detail-v1");
	if (!id) {
		cmd->reply = "Usage: GET subscriber-detail-v1.<IMSI-or-MSISDN>";
		return CTRL_CMD_ERROR;
	}

	vsub = ctrl_find_vsub(id);
	if (!vsub) {
		cmd->reply = "Subscriber not found";
		return CTRL_CMD_ERROR;
	}

	msc_a = msc_a_for_vsub(vsub, true);

	llist_for_each_entry(trans, &msc_ctrl_net->trans_list, entry) {
		if (trans->vsub != vsub)
			continue;
		if (trans->type == TRANS_CC && trans->cc.state == GSM_CSTATE_ACTIVE)
			active_calls++;
	}

	cmd->reply = talloc_strdup(cmd, "");
	ctrl_append_kv(cmd, "imsi", "%s", vsub->imsi);
	ctrl_append_kv(cmd, "msisdn", "%s", vsub->msisdn[0] ? vsub->msisdn : "");
	ctrl_append_kv(cmd, "imei", "%s", vsub->imei[0] ? vsub->imei : "");
	ctrl_append_kv(cmd, "tmsi", "%08X", vsub->tmsi != GSM_RESERVED_TMSI ? vsub->tmsi : 0);
	ctrl_append_kv(cmd, "lac", "%u", vsub->cgi.lai.lac);
	ctrl_append_kv(cmd, "cell_id", "%u", vsub->cgi.cell_identity);
	ctrl_append_kv(cmd, "ran", "%s", osmo_rat_type_name(vsub->cs.attached_via_ran));
	ctrl_append_kv(cmd, "lu_complete", "%u", vsub->lu_complete ? 1 : 0);
	ctrl_append_kv(cmd, "connected", "%u", msc_a ? 1 : 0);
	ctrl_append_kv(cmd, "paging", "%u", vsub->cs.is_paging ? 1 : 0);
	ctrl_append_kv(cmd, "active_calls", "%u", active_calls);
	if (msc_a) {
		ctrl_append_kv(cmd, "conn_state", "%s", osmo_fsm_inst_state_name(msc_a->c.fi));
		ctrl_append_kv(cmd, "conn_lac", "%u", msc_a->via_cell.lai.lac);
		ctrl_append_kv(cmd, "conn_cell_id", "%u", msc_a->via_cell.cell_identity);
	}

	vlr_subscr_put(vsub, VSUB_USE_CTRL);
	return CTRL_CMD_REPLY;
}
CTRL_CMD_DEFINE_RO(subscriber_detail, "subscriber-detail-v1 *");

static int get_active_call_list(struct ctrl_cmd *cmd, void *d)
{
	struct gsm_trans *trans;

	if (!msc_ctrl_net) {
		cmd->reply = "MSC CTRL commands not initialized";
		return CTRL_CMD_ERROR;
	}

	cmd->reply = talloc_strdup(cmd, "");

	llist_for_each_entry(trans, &msc_ctrl_net->trans_list, entry) {
		const char *imsi = "";
		const char *msisdn = "";

		if (trans->type != TRANS_CC)
			continue;
		if (trans->cc.state != GSM_CSTATE_ACTIVE)
			continue;

		if (trans->vsub) {
			imsi = trans->vsub->imsi;
			msisdn = trans->vsub->msisdn;
		}

		cmd->reply = talloc_asprintf_append(cmd->reply,
			"0x%08x,%s,%s,%s,%s,%u\n",
			trans->callref,
			imsi,
			msisdn,
			(trans->transaction_id & 0x08) ? "MO" : "MT",
			gsm48_cc_state_name(trans->cc.state),
			trans->transaction_id);
	}
	return CTRL_CMD_REPLY;
}
CTRL_CMD_DEFINE_RO(active_call_list, "active-call-list-v1");

CTRL_CMD_DEFINE_WO_NOVRF(sub_expire, "subscriber-expire");
static int set_sub_expire(struct ctrl_cmd *cmd, void *data)
{
	struct vlr_subscr *vsub;

	if (!msc_ctrl_net) {
		cmd->reply = "MSC CTRL commands not initialized";
		return CTRL_CMD_ERROR;
	}

	if (!msc_ctrl_net->vlr) {
		cmd->reply = "VLR not initialized";
		return CTRL_CMD_ERROR;
	}

	vsub = vlr_subscr_find_by_imsi(msc_ctrl_net->vlr, cmd->value, VSUB_USE_CTRL);
	if (!vsub) {
		LOGP(DCTRL, LOGL_ERROR, "Attempt to expire unknown subscriber IMSI=%s\n", cmd->value);
		cmd->reply = "IMSI unknown";
		return CTRL_CMD_ERROR;
	}

	LOGP(DCTRL, LOGL_NOTICE, "Expiring subscriber IMSI=%s\n", cmd->value);

	if (vlr_subscr_expire(vsub))
		LOGP(DCTRL, LOGL_NOTICE, "VLR released subscriber %s\n", vlr_subscr_name(vsub));

	if (osmo_use_count_total(&vsub->use_count) > 1)
		LOGP(DCTRL, LOGL_NOTICE, "Subscriber %s is still in use, should be released soon\n",
		     vlr_subscr_name(vsub));

	vlr_subscr_put(vsub, VSUB_USE_CTRL);

	return CTRL_CMD_REPLY;
}

static int set_subscriber_disconnect(struct ctrl_cmd *cmd, void *data)
{
	struct vlr_subscr *vsub;
	struct msc_a *msc_a;

	if (!msc_ctrl_net) {
		cmd->reply = "MSC CTRL commands not initialized";
		return CTRL_CMD_ERROR;
	}

	vsub = ctrl_find_vsub(cmd->value);
	if (!vsub) {
		LOGP(DCTRL, LOGL_ERROR, "Attempt to disconnect unknown subscriber ID=%s\n", cmd->value);
		cmd->reply = "Subscriber not found";
		return CTRL_CMD_ERROR;
	}

	LOGP(DCTRL, LOGL_NOTICE, "Disconnecting subscriber %s\n", vlr_subscr_name(vsub));

	msc_a = msc_a_for_vsub(vsub, false);
	if (msc_a)
		msc_a_release_cn(msc_a);

	if (vlr_subscr_expire(vsub))
		LOGP(DCTRL, LOGL_NOTICE, "VLR released subscriber %s\n", vlr_subscr_name(vsub));

	vlr_subscr_put(vsub, VSUB_USE_CTRL);
	cmd->reply = "OK";
	return CTRL_CMD_REPLY;
}
CTRL_CMD_DEFINE_WO_NOVRF(subscriber_disconnect, "subscriber-disconnect-v1");

static int set_call_disconnect(struct ctrl_cmd *cmd, void *data)
{
	struct gsm_trans *trans;
	uint32_t callref;
	char *endptr;

	if (!msc_ctrl_net) {
		cmd->reply = "MSC CTRL commands not initialized";
		return CTRL_CMD_ERROR;
	}

	callref = strtoul(cmd->value, &endptr, 0);
	if (endptr == cmd->value || (endptr[0] != '\0' && endptr[0] != '\n')) {
		cmd->reply = "Invalid callref";
		return CTRL_CMD_ERROR;
	}

	trans = trans_find_by_callref(msc_ctrl_net, TRANS_CC, callref);
	if (!trans) {
		LOGP(DCTRL, LOGL_ERROR, "Attempt to disconnect unknown callref=0x%08x\n", callref);
		cmd->reply = "Call not found";
		return CTRL_CMD_ERROR;
	}

	LOGP(DCTRL, LOGL_NOTICE, "Disconnecting call callref=0x%08x\n", callref);
	trans_free(trans);

	cmd->reply = "OK";
	return CTRL_CMD_REPLY;
}
CTRL_CMD_DEFINE_WO_NOVRF(call_disconnect, "call-disconnect-v1");

int msc_ctrl_cmds_install(struct gsm_network *net)
{
	int rc = 0;
	msc_ctrl_net = net;

	rc |= ctrl_cmd_install(CTRL_NODE_ROOT, &cmd_subscriber_list);
	rc |= ctrl_cmd_install(CTRL_NODE_ROOT, &cmd_subscriber_list_v2);
	rc |= ctrl_cmd_install(CTRL_NODE_ROOT, &cmd_subscriber_detail);
	rc |= ctrl_cmd_install(CTRL_NODE_ROOT, &cmd_active_call_list);
	rc |= ctrl_cmd_install(CTRL_NODE_ROOT, &cmd_sub_expire);
	rc |= ctrl_cmd_install(CTRL_NODE_ROOT, &cmd_subscriber_disconnect);
	rc |= ctrl_cmd_install(CTRL_NODE_ROOT, &cmd_call_disconnect);

	return rc;
}
