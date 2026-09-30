/*
 * IP-level rebuild of per-IMSI API trace packets.
 *
 * The kernel (SGs SCTP, GSUP TCP) and libosmo-sigtran (A/Iu M3UA/SCCP) strip
 * the transport before the MSC sees a message. For traced subscribers this
 * puts it back: real payload and socket endpoints, synthesized transport
 * counters, so that Wireshark decodes the result as IP / SCTP / M3UA / SCCP /
 * BSSAP|RANAP, IP / SCTP / SGsAP and IP / TCP / IPA / GSUP.
 *
 * (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <time.h>

#include <osmocom/core/bit16gen.h>
#include <osmocom/core/bit32gen.h>
#include <osmocom/core/linuxlist.h>
#include <osmocom/core/select.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/timer.h>
#include <osmocom/core/utils.h>
#include <osmocom/abis/ipa.h>
#include <osmocom/gsm/protocol/ipaccess.h>
#include <osmocom/gsupclient/gsup_client.h>
#include <osmocom/gsupclient/gsup_client_mux.h>
#include <osmocom/netif/stream.h>
#include <osmocom/sigtran/osmo_ss7.h>
#include <osmocom/sigtran/sccp_sap.h>

#include <osmocom/msc/msc_api.h>
#include <osmocom/msc/msc_iptrace.h>
#include <osmocom/msc/msc_roles.h>
#include <osmocom/msc/msub.h>
#include <osmocom/msc/ran_conn.h>
#include <osmocom/msc/ran_infra.h>
#include <osmocom/msc/ran_peer.h>
#include <osmocom/msc/sccp_ran.h>
#include <osmocom/msc/sgs_iface.h>
#include <osmocom/msc/sgs_server.h>
#include <osmocom/vlr/vlr.h>

/* Packets held per A/Iu connection until its IMSI is known. */
#define IPT_PENDING_MAX 32
/* Largest SCCP user data per DT1 segment / UDT (one length octet). */
#define IPT_SCCP_DATA_MAX 255
/* Re-scan the process's SCTP sockets for the M3UA association this often. */
#define IPT_M3UA_SCAN_SECS 10

#define IPT_SCTP_PPID_M3UA 3
#define IPT_M3UA_DATA_STREAM 1

struct ipt_ep {
	bool v6;
	uint8_t laddr[16];
	uint8_t paddr[16];
	uint16_t lport;
	uint16_t pport;
};

/* Transport counters, per socket and direction ([0] = tx, [1] = rx). */
struct ipt_flow {
	bool used;
	int fd;
	struct ipt_ep ep;
	uint32_t seq[2];
	uint16_t ssn[2];
};

struct ipt_pkt {
	struct llist_head entry;
	struct timeval tv;
	const char *link;
	const char *proto;
	bool is_rx;
	bool guess;
	size_t len;
	uint8_t data[];
};

static struct ipt_flow g_flows[16];
static unsigned int g_flow_next;
static uint16_t g_ip_id;
static const char *g_cl_imsi;

/* Inbound CR, built before up_l2() created the ran_conn. */
static struct {
	struct sccp_ran_inst *sri;
	uint32_t conn_id;
	struct llist_head pkts;
	bool valid;
} g_pending_cr;

void msc_iptrace_set_cl_imsi(const char *imsi)
{
	g_cl_imsi = imsi;
}

/* --- checksums --- */

static uint32_t csum_add(uint32_t sum, const uint8_t *p, size_t len)
{
	while (len > 1) {
		sum += (p[0] << 8) | p[1];
		p += 2;
		len -= 2;
	}
	if (len)
		sum += p[0] << 8;
	return sum;
}

static uint16_t csum_fold(uint32_t sum)
{
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ~sum & 0xffff;
}

static uint32_t crc32c(const uint8_t *p, size_t len)
{
	static uint32_t table[256];
	static bool init;
	uint32_t crc = 0xffffffff;

	if (!init) {
		for (unsigned int i = 0; i < 256; i++) {
			uint32_t c = i;
			for (unsigned int k = 0; k < 8; k++)
				c = (c & 1) ? (c >> 1) ^ 0x82F63B78 : c >> 1;
			table[i] = c;
		}
		init = true;
	}
	while (len--)
		crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

/* --- endpoints and flows --- */

static bool sa_to_addr(const struct sockaddr_storage *ss, bool *v6, uint8_t *addr, uint16_t *port)
{
	if (ss->ss_family == AF_INET) {
		const struct sockaddr_in *sin = (const struct sockaddr_in *)ss;
		*v6 = false;
		memcpy(addr, &sin->sin_addr, 4);
		*port = ntohs(sin->sin_port);
		return true;
	}
	if (ss->ss_family == AF_INET6) {
		const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)ss;
		if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr)) {
			*v6 = false;
			memcpy(addr, &sin6->sin6_addr.s6_addr[12], 4);
		} else {
			*v6 = true;
			memcpy(addr, &sin6->sin6_addr, 16);
		}
		*port = ntohs(sin6->sin6_port);
		return true;
	}
	return false;
}

