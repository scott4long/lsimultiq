/*-
 * Copyright (c) 1982, 1986, 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)sys_socket.c	8.1 (Berkeley) 6/10/93
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/domain.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/protosw.h>
#include <sys/sigio.h>
#include <sys/signal.h>
#include <sys/signalvar.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/filio.h>			/* XXX */
#include <sys/sockio.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/ucred.h>
#include <sys/un.h>
#include <sys/unpcb.h>
#include <sys/user.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/route.h>
#include <net/vnet.h>

#include <netinet/in.h>
#include <netinet/in_pcb.h>

#include <security/mac/mac_framework.h>

static fo_rdwr_t soo_read;
static fo_rdwr_t soo_write;
static fo_ioctl_t soo_ioctl;
static fo_poll_t soo_poll;
extern fo_kqfilter_t soo_kqfilter;
static fo_stat_t soo_stat;
static fo_close_t soo_close;
static fo_fill_kinfo_t soo_fill_kinfo;

struct fileops	socketops = {
	.fo_read = soo_read,
	.fo_write = soo_write,
	.fo_truncate = invfo_truncate,
	.fo_ioctl = soo_ioctl,
	.fo_poll = soo_poll,
	.fo_kqfilter = soo_kqfilter,
	.fo_stat = soo_stat,
	.fo_close = soo_close,
	.fo_chmod = invfo_chmod,
	.fo_chown = invfo_chown,
	.fo_sendfile = invfo_sendfile,
	.fo_fill_kinfo = soo_fill_kinfo,
	.fo_flags = DFLAG_PASSABLE
};

static int
soo_read(struct file *fp, struct uio *uio, struct ucred *active_cred,
    int flags, struct thread *td)
{
	struct socket *so = fp->f_data;
	int error;

#ifdef MAC
	error = mac_socket_check_receive(active_cred, so);
	if (error)
		return (error);
#endif
	error = soreceive(so, 0, uio, 0, 0, 0);
	return (error);
}

static int
soo_write(struct file *fp, struct uio *uio, struct ucred *active_cred,
    int flags, struct thread *td)
{
	struct socket *so = fp->f_data;
	int error;

#ifdef MAC
	error = mac_socket_check_send(active_cred, so);
	if (error)
		return (error);
#endif
	error = sosend(so, 0, uio, 0, 0, 0, uio->uio_td);
	if (error == EPIPE && (so->so_options & SO_NOSIGPIPE) == 0) {
		PROC_LOCK(uio->uio_td->td_proc);
		tdsignal(uio->uio_td, SIGPIPE);
		PROC_UNLOCK(uio->uio_td->td_proc);
	}
	return (error);
}

static int
soo_ioctl(struct file *fp, u_long cmd, void *data, struct ucred *active_cred,
    struct thread *td)
{
	struct socket *so = fp->f_data;
	int error = 0;

	switch (cmd) {
	case FIONBIO:
		SOCK_LOCK(so);
		if (*(int *)data)
			so->so_state |= SS_NBIO;
		else
			so->so_state &= ~SS_NBIO;
		SOCK_UNLOCK(so);
		break;

	case FIOASYNC:
		/*
		 * XXXRW: This code separately acquires SOCK_LOCK(so) and
		 * SOCKBUF_LOCK(&so->so_rcv) even though they are the same
		 * mutex to avoid introducing the assumption that they are
		 * the same.
		 */
		if (*(int *)data) {
			SOCK_LOCK(so);
			so->so_state |= SS_ASYNC;
			SOCK_UNLOCK(so);
			SOCKBUF_LOCK(&so->so_rcv);
			so->so_rcv.sb_flags |= SB_ASYNC;
			SOCKBUF_UNLOCK(&so->so_rcv);
			SOCKBUF_LOCK(&so->so_snd);
			so->so_snd.sb_flags |= SB_ASYNC;
			SOCKBUF_UNLOCK(&so->so_snd);
		} else {
			SOCK_LOCK(so);
			so->so_state &= ~SS_ASYNC;
			SOCK_UNLOCK(so);
			SOCKBUF_LOCK(&so->so_rcv);
			so->so_rcv.sb_flags &= ~SB_ASYNC;
			SOCKBUF_UNLOCK(&so->so_rcv);
			SOCKBUF_LOCK(&so->so_snd);
			so->so_snd.sb_flags &= ~SB_ASYNC;
			SOCKBUF_UNLOCK(&so->so_snd);
		}
		break;

	case FIONREAD:
		/* Unlocked read. */
		*(int *)data = sbavail(&so->so_rcv);
		break;

	case FIONWRITE:
		/* Unlocked read. */
		*(int *)data = sbavail(&so->so_snd);
		break;

	case FIONSPACE:
		/* Unlocked read. */
		if ((so->so_snd.sb_hiwat < sbused(&so->so_snd)) ||
		    (so->so_snd.sb_mbmax < so->so_snd.sb_mbcnt))
			*(int *)data = 0;
		else
			*(int *)data = sbspace(&so->so_snd);
		break;

	case FIOSETOWN:
		error = fsetown(*(int *)data, &so->so_sigio);
		break;

	case FIOGETOWN:
		*(int *)data = fgetown(&so->so_sigio);
		break;

	case SIOCSPGRP:
		error = fsetown(-(*(int *)data), &so->so_sigio);
		break;

	case SIOCGPGRP:
		*(int *)data = -fgetown(&so->so_sigio);
		break;

	case SIOCATMARK:
		/* Unlocked read. */
		*(int *)data = (so->so_rcv.sb_state & SBS_RCVATMARK) != 0;
		break;
	default:
		/*
		 * Interface/routing/protocol specific ioctls: interface and
		 * routing ioctls should have a different entry since a
		 * socket is unnecessary.
		 */
		if (IOCGROUP(cmd) == 'i')
			error = ifioctl(so, cmd, data, td);
		else if (IOCGROUP(cmd) == 'r') {
			CURVNET_SET(so->so_vnet);
			error = rtioctl_fib(cmd, data, so->so_fibnum);
			CURVNET_RESTORE();
		} else {
			CURVNET_SET(so->so_vnet);
			error = ((*so->so_proto->pr_usrreqs->pru_control)
			    (so, cmd, data, 0, td));
			CURVNET_RESTORE();
		}
		break;
	}
	return (error);
}

