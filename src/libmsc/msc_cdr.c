/* OsmoMSC CS/SMS CDR dumper (OsmoSGSN-style CSV + optional CTRL trap).
 *
 * Writes one line per completed (or abandoned) CS call or SMS.  This is
 * not a Huawei/MSOFTX ASN.1 CDR: OsmoMSC has no trunk, GT, MSRN, NCI or
 * subscriber-category data.  The file includes everything the MSC does
 * have: identities, numbers, CGI, RAN, timestamps, duration, CC/RP
 * cause, bearer/codec and SMS metadata.
 */

/* (C) 2026 by Osmocom contributors
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include <osmocom/msc/msc_cdr.h>

#include <osmocom/ctrl/control_if.h>
#include <osmocom/core/utils.h>
#include <osmocom/gsm/gsm23003.h>
#include <osmocom/gsm/gsm_utils.h>
#include <osmocom/gsm/mncc.h>
#include <osmocom/gsm/protocol/gsm_04_08.h>

#include <osmocom/msc/debug.h>
#include <osmocom/msc/gsm_data.h>
#include <osmocom/msc/mncc.h>
#include <osmocom/msc/msc_a.h>
#include <osmocom/msc/ran_infra.h>
#include <osmocom/msc/sdp_msg.h>
#include <osmocom/msc/transaction.h>
#include <osmocom/vlr/vlr.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define CDR_LINE_MAX 2048

static const char cdr_header[] =
	"timestamp,event,result,dir,imsi,imei,imeisv,tmsi,msisdn,"
	"calling,calling_ton,calling_npi,calling_present,calling_screen,"
	"called,called_ton,called_npi,connected,redirecting,"
	"src,src_ton,src_npi,dst,dst_ton,dst_npi,"
	"mcc,mnc,lac,ci,ran,mme,"
	"setup_time,answer_time,duration,ringing,"
	"cause,cause_loc,cause_coding,"
	"callref,tid,call_id,emergency,clir_sup,clir_inv,"
	"bearer,codec,cc_state,"
	"sms_id,sms_source,dcs,pid,udhi,rp_mr,text_len\n";

static bool cdr_enabled(const struct gsm_network *net)
{
	return net && (net->cdr.filename || net->cdr.trap);
}

static void fmt_now(char *buf, size_t len)
{
	struct timeval tv;
	struct tm tm;

	gettimeofday(&tv, NULL);
	gmtime_r(&tv.tv_sec, &tm);
	snprintf(buf, len, "%04d%02d%02d%02d%02d%02d%03d",
		 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		 tm.tm_hour, tm.tm_min, tm.tm_sec,
		 (int)(tv.tv_usec / 1000));
}

static void fmt_time(char *buf, size_t len, time_t t)
{
	struct tm tm;

	if (!t) {
		buf[0] = '\0';
		return;
	}
	gmtime_r(&t, &tm);
	snprintf(buf, len, "%04d%02d%02d%02d%02d%02d",
		 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		 tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static const char *itcap_name(int transfer)
{
	switch (transfer) {
	case GSM48_BCAP_ITCAP_SPEECH:
		return "speech";
	case GSM48_BCAP_ITCAP_UNR_DIG_INF:
		return "udi";
	case GSM48_BCAP_ITCAP_3k1_AUDIO:
		return "3.1khz";
	case GSM48_BCAP_ITCAP_FAX_G3:
		return "fax";
	default:
		return "";
	}
}

static const char *sms_source_name(enum gsm_sms_source_id src)
{
	switch (src) {
	case SMS_SOURCE_MS:
		return "ms";
	case SMS_SOURCE_VTY:
		return "vty";
	case SMS_SOURCE_SMPP:
		return "smpp";
	default:
		return "unknown";
	}
}

static const char *trans_ran_name(const struct gsm_trans *trans)
{
	enum osmo_rat_type ran = OSMO_RAT_UNKNOWN;

	if (trans->msc_a && trans->msc_a->c.ran)
		ran = trans->msc_a->c.ran->type;
	else if (trans->vsub)
		ran = trans->vsub->cs.attached_via_ran;

	return osmo_rat_type_name(ran);
}

static const char *trans_codec_name(const struct gsm_trans *trans)
{
	if (trans->type != TRANS_CC)
		return "";
	if (!trans->cc.local.audio_codecs.count)
		return "";
	return trans->cc.local.audio_codecs.codec[0].subtype_name;
}

static void maybe_print_header(FILE *f)
{
	if (ftell(f) != 0)
		return;
	fputs(cdr_header, f);
}

static void cdr_write(struct gsm_network *net, const char *line)
{
	FILE *f;

	if (net->cdr.trap && net->ctrl) {
		if (ctrl_cmd_send_trap(net->ctrl, "cdr-v1", (char *)line) < 0)
			LOGP(DMSC, LOGL_ERROR, "Failed to send CTRL trap cdr-v1\n");
	}

	if (!net->cdr.filename)
		return;

	f = fopen(net->cdr.filename, "a");
	if (!f) {
		LOGP(DMSC, LOGL_ERROR, "Failed to open CDR file %s\n",
		     net->cdr.filename);
		return;
	}
	maybe_print_header(f);
	fprintf(f, "%s\n", line);
	fclose(f);
}

void msc_cdr_note_setup(struct gsm_trans *trans, const struct gsm_mncc *setup)
{
	if (!trans || !setup)
		return;

	if (!trans->cdr.t_setup)
		trans->cdr.t_setup = time(NULL);
	trans->cdr.setup_seen = true;
	if (setup->msg_type)
		trans->cdr.setup_msg_type = setup->msg_type;

	if (setup->fields & MNCC_F_CALLING) {
		OSMO_STRLCPY_ARRAY(trans->cdr.calling, setup->calling.number);
		trans->cdr.calling_type = setup->calling.type;
		trans->cdr.calling_plan = setup->calling.plan;
		trans->cdr.calling_present = setup->calling.present;
		trans->cdr.calling_screen = setup->calling.screen;
	}
	if (setup->fields & MNCC_F_CALLED) {
		OSMO_STRLCPY_ARRAY(trans->cdr.called, setup->called.number);
		trans->cdr.called_type = setup->called.type;
		trans->cdr.called_plan = setup->called.plan;
	}
	if (setup->fields & MNCC_F_CONNECTED)
		OSMO_STRLCPY_ARRAY(trans->cdr.connected, setup->connected.number);
	if (setup->fields & MNCC_F_REDIRECTING)
		OSMO_STRLCPY_ARRAY(trans->cdr.redirecting, setup->redirecting.number);

	if (setup->fields & MNCC_F_EMERGENCY || setup->emergency)
		trans->cdr.emergency = 1;
	if (setup->clir.sup)
		trans->cdr.clir_sup = 1;
	if (setup->clir.inv)
		trans->cdr.clir_inv = 1;
	if (setup->fields & MNCC_F_CAUSE)
		msc_cdr_note_cause(trans, &setup->cause);
}

void msc_cdr_note_connected(struct gsm_trans *trans, const char *number)
{
	if (!trans || !number || !number[0])
		return;
	OSMO_STRLCPY_ARRAY(trans->cdr.connected, number);
}

void msc_cdr_note_cause(struct gsm_trans *trans, const struct gsm_mncc_cause *cause)
{
	if (!trans || !cause)
		return;
	trans->cdr.cause_present = true;
	trans->cdr.cause = cause->value;
	trans->cdr.cause_loc = cause->location;
	trans->cdr.cause_coding = cause->coding;
}

void msc_cdr_note_answer(struct gsm_trans *trans)
{
	if (!trans)
		return;
	if (!trans->cdr.t_answer)
		trans->cdr.t_answer = time(NULL);
}

static const char *call_dir(const struct gsm_trans *trans)
{
	switch (trans->cdr.setup_msg_type) {
	case MNCC_SETUP_IND:
		return "mo";
	case MNCC_SETUP_REQ:
		return "mt";
	default:
		return "";
	}
}

static const char *call_event(const struct gsm_trans *trans)
{
	switch (trans->cdr.setup_msg_type) {
	case MNCC_SETUP_IND:
		return "mo-call";
	case MNCC_SETUP_REQ:
		return "mt-call";
	default:
		return "call";
	}
}

static const char *call_result(const struct gsm_trans *trans)
{
	if (trans->cdr.t_answer)
		return "answered";
	if (trans->cdr.cause_present)
		return "failed";
	return "unanswered";
}

static void fill_subscr_loc(const struct gsm_trans *trans,
			    const char **imsi, const char **imei, const char **imeisv,
			    const char **msisdn, const char **tmsi,
			    char *tmsi_buf, size_t tmsi_buf_len,
			    char *mcc, size_t mcc_len,
			    char *mnc, size_t mnc_len,
			    char *lac, size_t lac_len,
			    char *ci, size_t ci_len,
			    const char **mme)
{
	const struct vlr_subscr *vsub = trans->vsub;

	*imsi = "";
	*imei = "";
	*imeisv = "";
	*msisdn = "";
	*tmsi = "";
	*mme = "";
	mcc[0] = mnc[0] = lac[0] = ci[0] = '\0';

	if (!vsub)
		return;

	if (vsub->imsi[0])
		*imsi = vsub->imsi;
	if (vsub->imei[0])
		*imei = vsub->imei;
	if (vsub->imeisv[0])
		*imeisv = vsub->imeisv;
	if (vsub->msisdn[0])
		*msisdn = vsub->msisdn;
	if (vsub->tmsi != GSM_RESERVED_TMSI) {
		snprintf(tmsi_buf, tmsi_buf_len, "%08x", vsub->tmsi);
		*tmsi = tmsi_buf;
	}

	if (vsub->cgi.lai.plmn.mcc)
		snprintf(mcc, mcc_len, "%03u", vsub->cgi.lai.plmn.mcc);
	if (vsub->cgi.lai.plmn.mcc || vsub->cgi.lai.plmn.mnc) {
		if (vsub->cgi.lai.plmn.mnc_3_digits)
			snprintf(mnc, mnc_len, "%03u", vsub->cgi.lai.plmn.mnc);
		else
			snprintf(mnc, mnc_len, "%02u", vsub->cgi.lai.plmn.mnc);
	}
	if (vsub->cgi.lai.lac)
		snprintf(lac, lac_len, "%u", vsub->cgi.lai.lac);
	if (vsub->cgi.cell_identity)
		snprintf(ci, ci_len, "%u", vsub->cgi.cell_identity);
	if (vsub->sgs.mme_name[0])
		*mme = vsub->sgs.mme_name;
}

void msc_cdr_call(struct gsm_trans *trans)
{
	struct gsm_network *net;
	char now[32], setup_t[32], answer_t[32];
	char tmsi_buf[12], mcc[8], mnc[8], lac[12], ci[12];
	char cause_s[12], loc_s[12], coding_s[12];
	char line[CDR_LINE_MAX];
	const char *imsi, *imei, *imeisv, *msisdn, *tmsi, *mme;
	time_t now_t, duration = 0, ringing = 0;

	if (!trans || trans->cdr.written)
		return;
	net = trans->net;
	if (!cdr_enabled(net))
		return;

	trans->cdr.written = true;
	now_t = time(NULL);
	fmt_now(now, sizeof(now));
	fmt_time(setup_t, sizeof(setup_t), trans->cdr.t_setup);
	fmt_time(answer_t, sizeof(answer_t), trans->cdr.t_answer);

	if (trans->cdr.t_answer)
		duration = now_t - trans->cdr.t_answer;
	if (trans->cdr.t_setup) {
		if (trans->cdr.t_answer)
			ringing = trans->cdr.t_answer - trans->cdr.t_setup;
		else
			ringing = now_t - trans->cdr.t_setup;
	}

	fill_subscr_loc(trans, &imsi, &imei, &imeisv, &msisdn, &tmsi, tmsi_buf,
			sizeof(tmsi_buf), mcc, sizeof(mcc), mnc, sizeof(mnc),
			lac, sizeof(lac), ci, sizeof(ci), &mme);

	if (trans->cdr.cause_present) {
		snprintf(cause_s, sizeof(cause_s), "%d", trans->cdr.cause);
		snprintf(loc_s, sizeof(loc_s), "%d", trans->cdr.cause_loc);
		snprintf(coding_s, sizeof(coding_s), "%d", trans->cdr.cause_coding);
	} else {
		cause_s[0] = loc_s[0] = coding_s[0] = '\0';
	}

	snprintf(line, sizeof(line),
		 /* 1-9 */ "%s,%s,%s,%s,%s,%s,%s,%s,%s,"
		 /* 10-14 */ "%s,%d,%d,%d,%d,"
		 /* 15-19 */ "%s,%d,%d,%s,%s,"
		 /* 20-25 */ ",,,,,,"
		 /* 26-31 */ "%s,%s,%s,%s,%s,%s,"
		 /* 32-35 */ "%s,%s,%ld,%ld,"
		 /* 36-38 */ "%s,%s,%s,"
		 /* 39-44 */ "0x%x,%u,%u,%d,%d,%d,"
		 /* 45-47 */ "%s,%s,%s,"
		 /* 48-54 */ ",,,,,,",
		 now, call_event(trans), call_result(trans), call_dir(trans),
		 imsi, imei, imeisv, tmsi, msisdn,
		 trans->cdr.calling, trans->cdr.calling_type, trans->cdr.calling_plan,
		 trans->cdr.calling_present, trans->cdr.calling_screen,
		 trans->cdr.called, trans->cdr.called_type, trans->cdr.called_plan,
		 trans->cdr.connected, trans->cdr.redirecting,
		 mcc, mnc, lac, ci, trans_ran_name(trans), mme,
		 setup_t, answer_t, (long)duration, (long)ringing,
		 cause_s, loc_s, coding_s,
		 trans->callref, trans->transaction_id, trans->call_id,
		 trans->cdr.emergency, trans->cdr.clir_sup, trans->cdr.clir_inv,
		 itcap_name(trans->bearer_cap.transfer), trans_codec_name(trans),
		 gsm48_cc_state_name(trans->cc.state));

	cdr_write(net, line);
}

