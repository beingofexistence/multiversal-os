/*-
 * Copyright (c) 1982, 1986, 1993
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
 *	@(#)vmmeter.h	8.2 (Berkeley) 7/10/94
 * $FreeBSD: src/sys/sys/vmmeter.h,v 1.21.2.2 2002/10/10 19:28:21 dillon Exp $
 */

#ifndef _SYS_VMMETER_H_
#define _SYS_VMMETER_H_

#ifndef _SYS_TYPES_H_
#include <sys/types.h>
#endif

struct globaldata;

/*
 * System wide statistics counters.
 */
struct vmmeter {
	/*
	 * General system activity.
	 */
#define vmmeter_uint_begin	v_swtch
	u_int v_swtch;		/* context switches */
	u_int v_trap;		/* calls to trap */
	u_int v_syscall;	/* calls to syscall() */
	u_int v_intr;		/* device interrupts */
	u_int v_ipi;		/* inter processor interrupts */
	u_int v_timer;		/* LAPIC timer interrupts */
	u_int v_soft;		/* software interrupts */
	/*
	 * Virtual memory activity.
	 */
	u_int v_vm_faults;	/* number of address memory faults */
	u_int v_cow_faults;	/* number of copy-on-writes */
	u_int v_cow_optim;	/* number of optimized copy-on-writes */
	u_int v_zfod;		/* pages zero filled on demand */
	u_int v_ozfod;		/* optimized zero fill pages */
	u_int v_swapin;		/* swap pager pageins */
	u_int v_swapout;	/* swap pager pageouts */
	u_int v_swappgsin;	/* swap pager pages paged in */
	u_int v_swappgsout;	/* swap pager pages paged out */
	u_int v_vnodein;	/* vnode pager pageins */
	u_int v_vnodeout;	/* vnode pager pageouts */
	u_int v_vnodepgsin;	/* vnode_pager pages paged in */
	u_int v_vnodepgsout;	/* vnode pager pages paged out */
	u_int v_intrans;	/* intransit blocking page faults */
	u_int v_reactivated;	/* number of pages reactivated from free list */
	u_int v_pdwakeups;	/* number of times daemon has awaken from sleep */
	u_int v_pdpages;	/* number of pages analyzed by daemon */

	u_int v_dfree;		/* pages freed by daemon */
	u_int v_pfree;		/* pages freed by exiting processes */
	u_int v_tfree;		/* total pages freed */
	/*
	 * Fork/vfork/rfork activity.
	 */
	u_int v_forks;		/* number of fork() calls */
	u_int v_vforks;		/* number of vfork() calls */
	u_int v_rforks;		/* number of rfork() calls */
	u_int v_exec;		/* number of exec() calls */
	u_int v_kthreads;	/* number of fork() calls by kernel */
	u_int v_forkpages;	/* number of VM pages affected by fork() */
	u_int v_vforkpages;	/* number of VM pages affected by vfork() */
	u_int v_rforkpages;	/* number of VM pages affected by rfork() */
	u_int v_kthreadpages;	/* number of VM pages affected by fork() by kernel */
	u_int v_intrans_coll;	/* intransit map collisions (total) */
	u_int v_intrans_wait;	/* intransit map collisions which blocked */
	u_int v_forwarded_ints; /* forwarded interrupts due to MP lock */
	u_int v_forwarded_hits;
	u_int v_forwarded_misses;
	u_int v_smpinvltlb;	/* nasty global invltlbs */
	u_int v_ppwakeups;	/* wakeups on processes stalled on VM */
	u_int v_lock_colls;	/* # of token, lock, or spin collisions */
	u_int v_wakeup_colls;	/* possible spurious wakeup IPIs */
	u_int v_reserved7;
#define vmmeter_uint_end	v_reserved7
	char  v_lock_name[32];	/* last-colliding token, lock, or spin name */
	void  *v_lock_addr;	/* last-colliding lock address */
};

/*
 * vmstats structure, global vmstats is the rollup, pcpu vmstats keeps
 * track of minor (generally positive) adjustments.  For moving targets,
 * the global vmstats structure represents the smallest likely value.
 *
 * This structure is cache sensitive, separate nominal read-only elements
 * from variable elements.
 */
