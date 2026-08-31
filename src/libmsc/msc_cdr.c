/* OsmoMSC charging ticket: CSV aligned with Huawei/OFCS CallEventRecord fields.
 *
 * OsmoMSC writes a human-readable ticket. A separate OFCS encoder turns
 * rotated files into Huawei bA*.dat. Default off until cdr filename is set.
 */

/* (C) 2026 by Osmocom contributors
 * SPDX-License-Identifier: AGPL-3.0+
 */

#include <osmocom/msc/msc_cdr.h>

#include <osmocom/ctrl/control_if.h>
#include <osmocom/core/timer.h>
#include <osmocom/core/utils.h>
#include <osmocom/gsm/gsm23003.h>
#include <osmocom/gsm/gsm_utils.h>
#include <osmocom/gsm/mncc.h>
#include <osmocom/gsm/protocol/gsm_04_08.h>
#include <osmocom/sigtran/sccp_sap.h>

#include <osmocom/msc/debug.h>
#include <osmocom/msc/gsm_data.h>
#include <osmocom/msc/mncc.h>
#include <osmocom/msc/msc_a.h>
#include <osmocom/msc/msc_i.h>
#include <osmocom/msc/ran_conn.h>
#include <osmocom/msc/ran_infra.h>
#include <osmocom/msc/ran_peer.h>
#include <osmocom/msc/sdp_msg.h>
#include <osmocom/msc/transaction.h>
#include <osmocom/vlr/vlr.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define CDR_LINE_MAX 4096

/* 3GPP TS 32.005 / Huawei CauseForTerm */
#define CFT_NORMAL_RELEASE		0
#define CFT_PARTIAL_RECORD		1
#define CFT_UNSUCCESSFUL_ATTEMPT	3
#define CFT_ABNORMAL_TERMINATION	4

static const char cdr_header[] =
	"record_type,record_number,sequence_number,partial,"
	"recording_entity,msc_address,"
	"served_imsi,served_imei,served_imeisv,served_msisdn,"
	"calling_number,called_number,translated_number,connected_number,"
	"destination_number,origination,service_centre,"
	"seizure_time,setup_time,alerting_time,answer_time,release_time,"
	"origination_time,delivery_time,"
	"call_duration,ringing,cause_for_term,diagnostics,"
	"call_reference,network_call_reference,"
	"mcc,mnc,lac,ci,global_area_id,first_mccmnc,last_mccmnc,"
	"ran,system_type,incoming_route,outgoing_route,"
	"bsc_pc,cipher,ms_classmark,hlr,mme,emergency,bearer,codec,result,"
	"sms_dcs,sms_pid,sms_rp_mr,sms_text_len\n";