static bool ep_from_fd(int fd, struct ipt_ep *ep)
{
	struct sockaddr_storage ss;
	socklen_t slen;
	bool lv6, pv6;

	memset(ep, 0, sizeof(*ep));
	if (fd < 0)
		return false;
	slen = sizeof(ss);
	if (getsockname(fd, (struct sockaddr *)&ss, &slen) < 0
	    || !sa_to_addr(&ss, &lv6, ep->laddr, &ep->lport))
		return false;
	slen = sizeof(ss);
	if (getpeername(fd, (struct sockaddr *)&ss, &slen) < 0
	    || !sa_to_addr(&ss, &pv6, ep->paddr, &ep->pport))
		return false;
	if (lv6 != pv6)
		return false;
	ep->v6 = lv6;
	return true;
}

static struct ipt_flow *flow_get(int fd, const struct ipt_ep *ep)
{
	struct ipt_flow *f;

	for (unsigned int i = 0; i < ARRAY_SIZE(g_flows); i++) {
		f = &g_flows[i];
		if (f->used && f->fd == fd && !memcmp(&f->ep, ep, sizeof(*ep)))
			return f;
	}
	f = &g_flows[g_flow_next++ % ARRAY_SIZE(g_flows)];
	*f = (struct ipt_flow){
		.used = true,
		.fd = fd,
		.ep = *ep,
		.seq = { 1, 1 },
	};
	return f;
}

/* The M3UA association is inside libosmo-sigtran, which does not expose its
 * socket. Find it among this process's connected SCTP sockets, leaving out
 * SGs. With more than one candidate the lowest fd wins (guess = true). */
static int m3ua_find(struct ipt_ep *ep_out, bool *guess)
{
	static int cached_fd = -1;
	static bool cached_guess;
	static time_t cached_at;
	struct timespec now;
	struct dirent *de;
	struct ipt_ep ep;
	DIR *d;
	int best = -1;
	unsigned int n = 0;

	osmo_clock_gettime(CLOCK_MONOTONIC, &now);
	if (cached_fd >= 0 && now.tv_sec - cached_at < IPT_M3UA_SCAN_SECS
	    && ep_from_fd(cached_fd, ep_out)) {
		*guess = cached_guess;
		return cached_fd;
	}

	d = opendir("/proc/self/fd");
	if (!d)
		return -1;
	while ((de = readdir(d))) {
		int fd, proto = 0, listening = 0;
		socklen_t l;

		if (de->d_name[0] < '0' || de->d_name[0] > '9')
			continue;
		fd = atoi(de->d_name);
		if (fd == dirfd(d))
			continue;
		l = sizeof(proto);
		if (getsockopt(fd, SOL_SOCKET, SO_PROTOCOL, &proto, &l) < 0 || proto != IPPROTO_SCTP)
			continue;
		l = sizeof(listening);
		if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &l) == 0 && listening)
			continue;
		if (!ep_from_fd(fd, &ep))
			continue;
		if (g_sgs && ep.lport == g_sgs->cfg.local_port)
			continue;
		n++;
		if (best < 0 || fd < best) {
			best = fd;
			*ep_out = ep;
		}
	}
	closedir(d);

	cached_fd = best;
	cached_guess = n > 1;
	cached_at = now.tv_sec;
	*guess = cached_guess;
	return best;
}

/* --- IP / SCTP / TCP --- */

static void ip_addrs(const struct ipt_ep *ep, bool is_rx, const uint8_t **src, const uint8_t **dst,
		     uint16_t *sport, uint16_t *dport)
{
	*src = is_rx ? ep->paddr : ep->laddr;
	*dst = is_rx ? ep->laddr : ep->paddr;
	*sport = is_rx ? ep->pport : ep->lport;
	*dport = is_rx ? ep->lport : ep->pport;
}

