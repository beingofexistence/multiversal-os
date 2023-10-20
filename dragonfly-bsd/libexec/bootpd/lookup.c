/*
 * lookup.c - Lookup IP address, HW address, netmask
 *
 * $FreeBSD: src/libexec/bootpd/lookup.c,v 1.7 1999/08/28 00:09:19 peter Exp $
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <sys/time.h>	/* for struct timeval in net/if.h */
#include <net/if.h>
#include <netinet/in.h>

#ifdef	ETC_ETHERS
#include <net/ethernet.h>
extern int ether_hostton();
#endif

#include <netdb.h>
#include <syslog.h>

#ifndef USE_BFUNCS
#include <memory.h>
/* Yes, memcpy is OK here (no overlapped copies). */
#define bcopy(a,b,c)    memcpy(b,a,c)
#endif

#include "bootp.h"
#include "lookup.h"
#include "report.h"

/*
 * Lookup an Ethernet address and return it.
 * Return NULL if addr not found.
 */
u_char *
lookup_hwa(char *hostname, int htype)
{
	switch (htype) {

		/* XXX - How is this done on other systems? -gwr */
#ifdef	ETC_ETHERS
	case HTYPE_ETHERNET:
	case HTYPE_IEEE802:
		{
			static struct ether_addr ea;
			/* This does a lookup in /etc/ethers */
			if (ether_hostton(hostname, &ea)) {
				report(LOG_ERR, "no HW addr for host \"%s\"",
					   hostname);
				return NULL;
			}
			return (u_char *) & ea;
		}
#endif /* ETC_ETHERS */

	default:
		report(LOG_ERR, "no lookup for HW addr type %d", htype);
	}							/* switch */

	/* If the system can't do it, just return an error. */
	return NULL;
}


/*
 * Lookup an IP address.
 * Return non-zero on failure.
 */
int
lookup_ipa(char *hostname, u_int32 *result)
{
	struct hostent *hp;
	hp = gethostbyname(hostname);
	if (!hp)
		return -1;
	bcopy(hp->h_addr, result, sizeof(*result));
	return 0;
}


/*
 * Lookup a netmask
 * addr and result are both in network order. Return non-zero on failure.
 *
 * XXX - This is OK as a default, but to really make this automatic,
 * we would need to get the subnet mask from the ether interface.
 * If this is wrong, specify the correct value in the bootptab.
 */
int
lookup_netmask(u_int32 addr, u_int32 *result)
{
	int32 m, a;

	a = ntohl(addr);
	m = 0;

	if (IN_CLASSA(a))
		m = IN_CLASSA_NET;

	if (IN_CLASSB(a))
		m = IN_CLASSB_NET;

	if (IN_CLASSC(a))
		m = IN_CLASSC_NET;

	if (!m)
		return -1;
	*result = htonl(m);
	return 0;
}
