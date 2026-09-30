#pragma once

/* IP-level rebuild of per-IMSI API traces: "[IMSI:x] IPPACKET:" lines carry a
 * raw IPv4/IPv6 datagram (pcap LINKTYPE_RAW) rebuilt from the socket the MSC
 * sent or received on. Payload bytes are the real ones; IP ID/TTL, SCTP TSN
 * and vtag, TCP seq, and SCCP local references are synthesized. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct gsup_client_mux;
struct osmo_sccp_addr;
struct sccp_ran_inst;
struct sgs_connection;

enum msc_iptrace_sccp_msg {
	MSC_IPT_SCCP_CR,
	MSC_IPT_SCCP_DT1,
	MSC_IPT_SCCP_RLSD,
};

void msc_iptrace_sgs(struct sgs_connection *sgc, const char *imsi, bool is_rx,
		     uint16_t stream, uint32_t tsn, const uint8_t *data, size_t len);
void msc_iptrace_gsup(struct gsup_client_mux *gcm, const char *imsi, bool is_rx,
		      const uint8_t *gsup, size_t len);

/* A/Iu. Call before up_l2() for an inbound CR; the SCCP connection and its
 * subscriber only exist afterwards. */
void msc_iptrace_sccp_rx_cr(struct sccp_ran_inst *sri, uint32_t conn_id,
			    const struct osmo_sccp_addr *calling_addr,
			    const struct osmo_sccp_addr *called_addr,
			    const uint8_t *data, size_t len);
void msc_iptrace_sccp_co(struct sccp_ran_inst *sri, uint32_t conn_id, bool is_rx,
			 enum msc_iptrace_sccp_msg type, const struct osmo_sccp_addr *peer_addr,
			 const uint8_t *data, size_t len);
/* Emit packets held for a connection whose IMSI became known during up_l2(). */
void msc_iptrace_sccp_flush(struct sccp_ran_inst *sri, uint32_t conn_id);
void msc_iptrace_sccp_udt_tx(struct sccp_ran_inst *sri, const struct osmo_sccp_addr *called_addr,
			     const uint8_t *data, size_t len);
/* Subscriber of the connection-less message being sent (BSSMAP/RANAP Paging). */
void msc_iptrace_set_cl_imsi(const char *imsi);
