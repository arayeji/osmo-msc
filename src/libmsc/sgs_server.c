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
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <osmocom/msc/sgs_iface.h>
#include <osmocom/msc/debug.h>
#include <osmocom/msc/sgs_server.h>
#include <osmocom/core/utils.h>
#include <osmocom/core/socket.h>
#include <osmocom/core/select.h>
#include <osmocom/netif/stream.h>
#include <netinet/sctp.h>

#define LOGSGC(sgc, lvl, fmt, args...) \
	LOGP(DSGS, lvl, "%s: " fmt, (sgc)->sockname, ## args)

static const char *sgs_sctp_assoc_chg_name(uint8_t state)
{
	switch (state) {
	case SCTP_COMM_UP:
		return "COMM_UP";
	case SCTP_COMM_LOST:
		return "COMM_LOST";
	case SCTP_RESTART:
		return "RESTART";
	case SCTP_SHUTDOWN_COMP:
		return "SHUTDOWN_COMP";
	case SCTP_CANT_STR_ASSOC:
		return "CANT_STR_ASSOC";
	default:
		return "UNKNOWN";
	}
}

static bool sgs_fd_remote_ip(int fd, char *buf, size_t buflen)
{
	struct sockaddr_storage ss;
	socklen_t slen = sizeof(ss);

	if (getpeername(fd, (struct sockaddr *)&ss, &slen) < 0)
		return false;
	if (ss.ss_family == AF_INET)
		return inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, buf, buflen) != NULL;
	if (ss.ss_family == AF_INET6)
		return inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, buf, buflen) != NULL;
	return false;
}

/* Close every existing SGs SCTP from the same remote IP. After an MME restart
 * the kernel often keeps the old association ESTAB until heartbeat timeout while
 * the MME already opened a new one; paging would keep going out on the dead
 * link and the new socket's recv queue would never be the one mme->conn reads. */
static void sgs_close_replaced_peer_conns(struct sgs_state *sgs, int new_fd)
{
	struct sgs_connection *sgc, *tmp;
	char new_ip[INET6_ADDRSTRLEN];
	char old_ip[INET6_ADDRSTRLEN];

	if (!sgs_fd_remote_ip(new_fd, new_ip, sizeof(new_ip)))
		return;

	llist_for_each_entry_safe(sgc, tmp, &sgs->conn_list, entry) {
		int fd;

		if (!sgc->srv)
			continue;
		fd = osmo_stream_srv_get_ofd(sgc->srv)->fd;
		if (!sgs_fd_remote_ip(fd, old_ip, sizeof(old_ip)))
			continue;
		if (strcmp(old_ip, new_ip) != 0)
			continue;
		LOGSGC(sgc, LOGL_NOTICE,
		       "Closing SGs link replaced by a new connection from %s\n", new_ip);
		osmo_stream_srv_destroy(sgc->srv);
	}
}

/* Handle one SCTP notification. Returns -EBADF if the connection was destroyed. */
static int sgs_conn_handle_sctp_notif(struct osmo_stream_srv *conn, struct sgs_connection *sgc,
				      struct msgb *msg)
{
	union sctp_notification *notif = (union sctp_notification *)msgb_data(msg);

	switch (notif->sn_header.sn_type) {
	case SCTP_ASSOC_CHANGE:
		LOGSGC(sgc, LOGL_NOTICE, "SCTP_ASSOC_CHANGE %s\n",
		       sgs_sctp_assoc_chg_name(notif->sn_assoc_change.sac_state));
		switch (notif->sn_assoc_change.sac_state) {
		case SCTP_COMM_LOST:
		case SCTP_SHUTDOWN_COMP:
		case SCTP_CANT_STR_ASSOC:
			osmo_stream_srv_destroy(conn);
			return -EBADF;
		case SCTP_RESTART:
			/* MME restarted: same accepted socket, new association.
			 * Keep reading — do not destroy. */
			LOGSGC(sgc, LOGL_NOTICE,
			       "SCTP association restarted (MME reboot); keeping SGs link\n");
			break;
		default:
			break;
		}
		break;
	case SCTP_SHUTDOWN_EVENT:
		/* Peer started a graceful shutdown. A rebooted MME often
		 * follows this with SCTP_RESTART on the same fd. Destroying
		 * here is what wedged SGs: we closed the socket the restart
		 * would have reused, and inbound LU sat unread. */
		LOGSGC(sgc, LOGL_NOTICE,
		       "SCTP_SHUTDOWN_EVENT; keeping socket for a possible SCTP RESTART\n");
		break;
	default:
		break;
	}
	return 0;
}

