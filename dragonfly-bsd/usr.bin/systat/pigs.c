/*-
 * Copyright (c) 1980, 1992, 1993
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
 * @(#)pigs.c	8.2 (Berkeley) 9/23/93
 */

/*
 * Pigs display from Bill Reeves at Lucasfilm
 */

#include <sys/user.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/sysctl.h>

#include <curses.h>
#include <err.h>
#include <kinfo.h>
#include <math.h>
#include <nlist.h>
#include <pwd.h>
#include <stdlib.h>

#include "systat.h"
#include "extern.h"

int compar(const void *, const void *);

static int nproc;
static struct p_times {
	float pt_pctcpu;
	struct kinfo_proc *pt_kp;
} *pt;

struct kinfo_cputime old_cp_time;
static int fscale;

WINDOW *
openpigs(void)
{
	return (subwin(stdscr, LINES-5-1, 0, 5, 0));
}

void
closepigs(WINDOW *w)
{
	if (w == NULL)
		return;
	wclear(w);
	wrefresh(w);
	delwin(w);
}


void
showpigs(void)
{
	int i, j, y, k;
	float total;
	int factor;
	const char *uname, *pname;
	char pidname[30];

	if (pt == NULL)
		return;
	/* Accumulate the percent of cpu per user. */
	total = 0.0;
	for (i = 0; i <= nproc; i++) {
		/* Accumulate the percentage. */
		total += pt[i].pt_pctcpu;
	}

	if (total < 1.0)
 		total = 1.0;
	factor = 50.0/total;

        qsort(pt, nproc + 1, sizeof (struct p_times), compar);
	y = 1;
	i = nproc + 1;
	if (i > wnd->_maxy-1)
		i = wnd->_maxy-1;
	for (k = 0; i > 0; i--, y++, k++) {
		if (pt[k].pt_pctcpu <= 0.01 &&
		    (pt[k].pt_kp == NULL ||
		    pt[k].pt_kp->kp_lwp.kl_slptime > 1)
		) {
			--y;
			continue;
		}
		if (pt[k].pt_kp == NULL) {
			uname = "";
			pname = "<idle>";
		} else {
			uname = user_from_uid(pt[k].pt_kp->kp_uid, 0);
			pname = pt[k].pt_kp->kp_comm;
		}
		wmove(wnd, y, 0);
		wclrtoeol(wnd);
		mvwaddstr(wnd, y, 0, uname);
		snprintf(pidname, sizeof(pidname), "%10.10s", pname);
		mvwaddstr(wnd, y, 9, pidname);
		wmove(wnd, y, 20);
		for (j = pt[k].pt_pctcpu*factor + 0.5; j > 0; j--)
			waddch(wnd, 'X');
	}
	wmove(wnd, y, 0); wclrtobot(wnd);
}

int
initpigs(void)
{
	size_t oldlen;

	if (kinfo_get_sched_cputime(&old_cp_time))
		err(1, "kinfo_get_sched_cputime");

	oldlen = sizeof(fscale);
	if (sysctlbyname("kern.fscale", &fscale, &oldlen, NULL, 0) < 0)
		err(1, "sysctlbyname");

	return(1);
}

void
fetchpigs(void)
{
	int i;
	float ftime;
	float *pctp;
	struct kinfo_proc *kpp, *pp;
	struct kinfo_cputime cp_time, diff_cp_time;
	double t;
	static int lastnproc = 0;

	if ((kpp = kvm_getprocs(kd, KERN_PROC_ALL, 0, &nproc)) == NULL) {
		error("%s", kvm_geterr(kd));
		if (pt)
			free(pt);
		return;
	}
	if (nproc > lastnproc) {
		free(pt);
		if ((pt =
		    malloc((nproc + 1) * sizeof(struct p_times))) == NULL) {
			error("Out of memory");
			die(0);
		}
	}
	lastnproc = nproc;
	/*
	 * calculate %cpu for each proc
	 */
	for (i = 0; i < nproc; i++) {
		pt[i].pt_kp = &kpp[i];
		pp = &kpp[i];
		pctp = &pt[i].pt_pctcpu;
		ftime = pp->kp_swtime;
		if (ftime == 0 || (pp->kp_flags & P_SWAPPEDOUT))
			*pctp = 0;
		else
			*pctp = ((double) pp->kp_lwp.kl_pctcpu / fscale);
	}
	/*
	 * and for the imaginary "idle" process
	 */
	if (kinfo_get_sched_cputime(&cp_time))
		err(1, "kinfo_get_sched_cputime");
	diff_cp_time.cp_user = cp_time.cp_user - old_cp_time.cp_user;
	diff_cp_time.cp_nice = cp_time.cp_nice - old_cp_time.cp_nice;
	diff_cp_time.cp_sys = cp_time.cp_sys - old_cp_time.cp_sys;
	diff_cp_time.cp_intr = cp_time.cp_intr - old_cp_time.cp_intr;
	diff_cp_time.cp_idle = cp_time.cp_idle - old_cp_time.cp_idle;
	old_cp_time = cp_time;
	t = diff_cp_time.cp_user + diff_cp_time.cp_nice +
	    diff_cp_time.cp_sys + diff_cp_time.cp_intr +
	    diff_cp_time.cp_idle;
	if (t == 0.0)
		t = 1.0;
	pt[nproc].pt_kp = NULL;
	pt[nproc].pt_pctcpu = diff_cp_time.cp_idle / t;
}

void
labelpigs(void)
{
	wmove(wnd, 0, 0);
	wclrtoeol(wnd);
	mvwaddstr(wnd, 0, 20,
	    "/0   /10  /20  /30  /40  /50  /60  /70  /80  /90  /100");
}

int
compar(const void *a, const void *b)
{
	const struct p_times *pta = (const struct p_times *)a;
	const struct p_times *ptb = (const struct p_times *)b;

	/*
	 * Check overall cpu percentage first.
	 */
	if (pta->pt_pctcpu > ptb->pt_pctcpu)
		return (-1);	/* a is better */
	else if (pta->pt_pctcpu < ptb->pt_pctcpu)
		return (1);	/* b is better */
	else
		return 0;
}
