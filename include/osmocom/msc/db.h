/* (C) 2008 by Jan Luebbe <jluebbe@debian.org>
 * (C) 2009 by Holger Hans Peter Freyther <zecke@selfish.org>
 * (C) 2022 by Harald Welte <laforge@osmocom.org>
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

#ifndef _DB_H
#define _DB_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <osmocom/gsm/gsm23003.h>

#include "gsm_subscriber.h"

#define VSUB_USE_SMS_RECEIVER "SMS-receiver"

struct gsm_network;
struct gsm_sms;
struct vlr_instance;
struct vlr_subscr;
struct osmo_location_area_id;
struct osmo_plmn_id;

/* SGs association row used for 3GPP TS 23.007 / 29.118 VLR restoration */
struct db_sgs_assoc {
	char imsi[GSM23003_IMSI_MAX_DIGITS + 1];
	char msisdn[GSM23003_MSISDN_MAX_DIGITS + 1];
	uint32_t tmsi;
	char mme_name[256];
	struct osmo_location_area_id lai;
	bool last_eutran_plmn_present;
	struct osmo_plmn_id last_eutran_plmn;
	/* 0 = never expire; else CLOCK_REALTIME seconds */
	time_t expire_unix;
};

typedef int (*db_sgs_assoc_cb_t)(void *data, const struct db_sgs_assoc *row);

/* one time initialisation */
int db_init(void *ctx, const char *fname, bool enable_sqlite_logging);
int db_prepare(void);
int db_fini(void);

/* SMS store-and-forward */
int db_sms_store(struct gsm_sms *sms);
struct gsm_sms *db_sms_get(struct gsm_network *net, unsigned long long id);
struct gsm_sms *db_sms_get_next_unsent(struct gsm_network *net,
				       unsigned long long min_sms_id,
				       int max_failed);
struct gsm_sms *db_sms_get_next_unsent_rr_msisdn(struct gsm_network *net,
						 const char *last_msisdn,
						 int max_failed);
struct gsm_sms *db_sms_get_unsent_for_subscr(struct vlr_subscr *vsub,
					     int max_failed);
int db_sms_mark_delivered(struct gsm_sms *sms);
int db_sms_inc_deliver_attempts(struct gsm_sms *sms);
int db_sms_delete_by_msisdn(const char *msisdn);
int db_sms_delete_message_by_id(unsigned long long sms_id);
int db_sms_delete_expired_message_by_id(unsigned long long sms_id);
void db_sms_delete_oldest_expired_message(void);

/* SGs VLR restoration (TS 23.007 / 29.118 5.1.2.2) */
int db_sgs_assoc_upsert(const struct vlr_subscr *vsub);
int db_sgs_assoc_delete(const char *imsi);
int db_sgs_assoc_delete_mme(const char *mme_name);
int db_sgs_assoc_foreach(db_sgs_assoc_cb_t cb, void *data);

#endif /* _DB_H */