static size_t ip_hdr_len(const struct ipt_ep *ep)
{
	return ep->v6 ? 40 : 20;
}

static void ip_hdr(uint8_t *b, const struct ipt_ep *ep, bool is_rx, uint8_t proto, size_t l4len)
{
	const uint8_t *src, *dst;
	uint16_t sport, dport;

	ip_addrs(ep, is_rx, &src, &dst, &sport, &dport);
	if (ep->v6) {
		osmo_store32be(0x60000000, b);
		osmo_store16be(l4len, b + 4);
		b[6] = proto;
		b[7] = 64;
		memcpy(b + 8, src, 16);
		memcpy(b + 24, dst, 16);
		return;
	}
	b[0] = 0x45;
	b[1] = 0;
	osmo_store16be(20 + l4len, b + 2);
	osmo_store16be(g_ip_id++, b + 4);
	osmo_store16be(0x4000, b + 6);
	b[8] = 64;
	b[9] = proto;
	b[10] = b[11] = 0;
	memcpy(b + 12, src, 4);
	memcpy(b + 16, dst, 4);
	osmo_store16be(csum_fold(csum_add(0, b, 20)), b + 10);
}

static uint16_t l4_csum(const struct ipt_ep *ep, bool is_rx, uint8_t proto, const uint8_t *l4, size_t len)
{
	const uint8_t *src, *dst;
	uint16_t sport, dport;
	size_t alen = ep->v6 ? 16 : 4;
	uint32_t sum = 0;

	ip_addrs(ep, is_rx, &src, &dst, &sport, &dport);
	sum = csum_add(sum, src, alen);
	sum = csum_add(sum, dst, alen);
	sum += proto;
	sum += len & 0xffff;
	if (ep->v6)
		sum += len >> 16;
	sum = csum_add(sum, l4, len);
	return csum_fold(sum);
}

static struct ipt_pkt *pkt_alloc(void *ctx, size_t len, const char *link, const char *proto,
				 bool is_rx, bool guess)
{
	struct ipt_pkt *pkt = talloc_size(ctx, sizeof(*pkt) + len);

	if (!pkt)
		return NULL;
	*pkt = (struct ipt_pkt){
		.link = link,
		.proto = proto,
		.is_rx = is_rx,
		.guess = guess,
		.len = len,
	};
	INIT_LLIST_HEAD(&pkt->entry);
	gettimeofday(&pkt->tv, NULL);
	return pkt;
}

/* One SCTP DATA chunk. tsn == 0: next synthesized TSN for this direction. */
static struct ipt_pkt *build_sctp(void *ctx, const struct ipt_ep *ep, struct ipt_flow *flow, bool is_rx,
				  uint32_t ppid, uint16_t stream, uint32_t tsn,
				  const uint8_t *payload, size_t len,
				  const char *link, const char *proto, bool guess)
{
	size_t pad = (4 - (len & 3)) & 3;
	size_t sctp_len = 12 + 16 + len + pad;
	size_t iphl = ip_hdr_len(ep);
	const uint8_t *src, *dst;
	uint16_t sport, dport;
	struct ipt_pkt *pkt;
	unsigned int dir = is_rx ? 1 : 0;
	uint8_t *s, *c;
	uint32_t crc;

	if (iphl + sctp_len > 0xffff)
		return NULL;
	pkt = pkt_alloc(ctx, iphl + sctp_len, link, proto, is_rx, guess);
	if (!pkt)
		return NULL;

	if (tsn)
		flow->seq[dir] = tsn + 1;
	else
		tsn = flow->seq[dir]++;

	ip_addrs(ep, is_rx, &src, &dst, &sport, &dport);
	s = pkt->data + iphl;
	osmo_store16be(sport, s);
	osmo_store16be(dport, s + 2);
	osmo_store32be(0x4d534300 | dir, s + 4);
	memset(s + 8, 0, 4);

	c = s + 12;
	c[0] = 0;	/* DATA */
	c[1] = 0x03;	/* B | E */
	osmo_store16be(16 + len, c + 2);
	osmo_store32be(tsn, c + 4);
	osmo_store16be(stream, c + 8);
	osmo_store16be(flow->ssn[dir]++, c + 10);
	osmo_store32be(ppid, c + 12);
	memcpy(c + 16, payload, len);
	memset(c + 16 + len, 0, pad);

	crc = crc32c(s, sctp_len);
	s[8] = crc & 0xff;
	s[9] = (crc >> 8) & 0xff;
	s[10] = (crc >> 16) & 0xff;
	s[11] = (crc >> 24) & 0xff;

	ip_hdr(pkt->data, ep, is_rx, IPPROTO_SCTP, sctp_len);
	return pkt;
}