void msc_cdr_sms(struct gsm_trans *trans, struct gsm_sms *sms,
		 const char *result, int cause)
{
	struct gsm_network *net;
	char now[32], setup_t[32];
	char tmsi_buf[12], mcc[8], mnc[8], lac[12], ci[12];
	char cause_s[12];
	char line[CDR_LINE_MAX];
	const char *imsi, *imei, *imeisv, *msisdn, *tmsi, *mme;
	const char *event, *dir;
	time_t now_t, duration = 0;

	if (!trans || trans->cdr.written || !sms)
		return;
	net = trans->net;
	if (!cdr_enabled(net))
		return;

	trans->cdr.written = true;
	now_t = time(NULL);
	fmt_now(now, sizeof(now));
	fmt_time(setup_t, sizeof(setup_t), sms->created);

	if (sms->created)
		duration = now_t - sms->created;

	if (sms->source == SMS_SOURCE_MS) {
		event = "mo-sms";
		dir = "mo";
	} else {
		event = "mt-sms";
		dir = "mt";
	}

	fill_subscr_loc(trans, &imsi, &imei, &imeisv, &msisdn, &tmsi, tmsi_buf,
			sizeof(tmsi_buf), mcc, sizeof(mcc), mnc, sizeof(mnc),
			lac, sizeof(lac), ci, sizeof(ci), &mme);

	if (cause)
		snprintf(cause_s, sizeof(cause_s), "%d", cause);
	else
		cause_s[0] = '\0';

	snprintf(line, sizeof(line),
		 /* 1-9 */ "%s,%s,%s,%s,%s,%s,%s,%s,%s,"
		 /* 10-14 */ ",,,,,"
		 /* 15-19 */ ",,,,,"
		 /* 20-25 */ "%s,%u,%u,%s,%u,%u,"
		 /* 26-31 */ "%s,%s,%s,%s,%s,%s,"
		 /* 32-35 */ "%s,,%ld,,"
		 /* 36-38 */ "%s,,"
		 /* 39-44 */ "0x%x,%u,%u,,,,"
		 /* 45-47 */ ",,,"
		 /* 48-54 */ "%llu,%s,%u,%u,%u,%u,%u",
		 now, event, result ? result : "", dir,
		 imsi, imei, imeisv, tmsi, msisdn,
		 sms->src.addr, sms->src.ton, sms->src.npi,
		 sms->dst.addr, sms->dst.ton, sms->dst.npi,
		 mcc, mnc, lac, ci, trans_ran_name(trans), mme,
		 setup_t, (long)duration,
		 cause_s,
		 trans->callref, trans->transaction_id, trans->call_id,
		 (unsigned long long)sms->id, sms_source_name(sms->source),
		 sms->data_coding_scheme, sms->protocol_id,
		 sms->ud_hdr_ind, trans->sms.sm_rp_mr,
		 sms->user_data_len);

	cdr_write(net, line);
}
