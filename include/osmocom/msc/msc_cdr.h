#pragma once

struct gsm_trans;
struct gsm_mncc;
struct gsm_mncc_cause;
struct gsm_sms;

/* Snapshot SETUP numbers/direction the first time we see them. */
void msc_cdr_note_setup(struct gsm_trans *trans, const struct gsm_mncc *setup);
void msc_cdr_note_connected(struct gsm_trans *trans, const char *number);
void msc_cdr_note_cause(struct gsm_trans *trans, const struct gsm_mncc_cause *cause);
void msc_cdr_note_answer(struct gsm_trans *trans);

/* Write one CS call record when the CC transaction is freed. */
void msc_cdr_call(struct gsm_trans *trans);

/* Write one SMS record (MO/MT). Safe to call more than once. */
void msc_cdr_sms(struct gsm_trans *trans, struct gsm_sms *sms,
		 const char *result, int cause);