static struct ipt_pkt *build_tcp(void *ctx, const struct ipt_ep *ep, struct ipt_flow *flow, bool is_rx,
				 const uint8_t *payload, size_t len, const char *link, const char *proto)
{
	size_t tcp_len = 20 + len;
	size_t iphl = ip_hdr_len(ep);
	const uint8_t *src, *dst;
	uint16_t sport, dport;
	struct ipt_pkt *pkt;
	unsigned int dir = is_rx ? 1 : 0;
	uint8_t *t;

	if (iphl + tcp_len > 0xffff)
		return NULL;
	pkt = pkt_alloc(ctx, iphl + tcp_len, link, proto, is_rx, false);
	if (!pkt)
		return NULL;

	ip_addrs(ep, is_rx, &src, &dst, &sport, &dport);
	t = pkt->data + iphl;
	osmo_store16be(sport, t);
	osmo_store16be(dport, t + 2);
	osmo_store32be(flow->seq[dir], t + 4);
	osmo_store32be(flow->seq[!dir], t + 8);
	t[12] = 5 << 4;
	t[13] = 0x18;	/* PSH | ACK */
	osmo_store16be(0xffff, t + 14);
	osmo_store16be(0, t + 16);
	osmo_store16be(0, t + 18);
	memcpy(t + 20, payload, len);
	flow->seq[dir] += len;

	osmo_store16be(l4_csum(ep, is_rx, IPPROTO_TCP, t, tcp_len), t + 16);
	ip_hdr(pkt->data, ep, is_rx, IPPROTO_TCP, tcp_len);
	return pkt;
}

static void pkt_emit(const char *imsi, struct ipt_pkt *pkt)
{
	msc_api_trace_ippacket(imsi, pkt->link, pkt->proto, pkt->is_rx, pkt->guess, &pkt->tv,
			       pkt->data, pkt->len);
}

/* --- SGs --- */

void msc_iptrace_sgs(struct sgs_connection *sgc, const char *imsi, bool is_rx,
		     uint16_t stream, uint32_t tsn, const uint8_t *data, size_t len)
{
	struct ipt_ep ep;
	struct ipt_pkt *pkt;
	int fd;

	if (!sgc || !sgc->srv || !imsi || !msc_api_trace_active(imsi))
		return;
	fd = osmo_stream_srv_get_ofd(sgc->srv)->fd;
	if (!ep_from_fd(fd, &ep))
		return;
	/* SGsAP PPID is 0 (29.118 9.1). */
	pkt = build_sctp(OTC_SELECT, &ep, flow_get(fd, &ep), is_rx, 0, stream, is_rx ? tsn : 0,
			 data, len, "sgs", "sgsap", false);
	if (!pkt)
		return;
	pkt_emit(imsi, pkt);
	talloc_free(pkt);
}

/* --- GSUP (IPA over TCP) --- */

void msc_iptrace_gsup(struct gsup_client_mux *gcm, const char *imsi, bool is_rx,
		      const uint8_t *gsup, size_t len)
{
	struct osmo_gsup_client *gc = gcm ? gcm->gsup_client : NULL;
	struct ipt_ep ep;
	struct ipt_pkt *pkt;
	uint8_t *ipa;
	int fd;

	if (!gc || !gc->link || !gc->link->ofd || !imsi || len + 1 > 0xffff)
		return;
	fd = gc->link->ofd->fd;
	if (!ep_from_fd(fd, &ep))
		return;

	ipa = talloc_size(OTC_SELECT, 4 + len);
	if (!ipa)
		return;
	osmo_store16be(len + 1, ipa);
	ipa[2] = IPAC_PROTO_OSMO;
	ipa[3] = IPAC_PROTO_EXT_GSUP;
	memcpy(ipa + 4, gsup, len);

	pkt = build_tcp(OTC_SELECT, &ep, flow_get(fd, &ep), is_rx, ipa, 4 + len, "gsup", "gsup");
	talloc_free(ipa);
	if (!pkt)
		return;
	pkt_emit(imsi, pkt);
	talloc_free(pkt);
}

