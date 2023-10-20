/*
 * Copyright (c) 1982, 1986, 1988, 1990, 1993
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
 * 3. Neither the name of the University nor the names of its contributors
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
 *	@(#)ip_output.c	8.3 (Berkeley) 1/21/94
 * $FreeBSD: src/sys/netinet/ip_output.c,v 1.99.2.37 2003/04/15 06:44:45 silby Exp $
 */

#define _IP_VHL

#include "opt_ipdn.h"
#include "opt_ipdivert.h"
#include "opt_mbuf_stress_test.h"
#include "opt_mpls.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/proc.h>
#include <sys/caps.h>
#include <sys/sysctl.h>
#include <sys/in_cksum.h>
#include <sys/lock.h>

#include <sys/thread2.h>
#include <sys/mplock2.h>
#include <sys/msgport2.h>

#include <net/if.h>
#include <net/netisr.h>
#include <net/pfil.h>
#include <net/route.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/in_pcb.h>
#include <netinet/in_var.h>
#include <netinet/ip_var.h>

#include <netproto/mpls/mpls_var.h>

static MALLOC_DEFINE(M_IPMOPTS, "ip_moptions", "internet multicast options");

#include <net/ipfw/ip_fw.h>
#include <net/dummynet/ip_dummynet.h>

#define print_ip(x, a, y)	 kprintf("%s %d.%d.%d.%d%s",\
				x, (ntohl(a.s_addr)>>24)&0xFF,\
				  (ntohl(a.s_addr)>>16)&0xFF,\
				  (ntohl(a.s_addr)>>8)&0xFF,\
				  (ntohl(a.s_addr))&0xFF, y);

u_short ip_id;

#ifdef MBUF_STRESS_TEST
int mbuf_frag_size = 0;
SYSCTL_INT(_net_inet_ip, OID_AUTO, mbuf_frag_size, CTLFLAG_RW,
	&mbuf_frag_size, 0, "Fragment outgoing mbufs to this size");
#endif

static int ip_do_rfc6864 = 1;
SYSCTL_INT(_net_inet_ip, OID_AUTO, rfc6864, CTLFLAG_RW, &ip_do_rfc6864, 0,
    "Don't generate IP ID for DF IP datagrams");

static struct mbuf *ip_insertoptions(struct mbuf *, struct mbuf *, int *);
static struct ifnet *ip_multicast_if(struct in_addr *, int *);
static void	ip_mloopback
	(struct ifnet *, struct mbuf *, struct sockaddr_in *, int);
static int	ip_getmoptions
	(struct sockopt *, struct ip_moptions *);
static int	ip_pcbopts(int, struct mbuf **, struct mbuf *);
static int	ip_setmoptions
	(struct sockopt *, struct ip_moptions **);

int	ip_optcopy(struct ip *, struct ip *);

extern	struct protosw inetsw[];

static int
ip_localforward(struct mbuf *m, const struct sockaddr_in *dst, int hlen)
{
	struct in_ifaddr_container *iac;

	/*
	 * We need to figure out if we have been forwarded to a local
	 * socket.  If so, then we should somehow "loop back" to
	 * ip_input(), and get directed to the PCB as if we had received
	 * this packet.  This is because it may be difficult to identify
	 * the packets you want to forward until they are being output
	 * and have selected an interface (e.g. locally initiated
	 * packets).  If we used the loopback inteface, we would not be
	 * able to control what happens as the packet runs through
	 * ip_input() as it is done through a ISR.
	 */
	LIST_FOREACH(iac, INADDR_HASH(dst->sin_addr.s_addr), ia_hash) {
		/*
		 * If the addr to forward to is one of ours, we pretend
		 * to be the destination for this packet.
		 */
		if (IA_SIN(iac->ia)->sin_addr.s_addr == dst->sin_addr.s_addr)
			break;
	}
	if (iac != NULL) {
		if (m->m_pkthdr.rcvif == NULL)
			m->m_pkthdr.rcvif = loif;
		if (m->m_pkthdr.csum_flags & CSUM_DELAY_DATA) {
			m->m_pkthdr.csum_flags |= CSUM_DATA_VALID |
						  CSUM_PSEUDO_HDR;
			m->m_pkthdr.csum_data = 0xffff;
		}
		m->m_pkthdr.csum_flags |= CSUM_IP_CHECKED | CSUM_IP_VALID;

		/*
		 * Make sure that the IP header is in one mbuf,
		 * required by ip_input
		 */
		if (m->m_len < hlen) {
			m = m_pullup(m, hlen);
			if (m == NULL) {
				/* The packet was freed; we are done */
				return 1;
			}
		}
		ip_input(m);

		return 1; /* The packet gets forwarded locally */
	}
	return 0;
}

/*
 * IP output.  The packet in mbuf chain m contains a skeletal IP
 * header (with len, off, ttl, proto, tos, src, dst).
 * The mbuf chain containing the packet will be freed.
 * The mbuf opt, if present, will not be freed.
 */