struct vmstats {
	/*
	 * Distribution of page usages.
	 */
	u_int v_page_size;	/* page size in bytes */
	u_int v_unused01;
	long v_page_count;	/* total number of pages in system */
	long v_free_severe;	/* severe depletion of pages below this pt */
	long v_free_reserved;	/* number of pages reserved for deadlock */

	/*
	 * Free page queues, pageout daemon starts below when v_free_count
	 * goes below v_free_min.
	 */
	long v_free_min;	/* minimum free pages to maintain */
	long v_free_target;	/* pageout daemon loop target */

	/*
	 * While operating, the pageout daemon pipelines up to N pages per
	 * loop, relooping as needed until all targets are achieved.
	 *
	 * active   -> inactive
	 * inactive -> cache	(1/4 of v_inactive_target)
	 */
	long v_inactive_target;	/* active -> inactive */

	/*
	 * The pageout daemon also starts when (v_free_count + v_cache_count)
	 * goes below v_paging_start, and generally continues until it
	 * goes above v_paging_target2.
	 */
	long v_paging_wait;	/* vs (free + cache), stall user-land */
	long v_paging_start;	/* vs (free + cache), start paging */
	long v_paging_target1;	/* vs (free + cache), slow down paging */
	long v_paging_target2;	/* vs (free + cache), stop paging */

	long v_pageout_free_min; /* min number pages reserved for kernel */
	long v_interrupt_free_min; /* reserved number of pages for int code */
	long v_dma_pages;	/* total dma-reserved pages */

	long v_unused_fixed01;
	long v_unused_fixed02;
	long v_unused_fixed03;

	long v_free_count;	/* number of pages free */
	long v_wire_count;	/* number of pages wired down */
	long v_active_count;	/* number of pages active */
	long v_inactive_count;	/* number of pages inactive */
	long v_cache_count;	/* number of pages on buffer cache queue */
	long v_dma_avail;	/* free dma-reserved pages */

	long v_unused_variable[9];
};

#ifdef _KERNEL

/* note: vmmeter 'cnt' structure is now per-cpu */
extern struct vmstats vmstats;

#endif

/* systemwide totals computed every five seconds */
struct vmtotal {
	long	t_rq;		/* length of the run queue */
	long	t_dw;		/* jobs in ``disk wait'' (neg priority) */
	long	t_pw;		/* jobs in page wait */
	long	t_sl;		/* jobs sleeping in core */
	long	t_sw;		/* swapped out runnable/short block jobs */
	int64_t	t_vm;		/* total virtual memory */
	int64_t	t_avm;		/* active virtual memory */
	long	t_rm;		/* total real memory in use */
	long	t_arm;		/* active real memory */
	int64_t	t_vmshr;	/* shared virtual memory */
	int64_t	t_avmshr;	/* active shared virtual memory */
	long	t_rmshr;	/* shared real memory */
	long	t_armshr;	/* active shared real memory */
	long	t_free;		/* free memory pages */
};

#ifdef PGINPROF
/*
 * Optional instrumentation.
 */

#define	NDMON	128
#define	NSMON	128

#define	DRES	20
#define	SRES	5

#define	PMONMIN	20
#define	PRES	50
#define	NPMON	64

#define	RMONMIN	130
#define	RRES	5
#define	NRMON	64

/* data and stack size distribution counters */
u_int	dmon[NDMON+1];
u_int	smon[NSMON+1];

/* page in time distribution counters */
u_int	pmon[NPMON+2];

/* reclaim time distribution counters */
u_int	rmon[NRMON+2];

int	pmonmin;
int	pres;
int	rmonmin;
int	rres;

u_int rectime;		/* accumulator for reclaim times */
u_int pgintime;		/* accumulator for page in times */

#endif	/* PGINPROF */

#ifdef _KERNEL

void vmstats_rollup(void);
void vmstats_rollup_cpu(struct globaldata *gd);

#endif
#endif