/* --- A/Iu: M3UA / SCCP --- */

static uint8_t bcd_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return 0;
}

/* ITU Q.713 3.4 party address, including the length octet. */
static size_t sccp_addr_enc(uint8_t *out, const struct osmo_sccp_addr *a)
{
	uint8_t *p = out + 2;
	uint8_t ai = 0;
	uint8_t gti = 0;
	size_t ndig = 0;

	if (a->presence & OSMO_SCCP_ADDR_T_PC)
		ai |= 0x01;
	if (a->presence & OSMO_SCCP_ADDR_T_SSN)
		ai |= 0x02;
	if (a->presence & OSMO_SCCP_ADDR_T_GT) {
		gti = a->gt.gti & 0x0f;
		ndig = strnlen(a->gt.digits, sizeof(a->gt.digits));
	}
	ai |= gti << 2;
	if (a->ri != OSMO_SCCP_RI_GT || !gti)
		ai |= 0x40;
	out[1] = ai;

	if (a->presence & OSMO_SCCP_ADDR_T_PC) {
		*p++ = a->pc & 0xff;
		*p++ = (a->pc >> 8) & 0x3f;
	}
	if (a->presence & OSMO_SCCP_ADDR_T_SSN)
		*p++ = a->ssn & 0xff;

	switch (gti) {
	case OSMO_SCCP_GTI_NAI_ONLY:
		*p++ = ((ndig & 1) ? 0x80 : 0) | (a->gt.nai & 0x7f);
		break;
	case OSMO_SCCP_GTI_TT_ONLY:
		*p++ = a->gt.tt;
		break;
	case OSMO_SCCP_GTI_TT_NPL_ENC:
		*p++ = a->gt.tt;
		*p++ = ((a->gt.npi & 0x0f) << 4) | ((ndig & 1) ? 1 : 2);
		break;
	case OSMO_SCCP_GTI_TT_NPL_ENC_NAI:
		*p++ = a->gt.tt;
		*p++ = ((a->gt.npi & 0x0f) << 4) | ((ndig & 1) ? 1 : 2);
		*p++ = a->gt.nai & 0x7f;
		break;
	default:
		break;
	}
	if (gti) {
		for (size_t i = 0; i < ndig; i += 2) {
			uint8_t lo = bcd_digit(a->gt.digits[i]);
			uint8_t hi = (i + 1 < ndig) ? bcd_digit(a->gt.digits[i + 1]) : 0;
			*p++ = lo | (hi << 4);
		}
	}
	out[0] = p - out - 1;
	return p - out;
}

static void sccp_ref(uint8_t *p, uint32_t ref)
{
	p[0] = ref & 0xff;
	p[1] = (ref >> 8) & 0xff;
	p[2] = (ref >> 16) & 0xff;
}

enum ipt_sccp_type {
	IPT_SCCP_CR = 0x01,
	IPT_SCCP_CC = 0x02,
	IPT_SCCP_RLSD = 0x04,
	IPT_SCCP_DT1 = 0x06,
	IPT_SCCP_UDT = 0x09,
};

/* The real SCCP local references live inside libosmo-sigtran. Use the SCU
 * conn_id for our side and a derived value for the peer's side. */
#define IPT_REF_LOCAL(conn_id) ((conn_id) & 0xffffff)
#define IPT_REF_PEER(conn_id) (((conn_id) & 0xffffff) ^ 0x800000)