/* call-back when data arrives on SGs */
static int sgs_conn_readable_cb(struct osmo_stream_srv *conn)
{
	struct osmo_fd *ofd = osmo_stream_srv_get_ofd(conn);
	struct sgs_connection *sgc = osmo_stream_srv_get_data(conn);

	/* Drain until EAGAIN so a SHUTDOWN notification plus the following
	 * RESTART/user-data are all handled in this wakeup. One recv per
	 * select() left the socket readable with a 200KB+ kernel queue while
	 * the process sat in poll() after we had already destroyed it. */
	for (;;) {
		struct msgb *msg = gsm29118_msgb_alloc();
		struct sctp_sndrcvinfo sinfo;
		int flags = 0;
		int rc;

		rc = sctp_recvmsg(ofd->fd, msgb_data(msg), msgb_tailroom(msg),
				  NULL, NULL, &sinfo, &flags);
		if (rc < 0) {
			msgb_free(msg);
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				return 0;
			LOGSGC(sgc, LOGL_NOTICE, "SCTP recv error: %s\n", strerror(errno));
			osmo_stream_srv_destroy(conn);
			return -EBADF;
		}
		if (rc == 0) {
			msgb_free(msg);
			osmo_stream_srv_destroy(conn);
			return -EBADF;
		}
		msgb_put(msg, rc);

		if (flags & MSG_NOTIFICATION) {
			rc = sgs_conn_handle_sctp_notif(conn, sgc, msg);
			msgb_free(msg);
			if (rc < 0)
				return rc;
			continue;
		}

		msg->l2h = msgb_data(msg);

		if (msgb_sctp_ppid(msg) != 0) {
			LOGSGC(sgc, LOGL_NOTICE, "Ignoring SCTP PPID %ld (spec violation)\n",
			       msgb_sctp_ppid(msg));
			msgb_free(msg);
			continue;
		}

		sgs_iface_rx(sgc, msg);
	}
}

/* call-back when new connection is closed ed on SGs */
static int sgs_conn_closed_cb(struct osmo_stream_srv *conn)
{
	struct sgs_connection *sgc = osmo_stream_srv_get_data(conn);

	LOGSGC(sgc, LOGL_NOTICE, "Connection lost\n");
	sgs_mme_detach_connection(sgc);
	llist_del(&sgc->entry);
	sgc->srv = NULL;
	return 0;
}

/* call-back when new connection is accept() ed on SGs */
static int sgs_accept_cb(struct osmo_stream_srv_link *link, int fd)
{
	struct sgs_state *sgs = osmo_stream_srv_link_get_data(link);
	struct sgs_connection *sgc = talloc_zero(link, struct sgs_connection);
	OSMO_ASSERT(sgc);
	sgc->sgs = sgs;
	osmo_sock_get_name_buf(sgc->sockname, sizeof(sgc->sockname), fd);
	sgs_close_replaced_peer_conns(sgs, fd);
	sgc->srv = osmo_stream_srv_create(sgc, link, fd, sgs_conn_readable_cb, sgs_conn_closed_cb, sgc);
	if (!sgc->srv) {
		talloc_free(sgc);
		return -1;
	}
	{
		int fl = fcntl(fd, F_GETFL);
		if (fl >= 0)
			fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	}
	LOGSGC(sgc, LOGL_INFO, "Accepted new SGs connection\n");
	llist_add_tail(&sgc->entry, &sgs->conn_list);

	return 0;
}

static struct sgs_state *sgs_state_alloc(void *ctx)
{
	struct sgs_state *sgs = talloc_zero(ctx, struct sgs_state);

	INIT_LLIST_HEAD(&sgs->mme_list);
	INIT_LLIST_HEAD(&sgs->conn_list);

	memcpy(sgs->cfg.timer, sgs_state_timer_defaults, sizeof(sgs->cfg.timer));
	memcpy(sgs->cfg.counter, sgs_state_counter_defaults, sizeof(sgs->cfg.counter));
	sgs->cfg.local_port = SGS_PORT_DEFAULT;
	osmo_strlcpy(sgs->cfg.local_addr, DEFAULT_SGS_SERVER_IP, sizeof(sgs->cfg.local_addr));
	osmo_strlcpy(sgs->cfg.vlr_name, DEFAULT_SGS_SERVER_VLR_NAME, sizeof(sgs->cfg.vlr_name));

	return sgs;
}

/*! allocate SGs new sgs state
 *  \param[in] ctx talloc context
 *  \returns returns allocated sgs state, NULL in case of error. */
struct sgs_state *sgs_server_alloc(void *ctx)
{
	struct sgs_state *sgs;
	struct osmo_stream_srv_link *link;

	sgs = sgs_state_alloc(ctx);
	if (!sgs)
		return NULL;

	sgs->srv_link = link = osmo_stream_srv_link_create(ctx);
	if (!sgs->srv_link)
		return NULL;

	osmo_stream_srv_link_set_nodelay(link, true);
	osmo_stream_srv_link_set_addr(link, sgs->cfg.local_addr);
	osmo_stream_srv_link_set_port(link, sgs->cfg.local_port);
	osmo_stream_srv_link_set_proto(link, IPPROTO_SCTP);
	osmo_stream_srv_link_set_data(link, sgs);
	osmo_stream_srv_link_set_accept_cb(link, sgs_accept_cb);

	return sgs;
}

/*! (re)open SGs interface (SCTP)
 *  \param[in] sgs associated sgs state
 *  \returns 0 in case of success, -EINVAL in case of error. */
int sgs_server_open(struct sgs_state *sgs)
{
	int rc;
	struct osmo_fd *ofd = osmo_stream_srv_link_get_ofd(sgs->srv_link);

	rc = osmo_stream_srv_link_open(sgs->srv_link);
	if (rc < 0) {
		LOGP(DSGS, LOGL_ERROR, "SGs socket cannot be opened: %s\n", strerror(errno));
		return -EINVAL;
	}

	LOGP(DSGS, LOGL_NOTICE, "SGs socket bound to %s\n", osmo_sock_get_name2(ofd->fd));
	return 0;
}