static int
soo_poll(struct file *fp, int events, struct ucred *active_cred,
    struct thread *td)
{
	struct socket *so = fp->f_data;
#ifdef MAC
	int error;

	error = mac_socket_check_poll(active_cred, so);
	if (error)
		return (error);
#endif
	return (sopoll(so, events, fp->f_cred, td));
}

static int
soo_stat(struct file *fp, struct stat *ub, struct ucred *active_cred,
    struct thread *td)
{
	struct socket *so = fp->f_data;
	struct sockbuf *sb;
#ifdef MAC
	int error;
#endif

	bzero((caddr_t)ub, sizeof (*ub));
	ub->st_mode = S_IFSOCK;
#ifdef MAC
	error = mac_socket_check_stat(active_cred, so);
	if (error)
		return (error);
#endif
	/*
	 * If SBS_CANTRCVMORE is set, but there's still data left in the
	 * receive buffer, the socket is still readable.
	 */
	sb = &so->so_rcv;
	SOCKBUF_LOCK(sb);
	if ((sb->sb_state & SBS_CANTRCVMORE) == 0 || sbavail(sb))
		ub->st_mode |= S_IRUSR | S_IRGRP | S_IROTH;
	ub->st_size = sbavail(sb) - sb->sb_ctl;
	SOCKBUF_UNLOCK(sb);

	sb = &so->so_snd;
	SOCKBUF_LOCK(sb);
	if ((sb->sb_state & SBS_CANTSENDMORE) == 0)
		ub->st_mode |= S_IWUSR | S_IWGRP | S_IWOTH;
	SOCKBUF_UNLOCK(sb);
	ub->st_uid = so->so_cred->cr_uid;
	ub->st_gid = so->so_cred->cr_gid;
	return (*so->so_proto->pr_usrreqs->pru_sense)(so, ub);
}

/*
 * API socket close on file pointer.  We call soclose() to close the socket
 * (including initiating closing protocols).  soclose() will sorele() the
 * file reference but the actual socket will not go away until the socket's
 * ref count hits 0.
 */
static int
soo_close(struct file *fp, struct thread *td)
{
	int error = 0;
	struct socket *so;

	so = fp->f_data;
	fp->f_ops = &badfileops;
	fp->f_data = NULL;

	if (so)
		error = soclose(so);
	return (error);
}

static int
soo_fill_kinfo(struct file *fp, struct kinfo_file *kif, struct filedesc *fdp)
{
	struct sockaddr *sa;
	struct inpcb *inpcb;
	struct unpcb *unpcb;
	struct socket *so;
	int error;

	kif->kf_type = KF_TYPE_SOCKET;
	so = fp->f_data;
	kif->kf_sock_domain = so->so_proto->pr_domain->dom_family;
	kif->kf_sock_type = so->so_type;
	kif->kf_sock_protocol = so->so_proto->pr_protocol;
	kif->kf_un.kf_sock.kf_sock_pcb = (uintptr_t)so->so_pcb;
	switch (kif->kf_sock_domain) {
	case AF_INET:
	case AF_INET6:
		if (kif->kf_sock_protocol == IPPROTO_TCP) {
			if (so->so_pcb != NULL) {
				inpcb = (struct inpcb *)(so->so_pcb);
				kif->kf_un.kf_sock.kf_sock_inpcb =
				    (uintptr_t)inpcb->inp_ppcb;
			}
		}
		break;
	case AF_UNIX:
		if (so->so_pcb != NULL) {
			unpcb = (struct unpcb *)(so->so_pcb);
			if (unpcb->unp_conn) {
				kif->kf_un.kf_sock.kf_sock_unpconn =
				    (uintptr_t)unpcb->unp_conn;
				kif->kf_un.kf_sock.kf_sock_rcv_sb_state =
				    so->so_rcv.sb_state;
				kif->kf_un.kf_sock.kf_sock_snd_sb_state =
				    so->so_snd.sb_state;
			}
		}
		break;
	}
	error = so->so_proto->pr_usrreqs->pru_sockaddr(so, &sa);
	if (error == 0 && sa->sa_len <= sizeof(kif->kf_sa_local)) {
		bcopy(sa, &kif->kf_sa_local, sa->sa_len);
		free(sa, M_SONAME);
	}
	error = so->so_proto->pr_usrreqs->pru_peeraddr(so, &sa);
	if (error == 0 && sa->sa_len <= sizeof(kif->kf_sa_peer)) {
		bcopy(sa, &kif->kf_sa_peer, sa->sa_len);
		free(sa, M_SONAME);
	}
	strncpy(kif->kf_path, so->so_proto->pr_domain->dom_name,
	    sizeof(kif->kf_path));
	return (0);	
}