#pragma once

#include <stdbool.h>

struct gsm_network;
struct gsm_trans;
struct gsm_mncc;
struct gsm_mncc_cause;
struct gsm_sms;
struct gsm_network;
struct vlr_subscr;
struct msc_a;

/* OFCS / Huawei CallEventRecordType */
#define MSC_CDR_MOC		0
#define MSC_CDR_MTC		1
#define MSC_CDR_MOSMS		6
#define MSC_CDR_MTSMS		7
#define MSC_CDR_SS		10
#define MSC_CDR_LU		13

enum msc_cdr_rotate {
	MSC_CDR_ROTATE_NONE = 0,
	MSC_CDR_ROTATE_HOURLY,
	MSC_CDR_ROTATE_DAILY,
};

void msc_cdr_init(struct gsm_network *net);
void msc_cdr_reconfigure(struct gsm_network *net);

void msc_cdr_note_setup(struct gsm_trans *trans, const struct gsm_mncc *setup);
void msc_cdr_note_connected(struct gsm_trans *trans, const char *number);
void msc_cdr_note_cause(struct gsm_trans *trans, const struct gsm_mncc_cause *cause);
void msc_cdr_note_answer(struct gsm_trans *trans);

void msc_cdr_call(struct gsm_trans *trans);
void msc_cdr_sms(struct gsm_trans *trans, struct gsm_sms *sms,
		 const char *result, int cause);
void msc_cdr_ss(struct gsm_trans *trans);
void msc_cdr_lu(struct gsm_network *net, struct vlr_subscr *vsub,
		struct msc_a *msc_a, bool success);