static size_t sccp_build(uint8_t *out, enum ipt_sccp_type type, bool is_rx, uint32_t conn_id,
			 const struct osmo_sccp_addr *local, const struct osmo_sccp_addr *peer,
			 const uint8_t *data, size_t len, bool more)
{
	uint32_t ref_to = is_rx ? IPT_REF_LOCAL(conn_id) : IPT_REF_PEER(conn_id);
	uint32_t ref_from = is_rx ? IPT_REF_PEER(conn_id) : IPT_REF_LOCAL(conn_id);
	const struct osmo_sccp_addr *called = is_rx ? local : peer;
	const struct osmo_sccp_addr *calling = is_rx ? peer : local;
	uint8_t *p = out;
	uint8_t *ptr;
	size_t l;

	if (len > IPT_SCCP_DATA_MAX)
		len = IPT_SCCP_DATA_MAX;

	*p++ = type;
	switch (type) {
	case IPT_SCCP_CR:
		sccp_ref(p, ref_from);
		p += 3;
		*p++ = 0x02;
		ptr = p;
		p += 2;
		ptr[0] = p - ptr;
		p += sccp_addr_enc(p, called);
		ptr[1] = p - (ptr + 1);
		*p++ = 0x04;	/* calling party address */
		l = sccp_addr_enc(p, calling);
		p += l;
		if (len) {
			*p++ = 0x0f;	/* data */
			*p++ = len;
			memcpy(p, data, len);
			p += len;
		}
		*p++ = 0x00;
		break;
	case IPT_SCCP_CC:
		sccp_ref(p, ref_to);
		sccp_ref(p + 3, ref_from);
		p += 6;
		*p++ = 0x02;
		*p++ = 0x00;
		break;
	case IPT_SCCP_DT1:
		sccp_ref(p, ref_to);
		p += 3;
		*p++ = more ? 0x01 : 0x00;
		*p++ = 1;
		*p++ = len;
		memcpy(p, data, len);
		p += len;
		break;
	case IPT_SCCP_RLSD:
		sccp_ref(p, ref_to);
		sccp_ref(p + 3, ref_from);
		p += 6;
		*p++ = 0x00;	/* end user originated */
		if (len) {
			*p++ = 1;
			*p++ = 0x0f;
			*p++ = len;
			memcpy(p, data, len);
			p += len;
			*p++ = 0x00;
		} else {
			*p++ = 0x00;
		}
		break;
	case IPT_SCCP_UDT:
		*p++ = 0x00;	/* class 0 */
		ptr = p;
		p += 3;
		ptr[0] = p - ptr;
		p += sccp_addr_enc(p, called);
		ptr[1] = p - (ptr + 1);
		p += sccp_addr_enc(p, calling);
		ptr[2] = p - (ptr + 2);
		*p++ = len;
		memcpy(p, data, len);
		p += len;
		break;
	}
	return p - out;
}

/* RFC 4666 3.3.1 DATA with a Protocol Data parameter. */
static size_t m3ua_build(uint8_t *out, uint32_t opc, uint32_t dpc, uint8_t ni, uint8_t sls,
			 const uint8_t *sccp, size_t len)
{
	size_t pd_len = 4 + 12 + len;
	size_t pad = (4 - (pd_len & 3)) & 3;
	size_t total = 8 + pd_len + pad;

	out[0] = 1;	/* version */
	out[1] = 0;
	out[2] = 1;	/* Transfer */
	out[3] = 1;	/* DATA */
	osmo_store32be(total, out + 4);
	osmo_store16be(0x0210, out + 8);
	osmo_store16be(pd_len, out + 10);
	osmo_store32be(opc, out + 12);
	osmo_store32be(dpc, out + 16);
	out[20] = 3;	/* SI = SCCP */
	out[21] = ni;
	out[22] = 0;
	out[23] = sls;
	memcpy(out + 24, sccp, len);
	memset(out + 24 + len, 0, pad);
	return total;
}

static const char *sri_link(const struct sccp_ran_inst *sri)
{
	return sri->ran && sri->ran->type == OSMO_RAT_UTRAN_IU ? "iu" : "a";
}

static const char *sri_proto(const struct sccp_ran_inst *sri)
{
	return sri->ran && sri->ran->type == OSMO_RAT_UTRAN_IU ? "ranap" : "bssap";
}