int
ip_output(struct mbuf *m0, struct mbuf *opt, struct route *ro,
	  int flags, struct ip_moptions *imo, struct inpcb *inp)
{
	struct ip *ip;
	struct ifnet *ifp = NULL;	/* keep compiler happy */
	struct mbuf *m;
	int hlen = sizeof(struct ip);
	int len, error = 0;
	struct sockaddr_in *dst = NULL;	/* keep compiler happy */
	struct in_ifaddr *ia = NULL;
	int isbroadcast, sw_csum;
	struct in_addr pkt_dst;
	struct route iproute;
	struct m_tag *mtag;
	struct sockaddr_in *next_hop = NULL;
	int src_was_INADDR_ANY = 0;	/* as the name says... */

	ASSERT_NETISR_NCPUS(mycpuid);

	m = m0;
	M_ASSERTPKTHDR(m);

	if (ro == NULL) {
		ro = &iproute;
		bzero(ro, sizeof *ro);
	} else if (ro->ro_rt != NULL && ro->ro_rt->rt_cpuid != mycpuid) {
		if (flags & IP_DEBUGROUTE) {
			panic("ip_output: rt rt_cpuid %d accessed on cpu %d\n",
			    ro->ro_rt->rt_cpuid, mycpuid);
		}

		/*
		 * XXX
		 * If the cached rtentry's owner CPU is not the current CPU,
		 * then don't touch the cached rtentry (remote free is too
		 * expensive in this context); just relocate the route.
		 */
		ro = &iproute;
		bzero(ro, sizeof *ro);
	}

	if (m->m_pkthdr.fw_flags & IPFORWARD_MBUF_TAGGED) {
		/* Next hop */
		mtag = m_tag_find(m, PACKET_TAG_IPFORWARD, NULL);
		KKASSERT(mtag != NULL);
		next_hop = m_tag_data(mtag);
	}

	if (m->m_pkthdr.fw_flags & DUMMYNET_MBUF_TAGGED) {
		struct dn_pkt *dn_pkt;

		/* Extract info from dummynet tag */
		mtag = m_tag_find(m, PACKET_TAG_DUMMYNET, NULL);
		KKASSERT(mtag != NULL);
		dn_pkt = m_tag_data(mtag);

		/*
		 * The packet was already tagged, so part of the
		 * processing was already done, and we need to go down.
		 * Get the calculated parameters from the tag.
		 */
		ifp = dn_pkt->ifp;

		KKASSERT(ro == &iproute);
		*ro = dn_pkt->ro; /* structure copy */
		KKASSERT(ro->ro_rt == NULL || ro->ro_rt->rt_cpuid == mycpuid);

		dst = dn_pkt->dn_dst;
		if (dst == (struct sockaddr_in *)&(dn_pkt->ro.ro_dst)) {
			/* If 'dst' points into dummynet tag, adjust it */
			dst = (struct sockaddr_in *)&(ro->ro_dst);
		}

		ip = mtod(m, struct ip *);
		hlen = IP_VHL_HL(ip->ip_vhl) << 2 ;
		if (ro->ro_rt)
			ia = ifatoia(ro->ro_rt->rt_ifa);
		goto sendit;
	}

	if (opt) {
		len = 0;
		m = ip_insertoptions(m, opt, &len);
		if (len != 0)
			hlen = len;
	}
	ip = mtod(m, struct ip *);

	/*
	 * Fill in IP header.
	 */
	if (!(flags & (IP_FORWARDING|IP_RAWOUTPUT))) {
		ip->ip_vhl = IP_MAKE_VHL(IPVERSION, hlen >> 2);
		ip->ip_off &= htons(IP_DF);
		if (ip_do_rfc6864 && (ip->ip_off & htons(IP_DF)))
			ip->ip_id = 0;
		else
			ip->ip_id = ip_newid();
		ipstat.ips_localout++;
	} else {
		hlen = IP_VHL_HL(ip->ip_vhl) << 2;
	}

reroute:
	pkt_dst = next_hop ? next_hop->sin_addr : ip->ip_dst;

	dst = (struct sockaddr_in *)&ro->ro_dst;
	/*
	 * If there is a cached route,
	 * check that it is to the same destination
	 * and is still up.  If not, free it and try again.
	 * The address family should also be checked in case of sharing the
	 * cache with IPv6.
	 */
	if (ro->ro_rt &&
	    (!(ro->ro_rt->rt_flags & RTF_UP) ||
	     dst->sin_family != AF_INET ||
	     dst->sin_addr.s_addr != pkt_dst.s_addr)) {
		rtfree(ro->ro_rt);
		ro->ro_rt = NULL;
	}
	if (ro->ro_rt == NULL) {
		bzero(dst, sizeof *dst);
		dst->sin_family = AF_INET;
		dst->sin_len = sizeof *dst;
		dst->sin_addr = pkt_dst;
	}
	/*
	 * If routing to interface only,
	 * short circuit routing lookup.
	 */
	if (flags & IP_ROUTETOIF) {
		if ((ia = ifatoia(ifa_ifwithdstaddr(sintosa(dst)))) == NULL &&
		    (ia = ifatoia(ifa_ifwithnet(sintosa(dst)))) == NULL) {
			ipstat.ips_noroute++;
			error = ENETUNREACH;
			goto bad;
		}
		ifp = ia->ia_ifp;
		ip->ip_ttl = 1;
		isbroadcast = in_broadcast(dst->sin_addr, ifp);
	} else if (IN_MULTICAST(ntohl(pkt_dst.s_addr)) &&
		   imo != NULL && imo->imo_multicast_ifp != NULL) {
		/*
		 * Bypass the normal routing lookup for multicast
		 * packets if the interface is specified.
		 */
		ifp = imo->imo_multicast_ifp;
		ia = IFP_TO_IA(ifp);
		isbroadcast = 0;	/* fool gcc */
	} else {
		/*
		 * If this is the case, we probably don't want to allocate
		 * a protocol-cloned route since we didn't get one from the
		 * ULP.  This lets TCP do its thing, while not burdening
		 * forwarding or ICMP with the overhead of cloning a route.
		 * Of course, we still want to do any cloning requested by
		 * the link layer, as this is probably required in all cases
		 * for correct operation (as it is for ARP).
		 */
		if (ro->ro_rt == NULL)
			rtalloc_ign(ro, RTF_PRCLONING);
		if (ro->ro_rt == NULL) {
			ipstat.ips_noroute++;
			error = EHOSTUNREACH;
			goto bad;
		}
		ia = ifatoia(ro->ro_rt->rt_ifa);
		ifp = ro->ro_rt->rt_ifp;
		ro->ro_rt->rt_use++;
		if (ro->ro_rt->rt_flags & RTF_GATEWAY)
			dst = (struct sockaddr_in *)ro->ro_rt->rt_gateway;
		if (ro->ro_rt->rt_flags & RTF_HOST)
			isbroadcast = (ro->ro_rt->rt_flags & RTF_BROADCAST);
		else
			isbroadcast = in_broadcast(dst->sin_addr, ifp);
	}
	if (IN_MULTICAST(ntohl(pkt_dst.s_addr))) {
		m->m_flags |= M_MCAST;
		/*
		 * IP destination address is multicast.  Make sure "dst"
		 * still points to the address in "ro".  (It may have been
		 * changed to point to a gateway address, above.)
		 */
		dst = (struct sockaddr_in *)&ro->ro_dst;
		/*
		 * See if the caller provided any multicast options
		 */
		if (imo != NULL) {
			ip->ip_ttl = imo->imo_multicast_ttl;
			if (imo->imo_multicast_vif != -1) {
				ip->ip_src.s_addr =
				    ip_mcast_src ?
				    ip_mcast_src(imo->imo_multicast_vif) :
				    INADDR_ANY;
			}
		} else {
			ip->ip_ttl = IP_DEFAULT_MULTICAST_TTL;
		}
		/*
		 * Confirm that the outgoing interface supports multicast.
		 */
		if ((imo == NULL) || (imo->imo_multicast_vif == -1)) {
			if (!(ifp->if_flags & IFF_MULTICAST)) {
				ipstat.ips_noroute++;
				error = ENETUNREACH;
				goto bad;
			}
		}
		/*
		 * If source address not specified yet, use address of the
		 * outgoing interface.  In case, keep note we did that, so
		 * if the the firewall changes the next-hop causing the
		 * output interface to change, we can fix that.
		 */
		if (ip->ip_src.s_addr == INADDR_ANY || src_was_INADDR_ANY) {
			/* Interface may have no addresses. */
			if (ia != NULL) {
				ip->ip_src = IA_SIN(ia)->sin_addr;
				src_was_INADDR_ANY = 1;
			}
		}

		if (ip->ip_src.s_addr != INADDR_ANY) {
			struct in_multi *inm;

			inm = IN_LOOKUP_MULTI(&pkt_dst, ifp);
			if (inm != NULL &&
			    (imo == NULL || imo->imo_multicast_loop)) {
				/*
				 * If we belong to the destination multicast
				 * group on the outgoing interface, and the
				 * caller did not forbid loopback, loop back
				 * a copy.
				 */
				ip_mloopback(ifp, m, dst, hlen);
			} else {
				/*
				 * If we are acting as a multicast router,
				 * perform multicast forwarding as if the
				 * packet had just arrived on the interface
				 * to which we are about to send.  The
				 * multicast forwarding function recursively
				 * calls this function, using the IP_FORWARDING
				 * flag to prevent infinite recursion.
				 *
				 * Multicasts that are looped back by
				 * ip_mloopback(), above, will be forwarded by
				 * the ip_input() routine, if necessary.
				 */
				if (ip_mrouter && !(flags & IP_FORWARDING)) {
					/*
					 * If rsvp daemon is not running, do
					 * not set ip_moptions. This ensures
					 * that the packet is multicast and
					 * not just sent down one link as
					 * prescribed by rsvpd.
					 */
					if (!rsvp_on)
						imo = NULL;
					if (ip_mforward) {
						get_mplock();
						if (ip_mforward(ip, ifp,
						    m, imo) != 0) {
							m_freem(m);
							rel_mplock();
							goto done;
						}
						rel_mplock();
					}
				}
			}
		}

		/*
		 * Multicasts with a time-to-live of zero may be looped-
		 * back, above, but must not be transmitted on a network.
		 * Also, multicasts addressed to the loopback interface
		 * are not sent -- the above call to ip_mloopback() will
		 * loop back a copy if this host actually belongs to the
		 * destination group on the loopback interface.
		 */
		if (ip->ip_ttl == 0 || ifp->if_flags & IFF_LOOPBACK) {
			m_freem(m);
			goto done;
		}

		goto sendit;
	} else {
		m->m_flags &= ~M_MCAST;
	}

	/*
	 * If the source address is not specified yet, use the address
	 * of the outgoing interface.  In case, keep note we did that,
	 * so if the the firewall changes the next-hop causing the output
	 * interface to change, we can fix that.
	 */
	if (ip->ip_src.s_addr == INADDR_ANY || src_was_INADDR_ANY) {
		/* Interface may have no addresses. */
		if (ia != NULL) {
			ip->ip_src = IA_SIN(ia)->sin_addr;
			src_was_INADDR_ANY = 1;
		}
	}

	/*
	 * Look for broadcast address and
	 * verify user is allowed to send
	 * such a packet.
	 */
	if (isbroadcast) {
		if (!(ifp->if_flags & IFF_BROADCAST)) {
			error = EADDRNOTAVAIL;
			goto bad;
		}
		if (!(flags & IP_ALLOWBROADCAST)) {
			error = EACCES;
			goto bad;
		}
		/* don't allow broadcast messages to be fragmented */
		if (ntohs(ip->ip_len) > ifp->if_mtu) {
			error = EMSGSIZE;
			goto bad;
		}
		m->m_flags |= M_BCAST;
	} else {
		m->m_flags &= ~M_BCAST;
	}

sendit:

	/* We are already being fwd'd from a firewall. */
	if (next_hop != NULL)
		goto pass;

	/* No pfil hooks */
	if (!pfil_has_hooks(&inet_pfil_hook)) {
		if (m->m_pkthdr.fw_flags & DUMMYNET_MBUF_TAGGED) {
			/*
			 * Strip dummynet tags from stranded packets
			 */
			mtag = m_tag_find(m, PACKET_TAG_DUMMYNET, NULL);
			KKASSERT(mtag != NULL);
			m_tag_delete(m, mtag);
			m->m_pkthdr.fw_flags &= ~DUMMYNET_MBUF_TAGGED;
		}
		goto pass;
	}

	/*
	 * IpHack's section.
	 * - Xlate: translate packet's addr/port (NAT).
	 * - Firewall: deny/allow/etc.
	 * - Wrap: fake packet's addr/port <unimpl.>
	 * - Encapsulate: put it in another IP and send out. <unimp.>
	 */

	/*
	 * Run through list of hooks for output packets.
	 */
	error = pfil_run_hooks(&inet_pfil_hook, &m, ifp, PFIL_OUT);
	if (error != 0 || m == NULL)
		goto done;
	ip = mtod(m, struct ip *);

	if (m->m_pkthdr.fw_flags & IPFORWARD_MBUF_TAGGED) {
		/*
		 * Check dst to make sure it is directly reachable on the
		 * interface we previously thought it was.
		 * If it isn't (which may be likely in some situations) we have
		 * to re-route it (ie, find a route for the next-hop and the
		 * associated interface) and set them here. This is nested
		 * forwarding which in most cases is undesirable, except where
		 * such control is nigh impossible. So we do it here.
		 * And I'm babbling.
		 */
		mtag = m_tag_find(m, PACKET_TAG_IPFORWARD, NULL);
		KKASSERT(mtag != NULL);
		next_hop = m_tag_data(mtag);

		/*
		 * Try local forwarding first
		 */
		if (ip_localforward(m, next_hop, hlen))
			goto done;

		/*
		 * Relocate the route based on next_hop.
		 * If the current route is inp's cache, keep it untouched.
		 */
		if (ro == &iproute && ro->ro_rt != NULL) {
			RTFREE(ro->ro_rt);
			ro->ro_rt = NULL;
		}
		ro = &iproute;
		bzero(ro, sizeof *ro);

		/*
		 * Forwarding to broadcast address is not allowed.
		 * XXX Should we follow IP_ROUTETOIF?
		 */
		flags &= ~(IP_ALLOWBROADCAST | IP_ROUTETOIF);

		/* We are doing forwarding now */
		flags |= IP_FORWARDING;

		goto reroute;
	}

	if (m->m_pkthdr.fw_flags & DUMMYNET_MBUF_TAGGED) {
		struct dn_pkt *dn_pkt;

		mtag = m_tag_find(m, PACKET_TAG_DUMMYNET, NULL);
		KKASSERT(mtag != NULL);
		dn_pkt = m_tag_data(mtag);

		/*
		 * Under certain cases it is not possible to recalculate
		 * 'ro' and 'dst', let alone 'flags', so just save them in
		 * dummynet tag and avoid the possible wrong reculcalation
		 * when we come back to ip_output() again.
		 *
		 * All other parameters have been already used and so they
		 * are not needed anymore.
		 * XXX if the ifp is deleted while a pkt is in dummynet,
		 * we are in trouble! (TODO use ifnet_detach_event)
		 *
		 * We need to copy *ro because for ICMP pkts (and maybe
		 * others) the caller passed a pointer into the stack;
		 * dst might also be a pointer into *ro so it needs to
		 * be updated.
		 */
		dn_pkt->ro = *ro;
		if (ro->ro_rt)
			ro->ro_rt->rt_refcnt++;
		if (dst == (struct sockaddr_in *)&ro->ro_dst) {
			/* 'dst' points into 'ro' */
			dst = (struct sockaddr_in *)&(dn_pkt->ro.ro_dst);
		}
		dn_pkt->dn_dst = dst;
		dn_pkt->flags = flags;

		ip_dn_queue(m);
		goto done;
	}

	if (m->m_pkthdr.fw_flags & IPFW_MBUF_CONTINUE) {
		/* ipfw was disabled/unloaded. */
		m_freem(m);
		goto done;
	}
pass:
	/* 127/8 must not appear on wire - RFC1122. */
	if ((ntohl(ip->ip_dst.s_addr) >> IN_CLASSA_NSHIFT) == IN_LOOPBACKNET ||
	    (ntohl(ip->ip_src.s_addr) >> IN_CLASSA_NSHIFT) == IN_LOOPBACKNET) {
		if (!(ifp->if_flags & IFF_LOOPBACK)) {
			ipstat.ips_badaddr++;
			error = EADDRNOTAVAIL;
			goto bad;
		}
	}
	if (ip->ip_src.s_addr == INADDR_ANY ||
	    IN_MULTICAST(ntohl(ip->ip_src.s_addr))) {
		ipstat.ips_badaddr++;
		error = EADDRNOTAVAIL;
		goto bad;
	}

	if ((m->m_pkthdr.csum_flags & CSUM_TSO) == 0) {
		m->m_pkthdr.csum_flags |= CSUM_IP;
		sw_csum = m->m_pkthdr.csum_flags & ~ifp->if_hwassist;
		if (sw_csum & CSUM_DELAY_DATA) {
			in_delayed_cksum(m);
			sw_csum &= ~CSUM_DELAY_DATA;
		}
		m->m_pkthdr.csum_flags &= ifp->if_hwassist;
	} else {
		sw_csum = 0;
	}
	m->m_pkthdr.csum_iphlen = hlen;

	/*
	 * If small enough for interface, or the interface will take
	 * care of the fragmentation or segmentation for us, can just
	 * send directly.
	 */
	if (ntohs(ip->ip_len) <= ifp->if_mtu ||
	    ((ifp->if_hwassist & CSUM_FRAGMENT) &&
	      !(ip->ip_off & htons(IP_DF))) ||
	    (m->m_pkthdr.csum_flags & CSUM_TSO))
	{
		ip->ip_sum = 0;
		if (sw_csum & CSUM_DELAY_IP) {
			if (ip->ip_vhl == IP_VHL_BORING)
				ip->ip_sum = in_cksum_hdr(ip);
			else
				ip->ip_sum = in_cksum(m, hlen);
		}

		/* Record statistics for this interface address. */
		if (!(flags & IP_FORWARDING) && ia) {
			IFA_STAT_INC(&ia->ia_ifa, opackets, 1);
			IFA_STAT_INC(&ia->ia_ifa, obytes, m->m_pkthdr.len);
		}

#ifdef MBUF_STRESS_TEST
		if (mbuf_frag_size && m->m_pkthdr.len > mbuf_frag_size) {
			struct mbuf *m1, *m2;
			int length, tmp;

			tmp = length = m->m_pkthdr.len;

			while ((length -= mbuf_frag_size) >= 1) {
				m1 = m_split(m, length, M_NOWAIT);
				if (m1 == NULL)
					break;
				m2 = m;
				while (m2->m_next != NULL)
					m2 = m2->m_next;
				m2->m_next = m1;
			}
			m->m_pkthdr.len = tmp;
		}
#endif

#ifdef MPLS
		if (!mpls_output_process(m, ro->ro_rt))
			goto done;
#endif
		error = ifp->if_output(ifp, m, (struct sockaddr *)dst,
				       ro->ro_rt);
		goto done;
	}

	if (ip->ip_off & htons(IP_DF)) {
		error = EMSGSIZE;
		/*
		 * This case can happen if the user changed the MTU
		 * of an interface after enabling IP on it.  Because
		 * most netifs don't keep track of routes pointing to
		 * them, there is no way for one to update all its
		 * routes when the MTU is changed.
		 */
		if ((ro->ro_rt->rt_flags & (RTF_UP | RTF_HOST)) &&
		    !(ro->ro_rt->rt_rmx.rmx_locks & RTV_MTU) &&
		    (ro->ro_rt->rt_rmx.rmx_mtu > ifp->if_mtu)) {
			ro->ro_rt->rt_rmx.rmx_mtu = ifp->if_mtu;
		}
		ipstat.ips_cantfrag++;
		goto bad;
	}

	/*
	 * Too large for interface; fragment if possible. If successful,
	 * on return, m will point to a list of packets to be sent.
	 */
	error = ip_fragment(ip, &m, ifp->if_mtu, ifp->if_hwassist, sw_csum);
	if (error)
		goto bad;
	for (; m; m = m0) {
		m0 = m->m_nextpkt;
		m->m_nextpkt = NULL;
		if (error == 0) {
			/* Record statistics for this interface address. */
			if (ia != NULL) {
				IFA_STAT_INC(&ia->ia_ifa, opackets, 1);
				IFA_STAT_INC(&ia->ia_ifa, obytes,
				    m->m_pkthdr.len);
			}
#ifdef MPLS
			if (!mpls_output_process(m, ro->ro_rt))
				continue;
#endif
			error = ifp->if_output(ifp, m, (struct sockaddr *)dst,
					       ro->ro_rt);
		} else {
			m_freem(m);
		}
	}

	if (error == 0)
		ipstat.ips_fragmented++;

done:
	if (ro == &iproute && ro->ro_rt != NULL) {
		RTFREE(ro->ro_rt);
		ro->ro_rt = NULL;
	}
	return (error);
bad:
	m_freem(m);
	goto done;
}

