#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <osmocom/core/linuxlist.h>

struct gsm_network;
struct log_target;

#define MSC_API_PORT_DEFAULT 8080
#define MSC_API_BIND_DEFAULT "127.0.0.1"
#define MSC_API_TOKEN_MAXLEN 128

/* One active per-IMSI debug trace: a dedicated libosmocore stderr log target
 * (captured by journald) whose VLR-subscriber filter is pinned to this
 * subscriber, so only that IMSI's lines are emitted at DEBUG. */
struct msc_api_trace {
	struct llist_head entry;
	char imsi[16];
	struct log_target *target;
};

struct msc_api_state {
	struct gsm_network *net;
	struct osmo_stream_srv_link *srv_link;
	struct llist_head traces;

	struct {
		char bind_addr[48];
		uint16_t port;
		char token[MSC_API_TOKEN_MAXLEN];
	} cfg;
};

struct vty;

struct msc_api_state *msc_api_alloc(void *ctx, struct gsm_network *net);
bool msc_api_configured(const struct msc_api_state *api);
int msc_api_open(struct msc_api_state *api);
void msc_api_close(struct msc_api_state *api);
int msc_api_config_write(struct vty *vty);
void msc_api_vty_init(void);
