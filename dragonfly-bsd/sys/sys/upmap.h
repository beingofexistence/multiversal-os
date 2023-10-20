/*
 * Copyright (c) 2014 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef	_SYS_UPMAP_H_
#define	_SYS_UPMAP_H_

#ifndef _SYS_TYPES_H_
#include <sys/types.h>
#endif
#ifndef _SYS_TIME_H_
#include <sys/time.h>
#endif

#define UPMAP_MAXPROCTITLE	1024
#define LPMAP_MAXTHREADTITLE	1024
#define LPMAP_MAPSIZE		65536
#define UPMAP_MAPSIZE		65536
#define KPMAP_MAPSIZE		65536

#define LPMAP_VERSION		1
#define UPMAP_VERSION		1
#define KPMAP_VERSION		1

typedef uint64_t	forkid_t;

typedef struct ukpheader {
	uint16_t	type;		/* element type */
	uint16_t	offset;		/* offset from map base, max 65535 */
} ukpheader_t;

#define UKPLEN_MASK		0x0F00
#define UKPLEN_1		0x0000
#define UKPLEN_2		0x0100
#define UKPLEN_4		0x0200
#define UKPLEN_8		0x0300
#define UKPLEN_16		0x0400
#define UKPLEN_32		0x0500
#define UKPLEN_64		0x0600
#define UKPLEN_128		0x0700
#define UKPLEN_256		0x0800
#define UKPLEN_512		0x0900
#define UKPLEN_1024		0x0A00

#define UKPLEN_TS		((sizeof(struct timespec) == 8) ? \
					UKPLEN_16 : UKPLEN_32)
#define UKPLEN_DECODE(type)	(1 << ((type >> 8) & 0x0F))

/*
 * Global types - may exist in all three mapping types
 */
#define UKPTYPE_VERSION		(0x0001 | UKPLEN_4)	/* always first */

/*
 * /dev/lpmap - per-thread
 */
#define LPTYPE_RESERVEDINT0	(0x4010 | UKPLEN_4)
#define LPTYPE_RESERVEDINT1	(0x4011 | UKPLEN_4)
#define LPTYPE_BLOCKALLSIGS	(0x4012 | UKPLEN_4)
#define LPTYPE_THREAD_TITLE	(0x4013 | UKPLEN_1024)
#define LPTYPE_THREAD_TID	(0x4014 | UKPLEN_4)

/*
 * /dev/upmap - per-process
 */
#define UPTYPE_RUNTICKS		(0x0010 | UKPLEN_4)
#define UPTYPE_FORKID		(0x0011 | UKPLEN_8)
#define UPTYPE_PID		(0x0012 | UKPLEN_4)
#define UPTYPE_PROC_TITLE	(0x0013 | UKPLEN_1024)
#define UPTYPE_INVFORK		(0x0014 | UKPLEN_4)

/*
 * /dev/kpmap - kernel-wide
 */
#define KPTYPE_UPTICKS		(0x8000 | UKPLEN_4)
#define KPTYPE_TS_UPTIME	(0x8001 | UKPLEN_TS)
#define KPTYPE_TS_REALTIME	(0x8002 | UKPLEN_TS)
#define KPTYPE_TSC_FREQ		(0x8003 | UKPLEN_8)
#define KPTYPE_TICK_FREQ	(0x8004 | UKPLEN_8)
#define KPTYPE_FAST_GTOD	(0x8005 | UKPLEN_4)

#if defined(_KERNEL) || defined(_KERNEL_STRUCTURES)

/*
 * (writable) user per-thread map via /dev/lpmap.
 *
 * ABSOLUTE LOCATIONS CAN CHANGE, ITERATE HEADERS FOR THE TYPE YOU DESIRE
 * UNTIL YOU HIT TYPE 0, THEN CACHE THE RESULTING POINTER.
 *
 * If you insist, at least check that the version matches LPMAP_VERSION.
 *
 * --
 *
 * The current thread can block all blockable signals by (atomically)
 * incrementing blockallsigs.  If the kernel receives a signal while
 * the low 31 bits of blockallsigs are non-zero, the received signal
 * will be made pending but not acted upon and bit 31 of blockallsigs
 * will be set.  The signal mask is not affected.
 *
 * Upon decrementing blockallsigs to 0 (low 31 bits to 0), again atomically,
 * userland should then check to see if bit 31 is set, clear it, and then
 * issue any real system call to force the kernel to re-check pending signals
 * and act upon them.
 */
struct sys_lpmap {
	ukpheader_t	header[64];
	uint32_t	version;
	uint32_t	tid;
	uint32_t	reserved02;
	uint32_t	blockallsigs;
	char		thread_title[LPMAP_MAXTHREADTITLE];
};

/*
 * (writable) user per-process map via /dev/upmap.
 *
 * ABSOLUTE LOCATIONS CAN CHANGE, ITERATE HEADERS FOR THE TYPE YOU DESIRE
 * UNTIL YOU HIT TYPE 0, THEN CACHE THE RESULTING POINTER.
 *
 * If you insist, at least check that the version matches UPMAP_VERSION.
 */
struct sys_upmap {
	ukpheader_t	header[64];
	uint32_t	version;
	uint32_t	runticks;	/* running scheduler ticks */
	forkid_t	forkid;		/* unique 2^64 (fork detect) NOT MONO */
	uint32_t	invfork;	/* vfork active */
	pid_t		pid;		/* process id */
	uint32_t	reserved[16];
	char		proc_title[UPMAP_MAXPROCTITLE];
};

/*
 * (read-only) kernel per-cpu map via /dev/kpmap.
 *
 * ABSOLUTE LOCATIONS CAN CHANGE, ITERATE HEADERS FOR THE TYPE YOU DESIRE
 * UNTIL YOU HIT TYPE 0, THEN CACHE THE RESULTING POINTER.
 *
 * If you insist, at least check that the version matches KPMAP_VERSION.
 *
 * Procedure for reading stable values from ts_uptime/ts_realtime.  This
 * avoids looping in nearly all cases, including during a kernel update.
 * The only case where this might loop is if the kernel deschedules
 * the user thread for more than 1 tick.
 *
 *	do {
 *		w = upticks;
 *		cpu_lfence();
 *		load ts_uptime[w & 1] or ts_realtime[w & 1]
 *		cpu_lfence();
 *		w = upticks - w;
 *	} while (w > 1);
 */
struct sys_kpmap {
	ukpheader_t	header[64];
	int32_t		version;
	int32_t		upticks;	/* userland reads ts_*[upticks & 1] */
	struct timespec	ts_uptime[2];	/* mono uptime @ticks (uncompensated) */
	struct timespec ts_realtime[2];	/* realtime @ticks resolution */
	int64_t		tsc_freq;	/* (if supported by cpu) */
	int32_t		tick_freq;	/* scheduler tick frequency */
	int32_t		fast_gtod;	/* fast gettimeofday() */
};

#endif

#ifdef _KERNEL
extern struct sys_kpmap *kpmap;
#endif

#endif