/*
 * Create a chain of fragments which fit the given mtu. m_frag points to the
 * mbuf to be fragmented; on return it points to the chain with the fragments.
 * Return 0 if no error. If error, m_frag may contain a partially built
 * chain of fragments that should be freed by the caller.
 *
 * if_hwassist_flags is the hw offload capabilities (see if_data.ifi_hwassist)
 * sw_csum contains the delayed checksums flags (e.g., CSUM_DELAY_IP).
 */
int
ip_fragment(struct ip *ip, struct mbuf **m_frag, int mtu,
	    u_long if_hwassist_flags, int sw_csum)
{
	int error = 0;
	int hlen = IP_VHL_HL(ip->ip_vhl) << 2;
	int len = (mtu - hlen) & ~7;	/* size of payload in each fragment */
	int off;
	struct mbuf *m0 = *m_frag;	/* the original packet		*/
	int firstlen;
	struct mbuf **mnext;
	int nfrags;

	if (ip->ip_off & htons(IP_DF)) { /* Fragmentation not allowed */
		ipstat.ips_cantfrag++;
		return EMSGSIZE;
	}

	/*
	 * Must be able to put at least 8 bytes per fragment.
	 */
	if (len < 8)
		return EMSGSIZE;

	/*
	 * If the interface will not calculate checksums on
	 * fragmented packets, then do it here.
	 */
	if ((m0->m_pkthdr.csum_flags & CSUM_DELAY_DATA) &&
	    !(if_hwassist_flags & CSUM_IP_FRAGS)) {
		in_delayed_cksum(m0);
		m0->m_pkthdr.csum_flags &= ~CSUM_DELAY_DATA;
	}

	if (len > PAGE_SIZE) {
		/*
		 * Fragment large datagrams such that each segment
		 * contains a multiple of PAGE_SIZE amount of data,
		 * plus headers. This enables a receiver to perform
		 * page-flipping zero-copy optimizations.
		 *
		 * XXX When does this help given that sender and receiver
		 * could have different page sizes, and also mtu could
		 * be less than the receiver's page size ?
		 */
		int newlen;
		struct mbuf *m;

		for (m = m0, off = 0; m && (off+m->m_len) <= mtu; m = m->m_next)
			off += m->m_len;

		/*
		 * firstlen (off - hlen) must be aligned on an
		 * 8-byte boundary
		 */
		if (off < hlen)
			goto smart_frag_failure;
		off = ((off - hlen) & ~7) + hlen;
		newlen = (~PAGE_MASK) & mtu;
		if ((newlen + sizeof(struct ip)) > mtu) {
			/* we failed, go back the default */
smart_frag_failure:
			newlen = len;
			off = hlen + len;
		}
		len = newlen;

	} else {
		off = hlen + len;
	}

	firstlen = off - hlen;
	mnext = &m0->m_nextpkt;		/* pointer to next packet */

	/*
	 * Loop through length of segment after first fragment,
	 * make new header and copy data of each part and link onto chain.
	 * Here, m0 is the original packet, m is the fragment being created.
	 * The fragments are linked off the m_nextpkt of the original
	 * packet, which after processing serves as the first fragment.
	 */
	for (nfrags = 1; off < ntohs(ip->ip_len); off += len, nfrags++) {
		struct ip *mhip;	/* ip header on the fragment */
		struct mbuf *m;
		int mhlen = sizeof(struct ip);

		MGETHDR(m, M_NOWAIT, MT_HEADER);
		if (m == NULL) {
			error = ENOBUFS;
			ipstat.ips_odropped++;
			goto done;
		}
		m->m_flags |= (m0->m_flags & M_MCAST) | M_FRAG;
		/*
		 * In the first mbuf, leave room for the link header, then
		 * copy the original IP header including options. The payload
		 * goes into an additional mbuf chain returned by m_copy().
		 */
		m->m_data += max_linkhdr;
		mhip = mtod(m, struct ip *);
		*mhip = *ip;
		if (hlen > sizeof(struct ip)) {
			mhlen = ip_optcopy(ip, mhip) + sizeof(struct ip);
			mhip->ip_vhl = IP_MAKE_VHL(IPVERSION, mhlen >> 2);
		}
		m->m_len = mhlen;
		/* XXX do we need to add ip->ip_off below ? */
		mhip->ip_off = htons(((off - hlen) >> 3) + ntohs(ip->ip_off));
		if (off + len >= ntohs(ip->ip_len)) {	/* last fragment */
			len = ntohs(ip->ip_len) - off;
			m->m_flags |= M_LASTFRAG;
		} else {
			mhip->ip_off |= htons(IP_MF);
		}
		mhip->ip_len = htons((u_short)(len + mhlen));
		m->m_next = m_copy(m0, off, len);
		if (m->m_next == NULL) {		/* copy failed */
			m_free(m);
			error = ENOBUFS;	/* ??? */
			ipstat.ips_odropped++;
			goto done;
		}
		m->m_pkthdr.len = mhlen + len;
		m->m_pkthdr.rcvif = NULL;
		m->m_pkthdr.csum_flags = m0->m_pkthdr.csum_flags;
		m->m_pkthdr.csum_iphlen = mhlen;
		mhip->ip_sum = 0;
		if (sw_csum & CSUM_DELAY_IP)
			mhip->ip_sum = in_cksum(m, mhlen);
		*mnext = m;
		mnext = &m->m_nextpkt;
	}
	ipstat.ips_ofragments += nfrags;

	/* set first marker for fragment chain */
	m0->m_flags |= M_FIRSTFRAG | M_FRAG;
	m0->m_pkthdr.csum_data = nfrags;

	/*
	 * Update first fragment by trimming what's been copied out
	 * and updating header.
	 */
	m_adj(m0, hlen + firstlen - ntohs(ip->ip_len));
	m0->m_pkthdr.len = hlen + firstlen;
	ip->ip_len = htons((u_short)m0->m_pkthdr.len);
	ip->ip_off |= htons(IP_MF);
	ip->ip_sum = 0;
	if (sw_csum & CSUM_DELAY_IP)
		ip->ip_sum = in_cksum(m0, hlen);

done:
	*m_frag = m0;
	return error;
}