/* Build SCCP (segmenting DT1), wrap each in M3UA / SCTP / IP, append to list. */
static unsigned int build_sccp_pkts(void *ctx, struct llist_head *list, struct sccp_ran_inst *sri,
				    enum ipt_sccp_type type, bool is_rx, uint32_t conn_id,
				    const struct osmo_sccp_addr *peer, const uint8_t *data, size_t len)
{
	struct osmo_ss7_instance *ss7 = osmo_sccp_get_ss7(sri->sccp);
	const struct osmo_sccp_addr *local = &sri->local_sccp_addr;
	uint32_t local_pc, peer_pc, opc, dpc;
	uint8_t sccp[IPT_SCCP_DATA_MAX + 128];
	uint8_t m3ua[sizeof(sccp) + 32];
	struct ipt_flow *flow;
	struct ipt_ep ep;
	unsigned int n = 0;
	bool guess = false;
	uint8_t ni;
	int fd;

	fd = m3ua_find(&ep, &guess);
	if (fd < 0)
		return 0;
	flow = flow_get(fd, &ep);

	local_pc = (local->presence & OSMO_SCCP_ADDR_T_PC) ? local->pc
			: (ss7 ? osmo_ss7_instance_get_primary_pc(ss7) : 0);
	peer_pc = (peer && (peer->presence & OSMO_SCCP_ADDR_T_PC)) ? peer->pc : 0;
	opc = is_rx ? peer_pc : local_pc;
	dpc = is_rx ? local_pc : peer_pc;
	ni = ss7 ? osmo_ss7_instance_get_network_indicator(ss7) : 2;

	do {
		size_t chunk = len > IPT_SCCP_DATA_MAX ? IPT_SCCP_DATA_MAX : len;
		bool more = type == IPT_SCCP_DT1 && len > chunk;
		struct osmo_sccp_addr none = {};
		size_t sl, ml;
		struct ipt_pkt *pkt;

		sl = sccp_build(sccp, type, is_rx, conn_id, local, peer ? peer : &none, data, chunk, more);
		ml = m3ua_build(m3ua, opc, dpc, ni, conn_id & 0x0f, sccp, sl);
		pkt = build_sctp(ctx, &ep, flow, is_rx, IPT_SCTP_PPID_M3UA, IPT_M3UA_DATA_STREAM, 0,
				 m3ua, ml, sri_link(sri), sri_proto(sri), guess);
		if (!pkt)
			break;
		llist_add_tail(&pkt->entry, list);
		n++;
		if (type != IPT_SCCP_DT1)
			break;
		data += chunk;
		len -= chunk;
	} while (len);
	return n;
}

static struct ran_conn *conn_find(struct sccp_ran_inst *sri, uint32_t conn_id)
{
	struct ran_conn *conn;

	llist_for_each_entry(conn, &sri->ran_conns, entry) {
		if (conn->sccp_conn_id == conn_id)
			return conn;
	}
	return NULL;
}

static const char *conn_imsi(struct ran_conn *conn)
{
	struct msc_role_common *c;
	struct vlr_subscr *vsub;

	if (conn->ipt_imsi[0])
		return conn->ipt_imsi;
	if (!conn->msc_role || !conn->msc_role->priv)
		return NULL;
	c = conn->msc_role->priv;
	if (!c->msub)
		return NULL;
	vsub = msub_vsub(c->msub);
	if (!vsub || !vsub->imsi[0])
		return NULL;
	OSMO_STRLCPY_ARRAY(conn->ipt_imsi, vsub->imsi);
	return conn->ipt_imsi;
}

static void pending_drop(struct ran_conn *conn)
{
	struct ipt_pkt *pkt, *tmp;

	llist_for_each_entry_safe(pkt, tmp, &conn->ipt_pending, entry) {
		llist_del(&pkt->entry);
		talloc_free(pkt);
	}
	conn->ipt_pending_n = 0;
}

static void pending_add(struct ran_conn *conn, struct llist_head *list)
{
	struct ipt_pkt *pkt, *tmp;

	llist_for_each_entry_safe(pkt, tmp, list, entry) {
		llist_del(&pkt->entry);
		talloc_steal(conn, pkt);
		llist_add_tail(&pkt->entry, &conn->ipt_pending);
		conn->ipt_pending_n++;
	}
	while (conn->ipt_pending_n > IPT_PENDING_MAX) {
		pkt = llist_first_entry(&conn->ipt_pending, struct ipt_pkt, entry);
		llist_del(&pkt->entry);
		talloc_free(pkt);
		conn->ipt_pending_n--;
	}
}

static void list_emit(const char *imsi, struct llist_head *list)
{
	struct ipt_pkt *pkt, *tmp;

	llist_for_each_entry_safe(pkt, tmp, list, entry) {
		llist_del(&pkt->entry);
		pkt_emit(imsi, pkt);
		talloc_free(pkt);
	}
}

static void pending_cr_clear(void)
{
	struct ipt_pkt *pkt, *tmp;

	if (!g_pending_cr.valid)
		return;
	llist_for_each_entry_safe(pkt, tmp, &g_pending_cr.pkts, entry) {
		llist_del(&pkt->entry);
		talloc_free(pkt);
	}
	g_pending_cr.valid = false;
}

static bool pending_cr_is(struct sccp_ran_inst *sri, uint32_t conn_id)
{
	return g_pending_cr.valid && g_pending_cr.sri == sri && g_pending_cr.conn_id == conn_id;
}

