/* SGs Interface according to 3GPP TS 23.272 + TS 29.118 */

/* (C) 2018-2019 by sysmocom s.f.m.c. GmbH
 * All Rights Reserved
 *
 * Author: Harald Welte, Philipp Maier
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

#include <errno.h>
#include <string.h>
#include <time.h>
#include <talloc.h>

#include <osmocom/core/utils.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/fsm.h>
#include <osmocom/core/socket.h>
#include <osmocom/core/select.h>
#include <osmocom/core/timer.h>

#include <osmocom/gsm/tlv.h>
#include <osmocom/gsm/gsm48.h>
#include <osmocom/gsm/gsm23003.h>
#include <osmocom/gsm/gsm29118.h>

#include <osmocom/netif/stream.h>

#include <osmocom/gsupclient/gsup_client.h>
#include <osmocom/gsupclient/gsup_client_mux.h>
#include <osmocom/vlr/vlr.h>
#include <osmocom/vlr/vlr_sgs.h>
#include <osmocom/msc/gsm_data.h>
#include <osmocom/msc/gsm_04_08.h>
#include <osmocom/msc/msub.h>
#include <osmocom/msc/msc_a.h>
#include <osmocom/msc/msc_i.h>

#include <osmocom/msc/debug.h>
#include <osmocom/msc/msc_api.h>
#include <osmocom/msc/sgs_iface.h>
#include <osmocom/msc/sgs_server.h>
#include <osmocom/msc/db.h>
#include <osmocom/gsm/protocol/gsm_29_118.h>

#include <osmocom/gsm/apn.h>

#define S(x) (1 << (x))

/* A pointer to the GSM network we work with. By the current paradigm,
 * there can only be one gsm_network per MSC. The pointer is set once
 * when calling sgs_iface_init() */
static struct gsm_network *gsm_network = NULL;

static bool sgs_gsup_is_up(void)
{
	if (!gsm_network || !gsm_network->gcm || !gsm_network->gcm->gsup_client)
		return false;
	return osmo_gsup_client_is_connected(gsm_network->gcm->gsup_client);
}

static struct osmo_fsm sgs_vlr_reset_fsm;
static void sgs_tx(struct sgs_connection *sgc, struct msgb *msg);

struct sgs_state *g_sgs;

enum sgs_persist_op {
	SGS_PERSIST_UPSERT,
	SGS_PERSIST_DEL_IMSI,
	SGS_PERSIST_DEL_MME,
};

struct sgs_persist_pending {
	struct llist_head entry;
	enum sgs_persist_op op;
	struct db_sgs_assoc row;
};

static LLIST_HEAD(sgs_persist_pending);

static void sgs_persist_flush(void *data);
static void sgs_persist_schedule(void);

bool sgs_vlr_persist_enabled(void)
{
	return g_sgs && g_sgs->cfg.vlr_persist;
}

static struct sgs_persist_pending *sgs_persist_find_imsi(const char *imsi)
{
	struct sgs_persist_pending *p;

	if (!imsi || !imsi[0])
		return NULL;
	llist_for_each_entry(p, &sgs_persist_pending, entry) {
		if (p->op == SGS_PERSIST_DEL_MME)
			continue;
		if (!strcmp(p->row.imsi, imsi))
			return p;
	}
	return NULL;
}

static struct sgs_persist_pending *sgs_persist_alloc(enum sgs_persist_op op)
{
	struct sgs_persist_pending *p = talloc_zero(g_sgs, struct sgs_persist_pending);

	if (!p)
		return NULL;
	p->op = op;
	llist_add_tail(&p->entry, &sgs_persist_pending);
	return p;
}

static void sgs_persist_schedule(void)
{
	unsigned int sec;

	if (!g_sgs || !g_sgs->cfg.vlr_persist)
		return;
	sec = g_sgs->cfg.vlr_persist_batch_sec;
	if (!sec) {
		sgs_persist_flush(g_sgs);
		return;
	}
	if (osmo_timer_pending(&g_sgs->persist_timer))
		return;
	osmo_timer_schedule(&g_sgs->persist_timer, sec, 0);
}

static void sgs_persist_flush(void *data)
{
	struct sgs_persist_pending *p, *n;
	unsigned int n_up = 0, n_del = 0;
	bool use_tx;

	if (g_sgs)
		osmo_timer_del(&g_sgs->persist_timer);
	if (llist_empty(&sgs_persist_pending))
		return;

	use_tx = db_trans_begin() == 0;
	llist_for_each_entry_safe(p, n, &sgs_persist_pending, entry) {
		switch (p->op) {
		case SGS_PERSIST_UPSERT:
			if (db_sgs_assoc_upsert_row(&p->row) == 0)
				n_up++;
			break;
		case SGS_PERSIST_DEL_IMSI:
			if (db_sgs_assoc_delete(p->row.imsi) == 0)
				n_del++;
			break;
		case SGS_PERSIST_DEL_MME:
			if (db_sgs_assoc_delete_mme(p->row.mme_name) == 0)
				n_del++;
			break;
		}
		llist_del(&p->entry);
		talloc_free(p);
	}
	if (use_tx && db_trans_commit() < 0)
		db_trans_rollback();
	if (n_up || n_del)
		LOGP(DSGS, LOGL_DEBUG, "VLR persist flush: %u upsert, %u delete\n", n_up, n_del);
}

void sgs_vlr_persist_init(struct sgs_state *sgs)
{
	if (!sgs)
		return;
	osmo_timer_setup(&sgs->persist_timer, sgs_persist_flush, sgs);
}

void sgs_vlr_persist_reconfig(void)
{
	if (!g_sgs)
		return;
	if (!g_sgs->cfg.vlr_persist) {
		struct sgs_persist_pending *p, *n;

		osmo_timer_del(&g_sgs->persist_timer);
		llist_for_each_entry_safe(p, n, &sgs_persist_pending, entry) {
			llist_del(&p->entry);
			talloc_free(p);
		}
		return;
	}
	if (!llist_empty(&sgs_persist_pending))
		sgs_persist_schedule();
}

void sgs_vlr_persist_vsub(struct vlr_subscr *vsub)
{
	struct sgs_persist_pending *p;

	if (!sgs_vlr_persist_enabled() || !vsub || !vsub->imsi[0] || !vsub->sgs.mme_name[0])
		return;
	if (!g_sgs->cfg.vlr_persist_batch_sec) {
		db_sgs_assoc_upsert(vsub);
		return;
	}
	p = sgs_persist_find_imsi(vsub->imsi);
	if (p)
		p->op = SGS_PERSIST_UPSERT;
	else
		p = sgs_persist_alloc(SGS_PERSIST_UPSERT);
	if (!p)
		return;
	db_sgs_assoc_from_vsub(&p->row, vsub);
	sgs_persist_schedule();
}

void sgs_vlr_persist_forget(const char *imsi)
{
	struct sgs_persist_pending *p;

	if (!sgs_vlr_persist_enabled() || !imsi || !imsi[0])
		return;
	if (!g_sgs->cfg.vlr_persist_batch_sec) {
		db_sgs_assoc_delete(imsi);
		return;
	}
	p = sgs_persist_find_imsi(imsi);
	if (!p)
		p = sgs_persist_alloc(SGS_PERSIST_DEL_IMSI);
	if (!p)
		return;
	p->op = SGS_PERSIST_DEL_IMSI;
	OSMO_STRLCPY_ARRAY(p->row.imsi, imsi);
	sgs_persist_schedule();
}

void sgs_vlr_persist_forget_mme(const char *mme_name)
{
	struct sgs_persist_pending *p;

	if (!sgs_vlr_persist_enabled() || !mme_name || !mme_name[0])
		return;
	if (!g_sgs->cfg.vlr_persist_batch_sec) {
		db_sgs_assoc_delete_mme(mme_name);
		return;
	}
	p = sgs_persist_alloc(SGS_PERSIST_DEL_MME);
	if (!p)
		return;
	OSMO_STRLCPY_ARRAY(p->row.mme_name, mme_name);
	sgs_persist_schedule();
}

/***********************************************************************
 * SGs state per MME connection
 ***********************************************************************/