void
in_delayed_cksum(struct mbuf *m)
{
	struct ip *ip;
	u_short csum, offset;

	ip = mtod(m, struct ip *);
	offset = IP_VHL_HL(ip->ip_vhl) << 2 ;
	csum = in_cksum_skip(m, ntohs(ip->ip_len), offset);
	if (m->m_pkthdr.csum_flags & CSUM_UDP && csum == 0)
		csum = 0xffff;
	offset += m->m_pkthdr.csum_data;	/* checksum offset */

	if (offset + sizeof(u_short) > m->m_len) {
		kprintf("delayed m_pullup, m->len: %d  off: %d  p: %d\n",
		    m->m_len, offset, ip->ip_p);
		/*
		 * XXX
		 * this shouldn't happen, but if it does, the
		 * correct behavior may be to insert the checksum
		 * in the existing chain instead of rearranging it.
		 */
		m = m_pullup(m, offset + sizeof(u_short));
	}
	*(u_short *)(m->m_data + offset) = csum;
}

/*
 * Insert IP options into preformed packet.
 * Adjust IP destination as required for IP source routing,
 * as indicated by a non-zero in_addr at the start of the options.
 *
 * XXX This routine assumes that the packet has no options in place.
 */
static struct mbuf *
ip_insertoptions(struct mbuf *m, struct mbuf *opt, int *phlen)
{
	struct ipoption *p = mtod(opt, struct ipoption *);
	struct mbuf *n;
	struct ip *ip = mtod(m, struct ip *);
	unsigned optlen;

	optlen = opt->m_len - sizeof p->ipopt_dst;
	if (optlen + (u_short)ntohs(ip->ip_len) > IP_MAXPACKET) {
		*phlen = 0;
		return (m);		/* XXX should fail */
	}
	if (p->ipopt_dst.s_addr)
		ip->ip_dst = p->ipopt_dst;
	if (m->m_flags & M_EXT || m->m_data - optlen < m->m_pktdat) {
		MGETHDR(n, M_NOWAIT, MT_HEADER);
		if (n == NULL) {
			*phlen = 0;
			return (m);
		}
		n->m_pkthdr.rcvif = NULL;
		n->m_pkthdr.len = m->m_pkthdr.len + optlen;
		m->m_len -= sizeof(struct ip);
		m->m_data += sizeof(struct ip);
		n->m_next = m;
		m = n;
		m->m_len = optlen + sizeof(struct ip);
		m->m_data += max_linkhdr;
		memcpy(mtod(m, void *), ip, sizeof(struct ip));
	} else {
		m->m_data -= optlen;
		m->m_len += optlen;
		m->m_pkthdr.len += optlen;
		bcopy(ip, mtod(m, caddr_t), sizeof(struct ip));
	}
	ip = mtod(m, struct ip *);
	bcopy(p->ipopt_list, ip + 1, optlen);
	*phlen = sizeof(struct ip) + optlen;
	ip->ip_vhl = IP_MAKE_VHL(IPVERSION, *phlen >> 2);
	ip->ip_len = htons(ntohs( ip->ip_len) + optlen);
	return (m);
}