static void pending_cr_adopt(struct sccp_ran_inst *sri, uint32_t conn_id, struct ran_conn *conn)
{
	if (!pending_cr_is(sri, conn_id))
		return;
	g_pending_cr.valid = false;
	pending_add(conn, &g_pending_cr.pkts);
}

/* Emit or drop what is held for conn, once its IMSI is known.
 * Returns the IMSI if it is being traced. */
static const char *conn_settle(struct ran_conn *conn)
{
	const char *imsi = conn_imsi(conn);

	if (!imsi)
		return NULL;
	if (!msc_api_trace_active(imsi)) {
		pending_drop(conn);
		return NULL;
	}
	list_emit(imsi, &conn->ipt_pending);
	conn->ipt_pending_n = 0;
	return imsi;
}

void msc_iptrace_sccp_rx_cr(struct sccp_ran_inst *sri, uint32_t conn_id,
			    const struct osmo_sccp_addr *calling_addr,
			    const struct osmo_sccp_addr *called_addr,
			    const uint8_t *data, size_t len)
{
	if (!msc_api_traces_any() || !sri || !calling_addr)
		return;

	pending_cr_clear();
	INIT_LLIST_HEAD(&g_pending_cr.pkts);
	g_pending_cr.sri = sri;
	g_pending_cr.conn_id = conn_id;
	g_pending_cr.valid = true;

	/* libosmo-sigtran answers the CR with a CC before up_l2() runs.
	 * Not OTC_SELECT: a CR that got no ran_conn is freed on the next one. */
	build_sccp_pkts(OTC_GLOBAL, &g_pending_cr.pkts, sri, IPT_SCCP_CR, true, conn_id,
			calling_addr, data, len);
	build_sccp_pkts(OTC_GLOBAL, &g_pending_cr.pkts, sri, IPT_SCCP_CC, false, conn_id,
			calling_addr, NULL, 0);
}

void msc_iptrace_sccp_co(struct sccp_ran_inst *sri, uint32_t conn_id, bool is_rx,
			 enum msc_iptrace_sccp_msg type, const struct osmo_sccp_addr *peer_addr,
			 const uint8_t *data, size_t len)
{
	LLIST_HEAD(pkts);
	struct ran_conn *conn;
	const char *imsi;
	enum ipt_sccp_type st;

	if (!msc_api_traces_any() || !sri)
		return;
	conn = conn_find(sri, conn_id);
	if (!conn)
		return;
	pending_cr_adopt(sri, conn_id, conn);

	imsi = conn_imsi(conn);
	if (imsi && !msc_api_trace_active(imsi)) {
		pending_drop(conn);
		return;
	}

	switch (type) {
	case MSC_IPT_SCCP_CR:
		st = IPT_SCCP_CR;
		break;
	case MSC_IPT_SCCP_RLSD:
		st = IPT_SCCP_RLSD;
		break;
	default:
		st = IPT_SCCP_DT1;
		break;
	}
	if (!peer_addr && conn->ran_peer)
		peer_addr = &conn->ran_peer->peer_addr;
	if (!build_sccp_pkts(OTC_SELECT, &pkts, sri, st, is_rx, conn_id, peer_addr, data, len))
		return;

	if (imsi) {
		conn_settle(conn);
		list_emit(imsi, &pkts);
	} else {
		pending_add(conn, &pkts);
	}
}

void msc_iptrace_sccp_flush(struct sccp_ran_inst *sri, uint32_t conn_id)
{
	struct ran_conn *conn;

	if (!sri)
		return;
	conn = conn_find(sri, conn_id);
	if (!conn) {
		if (pending_cr_is(sri, conn_id))
			pending_cr_clear();
		return;
	}
	if (!msc_api_traces_any()) {
		pending_drop(conn);
		return;
	}
	pending_cr_adopt(sri, conn_id, conn);
	conn_settle(conn);
}

void msc_iptrace_sccp_udt_tx(struct sccp_ran_inst *sri, const struct osmo_sccp_addr *called_addr,
			     const uint8_t *data, size_t len)
{
	LLIST_HEAD(pkts);
	const char *imsi = g_cl_imsi;

	if (!sri || !imsi || !msc_api_trace_active(imsi))
		return;
	if (build_sccp_pkts(OTC_SELECT, &pkts, sri, IPT_SCCP_UDT, false, 0, called_addr, data, len))
		list_emit(imsi, &pkts);
}