static bool cdr_enabled(const struct gsm_network *net)
{
	return net && (net->cdr.filename || net->cdr.trap);
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

static const char *recording_entity(const struct gsm_network *net)
{
	if (net->cdr.recording_entity && net->cdr.recording_entity[0])
		return net->cdr.recording_entity;
	if (net->msc_ipa_name && net->msc_ipa_name[0])
		return net->msc_ipa_name;
	return "OsmoMSC";
}

static enum osmo_rat_type ctx_ran(const struct gsm_trans *trans,
				  const struct msc_a *msc_a,
				  const struct vlr_subscr *vsub)
{
	if (trans && trans->msc_a && trans->msc_a->c.ran)
		return trans->msc_a->c.ran->type;
	if (msc_a && msc_a->c.ran)
		return msc_a->c.ran->type;
	if (vsub)
		return vsub->cs.attached_via_ran;
	return OSMO_RAT_UNKNOWN;
}

static const char *system_type_str(enum osmo_rat_type ran)
{
	switch (ran) {
	case OSMO_RAT_GERAN_A:
		return "2"; /* gERAN */
	case OSMO_RAT_UTRAN_IU:
		return "1"; /* iuUTRAN */
	default:
		return "0";
	}
}

static const char *incoming_route(enum osmo_rat_type ran)
{
	switch (ran) {
	case OSMO_RAT_GERAN_A:
		return "GERAN";
	case OSMO_RAT_UTRAN_IU:
		return "UTRAN";
	case OSMO_RAT_EUTRAN_SGS:
		return "SGS";
	default:
		return "";
	}
}

static const char *trans_codec_name(const struct gsm_trans *trans)
{
	if (!trans || trans->type != TRANS_CC)
		return "";
	if (!trans->cc.local.audio_codecs.count)
		return "";
	return trans->cc.local.audio_codecs.codec[0].subtype_name;
}

static int cause_for_term(bool answered, bool partial, bool cause_present, int cause)
{
	if (partial)
		return CFT_PARTIAL_RECORD;
	if (!answered)
		return CFT_UNSUCCESSFUL_ATTEMPT;
	if (!cause_present)
		return CFT_NORMAL_RELEASE;
	switch (cause) {
	case GSM48_CC_CAUSE_NORM_CALL_CLEAR:
	case GSM48_CC_CAUSE_NORMAL_UNSPEC:
		return CFT_NORMAL_RELEASE;
	default:
		return CFT_ABNORMAL_TERMINATION;
	}
}

static void fill_classmark(const struct vlr_subscr *vsub, char *buf, size_t len)
{
	buf[0] = '\0';
	if (!vsub || !vsub->classmark.classmark2_len)
		return;
	osmo_hexdump_buf(buf, len, (const unsigned char *)&vsub->classmark.classmark2,
			 vsub->classmark.classmark2_len, "", false);
}

static void fill_hlr(const struct vlr_subscr *vsub, char *buf, size_t len)
{
	buf[0] = '\0';
	if (!vsub || !vsub->hlr.len)
		return;
	osmo_hexdump_buf(buf, len, vsub->hlr.buf, vsub->hlr.len, "", false);
}

static void fill_bsc_pc(const struct gsm_trans *trans, const struct msc_a *msc_a,
			char *buf, size_t len)
{
	const struct msc_a *a = msc_a;
	struct msc_i *msc_i;
	struct ran_peer *rp;

	buf[0] = '\0';
	if (!a && trans)
		a = trans->msc_a;
	if (!a)
		return;
	msc_i = msc_a_msc_i(a);
	if (!msc_i || !msc_i->ran_conn || !msc_i->ran_conn->ran_peer)
		return;
	rp = msc_i->ran_conn->ran_peer;
	if (rp->peer_addr.presence & OSMO_SCCP_ADDR_T_PC)
		snprintf(buf, len, "%u", rp->peer_addr.pc);
}

static void fill_cipher(const struct gsm_trans *trans, const struct msc_a *msc_a,
			char *buf, size_t len)
{
	const struct msc_a *a = msc_a;

	buf[0] = '\0';
	if (!a && trans)
		a = trans->msc_a;
	if (!a || !a->geran_encr.alg_id)
		return;
	snprintf(buf, len, "%u", a->geran_encr.alg_id);
}

static void cdr_maybe_rotate(struct gsm_network *net)
{
	time_t now;
	struct tm tm_now, tm_prev;
	char dest[PATH_MAX];
	bool changed = false;

	if (!net->cdr.filename || net->cdr.rotate == MSC_CDR_ROTATE_NONE)
		return;

	now = time(NULL);
	if (!net->cdr.rotate_anchor) {
		net->cdr.rotate_anchor = now;
		return;
	}

	gmtime_r(&now, &tm_now);
	gmtime_r(&net->cdr.rotate_anchor, &tm_prev);

	if (net->cdr.rotate == MSC_CDR_ROTATE_HOURLY)
		changed = tm_now.tm_year != tm_prev.tm_year
			  || tm_now.tm_yday != tm_prev.tm_yday
			  || tm_now.tm_hour != tm_prev.tm_hour;
	else
		changed = tm_now.tm_year != tm_prev.tm_year
			  || tm_now.tm_yday != tm_prev.tm_yday;

	if (!changed)
		return;

	snprintf(dest, sizeof(dest), "%s.%04d%02d%02d%02d",
		 net->cdr.filename,
		 tm_prev.tm_year + 1900, tm_prev.tm_mon + 1,
		 tm_prev.tm_mday, tm_prev.tm_hour);
	if (rename(net->cdr.filename, dest) < 0) {
		if (errno != ENOENT)
			LOGP(DMSC, LOGL_ERROR, "CDR rotate rename %s -> %s failed: %s\n",
			     net->cdr.filename, dest, strerror(errno));
	} else {
		LOGP(DMSC, LOGL_NOTICE, "CDR rotated to %s\n", dest);
	}

	net->cdr.rotate_anchor = now;
}

static void cdr_rotate_cb(void *data)
{
	struct gsm_network *net = data;

	cdr_maybe_rotate(net);
	if (net->cdr.rotate != MSC_CDR_ROTATE_NONE && net->cdr.filename)
		osmo_timer_schedule(&net->cdr.rotate_timer, 60, 0);
}

void msc_cdr_reconfigure(struct gsm_network *net)
{
	osmo_timer_del(&net->cdr.rotate_timer);
	if (net->cdr.rotate != MSC_CDR_ROTATE_NONE && net->cdr.filename)
		osmo_timer_schedule(&net->cdr.rotate_timer, 60, 0);
}

void msc_cdr_init(struct gsm_network *net)
{
	osmo_timer_setup(&net->cdr.rotate_timer, cdr_rotate_cb, net);
	net->cdr.next_record_number = 1;
}

static void maybe_print_header(FILE *f)
{
	if (ftell(f) != 0)
		return;
	fputs(cdr_header, f);
}

static void cdr_write_line(struct gsm_network *net, const char *line)
{
	FILE *f;

	if (net->cdr.trap && net->ctrl) {
		if (ctrl_cmd_send_trap(net->ctrl, "cdr-v1", (char *)line) < 0)
			LOGP(DMSC, LOGL_ERROR, "Failed to send CTRL trap cdr-v1\n");
	}

	if (!net->cdr.filename)
		return;

	cdr_maybe_rotate(net);

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

static uint64_t next_record_number(struct gsm_network *net)
{
	return net->cdr.next_record_number++;
}

static void emit_row(struct gsm_network *net, struct vlr_subscr *vsub,
		     struct gsm_trans *trans, struct msc_a *msc_a,
		     int record_type, int sequence, int partial,
		     const char *calling, const char *called,
		     const char *translated, const char *connected,
		     const char *destination, const char *origination,
		     const char *service_centre,
		     time_t t_seizure, time_t t_setup, time_t t_alert,
		     time_t t_answer, time_t t_release,
		     time_t t_origination, time_t t_delivery,
		     long duration, long ringing,
		     int cft, const char *diagnostics,
		     uint32_t callref,
		     const char *outgoing_route,
		     int emergency, const char *bearer, const char *codec,
		     const char *result,
		     const char *sms_dcs, const char *sms_pid,
		     const char *sms_rp_mr, const char *sms_text_len)
{
	char line[CDR_LINE_MAX];
	char seizure[32], setup[32], alert[32], answer[32], release[32];
	char orig_t[32], deliv[32];
	char mcc[8], mnc[8], lac[12], ci[12], gai[48], mccmnc[16];
	char classmark[80], hlr[80], bsc_pc[16], cipher[8];
	const char *imsi = "", *imei = "", *imeisv = "", *msisdn = "", *mme = "";
	enum osmo_rat_type ran;
	const char *entity;

	if (!cdr_enabled(net))
		return;

	entity = recording_entity(net);
	fmt_time(seizure, sizeof(seizure), t_seizure);
	fmt_time(setup, sizeof(setup), t_setup);
	fmt_time(alert, sizeof(alert), t_alert);
	fmt_time(answer, sizeof(answer), t_answer);
	fmt_time(release, sizeof(release), t_release);
	fmt_time(orig_t, sizeof(orig_t), t_origination);
	fmt_time(deliv, sizeof(deliv), t_delivery);

	mcc[0] = mnc[0] = lac[0] = ci[0] = gai[0] = mccmnc[0] = '\0';
	if (vsub) {
		if (vsub->imsi[0])
			imsi = vsub->imsi;
		if (vsub->imei[0])
			imei = vsub->imei;
		if (vsub->imeisv[0])
			imeisv = vsub->imeisv;
		if (vsub->msisdn[0])
			msisdn = vsub->msisdn;
		if (vsub->sgs.mme_name[0])
			mme = vsub->sgs.mme_name;
		if (vsub->cgi.lai.plmn.mcc)
			snprintf(mcc, sizeof(mcc), "%03u", vsub->cgi.lai.plmn.mcc);
		if (vsub->cgi.lai.plmn.mcc || vsub->cgi.lai.plmn.mnc) {
			if (vsub->cgi.lai.plmn.mnc_3_digits)
				snprintf(mnc, sizeof(mnc), "%03u", vsub->cgi.lai.plmn.mnc);
			else
				snprintf(mnc, sizeof(mnc), "%02u", vsub->cgi.lai.plmn.mnc);
		}
		if (vsub->cgi.lai.lac)
			snprintf(lac, sizeof(lac), "%u", vsub->cgi.lai.lac);
		if (vsub->cgi.cell_identity)
			snprintf(ci, sizeof(ci), "%u", vsub->cgi.cell_identity);
		if (mcc[0] && mnc[0]) {
			snprintf(mccmnc, sizeof(mccmnc), "%s%s", mcc, mnc);
			snprintf(gai, sizeof(gai), "%s-%s-%s-%s", mcc, mnc, lac, ci);
		}
	}

	ran = ctx_ran(trans, msc_a, vsub);
	fill_classmark(vsub, classmark, sizeof(classmark));
	fill_hlr(vsub, hlr, sizeof(hlr));
	fill_bsc_pc(trans, msc_a, bsc_pc, sizeof(bsc_pc));
	fill_cipher(trans, msc_a, cipher, sizeof(cipher));

	snprintf(line, sizeof(line),
		 "%d,%" PRIu64 ",%d,%d,"
		 "%s,%s,"
		 "%s,%s,%s,%s,"
		 "%s,%s,%s,%s,"
		 "%s,%s,%s,"
		 "%s,%s,%s,%s,%s,"
		 "%s,%s,"
		 "%ld,%ld,%d,%s,"
		 "0x%x,0x%x,"
		 "%s,%s,%s,%s,%s,%s,%s,"
		 "%s,%s,%s,%s,"
		 "%s,%s,%s,%s,%s,%d,%s,%s,%s,"
		 "%s,%s,%s,%s",
		 record_type, next_record_number(net), sequence, partial,
		 entity, entity,
		 imsi, imei, imeisv, msisdn,
		 calling ? calling : "", called ? called : "",
		 translated ? translated : "", connected ? connected : "",
		 destination ? destination : "", origination ? origination : "",
		 service_centre ? service_centre : "",
		 seizure, setup, alert, answer, release,
		 orig_t, deliv,
		 duration, ringing, cft, diagnostics ? diagnostics : "",
		 callref, callref,
		 mcc, mnc, lac, ci, gai, mccmnc, mccmnc,
		 osmo_rat_type_name(ran), system_type_str(ran),
		 incoming_route(ran), outgoing_route ? outgoing_route : "",
		 bsc_pc, cipher, classmark, hlr, mme, emergency,
		 bearer ? bearer : "", codec ? codec : "",
		 result ? result : "",
		 sms_dcs ? sms_dcs : "", sms_pid ? sms_pid : "",
		 sms_rp_mr ? sms_rp_mr : "", sms_text_len ? sms_text_len : "");

	cdr_write_line(net, line);
}

static void cdr_interval_cb(void *data);

static void cdr_arm_interval(struct gsm_trans *trans)
{
	if (!trans || !trans->net || !trans->net->cdr.interval)
		return;
	osmo_timer_setup(&trans->cdr.interval_timer, cdr_interval_cb, trans);
	osmo_timer_schedule(&trans->cdr.interval_timer, trans->net->cdr.interval, 0);
}

static void emit_call(struct gsm_trans *trans, bool partial)
{
	struct gsm_network *net;
	time_t now_t;
	long duration = 0, ringing = 0;
	int rec_type, cft;
	char diag[16];
	const char *result;
	const char *out_route;

	if (!trans)
		return;
	net = trans->net;
	if (!cdr_enabled(net))
		return;
	if (!partial && trans->cdr.written)
		return;

	now_t = time(NULL);
	if (trans->cdr.t_answer)
		duration = now_t - trans->cdr.t_answer;
	if (trans->cdr.t_setup) {
		if (trans->cdr.t_answer)
			ringing = trans->cdr.t_answer - trans->cdr.t_setup;
		else
			ringing = now_t - trans->cdr.t_setup;
	}

	if (trans->cdr.setup_msg_type == MNCC_SETUP_REQ)
		rec_type = MSC_CDR_MTC;
	else
		rec_type = MSC_CDR_MOC;

	cft = cause_for_term(!!trans->cdr.t_answer, partial,
			     trans->cdr.cause_present, trans->cdr.cause);
	if (trans->cdr.cause_present)
		snprintf(diag, sizeof(diag), "%d", trans->cdr.cause);
	else
		diag[0] = '\0';

	if (trans->cdr.t_answer)
		result = partial ? "partial" : "answered";
	else if (trans->cdr.cause_present)
		result = "failed";
	else
		result = "unanswered";

	out_route = rec_type == MSC_CDR_MOC ? "MNCC" : incoming_route(ctx_ran(trans, NULL, trans->vsub));

	emit_row(net, trans->vsub, trans, trans->msc_a,
		 rec_type, trans->cdr.sequence_number, partial ? 1 : 0,
		 trans->cdr.calling, trans->cdr.called,
		 trans->cdr.redirecting, trans->cdr.connected,
		 "", "", "",
		 trans->cdr.t_setup, trans->cdr.t_setup, trans->cdr.t_alert,
		 trans->cdr.t_answer, now_t,
		 0, 0,
		 duration, ringing, cft, diag,
		 trans->callref,
		 out_route,
		 trans->cdr.emergency,
		 itcap_name(trans->bearer_cap.transfer),
		 trans_codec_name(trans),
		 result,
		 "", "", "", "");

	trans->cdr.sequence_number++;
	if (!partial)
		trans->cdr.written = true;
}

static void cdr_interval_cb(void *data)
{
	struct gsm_trans *trans = data;

	if (!trans_is_live(trans) || trans->type != TRANS_CC)
		return;
	emit_call(trans, true);
	cdr_arm_interval(trans);
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
	cdr_arm_interval(trans);
}

void msc_cdr_call(struct gsm_trans *trans)
{
	if (!trans)
		return;
	osmo_timer_del(&trans->cdr.interval_timer);
	emit_call(trans, false);
}

void msc_cdr_sms(struct gsm_trans *trans, struct gsm_sms *sms,
		 const char *result, int cause)
{
	struct gsm_network *net;
	time_t now_t;
	int rec_type, cft;
	char diag[16], dcs[8], pid[8], mr[8], tlen[8];
	const char *dest, *orig;

	if (!trans || trans->cdr.written || !sms)
		return;
	net = trans->net;
	if (!cdr_enabled(net))
		return;

	trans->cdr.written = true;
	now_t = time(NULL);

	if (sms->source == SMS_SOURCE_MS) {
		rec_type = MSC_CDR_MOSMS;
		dest = sms->dst.addr;
		orig = sms->src.addr[0] ? sms->src.addr : (trans->vsub ? trans->vsub->msisdn : "");
	} else {
		rec_type = MSC_CDR_MTSMS;
		dest = sms->dst.addr;
		orig = sms->src.addr;
	}

	if (result && !strcmp(result, "delivered"))
		cft = CFT_NORMAL_RELEASE;
	else
		cft = CFT_UNSUCCESSFUL_ATTEMPT;

	if (cause)
		snprintf(diag, sizeof(diag), "%d", cause);
	else
		diag[0] = '\0';
	snprintf(dcs, sizeof(dcs), "%u", sms->data_coding_scheme);
	snprintf(pid, sizeof(pid), "%u", sms->protocol_id);
	snprintf(mr, sizeof(mr), "%u", trans->sms.sm_rp_mr);
	snprintf(tlen, sizeof(tlen), "%u", sms->user_data_len);

	emit_row(net, trans->vsub, trans, trans->msc_a,
		 rec_type, 0, 0,
		 rec_type == MSC_CDR_MOSMS ? orig : orig,
		 rec_type == MSC_CDR_MOSMS ? dest : dest,
		 "", "",
		 rec_type == MSC_CDR_MOSMS ? dest : "",
		 rec_type == MSC_CDR_MTSMS ? orig : "",
		 "",
		 sms->created, sms->created, 0, 0, now_t,
		 sms->created, now_t,
		 sms->created ? (long)(now_t - sms->created) : 0, 0,
		 cft, diag,
		 trans->callref,
		 rec_type == MSC_CDR_MOSMS ? "SMSC" : incoming_route(ctx_ran(trans, NULL, trans->vsub)),
		 0, "", "",
		 result ? result : "",
		 dcs, pid, mr, tlen);
}

void msc_cdr_ss(struct gsm_trans *trans)
{
	time_t now_t;

	if (!trans || trans->cdr.written)
		return;
	if (!cdr_enabled(trans->net))
		return;

	trans->cdr.written = true;
	now_t = time(NULL);
	emit_row(trans->net, trans->vsub, trans, trans->msc_a,
		 MSC_CDR_SS, 0, 0,
		 "", "", "", "", "", "", "",
		 0, 0, 0, 0, now_t, 0, 0,
		 0, 0, CFT_NORMAL_RELEASE, "",
		 trans->callref, "SS",
		 0, "", "", "ss",
		 "", "", "", "");
}

void msc_cdr_lu(struct gsm_network *net, struct vlr_subscr *vsub,
		struct msc_a *msc_a, bool success)
{
	time_t now_t;

	if (!net || !vsub)
		return;
	if (!cdr_enabled(net))
		return;

	now_t = time(NULL);
	emit_row(net, vsub, NULL, msc_a,
		 MSC_CDR_LU, 0, 0,
		 "", "", "", "", "", "", "",
		 0, 0, 0, 0, now_t, 0, 0,
		 0, 0,
		 success ? CFT_NORMAL_RELEASE : CFT_UNSUCCESSFUL_ATTEMPT,
		 "",
		 0, incoming_route(ctx_ran(NULL, msc_a, vsub)),
		 0, "", "", success ? "accepted" : "rejected",
		 "", "", "", "");
}