/*
 * Copy options from ip to jp,
 * omitting those not copied during fragmentation.
 */
int
ip_optcopy(struct ip *ip, struct ip *jp)
{
	u_char *cp, *dp;
	int opt, optlen, cnt;

	cp = (u_char *)(ip + 1);
	dp = (u_char *)(jp + 1);
	cnt = (IP_VHL_HL(ip->ip_vhl) << 2) - sizeof(struct ip);
	for (; cnt > 0; cnt -= optlen, cp += optlen) {
		opt = cp[0];
		if (opt == IPOPT_EOL)
			break;
		if (opt == IPOPT_NOP) {
			/* Preserve for IP mcast tunnel's LSRR alignment. */
			*dp++ = IPOPT_NOP;
			optlen = 1;
			continue;
		}

		KASSERT(cnt >= IPOPT_OLEN + sizeof *cp,
		    ("ip_optcopy: malformed ipv4 option"));
		optlen = cp[IPOPT_OLEN];
		KASSERT(optlen >= IPOPT_OLEN + sizeof *cp && optlen <= cnt,
		    ("ip_optcopy: malformed ipv4 option"));

		/* bogus lengths should have been caught by ip_dooptions */
		if (optlen > cnt)
			optlen = cnt;
		if (IPOPT_COPIED(opt)) {
			bcopy(cp, dp, optlen);
			dp += optlen;
		}
	}
	for (optlen = dp - (u_char *)(jp+1); optlen & 0x3; optlen++)
		*dp++ = IPOPT_EOL;
	return (optlen);
}

/*
 * IP socket option processing.
 */
