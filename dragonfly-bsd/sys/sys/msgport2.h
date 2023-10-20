/*
 * SYS/MSGPORT2.H
 *
 *	Implements Inlines for LWKT messages and ports.
 */

#ifndef _SYS_MSGPORT2_H_
#define _SYS_MSGPORT2_H_

#ifndef _KERNEL
#error "This file should not be included by userland programs."
#endif

#ifndef _SYS_SYSTM_H_
#include <sys/systm.h>
#endif

#ifdef MALLOC_DECLARE
MALLOC_DECLARE(M_LWKTMSG);
#endif

/*
 * Initialize a LWKT message structure.  Note that if the message supports
 * an abort MSGF_ABORTABLE must be passed in flags.
 *
 * Note that other areas of the LWKT msg may already be initialized, so we
 * do not zero the message here.
 *
 * Messages are marked as DONE until sent.
 */
static __inline
void
lwkt_initmsg(lwkt_msg_t msg, lwkt_port_t rport, int flags)
{
    msg->ms_flags = MSGF_DONE | flags;
    msg->ms_reply_port = rport;
}

static __inline
void
lwkt_initmsg_abortable(lwkt_msg_t msg, lwkt_port_t rport, int flags,
		       void (*abortfn)(lwkt_msg_t))
{
    lwkt_initmsg(msg, rport, flags | MSGF_ABORTABLE);
    msg->ms_abortfn = abortfn;
}

static __inline
void
lwkt_replymsg(lwkt_msg_t msg, int error)
{
    lwkt_port_t port;

    msg->ms_error = error;
    port = msg->ms_reply_port;
    port->mp_replyport(port, msg);
}

/*
 * Retrieve the next message from the port's message queue, return NULL
 * if no messages are pending.  The retrieved message will either be a
 * request or a reply based on the MSGF_REPLY bit.
 *
 * If the backend port is a thread port, the the calling thread MUST
 * own the port.
 */
static __inline
void *
lwkt_getport(lwkt_port_t port)
{
    return(port->mp_getport(port));
}

static __inline
void *
lwkt_waitport(lwkt_port_t port, int flags)
{
    return(port->mp_waitport(port, flags));
}

static __inline
int
lwkt_waitmsg(lwkt_msg_t msg, int flags)
{
    return(msg->ms_reply_port->mp_waitmsg(msg, flags));
}


static __inline
int
lwkt_checkmsg(lwkt_msg_t msg)
{
    return(msg->ms_flags & MSGF_DONE);
}

static __inline
int
lwkt_dropmsg(lwkt_msg_t msg)
{
    lwkt_port_t port;
    int error = ENOENT;

    KKASSERT(msg->ms_flags & MSGF_DROPABLE);
    port = msg->ms_target_port;
    if (port)
	    error = port->mp_dropmsg(port, msg);
    return (error);
}

static __inline
void
lwkt_setmsg_receipt(lwkt_msg_t msg, void (*receiptfn)(lwkt_msg_t, lwkt_port_t))
{
	msg->ms_flags |= MSGF_RECEIPT;
	msg->ms_receiptfn = receiptfn;
}

#endif	/* _SYS_MSGPORT2_H_ */