#define LOGSGC(sgc, lvl, fmt, args...)				\
	LOGP(DSGS, lvl, "%s: " fmt, sgc->sockname, ## args)

#define LOGSGC_VSUB(sgc, sub_info, lvl, fmt, args...)			\
	LOGP(DSGS, lvl, "(sub %s) %s: " fmt, sub_info, sgc->sockname, ## args)

#define LOGMME(mme, lvl, fmt, args...)					\
	LOGP(DSGS, lvl, "%s: " fmt, mme->fqdn ? mme->fqdn : mme->conn->sockname, ## args)

enum sgs_vlr_reset_fsm_state {
	SGS_VLRR_ST_NULL,
	SGS_VLRR_ST_WAIT_ACK,
	SGS_VLRR_ST_COMPLETE,
};

enum sgs_vlr_reset_fsm_event {
	SGS_VLRR_E_START_RESET,
	SGS_VLRR_E_RX_RESET_ACK,
};

/***********************************************************************
 * SGs utility functions
 ***********************************************************************/

/* Check if there are connections associated with a given subscriber. If yes,
 * make sure that those connections are tossed. */
static void subscr_conn_toss(struct vlr_subscr *vsub)
{
	struct msub *msub;
	struct msc_a *msc_a;

	msub = msub_for_vsub(vsub);
	if (!msub)
		return;

	msc_a = msub_msc_a(msub);

	LOG_MSUB(msub, LOGL_ERROR, "Force releasing previous subscriber connection: an SGs connection for this"
		 " subscriber is being initiated\n");

	/* Detach first so a new SGs msub can take this vsub. Do not free SMS
	 * transactions here: the last msc_a_put() synchronously RELEASE+term
	 * the SGs conn, and any later use of msc_a/msub is a SEGV. */
	if (msc_a)
		msc_a_get(msc_a, __func__);
	msub_set_vsub(msub, NULL);

	if (!msc_a) {
		osmo_fsm_inst_term(msub->fi, OSMO_FSM_TERM_ERROR, NULL);
		return;
	}

	/* Already going away; do not term (that UAF's a conn in RELEASED). */
	if (!msc_a_in_release(msc_a))
		msc_a_release_mo(msc_a, GSM_CAUSE_AUTH_FAILED);
	msc_a_put(msc_a, __func__);
}

/* Allocate a new subscriber connection */
static struct msc_a *subscr_conn_allocate_sgs(struct sgs_connection *sgc, struct vlr_subscr *vsub, bool mt)
{
	struct msub *msub;
	struct msc_a *msc_a;
	int rc;

	subscr_conn_toss(vsub);

	msub = msub_alloc(gsm_network);
	msc_a = msc_a_alloc(msub,
			    &msc_ran_infra[OSMO_RAT_EUTRAN_SGS]);
	msc_a->complete_layer3_type = mt ? COMPLETE_LAYER3_PAGING_RESP : COMPLETE_LAYER3_CM_SERVICE_REQ;

	rc = msub_set_vsub(msub, vsub);
	if (rc != 0) {
		LOGSGC(sgc, LOGL_ERROR, "Failed to associate SGs connection with %s\n",
		       vlr_subscr_name(vsub));
		osmo_fsm_inst_term(msc_a->c.fi, OSMO_FSM_TERM_ERROR, NULL);
		return NULL;
	}

	if (mt)
		msc_a_get(msc_a, MSC_A_USE_PAGING_RESPONSE);

	/* Accept the connection immediately, since the UE is already
	 * authenticated by the MME no authentication is required. */
	osmo_fsm_inst_dispatch(msc_a->c.fi, MSC_A_EV_COMPLETE_LAYER_3_OK, NULL);
	osmo_fsm_inst_dispatch(msc_a->c.fi, MSC_A_EV_AUTHENTICATED, NULL);

	return msc_a;
}

struct sgs_mme_ctx *sgs_mme_by_fqdn(struct sgs_state *sgs, const char *mme_fqdn)
{
	struct sgs_mme_ctx *mme;

	llist_for_each_entry(mme, &sgs->mme_list, entry) {
		if (!strcasecmp(mme_fqdn, mme->fqdn))
			return mme;
	}
	return NULL;
}

static struct sgs_mme_ctx *sgs_mme_alloc(struct sgs_state *sgs, const char *mme_fqdn, const struct osmo_gummei *gummei)
{
	struct sgs_mme_ctx *mme;

	OSMO_ASSERT(sgs_mme_by_fqdn(sgs, mme_fqdn) == NULL);

	mme = talloc_zero(sgs, struct sgs_mme_ctx);
	if (!mme)
		return NULL;
	mme->sgs = sgs;
	OSMO_STRLCPY_ARRAY(mme->fqdn, mme_fqdn);
	mme->fi = osmo_fsm_inst_alloc(&sgs_vlr_reset_fsm, mme, mme, LOGL_INFO, osmo_gummei_name(gummei));
	if (!mme->fi) {
		talloc_free(mme);
		return NULL;
	}
	llist_add_tail(&mme->entry, &sgs->mme_list);
	return mme;
}

/* 3GPP TS 29.118 5.7 VLR Reset: after this process starts the FSM is
 * SGS_VLRR_ST_NULL. Send RESET-IND once; do not send again after COMPLETE
 * (MME SCTP reconnect while this MSC is still up). */
static void sgs_mme_maybe_vlr_reset(struct sgs_mme_ctx *mme)
{
	if (!mme || !mme->fi || !mme->conn)
		return;
	if (mme->fi->state != SGS_VLRR_ST_NULL)
		return;
	/* RESET makes the MME re-LU the whole VLR. Do not start that while
	 * GSUP is down — every LU fails and used to RESET again. */
	if (!sgs_gsup_is_up()) {
		LOGMME(mme, LOGL_NOTICE,
		       "VLR reset deferred: GSUP not connected\n");
		return;
	}

	LOGMME(mme, LOGL_NOTICE,
	       "VLR reset: sending SGsAP-RESET-INDICATION (TS 29.118 5.7)\n");
	osmo_fsm_inst_dispatch(mme->fi, SGS_VLRR_E_START_RESET, NULL);
}

/* Decode and verify MME name */
static int decode_mme_name(char *mme_name, const struct tlv_parsed *tp)
{
	const uint8_t *mme_name_enc = TLVP_VAL_MINLEN(tp, SGSAP_IE_MME_NAME, SGS_MME_NAME_LEN);
	struct osmo_gummei gummei;

	if (!mme_name_enc)
		return -EINVAL;

	/* some implementations use FDQN format violating TS 29.118 9.3.14 */
	if (!osmo_parse_mme_domain(&gummei, (const char *) mme_name_enc)) {
		memcpy(mme_name, mme_name_enc, TLVP_LEN(tp, SGSAP_IE_MME_NAME));
		return 0;
	}

	/* decode the MME name from DNS labels to string */
	osmo_apn_to_str(mme_name, TLVP_VAL(tp, SGSAP_IE_MME_NAME), TLVP_LEN(tp, SGSAP_IE_MME_NAME));

	/* try to parse the MME name into a GUMMEI as a test for the format */
	if (osmo_parse_mme_domain(&gummei, mme_name) < 0)
		return -EINVAL;

	return 0;
}

/* A MME FQDN was received (e.g. RESET-IND/RESET-ACK/LU-REQ) */
void sgs_mme_detach_connection(struct sgs_connection *sgc)
{
	if (!sgc || !sgc->mme)
		return;

	if (sgc->mme->conn == sgc)
		sgc->mme->conn = NULL;
	sgc->mme = NULL;
}

static int sgs_mme_fqdn_received(struct sgs_connection *sgc, const char *mme_fqdn)
{
	struct sgs_mme_ctx *mme;
	struct osmo_gummei gummei;
	struct sgs_connection *stale;

	/* caller must pass in a valid FQDN string syntax */
	OSMO_ASSERT(osmo_parse_mme_domain(&gummei, mme_fqdn) == 0);

	if (!sgc->mme) {
		/* attempt to find MME with given name */
		mme = sgs_mme_by_fqdn(sgc->sgs, mme_fqdn);
		if (!mme)
			mme = sgs_mme_alloc(sgc->sgs, mme_fqdn, &gummei);
		OSMO_ASSERT(mme);

		if (mme->conn && mme->conn != sgc) {
			stale = mme->conn;
			LOGSGC(sgc, LOGL_NOTICE,
			       "Replacing stale SGs link %s with %s\n",
			       stale->sockname, sgc->sockname);
			sgs_mme_detach_connection(stale);
			sgs_conn_schedule_destroy(stale);
		}
		mme->conn = sgc;
		sgc->mme = mme;
	} else {
		mme = sgc->mme;
		if (strcasecmp(mme->fqdn, mme_fqdn) != 0) {
			LOGMME(mme, LOGL_ERROR, "Rx MME name \"%s\" in packet from MME \"%s\" ?!?\n", mme_fqdn,
			       mme->fqdn);
			return -2;
		}
		if (mme->conn != sgc) {
			stale = mme->conn;
			if (stale) {
				LOGSGC(sgc, LOGL_NOTICE,
				       "Adopting SGs link %s, closing stale link %s\n",
				       sgc->sockname, stale->sockname);
				sgs_mme_detach_connection(stale);
				sgs_conn_schedule_destroy(stale);
			}
			mme->conn = sgc;
			sgc->mme = mme;
		}
	}

	sgs_mme_maybe_vlr_reset(mme);
	return 0;
}

/* Safely get the mme-name for an sgs-connection */
static char *sgs_mme_fqdn_get(struct sgs_connection *sgc)
{
	if (!sgc)
		return NULL;
	if (!sgc->mme)
		return NULL;
	if (sgc->mme->fqdn[0] == '\0')
		return NULL;
	return sgc->mme->fqdn;
}

/* Find an sgs_mme_ctx for a given vlr subscriber, also check result */
struct sgs_mme_ctx *sgs_mme_ctx_by_vsub(struct vlr_subscr *vsub, uint8_t msg_type)
{
	struct sgs_mme_ctx *mme;

	/* Find SGS connection by MME name */
	mme = sgs_mme_by_fqdn(g_sgs, vsub->sgs.mme_name);
	if (!mme) {
		LOGP(DSGS, LOGL_ERROR, "(sub %s) Tx %s cannot find suitable MME!\n",
		     vlr_subscr_name(vsub), sgsap_msg_type_name(msg_type));
		return NULL;
	}
	if (!mme->conn) {
		LOGP(DSGS, LOGL_ERROR,
		     "(sub %s) Tx %s suitable MME found, but no SGS connection present!\n",
		     vlr_subscr_name(vsub), sgsap_msg_type_name(msg_type));
		return NULL;
	}
	if (!mme->sgs) {
		LOGP(DSGS, LOGL_ERROR,
		     "(sub %s) Tx %s suitable MME found, but no SGS state present!\n",
		     vlr_subscr_name(vsub), sgsap_msg_type_name(msg_type));
		return NULL;
	}

	return mme;
}

/* Make sure that the subscriber is known and that the subscriber is in the
 * SGs associated state. In case of failure the function returns false and
 * automatically sends a release message to the MME */
static bool check_sgs_association(struct sgs_connection *sgc, struct msgb *msg, char *imsi,
				  bool allow_unassociated)
{
	struct vlr_subscr *vsub;
	struct msgb *resp;
	uint8_t msg_type = msg->data[0];

	/* Subscriber must be known by the VLR */
	vsub = vlr_subscr_find_by_imsi(gsm_network->vlr, imsi, __func__);
	if (!vsub) {
		LOGSGC(sgc, LOGL_DEBUG, "SGsAP Message %s with unknown IMSI (%s), releasing\n",
		       sgsap_msg_type_name(msg_type), imsi);
		resp = gsm29118_create_release_req(imsi, SGSAP_SGS_CAUSE_IMSI_UNKNOWN);
		sgs_tx(sgc, resp);
		return false;
	}

	/* SERVICE-REQUEST after VLR restart arrives in SGs-NULL (29.118 5.1.2.2). */
	if (vsub->sgs_fsm->state != SGS_UE_ST_ASSOCIATED) {
		if (allow_unassociated &&
		    (vsub->sgs_fsm->state == SGS_UE_ST_NULL ||
		     vsub->sgs_fsm->state == SGS_UE_ST_LA_UPD_PRES)) {
			vlr_subscr_put(vsub, __func__);
			return true;
		}
		LOGSGC(sgc, LOGL_DEBUG, "(sub %s) SGsAP Message %s subscriber not SGs-associated, releasing\n",
		       vlr_subscr_name(vsub), sgsap_msg_type_name(msg_type));
		resp = gsm29118_create_release_req(vsub->imsi, SGSAP_SGS_CAUSE_IMSI_DET_EPS_NONEPS);
		sgs_tx(sgc, resp);
		vlr_subscr_put(vsub, __func__);
		return false;
	}

	vlr_subscr_put(vsub, __func__);
	return true;
}

/***********************************************************************
 * SGsAP transmit functions
 ***********************************************************************/

static void sgsap_msg_payload(const struct msgb *msg, const uint8_t **data, size_t *len)
{
	if (msg->l2h) {
		*data = msgb_l2(msg);
		*len = msgb_l2len(msg);
	} else {
		*data = msg->data;
		*len = msg->tail - msg->data;
	}
}

static const char *sgs_imsi_from_msg(struct msgb *msg, char *buf, size_t buf_len)
{
	struct tlv_parsed tp;
	struct osmo_mobile_identity mi;
	const uint8_t *payload;
	size_t plen;

	sgsap_msg_payload(msg, &payload, &plen);
	if (plen < 1)
		return NULL;

	if (tlv_parse(&tp, &sgsap_ie_tlvdef, (uint8_t *)payload + 1, plen - 1, 0, 0) < 0)
		return NULL;
	if (!TLVP_PRESENT(&tp, SGSAP_IE_IMSI))
		return NULL;
	if (osmo_mobile_identity_decode(&mi, TLVP_VAL(&tp, SGSAP_IE_IMSI),
					 TLVP_LEN(&tp, SGSAP_IE_IMSI), false))
		return NULL;
	if (mi.type != GSM_MI_TYPE_IMSI)
		return NULL;
	osmo_strlcpy(buf, mi.imsi, buf_len);
	return buf;
}

/* Send message out to remote end (final step) */
static void sgs_tx(struct sgs_connection *sgc, struct msgb *msg)
{
	char imsi_buf[GSM23003_IMSI_MAX_DIGITS + 1];
	const char *imsi;

	if (!msg) {
		LOGSGC(sgc, LOGL_NOTICE, "Null message, cannot transmit!\n");
		return;
	}

	imsi = sgs_imsi_from_msg(msg, imsi_buf, sizeof(imsi_buf));
	if (imsi) {
		const uint8_t *payload;
		size_t plen;

		sgsap_msg_payload(msg, &payload, &plen);
		msc_api_trace_packet(imsi, "sgsap", false, payload, plen);
	}

	msgb_sctp_ppid(msg) = 0;
	if (!sgc || !sgc->srv) {
		LOGP(DSGS, LOGL_NOTICE, "Cannot transmit %s: connection dead. Discarding\n",
		     sgsap_msg_type_name(msg->data[0]));
		msgb_free(msg);
		return;
	}
	osmo_stream_srv_send(sgc->srv, msg);
}

/* Get some subscriber info from ISMI (for the log text) */
const char *subscr_info(const char *imsi)
{
	const char *subscr_string = "<unknown>";
	struct vlr_subscr *vsub;

	if (imsi) {
		vsub = vlr_subscr_find_by_imsi(gsm_network->vlr, imsi, __func__);
		if (!vsub)
			subscr_string = imsi;
		else {
			subscr_string = vlr_subscr_name(vsub);
			vlr_subscr_put(vsub, __func__);
		}
	}

	return subscr_string;
}

/* Comfortable status message generator that also generates some basic
 * context-dependent log output */
static int sgs_tx_status(struct sgs_connection *sgc, const char *imsi, enum sgsap_sgs_cause cause, struct msgb *msg,
			 int sgsap_iei)
{
	struct msgb *resp;

	if (sgsap_iei < 0) {
		LOGSGC_VSUB(sgc, subscr_info(imsi), LOGL_ERROR, "Rx %s failed with cause %s!\n",
			    sgsap_msg_type_name(msg->data[0]), sgsap_sgs_cause_name(cause));
	} else if (cause == SGSAP_SGS_CAUSE_MISSING_MAND_IE) {
		LOGSGC_VSUB(sgc, subscr_info(imsi), LOGL_ERROR, "Rx %s with missing mandatory %s IEI!\n",
			    sgsap_msg_type_name(msg->data[0]), sgsap_iei_name(sgsap_iei));
	} else if (cause == SGSAP_SGS_CAUSE_INVALID_MAND_IE) {
		LOGSGC_VSUB(sgc, subscr_info(imsi), LOGL_ERROR, "Rx %s with invalid mandatory %s IEI!\n",
			    sgsap_msg_type_name(msg->data[0]), sgsap_iei_name(sgsap_iei));
	} else if (cause == SGSAP_SGS_CAUSE_COND_IE_ERROR) {
		LOGSGC_VSUB(sgc, subscr_info(imsi), LOGL_ERROR, "Rx %s with erroneous conditional %s IEI!\n",
			    sgsap_msg_type_name(msg->data[0]), sgsap_iei_name(sgsap_iei));
	} else {
		LOGSGC_VSUB(sgc, subscr_info(imsi), LOGL_ERROR, "Rx %s failed with cause %s at %s IEI!\n",
			    sgsap_msg_type_name(msg->data[0]), sgsap_sgs_cause_name(cause), sgsap_iei_name(sgsap_iei));
	}

	resp = gsm29118_create_status(imsi, cause, msg);
	sgs_tx(sgc, resp);
	return 0;
}

/* The Reject Cause IE of SGsAP-LOCATION-UPDATE-REJECT carries the value part of
 * the 3GPP TS 24.008 Reject cause IE (3GPP TS 29.118 9.4.13), *not* an SGs
 * cause. Passing an enum sgsap_sgs_cause here made every HLR/GSUP failure reach
 * the UE as MM/EMM cause #3 "Illegal MS" (SGSAP_SGS_CAUSE_IMSI_UNKNOWN == 3),
 * which per 3GPP TS 24.008 4.4.4.7 / 3GPP TS 24.301 5.5.3.3.5 invalidates the
 * (U)SIM for CS - and for a combined procedure also EPS - service until
 * switch-off or UICC removal. A transient HLR outage must not do that. */
static uint8_t sgs_lu_rej_cause(const struct sgs_lu_response *response)
{
	if (response->cause)
		return response->cause;
	return GSM48_REJECT_NETWORK_FAILURE;
}

/* Called by VLR via callback, transmits the location update response or
 * reject, depending on the outcome of the location update. */
static void sgs_tx_loc_upd_resp_cb(struct sgs_lu_response *response)
{
	struct msgb *resp;
	struct vlr_subscr *vsub = response->vsub;
	struct sgs_mme_ctx *mme;
	uint8_t new_id[2 + GSM48_TMSI_LEN];
	uint8_t *new_id_ptr = NULL;
	int new_id_len = 0;

	/* Send if the MME socket is up. Always finish the LU so a missing
	 * conn cannot leak VSUB_USE_SGS_LU (that grew the VLR into millions). */
	mme = sgs_mme_by_fqdn(g_sgs, vsub->sgs.mme_name);

	/* A single HLR/GSUP failure is not a VLR reset. Sending RESET-IND
	 * here made the MME re-LU everyone; each fail RESET again (hang). */
	if (response->error) {
		if (mme && mme->conn) {
			resp = gsm29118_create_lu_rej(vsub->imsi, sgs_lu_rej_cause(response), &vsub->sgs.lai);
			sgs_tx(mme->conn, resp);
		}
		vlr_sgs_loc_update_rej_sent(vsub);
		return;
	}

	/* Handle LU accept/reject */
	if (response->accepted) {
		if (vsub->tmsi_new != GSM_RESERVED_TMSI) {
			struct osmo_mobile_identity tmsi_mi = {
				.type = GSM_MI_TYPE_TMSI,
				.tmsi = vsub->tmsi_new,
			};
			new_id_len = osmo_mobile_identity_encode_buf(new_id, sizeof(new_id), &tmsi_mi, false);
			if (new_id_len > 0) {
				new_id_ptr = new_id;
			} else {
				/* Failure to encode the TMSI is not actually possible here, this is just for paranoia
				 * and coverity scan. */
				new_id_len = 0;
				LOGPFSMSL(vsub->sgs_fsm, DMM, LOGL_ERROR, "Cannot encode TMSI Mobile Identity\n");
			}
		}
		if (mme && mme->conn) {
			resp = gsm29118_create_lu_ack(vsub->imsi, &vsub->sgs.lai, new_id_ptr, new_id_len);
			sgs_tx(mme->conn, resp);
		}
		vlr_sgs_loc_update_acc_sent(vsub);
	} else {
		if (mme && mme->conn) {
			resp = gsm29118_create_lu_rej(vsub->imsi, sgs_lu_rej_cause(response), &vsub->sgs.lai);
			sgs_tx(mme->conn, resp);
		}
		vlr_sgs_loc_update_rej_sent(vsub);
	}
}

/* Called by VLR via callback, transmits MM information to the UE */
static void sgs_tx_mm_info_cb(struct vlr_subscr *vsub)
{
	struct msgb *msg;
	struct msgb *msg_mm_info;
	struct sgs_mme_ctx *mme;

	/* The sending of MM information requests is an optional feature and
	 * depends on the network configuration (VTY) */
	if (!gsm_network->send_mm_info)
		return;

	mme = sgs_mme_ctx_by_vsub(vsub, SGSAP_MSGT_MM_INFO_REQ);
	if (!mme)
		return;

	/* Create and send MM information request message, see also:
	 * 3GPP TS 29.118, chapter 8.12 SGsAP-MM-INFORMATION-REQUEST and
	 * 3GPP TS 29.018, chapter 18.4.16 MM information. */
	msg_mm_info = gsm48_create_mm_info(gsm_network);
	msg = gsm29118_create_mm_info_req(vsub->imsi, msg_mm_info->data + 2, msg_mm_info->len - 2);
	sgs_tx(mme->conn, msg);
	msgb_free(msg_mm_info);
}

enum sgsap_service_ind sgs_serv_ind_from_paging_cause(enum paging_cause cause)
{
	switch (cause) {
	case PAGING_CAUSE_CALL_CONVERSATIONAL:
	case PAGING_CAUSE_CALL_STREAMING:
	case PAGING_CAUSE_CALL_INTERACTIVE:
	case PAGING_CAUSE_CALL_BACKGROUND:
		return SGSAP_SERV_IND_CS_CALL;

	case PAGING_CAUSE_UNSPECIFIED:
	case PAGING_CAUSE_SIGNALLING_LOW_PRIO:
	case PAGING_CAUSE_SIGNALLING_HIGH_PRIO:
		return SGSAP_SERV_IND_SMS;

	default:
		OSMO_ASSERT(false);
	}
}

/*! Page UE through SGs interface or inform about an expired paging
 *  \param[in] vsub subscriber context
 *  \param[in] serv_ind service indicator (sms or voide)
 *  \returns 0 in case of success, -EINVAL in case of error. */
int sgs_iface_paging_cb(struct vlr_subscr *vsub, enum sgsap_service_ind serv_ind)
{
	struct msgb *resp;
	struct gsm29118_paging_req paging_params;
	struct sgs_mme_ctx *mme;

	if (serv_ind == SGSAP_SERV_IND_PAGING_TIMEOUT) {
		paging_expired(vsub);
		return 0;
	}

	if (vsub->imsi_detached_flag) {
		LOGPFSMSL(vsub->sgs_fsm, DPAG, LOGL_DEBUG, "Will not Page (IMSI detached)\n");
		return -EINVAL;
	}

	/* After persist restore, MT SMS would page ~all unconfirmed UEs at once
	 * and fill the SGs socket with unanswered Ts5 work. CS paging is not
	 * capped. Confirmed associations use the normal SMS-queue limit. */
#define SGS_MAX_UNCONFIRMED_PAGES 64
	if (serv_ind != SGSAP_SERV_IND_CS_CALL &&
	    !vsub->conf_by_radio_contact_ind &&
	    !vlr_sgs_pag_pend(vsub) &&
	    vlr_sgs_pag_inflight() >= SGS_MAX_UNCONFIRMED_PAGES) {
		LOGPFSMSL(vsub->sgs_fsm, DPAG, LOGL_DEBUG,
			  "Defer SGs page (unconfirmed inflight cap %u)\n",
			  SGS_MAX_UNCONFIRMED_PAGES);
		return 0;
	}

	/* 3GPP TS 29.118 5.1.2.2: page from ASSOCIATED, LA-UPDATE-PRESENT,
	 * or SGs-NULL while Confirmed by Radio Contact is false (VLR restart). */
	if (vsub->sgs_fsm->state == SGS_UE_ST_NULL && vsub->conf_by_radio_contact_ind == true) {
		LOGPFSMSL(vsub->sgs_fsm, DPAG, LOGL_ERROR, "Will not Page (conf_by_radio_contact_ind == true)\n");
		return -EINVAL;
	}

	mme = sgs_mme_ctx_by_vsub(vsub, SGSAP_MSGT_PAGING_REQ);
	if (!mme) {
		LOGPFSMSL(vsub->sgs_fsm, DPAG, LOGL_ERROR, "Will not Page (no MME)\n");
		return -EINVAL;
	}

	/* Check if there is still a paging in progress for this subscriber,
	 * if yes, don't initiate another paging request. */
	if (vlr_sgs_pag_pend(vsub))
		return 0;

	LOGMME(mme, LOGL_INFO, "Paging on SGs: %s for %s (conf_by_radio_contact_ind=%d)\n",
	       vlr_subscr_name(vsub), sgsap_service_ind_name(serv_ind), vsub->conf_by_radio_contact_ind);

	memset(&paging_params, 0, sizeof(paging_params));
	osmo_strlcpy(paging_params.imsi, vsub->imsi, sizeof(paging_params.imsi));
	osmo_strlcpy(paging_params.vlr_name, mme->sgs->cfg.vlr_name, sizeof(paging_params.vlr_name));
	paging_params.serv_ind = serv_ind;
	if (vsub->conf_by_radio_contact_ind == true) {
		memcpy(&paging_params.lai, &vsub->sgs.lai, sizeof(paging_params.lai));
		paging_params.lai_present = true;
	}
	resp = gsm29118_create_paging_req(&paging_params);
	sgs_tx(mme->conn, resp);

	/* FIXME: If we are in SGS_UE_ST_NULL while sub->conf_by_radio_contact_ind == false,
	 * we are supposed to start a search procedure as defined in 3GPP TS 23.018 */

	/* Inform the VLR that a paging via SGs is in progress */
	vlr_sgs_pag(vsub, serv_ind);

	/* Return a page count of 1 (success) */
	return 1;
}

/***********************************************************************
 * SGs incoming messages from the MME
 ***********************************************************************/

/* Safely read out the SGs cause code from a given message/tlv set, send status
 * message in case the cause code is invalid or missing. */
static int sgs_cause_from_msg(struct sgs_connection *sgc, struct msgb *msg, const struct tlv_parsed *tp,
			      const char *imsi)
{
	enum sgsap_sgs_cause cause;
	const uint8_t *cause_ptr;
	cause_ptr = TLVP_VAL_MINLEN(tp, SGSAP_IE_SGS_CAUSE, 1);
	if (!cause_ptr) {
		sgs_tx_status(sgc, imsi, SGSAP_SGS_CAUSE_MISSING_MAND_IE, msg, SGSAP_IE_SGS_CAUSE);
		return -1;
	} else
		cause = *cause_ptr;
	return cause;
}

/* SGsAP-STATUS 3GPP TS 29.118, chapter 8.18 */
static int sgs_rx_status(struct sgs_connection *sgc, struct msgb *msg, const struct tlv_parsed *tp, const char *imsi)
{
	int cause;
	const uint8_t *err_msg;
	const char *imsi_ptr;
	char *err_msg_hex = "(none)";

	cause = sgs_cause_from_msg(sgc, msg, tp, NULL);
	if (cause < 0)
		return 0;

	if (imsi[0] != '\0')
		imsi_ptr = imsi;
	else
		imsi_ptr = "<none>";

	if (TLVP_PRESENT(tp, SGSAP_IE_ERR_MSG))
		err_msg = TLVP_VAL(tp, SGSAP_IE_ERR_MSG);
	else
		err_msg = NULL;

	if (err_msg)
		err_msg_hex = osmo_hexdump(err_msg, TLVP_LEN(tp, SGSAP_IE_ERR_MSG));

	LOGSGC(sgc, LOGL_NOTICE, "Rx STATUS cause=%s, IMSI=%s, err_msg=%s\n",
	       sgsap_sgs_cause_name(cause), imsi_ptr, err_msg_hex);

	return 0;
}

/* SGsAP-RESET-INDICATION 3GPP TS 29.118, chapter 8.16 */
static int sgs_rx_reset_ind(struct sgs_connection *sgc, struct msgb *msg, const struct tlv_parsed *tp)
{
	struct gsm29118_reset_msg reset_params;
	struct msgb *resp;

	memset(&reset_params, 0, sizeof(reset_params));
	osmo_strlcpy(reset_params.vlr_name, sgc->sgs->cfg.vlr_name, sizeof(reset_params.vlr_name));
	reset_params.vlr_name_present = true;

	resp = gsm29118_create_reset_ack(&reset_params);

	/* MME failure (29.118 5.8): drop SGs assoc for this MME only.
	 * The MME lost its associations, so persisted rows cannot page. */
	if (sgc->mme) {
		sgs_vlr_persist_forget_mme(sgc->mme->fqdn);
		vlr_sgs_reset_mme(gsm_network->vlr, sgc->mme->fqdn);
	} else
		vlr_sgs_reset(gsm_network->vlr);

	sgs_tx(sgc, resp);
	return 0;
}

/* SGsAP-RESET-ACK 3GPP TS 29.118, chapter 8.15 */
static int sgs_rx_reset_ack(struct sgs_connection *sgc, struct msgb *msg, const struct tlv_parsed *tp)
{
	/* dispatch event to VLR reset FSM for this MME */
	if (sgc->mme && sgc->mme->fi)
		osmo_fsm_inst_dispatch(sgc->mme->fi, SGS_VLRR_E_RX_RESET_ACK, msg);
	return 0;
}

/* SGsAP-LOCATION-UPDATE-REQUEST 3GPP TS 29.118, chapter 8.11 */
static int sgs_rx_loc_upd_req(struct sgs_connection *sgc, struct msgb *msg, const struct tlv_parsed *tp, char *imsi)
{
	struct msgb *resp;
	const uint8_t *lu_type_ie;
	enum vlr_lu_type type;
	struct osmo_location_area_id new_lai;
	const struct gsm48_loc_area_id *gsm48_lai;
	int rc;
	char *mme_name;
	struct vlr_sgs_cfg vlr_sgs_cfg;
	struct vlr_subscr *vsub;
	struct osmo_plmn_id last_eutran_plmn_buf, *last_eutran_plmn = NULL;

	if (!sgs_gsup_is_up()) {
		resp = gsm29118_create_lu_rej(imsi, GSM48_REJECT_NETWORK_FAILURE, NULL);
		sgs_tx(sgc, resp);
		return 0;
	}

	/* Check for lingering connections */
	vsub = vlr_subscr_find_by_imsi(gsm_network->vlr, imsi, __func__);
	if (vsub) {
		subscr_conn_toss(vsub);
		vlr_subscr_put(vsub, __func__);
		vsub = NULL;
	}

	/* Determine MME-Name */
	mme_name = sgs_mme_fqdn_get(sgc);
	if (!mme_name) {
		resp = gsm29118_create_lu_rej(imsi, GSM48_REJECT_NETWORK_FAILURE, NULL);
		sgs_tx(sgc, resp);
		return 0;
	}

	/* Parse LU-Type */
	lu_type_ie = TLVP_VAL_MINLEN(tp, SGSAP_IE_EPS_LU_TYPE, 1);
	if (!lu_type_ie)
		return sgs_tx_status(sgc, imsi, SGSAP_SGS_CAUSE_MISSING_MAND_IE, msg, SGSAP_IE_EPS_LU_TYPE);
	if (lu_type_ie[0] == 0x01)
		type = VLR_LU_TYPE_IMSI_ATTACH;
	else
		type = VLR_LU_TYPE_REGULAR;

	/* Parse LAI of the new location */
	gsm48_lai = (struct gsm48_loc_area_id *)TLVP_VAL_MINLEN(tp, SGSAP_IE_LAI, 5);
	if (!gsm48_lai)
		return sgs_tx_status(sgc, imsi, SGSAP_SGS_CAUSE_MISSING_MAND_IE, msg, SGSAP_IE_LAI);
	gsm48_decode_lai2(gsm48_lai, &new_lai);

	/* 3GPP TS 23.272 sec 4.3.3 (CSFB):
	 * "During the SGs location update procedure, obtaining the last used LTE PLMN ID via TAI"
	 */
	if (TLVP_PRES_LEN(tp, SGSAP_IE_TAI, 3)) {
		last_eutran_plmn = &last_eutran_plmn_buf;
		osmo_plmn_from_bcd(TLVP_VAL(tp, SGSAP_IE_TAI), last_eutran_plmn);
		/* TODO: we could also gather the TAC from here, but we don't need it yet */
	} else if (TLVP_PRES_LEN(tp, SGSAP_IE_EUTRAN_CGI, 3)) {
		/* Since TAI is optional, let's try harder getting Last Used
		 * E-UTRAN PLMN ID by fetching it from E-UTRAN CGI */
		last_eutran_plmn = &last_eutran_plmn_buf;
		osmo_plmn_from_bcd(TLVP_VAL(tp, SGSAP_IE_EUTRAN_CGI), last_eutran_plmn);
		/* TODO: we could also gather the ECI from here, but we don't need it yet */
	} else {
		LOGSGC(sgc, LOGL_INFO, "Receiving SGsAP-LOCATION-UPDATE-REQUEST without TAI nor "
		       "E-CGI IEs, fast fallback GERAN->EUTRAN won't be possible!\n");
	}

	/* Perform actual location update */
	memcpy(vlr_sgs_cfg.timer, sgc->sgs->cfg.timer, sizeof(vlr_sgs_cfg.timer));
	memcpy(vlr_sgs_cfg.counter, sgc->sgs->cfg.counter, sizeof(vlr_sgs_cfg.counter));
	rc = vlr_sgs_loc_update(gsm_network->vlr, &vlr_sgs_cfg, sgs_tx_loc_upd_resp_cb, sgs_iface_paging_cb,
				sgs_tx_mm_info_cb, mme_name, type, imsi, &new_lai, last_eutran_plmn);
	if (rc != 0) {
		resp = gsm29118_create_lu_rej(imsi, GSM48_REJECT_NETWORK_FAILURE, NULL);
		sgs_tx(sgc, resp);
	}

	return 0;
}

/* SGsAP-IMSI-DETACH-INDICATION 3GPP TS 29.118, chapter 8.8 */
static int sgs_rx_imsi_det_ind(struct sgs_connection *sgc, struct msgb *msg, const struct tlv_parsed *tp, char *imsi)
{
	struct msgb *resp;
	enum sgsap_imsi_det_noneps_type type;
	const uint8_t *type_ie;

	type_ie = TLVP_VAL_MINLEN(tp, SGSAP_IE_IMSI_DET_NONEPS_TYPE, 1);
	if (!type_ie)
		return sgs_tx_status(sgc, imsi, SGSAP_SGS_CAUSE_MISSING_MAND_IE, msg, SGSAP_IE_IMSI_DET_NONEPS_TYPE);

	switch (type_ie[0]) {
	case SGSAP_ID_NONEPS_T_EXPLICIT_UE_NONEPS:
		type = SGSAP_ID_NONEPS_T_EXPLICIT_UE_NONEPS;
		break;
	case SGSAP_ID_NONEPS_T_COMBINED_UE_EPS_NONEPS:
		type = SGSAP_ID_NONEPS_T_COMBINED_UE_EPS_NONEPS;
		break;
	case SGSAP_ID_NONEPS_T_IMPLICIT_UE_EPS_NONEPS:
		type = SGSAP_ID_NONEPS_T_IMPLICIT_UE_EPS_NONEPS;
		break;
	default:
		return sgs_tx_status(sgc, imsi, SGSAP_SGS_CAUSE_INVALID_MAND_IE, msg, SGSAP_IE_IMSI_DET_NONEPS_TYPE);
		break;
	}

	vlr_sgs_imsi_detach(gsm_network->vlr, imsi, type);
	resp = gsm29118_create_imsi_det_ack(imsi);
	sgs_tx(sgc, resp);

	return 0;
}

/* SGsAP-EPS-DETACH-INDICATION 3GPP TS 29.118, chapter 8.6 */
static int sgs_rx_eps_det_ind(struct sgs_connection *sgc, struct msgb *msg, const struct tlv_parsed *tp, char *imsi)
{
	struct msgb *resp;
	enum sgsap_imsi_det_eps_type type;
	const uint8_t *type_ie;

	type_ie = TLVP_VAL_MINLEN(tp, SGSAP_IE_IMSI_DET_EPS_TYPE, 1);
	if (!type_ie)
		return sgs_tx_status(sgc, imsi, SGSAP_SGS_CAUSE_MISSING_MAND_IE, msg, SGSAP_IE_IMSI_DET_EPS_TYPE);

	switch (type_ie[0]) {
	case SGSAP_ID_EPS_T_NETWORK_INITIATED:
		type = SGSAP_ID_EPS_T_NETWORK_INITIATED;
		break;
	case SGSAP_ID_EPS_T_UE_INITIATED:
		type = SGSAP_ID_EPS_T_UE_INITIATED;
		break;
	case SGSAP_ID_EPS_T_EPS_NOT_ALLOWED:
		type = SGSAP_ID_EPS_T_EPS_NOT_ALLOWED;
		break;
	default:
		return sgs_tx_status(sgc, imsi, SGSAP_SGS_CAUSE_INVALID_MAND_IE, msg, SGSAP_IE_IMSI_DET_EPS_TYPE);
		break;
	}

	vlr_sgs_eps_detach(gsm_network->vlr, imsi, type);
	resp = gsm29118_create_eps_det_ack(imsi);
	sgs_tx(sgc, resp);

	return 0;
}

/* SGsAP-PAGING-REJECT 3GPP TS 29.118, chapter 8.13 */
static int sgs_rx_pag_rej(struct sgs_connection *sgc, struct msgb *msg, const struct tlv_parsed *tp, char *imsi)
{
	int cause;
	struct vlr_subscr *vsub;

	cause = sgs_cause_from_msg(sgc, msg, tp, NULL);
	if (cause < 0)
		return 0;

	/* Subscriber must be known by the VLR */
	vsub = vlr_subscr_find_by_imsi(gsm_network->vlr, imsi, __func__);
	if (!vsub)
		return sgs_tx_status(sgc, imsi, SGSAP_SGS_CAUSE_IMSI_UNKNOWN, msg, SGSAP_IE_IMSI);

	/* Inform the VLR */
	vlr_sgs_pag_rej(gsm_network->vlr, imsi, cause);

	/* Stop all paging activity */
	paging_expired(vsub);

	/* Depending on the cause code some action is required */
	if (cause == SGSAP_SGS_CAUSE_MT_CSFB_REJ_USER) {
		/* FIXME: We are supposed to trigger a User Determined User Busy (UDUB)
		 * as specified in 3GPP TS 24.082 here, SGs association state shall not
		 * be changed */
		LOGSGC(sgc, LOGL_ERROR,
		       "Rx %s with SGSAP_SGS_CAUSE_MT_CSFB_REJ_USER, but sending UDUP is not implemented yet!\n",
		       sgsap_msg_type_name(msg->data[0]));
	} else if (cause == SGSAP_SGS_CAUSE_IMSI_DET_EPS) {
		/* FIXME: In this case we should send the paging via A/Iu interface */
		OSMO_ASSERT(false);
	}

	vlr_subscr_put(vsub, __func__);
	return 0;
}

/* SGsAP-UE-UNREACHABLE 3GPP TS 29.118, chapter 8.21 */
static int sgs_rx_ue_unr(struct sgs_connection *sgc, struct msgb *msg, const struct tlv_parsed *tp, char *imsi)
{
	int cause;

	cause = sgs_cause_from_msg(sgc, msg, tp, NULL);
	if (cause < 0)
		return 0;

	vlr_sgs_ue_unr(gsm_network->vlr, imsi, cause);

	return 0;
}

/* SGsAP-TMSI-REALLOCATION-COMPLETE 3GPP TS 29.118, chapter 8.19 */
static int sgs_rx_tmsi_reall_cmpl(struct sgs_connection *sgc, struct msgb *msg, const struct tlv_parsed *tp, char *imsi)
{
	vlr_sgs_tmsi_reall_compl(gsm_network->vlr, imsi);
	return 0;
}

/* SGsAP-SERVICE-REQUEST 3GPP TS 29.118, chapter 8.17 */
static int sgs_rx_service_req(struct sgs_connection *sgc, struct msgb *msg, const struct tlv_parsed *tp, char *imsi)
{
	enum sgsap_service_ind serv_ind;
	const uint8_t *serv_ind_ie;
	struct msc_a *msc_a;
	struct vlr_subscr *vsub;
	bool was_unassociated;
	bool was_paging;

	/* Note: While in other RAN concepts a service request is used to
	 * initiate mobile originated operation, the service request in SGsAP
	 * is comparable to a paging response. The SGsAP SERVICE REQUEST must
	 * not be confused or compared with a CM SERVICE REQUEST! */

	if (!check_sgs_association(sgc, msg, imsi, true))
		return 0;

	vsub = vlr_subscr_find_by_imsi(gsm_network->vlr, imsi, __func__);
	/* Note: vsub is already sufficiently verified by check_sgs_association(),
	 * we must have a vsub at this point! */
	OSMO_ASSERT(vsub);

	was_unassociated = (vsub->sgs_fsm->state != SGS_UE_ST_ASSOCIATED);
	was_paging = vlr_sgs_pag_pend(vsub);

	/* 29.118 5.1.2.2: paging response in SGs-NULL after VLR restart. */
	if (vsub->sgs_fsm->state == SGS_UE_ST_NULL)
		vlr_sgs_rx_service_req(vsub);

	/* Associated without a page: spec STATUS. Unassociated without a page
	 * (late Ts5 or implicit confirm): keep the association, no STATUS. */
	if (!was_paging) {
		vlr_subscr_put(vsub, __func__);
		if (!was_unassociated)
			return sgs_tx_status(sgc, imsi, SGSAP_SGS_CAUSE_MSG_INCOMP_STATE, msg, -1);
		return 0;
	}
	serv_ind_ie = TLVP_VAL_MINLEN(tp, SGSAP_IE_SERVICE_INDICATOR, 1);

	if (!serv_ind_ie) {
		vlr_subscr_put(vsub, __func__);
		return sgs_tx_status(sgc, imsi, SGSAP_SGS_CAUSE_MISSING_MAND_IE, msg, SGSAP_IE_SERVICE_INDICATOR);
	}
	if (serv_ind_ie[0] == SGSAP_SERV_IND_CS_CALL)
		serv_ind = serv_ind_ie[0];
	else if (serv_ind_ie[0] == SGSAP_SERV_IND_SMS)
		serv_ind = serv_ind_ie[0];
	else {
		vlr_subscr_put(vsub, __func__);
		return sgs_tx_status(sgc, imsi, SGSAP_SGS_CAUSE_INVALID_MAND_IE, msg, SGSAP_IE_SERVICE_INDICATOR);
	}

	/* FIXME: The MME shall include an UE EMM Mode IE, but the field is
	 * marked optional. (Why do we need this info at all?) */

	/* Report to the VLR that the paging has successfully completed */
	vlr_sgs_pag_ack(gsm_network->vlr, imsi);

	/* Exit early when the service indicator indicates that a call is being
	 * established. In those cases we do not allocate a connection, instead
	 * the connection will be allocated when the MS is appearing on the
	 * A-Interface. */
	if (serv_ind == SGSAP_SERV_IND_CS_CALL) {
		vlr_subscr_put(vsub, __func__);
		return 0;
	}

	/* Allocate subscriber connection */
	msc_a = subscr_conn_allocate_sgs(sgc, vsub, true);
	if (!msc_a) {
		vlr_subscr_put(vsub, __func__);
		return sgs_tx_status(sgc, imsi, SGSAP_SGS_CAUSE_MSG_INCOMP_STATE, msg, -1);
	}

	/* The msub has added a get() for the vsub, balance above vlr_subscr_find_by_imsi() */
	vlr_subscr_put(vsub, __func__);
	return 0;
}

/* SGsAP-UPLINK-UNITDATA 3GPP TS 29.118, chapter 8.22 */
static int sgs_rx_ul_ud(struct sgs_connection *sgc, struct msgb *msg, const struct tlv_parsed *tp, char *imsi)
{
	struct msc_a *msc_a;
	const uint8_t *nas_msg_container_ie;
	struct vlr_subscr *vsub;

	if (!check_sgs_association(sgc, msg, imsi, false))
		return 0;

	vsub = vlr_subscr_find_by_imsi(gsm_network->vlr, imsi, __func__);
	/* Note: vsub is already sufficiently verified by check_sgs_association(),
	 * we must have a vsub at this point! */
	OSMO_ASSERT(vsub);

	/* Try to find existing connection (MT) or allocate a new one (MO) */
	msc_a = msc_a_for_vsub(vsub, true);
	if (!msc_a)
		msc_a = subscr_conn_allocate_sgs(sgc, vsub, false);

	/* Balance above vlr_subscr_find_by_imsi() */
	vlr_subscr_put(vsub, __func__);

	/* If we do not find an existing connection and allocating a new one
	 * failed, give up and return status. */
	if (!msc_a)
		return sgs_tx_status(sgc, imsi, SGSAP_SGS_CAUSE_MSG_INCOMP_STATE, msg, 0);

	if (msc_a->c.ran->type != OSMO_RAT_EUTRAN_SGS) {
		LOGSGC(sgc, LOGL_ERROR,
		       "Receiving uplink unit-data for non-sgs connection -- discarding message!\n");
		return -EINVAL;
	}

	nas_msg_container_ie = TLVP_VAL_MINLEN(tp, SGSAP_IE_NAS_MSG_CONTAINER, 1);
	if (!nas_msg_container_ie)
		return sgs_tx_status(sgc, imsi, SGSAP_SGS_CAUSE_MISSING_MAND_IE, msg, SGSAP_IE_NAS_MSG_CONTAINER);

	/* ran_conn_dtap expects the dtap payload in l3h */
	msg->l3h = (uint8_t *)nas_msg_container_ie;
	msc_a_up_l3(msc_a, msg);

	return 0;
}

/* SGsAP-MO-CSFB-INDICATION, chapter 8.25 */
static int sgs_rx_csfb_ind(struct sgs_connection *sgc, struct msgb *msg, const struct tlv_parsed *tp, char *imsi)
{
	struct vlr_subscr *vsub;
	struct osmo_plmn_id last_eutran_plmn_buf;
	const struct osmo_plmn_id *last_eutran_plmn = &last_eutran_plmn_buf;

	/* The MME informs us with this message that the UE has initiated a
	 * service request for MO CS fallback. There is not much we can do with
	 * this information, however, we can check if the subscriber actually
	 * exists in the VLR and if there are any lingering connections open.*/

	vsub = vlr_subscr_find_by_imsi(gsm_network->vlr, imsi, __func__);
	if (!vsub) {
		/* A CSFB indication for a subscriber we do not know (yet) is
		 * common when the UE attempts CSFB before its SGs LU reached
		 * us; reply with STATUS but do not treat it as an error. */
		struct msgb *resp;
		LOGSGC(sgc, LOGL_INFO, "Rx %s for unknown IMSI %s, replying with status (cause %s)\n",
		       sgsap_msg_type_name(msg->data[0]), imsi,
		       sgsap_sgs_cause_name(SGSAP_SGS_CAUSE_IMSI_UNKNOWN));
		resp = gsm29118_create_status(imsi, SGSAP_SGS_CAUSE_IMSI_UNKNOWN, msg);
		sgs_tx(sgc, resp);
		return 0;
	}

	/* 3GPP TS 23.272 sec 4.3.3 (CSFB):
	 * "During the SGs location update procedure, obtaining the last used LTE PLMN ID via TAI"
	 */
	if (TLVP_PRES_LEN(tp, SGSAP_IE_TAI, 3)) {
		osmo_plmn_from_bcd(TLVP_VAL(tp, SGSAP_IE_TAI), &last_eutran_plmn_buf);
		/* TODO: we could also gather the TAC from here, but we don't need it yet */
	} else if (TLVP_PRES_LEN(tp, SGSAP_IE_EUTRAN_CGI, 3)) {
		/* Since TAI is optional, let's try harder getting Last Used
		 * E-UTRAN PLMN ID by fetching it from E-UTRAN CGI */
		osmo_plmn_from_bcd(TLVP_VAL(tp, SGSAP_IE_EUTRAN_CGI), &last_eutran_plmn_buf);
		/* TODO: we could also gather the ECI from here, but we don't need it yet */
	} else {
		LOGSGC(sgc, LOGL_INFO, "Receiving SGsAP-MO-CSFB-INDICATION without TAI nor "
		       "E-CGI IEs, and they are not known from previous SGsAP-LOCATION-UPDATE-REQUEST. "
		       "Fast fallback GERAN->EUTRAN won't be possible!\n");
		last_eutran_plmn = NULL;
	}

	vlr_subscr_set_last_used_eutran_plmn_id(vsub, last_eutran_plmn);

	/* Check for lingering connections */
	subscr_conn_toss(vsub);

	vlr_subscr_put(vsub, __func__);
	return 0;
}

/* SGsAP-UE-ACTIVITY-INDICATION, chapter 8.20 */
static int sgs_rx_ue_act_ind(struct sgs_connection *sgc, struct msgb *msg, const struct tlv_parsed *tp, char *imsi)
{
	/* In this MSC/VLR implementation we do not support the alerting
	 * procedure yet and therefore we will never request any alerting
	 * at the MME. Given that it is unlikely that we ever get activity
	 * indications from the MME, but if we do we should not act all too
	 * hostile and ignore the indication silently. */

	LOGSGC(sgc, LOGL_ERROR, "Rx %s unexpected, we do not implement alerting yet, ignoring!\n",
	       sgsap_msg_type_name(msg->data[0]));

	return 0;
}

#define TX_STATUS_AND_LOG(sgc, msg_type, cause, fmt) \
	LOGSGC(sgc, LOGL_ERROR, fmt, sgsap_msg_type_name(msg_type));	\
	resp = gsm29118_create_status(NULL, cause, msg); \
	sgs_tx(sgc, resp); \

/*! Process incoming SGs message (see sgs_server.c)
 *  \param[in] sgc related sgs connection
 *  \param[in] msg received message
 *  \returns 0 in case of success, -EINVAL in case of error. */
int sgs_iface_rx(struct sgs_connection *sgc, struct msgb *msg)
{
	struct msgb *resp;
	uint8_t msg_type = msg->l2h[0];
	struct tlv_parsed tp;
	int rc;
	char imsi[GSM48_MI_SIZE];
	char mme_name[SGS_MME_NAME_LEN + 1];

	memset(imsi, 0, sizeof(imsi));
	memset(mme_name, 0, sizeof(mme_name));

	/* When the receiving entity receives a message that is too short to contain a complete
	 * message type information element, the receiving entity shall ignore that message. */
	if (msgb_l2len(msg) < 1)
		goto error;

	/* Parse TLV elements */
	rc = tlv_parse(&tp, &sgsap_ie_tlvdef, msgb_l2(msg) + 1, msgb_l2len(msg) - 1, 0, 0);
	if (rc < 0)
		LOGSGC(sgc, LOGL_NOTICE, "SGsAP Message %s contains unknown TLV IEs\n", sgsap_msg_type_name(msg_type));

	/* Most of the messages contain an IMSI as mandatory IE, parse it right here */
	if (!TLVP_PRESENT(&tp, SGSAP_IE_IMSI) &&
	    msg_type != SGSAP_MSGT_STATUS && msg_type != SGSAP_MSGT_RESET_IND && msg_type != SGSAP_MSGT_RESET_ACK) {
		/* reject the message; all but the three above have mandatory IMSI */
		TX_STATUS_AND_LOG(sgc, msg_type, SGSAP_SGS_CAUSE_MISSING_MAND_IE,
				  "SGsAP Message %s without IMSI, dropping\n");
		goto error;
	}

	if (TLVP_PRESENT(&tp, SGSAP_IE_IMSI)) {
		struct osmo_mobile_identity mi;
		if (osmo_mobile_identity_decode(&mi,
						TLVP_VAL(&tp, SGSAP_IE_IMSI),
						TLVP_LEN(&tp, SGSAP_IE_IMSI), false)
		    ||  mi.type != GSM_MI_TYPE_IMSI) {
			TX_STATUS_AND_LOG(sgc, msg_type, SGSAP_SGS_CAUSE_INVALID_MAND_IE,
					  "SGsAP Message %s with invalid IMSI, dropping\n");
			goto error;
		}
		OSMO_STRLCPY_ARRAY(imsi, mi.imsi);
	}

	if (imsi[0]) {
		const uint8_t *payload;
		size_t plen;

		sgsap_msg_payload(msg, &payload, &plen);
		msc_api_trace_packet(imsi, "sgsap", true, payload, plen);
	}

	/* Some messages contain an MME-NAME as mandatory IE, parse it right here. The
	 * MME-NAME is also immediately registered with the sgc, so it will be implicitly
	 * known to all functions that have access to the sgc context. */
	if (!TLVP_PRESENT(&tp, SGSAP_IE_MME_NAME)
	    && (msg_type == SGSAP_MSGT_RESET_IND || msg_type == SGSAP_MSGT_RESET_ACK
		|| msg_type == SGSAP_MSGT_LOC_UPD_REQ || msg_type == SGSAP_MSGT_IMSI_DET_IND
		|| msg_type == SGSAP_MSGT_EPS_DET_IND)) {
		TX_STATUS_AND_LOG(sgc, msg_type, SGSAP_SGS_CAUSE_MISSING_MAND_IE,
				  "SGsAP Message %s without MME-Name, dropping\n");
		goto error;
	}

	if (TLVP_PRESENT(&tp, SGSAP_IE_MME_NAME)) {
		if (decode_mme_name(mme_name, &tp) != 0) {
			TX_STATUS_AND_LOG(sgc, msg_type, SGSAP_SGS_CAUSE_INVALID_MAND_IE,
					  "SGsAP Message %s with invalid MME-Name, dropping\n");
			goto error;
		}
		/* Regsister/check mme_name with sgc */
		if (sgs_mme_fqdn_received(sgc, mme_name) < 0) {
			TX_STATUS_AND_LOG(sgc, msg_type, SGSAP_SGS_CAUSE_MSG_INCOMP_STATE,
					  "SGsAP Message %s with invalid MME-Name, dropping\n");
			goto error;
		}
	}

	/* dispatch msg to various handler functions.  msgb ownership remains here! */
	rc = -EINVAL;
	switch (msg_type) {
	case SGSAP_MSGT_STATUS:
		rc = sgs_rx_status(sgc, msg, &tp, imsi);
		break;
	case SGSAP_MSGT_RESET_IND:
		rc = sgs_rx_reset_ind(sgc, msg, &tp);
		break;
	case SGSAP_MSGT_RESET_ACK:
		rc = sgs_rx_reset_ack(sgc, msg, &tp);
		break;
	case SGSAP_MSGT_LOC_UPD_REQ:
		rc = sgs_rx_loc_upd_req(sgc, msg, &tp, imsi);
		break;
	case SGSAP_MSGT_IMSI_DET_IND:
		rc = sgs_rx_imsi_det_ind(sgc, msg, &tp, imsi);
		break;
	case SGSAP_MSGT_EPS_DET_IND:
		rc = sgs_rx_eps_det_ind(sgc, msg, &tp, imsi);
		break;
	case SGSAP_MSGT_PAGING_REJ:
		rc = sgs_rx_pag_rej(sgc, msg, &tp, imsi);
		break;
	case SGSAP_MSGT_UE_UNREACHABLE:
		rc = sgs_rx_ue_unr(sgc, msg, &tp, imsi);
		break;
	case SGSAP_MSGT_TMSI_REALL_CMPL:
		rc = sgs_rx_tmsi_reall_cmpl(sgc, msg, &tp, imsi);
		break;
	case SGSAP_MSGT_SERVICE_REQ:
		rc = sgs_rx_service_req(sgc, msg, &tp, imsi);
		break;
	case SGSAP_MSGT_UL_UD:
		rc = sgs_rx_ul_ud(sgc, msg, &tp, imsi);
		break;
	case SGSAP_MSGT_MO_CSFB_IND:
		rc = sgs_rx_csfb_ind(sgc, msg, &tp, imsi);
		break;
	case SGSAP_MSGT_UE_ACT_IND:
		rc = sgs_rx_ue_act_ind(sgc, msg, &tp, imsi);
		break;
	case SGSAP_MSGT_ALERT_ACK:
	case SGSAP_MSGT_ALERT_REJ:
		LOGSGC(sgc, LOGL_ERROR, "Rx unmplemented SGsAP %s: %s\n",
		       sgsap_msg_type_name(msg_type), msgb_hexdump(msg));
		resp = gsm29118_create_status(imsi, SGSAP_SGS_CAUSE_MSG_UNKNOWN, msg);
		sgs_tx(sgc, resp);
		rc = 0;
		break;
	default:
		LOGSGC(sgc, LOGL_ERROR, "Rx unknown SGsAP message type 0x%02x: %s\n", msg_type, msgb_hexdump(msg));
		resp = gsm29118_create_status(imsi, SGSAP_SGS_CAUSE_MSG_UNKNOWN, msg);
		sgs_tx(sgc, resp);
		rc = 0;
		break;
	}

	/* Catch unhandled errors */
	if (rc < 0) {
		/* Note: Usually the sgs_rx_ should catch errors locally and
		 * eimit a status message with proper cause code, including
		 * a suitable log message. If we end up here, something is
		 * not right and should be fixed */
		LOGSGC(sgc, LOGL_ERROR, "Rx unable to decode SGsAP %s: %s\n",
		       sgsap_msg_type_name(msg_type), msgb_hexdump(msg));
		resp = gsm29118_create_status(imsi, SGSAP_SGS_CAUSE_MSG_UNKNOWN, msg);
		sgs_tx(sgc, resp);
	}

error:
	msgb_free(msg);
	return 0;
}

/***********************************************************************
 * SGs connection "VLR Reset Procedure" FSM
 ***********************************************************************/

static const struct value_string sgs_vlr_reset_fsm_event_names[] = {
	{SGS_VLRR_E_START_RESET, "START-RESET"},
	{SGS_VLRR_E_RX_RESET_ACK, "RX-RESET-ACK"},
	{0, NULL}
};

static void sgs_vlr_reset_fsm_null(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	switch (event) {
	case SGS_VLRR_E_RX_RESET_ACK:
		break;
	default:
		OSMO_ASSERT(0);
		break;
	}
}

static void sgs_vlr_reset_fsm_wait_ack(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	switch (event) {
	case SGS_VLRR_E_RX_RESET_ACK:
		osmo_fsm_inst_state_chg(fi, SGS_VLRR_ST_COMPLETE, 0, 0);
		break;
	default:
		OSMO_ASSERT(0);
		break;
	}
}

static void sgs_vlr_reset_fsm_complete(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	switch (event) {
	case SGS_VLRR_E_RX_RESET_ACK:
		break;
	default:
		OSMO_ASSERT(0);
		break;
	}
}

static void sgs_vlr_reset_fsm_allstate(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct msgb *reset_ind;
	struct gsm29118_reset_msg reset_params;
	struct sgs_mme_ctx *mme = (struct sgs_mme_ctx *)fi->priv;
	struct sgs_connection *sgc = mme->conn;
	struct sgs_state *sgs = mme->sgs;

	switch (event) {
	case SGS_VLRR_E_START_RESET:
		if (!sgc) {
			LOGMME(mme, LOGL_ERROR, "VLR reset: no SGs connection, cannot send RESET-IND\n");
			return;
		}
		osmo_fsm_inst_state_chg(fi, SGS_VLRR_ST_NULL, 0, 0);
		mme->ns11_remaining = sgs->cfg.counter[SGS_STATE_NS11];
		/* send a reset message and enter WAIT_ACK state */
		memset(&reset_params, 0, sizeof(reset_params));
		osmo_strlcpy(reset_params.vlr_name, sgs->cfg.vlr_name, sizeof(reset_params.vlr_name));
		reset_params.vlr_name_present = true;
		reset_ind = gsm29118_create_reset_ind(&reset_params);
		sgs_tx(sgc, reset_ind);

		/* VLR restart toward this MME only (do not wipe other MMEs).
		 * Keep SgsAssoc rows so 29.118 5.1.2.2 can page from SGs-NULL
		 * while Confirmed by Radio Contact is false. */
		vlr_sgs_reset_mme(gsm_network->vlr, mme->fqdn);

		osmo_fsm_inst_state_chg(fi, SGS_VLRR_ST_WAIT_ACK, sgs->cfg.timer[SGS_STATE_TS11], 11);
		break;
	default:
		OSMO_ASSERT(0);
		break;
	}
}

static int sgs_vlr_reset_fsm_timer_cb(struct osmo_fsm_inst *fi)
{
	struct msgb *reset_ind;
	struct gsm29118_reset_msg reset_params;
	struct sgs_mme_ctx *mme = (struct sgs_mme_ctx *)fi->priv;
	struct sgs_connection *sgc = mme->conn;
	struct sgs_state *sgs = mme->sgs;

	switch (fi->T) {
	case 11:
		if (mme->ns11_remaining >= 1) {
			memset(&reset_params, 0, sizeof(reset_params));
			osmo_strlcpy(reset_params.vlr_name, sgs->cfg.vlr_name, sizeof(reset_params.vlr_name));
			reset_params.vlr_name_present = true;
			reset_ind = gsm29118_create_reset_ind(&reset_params);
			sgs_tx(sgc, reset_ind);
			osmo_fsm_inst_state_chg(fi, SGS_VLRR_ST_WAIT_ACK, sgs->cfg.timer[SGS_STATE_TS11], 11);
			mme->ns11_remaining--;
		} else {
			LOGMME(mme, LOGL_ERROR, "Ts11 expired more than %u (Ns11) times, giving up\n",
			       sgs->cfg.counter[SGS_STATE_NS11]);
			osmo_fsm_inst_state_chg(fi, SGS_VLRR_ST_NULL, 0, 0);
		}
		break;
	default:
		OSMO_ASSERT(0);
		break;
	}
	return 0;
}

static const struct osmo_fsm_state sgs_vlr_reset_fsm_states[] = {
	[SGS_VLRR_ST_NULL] = {
		/* We haven't even tried yet to send a RESET */
		.name = "NULL",
		.action = sgs_vlr_reset_fsm_null,
		.in_event_mask = S(SGS_VLRR_E_RX_RESET_ACK),
		.out_state_mask = S(SGS_VLRR_ST_NULL) | S(SGS_VLRR_ST_WAIT_ACK),
	},
	[SGS_VLRR_ST_WAIT_ACK] = {
		/* We're waiting for a SGsAP_RESET_ACK */
		.name = "WAIT-ACK",
		.action = sgs_vlr_reset_fsm_wait_ack,
		.in_event_mask = S(SGS_VLRR_E_RX_RESET_ACK),
		.out_state_mask = S(SGS_VLRR_ST_NULL) |
		S(SGS_VLRR_ST_COMPLETE) | S(SGS_VLRR_ST_WAIT_ACK),
	},
	[SGS_VLRR_ST_COMPLETE] = {
		/* Reset procedure to this MME has been completed */
		.name = "COMPLETE",
		.action = sgs_vlr_reset_fsm_complete,
		.in_event_mask = S(SGS_VLRR_E_RX_RESET_ACK),
		.out_state_mask = S(SGS_VLRR_ST_NULL) | S(SGS_VLRR_ST_COMPLETE),
	},
};

static struct osmo_fsm sgs_vlr_reset_fsm = {
	.name = "SGs-VLR-RESET",
	.states = sgs_vlr_reset_fsm_states,
	.num_states = ARRAY_SIZE(sgs_vlr_reset_fsm_states),
	.allstate_event_mask = S(SGS_VLRR_E_START_RESET),
	.allstate_action = sgs_vlr_reset_fsm_allstate,
	.timer_cb = sgs_vlr_reset_fsm_timer_cb,
	.log_subsys = DSGS,
	.event_names = sgs_vlr_reset_fsm_event_names,
};

/*! Send unit-data through SGs interface (see msc_ifaces.c)
 *  \param[in] msg layer 3 message to send.
 *  \returns 0 in case of success, -EINVAL in case of error. */
int sgs_iface_tx_dtap_ud(struct msc_a *msc_a, struct msgb *msg)
{
	struct msgb *msg_sgs;
	struct sgs_mme_ctx *mme;
	int rc = -EINVAL;
	struct vlr_subscr *vsub = msc_a_vsub(msc_a);

	if (!vsub) {
		LOG_MSC_A(msc_a, LOGL_ERROR, "Cannot Tx SGs DTAP: subscriber already detached\n");
		goto error;
	}

	mme = sgs_mme_ctx_by_vsub(vsub, SGSAP_MSGT_DL_UD);
	if (!mme)
		goto error;

	/* Make sure the subscriber has a valid SGs association, otherwise
	 * don't let unit-data through. */
	if (vsub->sgs_fsm->state != SGS_UE_ST_ASSOCIATED) {
		LOG_MSC_A(msc_a, LOGL_NOTICE, "Cannot Tx %s: subscriber not SGs-associated\n",
			  sgsap_msg_type_name(SGSAP_MSGT_DL_UD));
		goto error;
	}

	msg_sgs = gsm29118_create_dl_ud(vsub->imsi, msg);
	sgs_tx(mme->conn, msg_sgs);
	rc = 0;

error:
	msgb_free(msg);
	return rc;
}

void sgs_iface_tx_release(struct vlr_subscr *vsub)
{
	struct msgb *msg_sgs;
	struct sgs_mme_ctx *mme;

	if (!vsub)
		return;

	mme = sgs_mme_ctx_by_vsub(vsub, SGSAP_MSGT_DL_UD);
	if (!mme)
		return;

	msg_sgs = gsm29118_create_release_req(vsub->imsi, 0);
	sgs_tx(mme->conn, msg_sgs);
}

/*! Send SGsAP-SERVICE-ABORT-REQUEST message to MME
 *  \param[in] vsub subscriber context */
void sgs_iface_tx_serv_abrt(struct vlr_subscr *vsub)
{
	struct msgb *msg_sgs;
	struct sgs_mme_ctx *mme;

	if (!vsub)
		return;

	/* The service abort procedure is only defined for MT calls,
	 * see also 3GPP TS 29.118, chapter 5.13.2 */
	if (vsub->sgs.paging_serv_ind != SGSAP_SERV_IND_CS_CALL)
		return;

	mme = sgs_mme_ctx_by_vsub(vsub, SGSAP_MSGT_DL_UD);
	if (!mme)
		return;

	msg_sgs = gsm29118_create_service_abort_req(vsub->imsi);
	sgs_tx(mme->conn, msg_sgs);
}

/*! initialize SGs new interface
 *  \param[in] ctx talloc context
 *  \param[in] network associated gsm network
 *  \returns returns allocated sgs_stae, NULL in case of error. */
static int sgs_restore_one(void *data, const struct db_sgs_assoc *row)
{
	struct gsm_network *net = data;
	struct vlr_subscr *vsub;
	struct timespec mono;
	time_t now = time(NULL);

	if (row->expire_unix > 0 && row->expire_unix <= now) {
		db_sgs_assoc_delete(row->imsi);
		return 0;
	}

	vsub = vlr_subscr_find_or_create_by_imsi(net->vlr, row->imsi, VSUB_USE_ATTACHED, NULL);
	if (!vsub)
		return -ENOMEM;

	if (row->msisdn[0])
		vlr_subscr_set_msisdn(vsub, row->msisdn);
	if (row->tmsi != GSM_RESERVED_TMSI) {
		vsub->tmsi = row->tmsi;
		vlr_subscr_rehash_tmsi(vsub);
	}
	OSMO_STRLCPY_ARRAY(vsub->sgs.mme_name, row->mme_name);
	vsub->sgs.lai = row->lai;
	vsub->cgi.lai = row->lai;
	vsub->cs.lac = row->lai.lac;
	if (row->last_eutran_plmn_present)
		vlr_subscr_set_last_used_eutran_plmn_id(vsub, &row->last_eutran_plmn);
	else
		vlr_subscr_set_last_used_eutran_plmn_id(vsub, NULL);

	if (g_sgs) {
		memcpy(vsub->sgs.cfg.timer, g_sgs->cfg.timer, sizeof(vsub->sgs.cfg.timer));
		memcpy(vsub->sgs.cfg.counter, g_sgs->cfg.counter, sizeof(vsub->sgs.cfg.counter));
	}
	vsub->sgs.paging_cb = sgs_iface_paging_cb;
	vsub->sgs.mminfo_cb = sgs_tx_mm_info_cb;
	vsub->sgs.response_cb = sgs_tx_loc_upd_resp_cb;

	vsub->cs.attached_via_ran = OSMO_RAT_EUTRAN_SGS;
	vsub->conf_by_radio_contact_ind = false;
	vsub->sub_dataconf_by_hlr_ind = false;
	vsub->loc_conf_in_hlr_ind = false;
	vlr_subscr_set_lu_complete(vsub, true);
	vsub->imsi_detached_flag = false;

	if (row->expire_unix > 0 && osmo_clock_gettime(CLOCK_MONOTONIC, &mono) == 0)
		vsub->expire_lu = mono.tv_sec + (row->expire_unix - now);
	else
		vsub->expire_lu = VLR_SUBSCRIBER_NO_EXPIRATION;

	/* SGs-NULL + unconfirmed: 29.118 5.7.2.1 / 5.1.2.2 */
	osmo_fsm_inst_state_chg(vsub->sgs_fsm, SGS_UE_ST_NULL, 0, 0);

	LOGP(DSGS, LOGL_DEBUG, "Restored SGs association IMSI=%s MME=%s (unconfirmed, SGs-NULL)\n",
	     vsub->imsi, vsub->sgs.mme_name);
	return 0;
}

int sgs_iface_restore_assocs(struct gsm_network *network)
{
	int n;

	if (!network || !network->vlr)
		return 0;
	if (!sgs_vlr_persist_enabled())
		return 0;

	n = db_sgs_assoc_foreach(sgs_restore_one, network);
	if (n < 0) {
		LOGP(DSGS, LOGL_ERROR, "Failed to restore SGs associations from database\n");
		return n;
	}
	if (n)
		LOGP(DSGS, LOGL_NOTICE,
		     "VLR restoration: loaded %d SGs association(s) (TS 23.007 / 29.118 5.1.2.2)\n", n);
	return 0;
}

struct sgs_state *sgs_iface_init(void *ctx, struct gsm_network *network)
{
	struct sgs_state *sgs;

	gsm_network = network;

	sgs = sgs_server_alloc(ctx);
	OSMO_ASSERT(sgs);

	/* We currently only support one SGs instance */
	if (g_sgs)
		return NULL;
	g_sgs = sgs;
	sgs_vlr_persist_init(sgs);

	return sgs;
}

static __attribute__((constructor)) void on_dso_load(void)
{
	OSMO_ASSERT(osmo_fsm_register(&sgs_vlr_reset_fsm) == 0);
}