void
ip_ctloutput(netmsg_t msg)
{
	struct socket *so = msg->base.nm_so;
	struct sockopt *sopt = msg->ctloutput.nm_sopt;
	struct	inpcb *inp = so->so_pcb;
	int	error, optval;

	error = optval = 0;

	/* Get socket's owner cpuid hint */
	if (sopt->sopt_level == SOL_SOCKET &&
	    sopt->sopt_dir == SOPT_GET &&
	    sopt->sopt_name == SO_CPUHINT) {
		optval = mycpuid;
		soopt_from_kbuf(sopt, &optval, sizeof(optval));
		goto done;
	}

	if (sopt->sopt_level != IPPROTO_IP) {
		error = EINVAL;
		goto done;
	}

	switch (sopt->sopt_name) {
	case IP_MULTICAST_IF:
	case IP_MULTICAST_VIF:
	case IP_MULTICAST_TTL:
	case IP_MULTICAST_LOOP:
	case IP_ADD_MEMBERSHIP:
	case IP_DROP_MEMBERSHIP:
		/*
		 * Handle multicast options in netisr0
		 */
		if (&curthread->td_msgport != netisr_cpuport(0)) {
			/* NOTE: so_port MUST NOT be checked in netisr0 */
			msg->lmsg.ms_flags |= MSGF_IGNSOPORT;
			lwkt_forwardmsg(netisr_cpuport(0), &msg->lmsg);
			return;
		}
		break;
	}

	switch (sopt->sopt_dir) {
	case SOPT_SET:
		switch (sopt->sopt_name) {
		case IP_OPTIONS:
#ifdef notyet
		case IP_RETOPTS:
#endif
		{
			struct mbuf *m;
			if (sopt->sopt_valsize > MLEN) {
				error = EMSGSIZE;
				break;
			}
			MGET(m, sopt->sopt_td ? M_WAITOK : M_NOWAIT, MT_HEADER);
			if (m == NULL) {
				error = ENOBUFS;
				break;
			}
			m->m_len = sopt->sopt_valsize;
			error = soopt_to_kbuf(sopt, mtod(m, void *), m->m_len,
					      m->m_len);
			error = ip_pcbopts(sopt->sopt_name,
					   &inp->inp_options, m);
			goto done;
		}

		case IP_TOS:
		case IP_TTL:
		case IP_MINTTL:
		case IP_RECVOPTS:
		case IP_RECVRETOPTS:
		case IP_RECVDSTADDR:
		case IP_RECVIF:
		case IP_RECVTOS:
		case IP_RECVTTL:
			error = soopt_to_kbuf(sopt, &optval, sizeof optval,
					     sizeof optval);
			if (error)
				break;
			switch (sopt->sopt_name) {
			case IP_TOS:
				inp->inp_ip_tos = optval;
				break;

			case IP_TTL:
				inp->inp_ip_ttl = optval;
				break;
			case IP_MINTTL:
				if (optval >= 0 && optval <= MAXTTL)
					inp->inp_ip_minttl = optval;
				else
					error = EINVAL;
				break;
#define	OPTSET(bit) \
	if (optval) \
		inp->inp_flags |= bit; \
	else \
		inp->inp_flags &= ~bit;

			case IP_RECVOPTS:
				OPTSET(INP_RECVOPTS);
				break;

			case IP_RECVRETOPTS:
				OPTSET(INP_RECVRETOPTS);
				break;

			case IP_RECVDSTADDR:
				OPTSET(INP_RECVDSTADDR);
				break;

			case IP_RECVIF:
				OPTSET(INP_RECVIF);
				break;

			case IP_RECVTOS:
				OPTSET(INP_RECVTOS);
				break;

			case IP_RECVTTL:
				OPTSET(INP_RECVTTL);
				break;
			}
			break;
#undef OPTSET

		case IP_MULTICAST_IF:
		case IP_MULTICAST_VIF:
		case IP_MULTICAST_TTL:
		case IP_MULTICAST_LOOP:
		case IP_ADD_MEMBERSHIP:
		case IP_DROP_MEMBERSHIP:
			error = ip_setmoptions(sopt, &inp->inp_moptions);
			break;

		case IP_PORTRANGE:
			error = soopt_to_kbuf(sopt, &optval, sizeof optval,
					    sizeof optval);
			if (error)
				break;

			switch (optval) {
			case IP_PORTRANGE_DEFAULT:
				inp->inp_flags &= ~(INP_LOWPORT);
				inp->inp_flags &= ~(INP_HIGHPORT);
				break;

			case IP_PORTRANGE_HIGH:
				inp->inp_flags &= ~(INP_LOWPORT);
				inp->inp_flags |= INP_HIGHPORT;
				break;

			case IP_PORTRANGE_LOW:
				inp->inp_flags &= ~(INP_HIGHPORT);
				inp->inp_flags |= INP_LOWPORT;
				break;

			default:
				error = EINVAL;
				break;
			}
			break;


		default:
			error = ENOPROTOOPT;
			break;
		}
		break;

	case SOPT_GET:
		switch (sopt->sopt_name) {
		case IP_OPTIONS:
		case IP_RETOPTS:
			if (inp->inp_options)
				soopt_from_kbuf(sopt, mtod(inp->inp_options,
							   char *),
						inp->inp_options->m_len);
			else
				sopt->sopt_valsize = 0;
			break;

		case IP_TOS:
		case IP_TTL:
		case IP_MINTTL:
		case IP_RECVOPTS:
		case IP_RECVRETOPTS:
		case IP_RECVDSTADDR:
		case IP_RECVTOS:
		case IP_RECVTTL:
		case IP_RECVIF:
		case IP_PORTRANGE:
			switch (sopt->sopt_name) {

			case IP_TOS:
				optval = inp->inp_ip_tos;
				break;

			case IP_TTL:
				optval = inp->inp_ip_ttl;
				break;
			case IP_MINTTL:
				optval = inp->inp_ip_minttl;
				break;

#define	OPTBIT(bit)	(inp->inp_flags & bit ? 1 : 0)

			case IP_RECVOPTS:
				optval = OPTBIT(INP_RECVOPTS);
				break;

			case IP_RECVRETOPTS:
				optval = OPTBIT(INP_RECVRETOPTS);
				break;

			case IP_RECVDSTADDR:
				optval = OPTBIT(INP_RECVDSTADDR);
				break;

			case IP_RECVTOS:
				optval = OPTBIT(INP_RECVTOS);
				break;

			case IP_RECVTTL:
				optval = OPTBIT(INP_RECVTTL);
				break;

			case IP_RECVIF:
				optval = OPTBIT(INP_RECVIF);
				break;

			case IP_PORTRANGE:
				if (inp->inp_flags & INP_HIGHPORT)
					optval = IP_PORTRANGE_HIGH;
				else if (inp->inp_flags & INP_LOWPORT)
					optval = IP_PORTRANGE_LOW;
				else
					optval = 0;
				break;
			}
			soopt_from_kbuf(sopt, &optval, sizeof optval);
			break;

		case IP_MULTICAST_IF:
		case IP_MULTICAST_VIF:
		case IP_MULTICAST_TTL:
		case IP_MULTICAST_LOOP:
		case IP_ADD_MEMBERSHIP:
		case IP_DROP_MEMBERSHIP:
			error = ip_getmoptions(sopt, inp->inp_moptions);
			break;

		default:
			error = ENOPROTOOPT;
			break;
		}
		break;
	}
done:
	lwkt_replymsg(&msg->lmsg, error);
}

/*
 * Set up IP options in pcb for insertion in output packets.
 * Store in mbuf with pointer in pcbopt, adding pseudo-option
 * with destination address if source routed.
 */
static int
ip_pcbopts(int optname, struct mbuf **pcbopt, struct mbuf *m)
{
	int cnt, optlen;
	u_char *cp;
	u_char opt;

	/* turn off any old options */
	if (*pcbopt)
		m_free(*pcbopt);
	*pcbopt = NULL;
	if (m == NULL || m->m_len == 0) {
		/*
		 * Only turning off any previous options.
		 */
		if (m != NULL)
			m_free(m);
		return (0);
	}

	if (m->m_len % sizeof(int32_t))
		goto bad;
	/*
	 * IP first-hop destination address will be stored before
	 * actual options; move other options back
	 * and clear it when none present.
	 */
	if (m->m_data + m->m_len + sizeof(struct in_addr) >= &m->m_dat[MLEN])
		goto bad;
	cnt = m->m_len;
	m->m_len += sizeof(struct in_addr);
	cp = mtod(m, u_char *) + sizeof(struct in_addr);
	bcopy(mtod(m, caddr_t), cp, cnt);
	bzero(mtod(m, caddr_t), sizeof(struct in_addr));

	for (; cnt > 0; cnt -= optlen, cp += optlen) {
		opt = cp[IPOPT_OPTVAL];
		if (opt == IPOPT_EOL)
			break;
		if (opt == IPOPT_NOP)
			optlen = 1;
		else {
			if (cnt < IPOPT_OLEN + sizeof *cp)
				goto bad;
			optlen = cp[IPOPT_OLEN];
			if (optlen < IPOPT_OLEN + sizeof *cp || optlen > cnt)
				goto bad;
		}
		switch (opt) {

		default:
			break;

		case IPOPT_LSRR:
		case IPOPT_SSRR:
			/*
			 * user process specifies route as:
			 *	->A->B->C->D
			 * D must be our final destination (but we can't
			 * check that since we may not have connected yet).
			 * A is first hop destination, which doesn't appear in
			 * actual IP option, but is stored before the options.
			 */
			if (optlen < IPOPT_MINOFF - 1 + sizeof(struct in_addr))
				goto bad;
			m->m_len -= sizeof(struct in_addr);
			cnt -= sizeof(struct in_addr);
			optlen -= sizeof(struct in_addr);
			cp[IPOPT_OLEN] = optlen;
			/*
			 * Move first hop before start of options.
			 */
			bcopy(&cp[IPOPT_OFFSET+1], mtod(m, caddr_t),
			      sizeof(struct in_addr));
			/*
			 * Then copy rest of options back
			 * to close up the deleted entry.
			 */
			bcopy(&cp[IPOPT_OFFSET+1] + sizeof(struct in_addr),
			      &cp[IPOPT_OFFSET+1],
			      cnt - (IPOPT_MINOFF - 1));
			break;
		}
	}
	if (m->m_len > MAX_IPOPTLEN + sizeof(struct in_addr))
		goto bad;
	*pcbopt = m;
	return (0);

bad:
	m_free(m);
	return (EINVAL);
}

/*
 * XXX
 * The whole multicast option thing needs to be re-thought.
 * Several of these options are equally applicable to non-multicast
 * transmission, and one (IP_MULTICAST_TTL) totally duplicates a
 * standard option (IP_TTL).
 */

/*
 * following RFC1724 section 3.3, 0.0.0.0/8 is interpreted as interface index.
 */
static struct ifnet *
ip_multicast_if(struct in_addr *a, int *ifindexp)
{
	int ifindex;
	struct ifnet *ifp;

	if (ifindexp)
		*ifindexp = 0;
	if (ntohl(a->s_addr) >> 24 == 0) {
		ifindex = ntohl(a->s_addr) & 0xffffff;
		if (ifindex < 0 || if_index < ifindex)
			return NULL;
		ifp = ifindex2ifnet[ifindex];
		if (ifindexp)
			*ifindexp = ifindex;
	} else {
		ifp = INADDR_TO_IFP(a);
	}
	return ifp;
}

/*
 * Set the IP multicast options in response to user setsockopt().
 */
static int
ip_setmoptions(struct sockopt *sopt, struct ip_moptions **imop)
{
	int error = 0;
	int i;
	struct ip_mreqn mreqn;
	struct ifnet *ifp;
	struct ip_moptions *imo = *imop;
	int ifindex;

	if (imo == NULL) {
		/*
		 * No multicast option buffer attached to the pcb;
		 * allocate one and initialize to default values.
		 */
		imo = kmalloc(sizeof *imo, M_IPMOPTS, M_WAITOK);

		imo->imo_multicast_ifp = NULL;
		imo->imo_multicast_addr.s_addr = INADDR_ANY;
		imo->imo_multicast_vif = -1;
		imo->imo_multicast_ttl = IP_DEFAULT_MULTICAST_TTL;
		imo->imo_multicast_loop = IP_DEFAULT_MULTICAST_LOOP;
		imo->imo_num_memberships = 0;
		/* Assign imo to imop after all fields are setup */
		cpu_sfence();
		*imop = imo;
	}
	switch (sopt->sopt_name) {
	/* store an index number for the vif you wanna use in the send */
	case IP_MULTICAST_VIF:
		if (legal_vif_num == 0) {
			error = EOPNOTSUPP;
			break;
		}
		error = soopt_to_kbuf(sopt, &i, sizeof i, sizeof i);
		if (error)
			break;
		if (!legal_vif_num(i) && (i != -1)) {
			error = EINVAL;
			break;
		}
		imo->imo_multicast_vif = i;
		break;

	case IP_MULTICAST_IF:
		/*
		 * Select the interface for outgoing multicast packets.
		 */
		if (sopt->sopt_valsize >= sizeof(mreqn)) {
			/*
			 * Linux compat.
			 */
			error = soopt_to_kbuf(sopt, &mreqn,
			    sizeof(mreqn), sizeof(mreqn));
			if (error)
				break;
		} else if (sopt->sopt_valsize >= sizeof(struct ip_mreq)) {
			/*
			 * Linux compat.
			 */
			mreqn.imr_ifindex = 0;
			error = soopt_to_kbuf(sopt, &mreqn,
			    sizeof(struct ip_mreq), sizeof(struct ip_mreq));
			if (error)
				break;
		} else {
			mreqn.imr_ifindex = 0;
			error = soopt_to_kbuf(sopt, &mreqn.imr_address,
			    sizeof(struct in_addr), sizeof(struct in_addr));
			if (error)
				break;
		}

		ifindex = mreqn.imr_ifindex;
		if (ifindex != 0) {
			if (ifindex < 0 || if_index < ifindex) {
				error = EINVAL;
				break;
			}
			ifp = ifindex2ifnet[ifindex];
			mreqn.imr_address.s_addr = htonl(ifindex & 0xffffff);
		} else {
			/*
			 * INADDR_ANY is used to remove a previous selection.
			 * When no interface is selected, a default one is
			 * chosen every time a multicast packet is sent.
			 */
			if (mreqn.imr_address.s_addr == INADDR_ANY) {
				imo->imo_multicast_ifp = NULL;
				break;
			}
			/*
			 * The selected interface is identified by its local
			 * IP address.  Find the interface and confirm that
			 * it supports multicasting.
			 */
			ifp = ip_multicast_if(&mreqn.imr_address, &ifindex);
		}

		if (ifp == NULL || !(ifp->if_flags & IFF_MULTICAST)) {
			error = EADDRNOTAVAIL;
			break;
		}
		imo->imo_multicast_ifp = ifp;
		if (ifindex)
			imo->imo_multicast_addr = mreqn.imr_address;
		else
			imo->imo_multicast_addr.s_addr = INADDR_ANY;
		break;

	case IP_MULTICAST_TTL:
		/*
		 * Set the IP time-to-live for outgoing multicast packets.
		 * The original multicast API required a char argument,
		 * which is inconsistent with the rest of the socket API.
		 * We allow either a char or an int.
		 */
		if (sopt->sopt_valsize == 1) {
			u_char ttl;
			error = soopt_to_kbuf(sopt, &ttl, 1, 1);
			if (error)
				break;
			imo->imo_multicast_ttl = ttl;
		} else {
			u_int ttl;
			error = soopt_to_kbuf(sopt, &ttl, sizeof ttl, sizeof ttl);
			if (error)
				break;
			if (ttl > 255)
				error = EINVAL;
			else
				imo->imo_multicast_ttl = ttl;
		}
		break;

	case IP_MULTICAST_LOOP:
		/*
		 * Set the loopback flag for outgoing multicast packets.
		 * Must be zero or one.  The original multicast API required a
		 * char argument, which is inconsistent with the rest
		 * of the socket API.  We allow either a char or an int.
		 */
		if (sopt->sopt_valsize == 1) {
			u_char loop;

			error = soopt_to_kbuf(sopt, &loop, 1, 1);
			if (error)
				break;
			imo->imo_multicast_loop = !!loop;
		} else {
			u_int loop;

			error = soopt_to_kbuf(sopt, &loop, sizeof loop,
					    sizeof loop);
			if (error)
				break;
			imo->imo_multicast_loop = !!loop;
		}
		break;

	case IP_ADD_MEMBERSHIP:
		/*
		 * Add a multicast group membership.
		 * Group must be a valid IP multicast address.
		 */
		if (sopt->sopt_valsize >= sizeof(mreqn)) {
			error = soopt_to_kbuf(sopt, &mreqn,
			    sizeof(mreqn), sizeof(mreqn));
			if (error)
				break;
		} else {
			mreqn.imr_ifindex = 0;
			error = soopt_to_kbuf(sopt, &mreqn,
			    sizeof(struct ip_mreq), sizeof(struct ip_mreq));
			if (error)
				break;
		}

		if (!IN_MULTICAST(ntohl(mreqn.imr_multiaddr.s_addr))) {
			error = EINVAL;
			break;
		}

		ifindex = mreqn.imr_ifindex;
		if (ifindex != 0) {
			if (ifindex < 0 || if_index < ifindex) {
				error = EINVAL;
				break;
			}
			ifp = ifindex2ifnet[ifindex];
		} else if (mreqn.imr_address.s_addr == INADDR_ANY) {
			struct sockaddr_in dst;
			struct rtentry *rt;

			/*
			 * If no interface address or index was provided,
			 * use the interface of the route to the given
			 * multicast address.
			 */
			bzero(&dst, sizeof(struct sockaddr_in));
			dst.sin_len = sizeof(struct sockaddr_in);
			dst.sin_family = AF_INET;
			dst.sin_addr = mreqn.imr_multiaddr;
			rt = rtlookup((struct sockaddr *)&dst);
			if (rt == NULL) {
				error = EADDRNOTAVAIL;
				break;
			}
			--rt->rt_refcnt;
			ifp = rt->rt_ifp;
		} else {
			ifp = ip_multicast_if(&mreqn.imr_address, NULL);
		}

		/*
		 * See if we found an interface, and confirm that it
		 * supports multicast.
		 */
		if (ifp == NULL || !(ifp->if_flags & IFF_MULTICAST)) {
			error = EADDRNOTAVAIL;
			break;
		}
		/*
		 * See if the membership already exists or if all the
		 * membership slots are full.
		 */
		for (i = 0; i < imo->imo_num_memberships; ++i) {
			if (imo->imo_membership[i]->inm_ifp == ifp &&
			    imo->imo_membership[i]->inm_addr.s_addr
						== mreqn.imr_multiaddr.s_addr)
				break;
		}
		if (i < imo->imo_num_memberships) {
			error = EADDRINUSE;
			break;
		}
		if (i == IP_MAX_MEMBERSHIPS) {
			error = ETOOMANYREFS;
			break;
		}
		/*
		 * Everything looks good; add a new record to the multicast
		 * address list for the given interface.
		 */
		if ((imo->imo_membership[i] =
		     in_addmulti(&mreqn.imr_multiaddr, ifp)) == NULL) {
			error = ENOBUFS;
			break;
		}
		++imo->imo_num_memberships;
		break;

	case IP_DROP_MEMBERSHIP:
		/*
		 * Drop a multicast group membership.
		 * Group must be a valid IP multicast address.
		 */
		if (sopt->sopt_valsize >= sizeof(mreqn)) {
			error = soopt_to_kbuf(sopt, &mreqn,
			    sizeof(mreqn), sizeof(mreqn));
			if (error)
				break;
		} else {
			mreqn.imr_ifindex = 0;
			error = soopt_to_kbuf(sopt, &mreqn,
			    sizeof(struct ip_mreq), sizeof(struct ip_mreq));
			if (error)
				break;
		}

		if (!IN_MULTICAST(ntohl(mreqn.imr_multiaddr.s_addr))) {
			error = EINVAL;
			break;
		}

		/*
		 * If an interface index or address was specified, get a
		 * pointer to its ifnet structure.
		 */
		ifindex = mreqn.imr_ifindex;
		if (ifindex != 0) {
			if (ifindex < 0 || if_index < ifindex) {
				error = EINVAL;
				break;
			}
			ifp = ifindex2ifnet[ifindex];
		} else if (mreqn.imr_address.s_addr == INADDR_ANY) {
			ifp = NULL;
		} else {
			ifp = ip_multicast_if(&mreqn.imr_address, NULL);
			if (ifp == NULL) {
				error = EADDRNOTAVAIL;
				break;
			}
		}
		/*
		 * Find the membership in the membership array.
		 */
		for (i = 0; i < imo->imo_num_memberships; ++i) {
			if ((ifp == NULL ||
			     imo->imo_membership[i]->inm_ifp == ifp) &&
			    imo->imo_membership[i]->inm_addr.s_addr ==
			    mreqn.imr_multiaddr.s_addr)
				break;
		}
		if (i == imo->imo_num_memberships) {
			error = EADDRNOTAVAIL;
			break;
		}
		/*
		 * Give up the multicast address record to which the
		 * membership points.
		 */
		in_delmulti(imo->imo_membership[i]);
		/*
		 * Remove the gap in the membership array.
		 */
		for (++i; i < imo->imo_num_memberships; ++i)
			imo->imo_membership[i-1] = imo->imo_membership[i];
		--imo->imo_num_memberships;
		break;

	default:
		error = EOPNOTSUPP;
		break;
	}

	return (error);
}

/*
 * Return the IP multicast options in response to user getsockopt().
 */
static int
ip_getmoptions(struct sockopt *sopt, struct ip_moptions *imo)
{
	struct in_addr addr;
	struct in_ifaddr *ia;
	int error, optval;
	u_char coptval;

	error = 0;
	switch (sopt->sopt_name) {
	case IP_MULTICAST_VIF:
		if (imo != NULL)
			optval = imo->imo_multicast_vif;
		else
			optval = -1;
		soopt_from_kbuf(sopt, &optval, sizeof optval);
		break;

	case IP_MULTICAST_IF:
		if (imo == NULL || imo->imo_multicast_ifp == NULL)
			addr.s_addr = INADDR_ANY;
		else if (imo->imo_multicast_addr.s_addr) {
			/* return the value user has set */
			addr = imo->imo_multicast_addr;
		} else {
			ia = IFP_TO_IA(imo->imo_multicast_ifp);
			addr.s_addr = (ia == NULL) ? INADDR_ANY
				: IA_SIN(ia)->sin_addr.s_addr;
		}
		soopt_from_kbuf(sopt, &addr, sizeof addr);
		break;

	case IP_MULTICAST_TTL:
		if (imo == NULL)
			optval = coptval = IP_DEFAULT_MULTICAST_TTL;
		else
			optval = coptval = imo->imo_multicast_ttl;
		if (sopt->sopt_valsize == 1)
			soopt_from_kbuf(sopt, &coptval, 1);
		else
			soopt_from_kbuf(sopt, &optval, sizeof optval);
		break;

	case IP_MULTICAST_LOOP:
		if (imo == NULL)
			optval = coptval = IP_DEFAULT_MULTICAST_LOOP;
		else
			optval = coptval = imo->imo_multicast_loop;
		if (sopt->sopt_valsize == 1)
			soopt_from_kbuf(sopt, &coptval, 1);
		else
			soopt_from_kbuf(sopt, &optval, sizeof optval);
		break;

	default:
		error = ENOPROTOOPT;
		break;
	}
	return (error);
}

/*
 * Discard the IP multicast options.
 */
void
ip_freemoptions(struct ip_moptions *imo)
{
	int i;

	if (imo != NULL) {
		for (i = 0; i < imo->imo_num_memberships; ++i)
			in_delmulti(imo->imo_membership[i]);
		kfree(imo, M_IPMOPTS);
	}
}

/*
 * Routine called from ip_output() to loop back a copy of an IP multicast
 * packet to the input queue of a specified interface.  Note that this
 * calls the output routine of the loopback "driver", but with an interface
 * pointer that might NOT be a loopback interface -- evil, but easier than
 * replicating that code here.
 */
static void
ip_mloopback(struct ifnet *ifp, struct mbuf *m, struct sockaddr_in *dst,
	     int hlen)
{
	struct ip *ip;
	struct mbuf *copym;

	copym = m_copypacket(m, M_NOWAIT);
	if (copym != NULL && (copym->m_flags & M_EXT || copym->m_len < hlen))
		copym = m_pullup(copym, hlen);
	if (copym != NULL) {
		/*
		 * if the checksum hasn't been computed, mark it as valid
		 */
		if (copym->m_pkthdr.csum_flags & CSUM_DELAY_DATA) {
			in_delayed_cksum(copym);
			copym->m_pkthdr.csum_flags &= ~CSUM_DELAY_DATA;
			copym->m_pkthdr.csum_flags |=
			    CSUM_DATA_VALID | CSUM_PSEUDO_HDR;
			copym->m_pkthdr.csum_data = 0xffff;
		}
		/*
		 * We don't bother to fragment if the IP length is greater
		 * than the interface's MTU.  Can this possibly matter?
		 */
		ip = mtod(copym, struct ip *);
		ip->ip_sum = 0;
		if (ip->ip_vhl == IP_VHL_BORING) {
			ip->ip_sum = in_cksum_hdr(ip);
		} else {
			ip->ip_sum = in_cksum(copym, hlen);
		}
		/*
		 * NB:
		 * It's not clear whether there are any lingering
		 * reentrancy problems in other areas which might
		 * be exposed by using ip_input directly (in
		 * particular, everything which modifies the packet
		 * in-place).  Yet another option is using the
		 * protosw directly to deliver the looped back
		 * packet.  For the moment, we'll err on the side
		 * of safety by using if_simloop().
		 */
#if 1 /* XXX */
		if (dst->sin_family != AF_INET) {
			kprintf("ip_mloopback: bad address family %d\n",
						dst->sin_family);
			dst->sin_family = AF_INET;
		}
#endif
		if_simloop(ifp, copym, dst->sin_family, 0);
	}
}
