/*-
 * Copyright (c) 1983, 1989, 1992, 1993
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
 */

/*
 * Cursed vmstat -- from Robert Elz.
 */

#include <sys/user.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/namei.h>
#include <sys/sysctl.h>
#include <sys/vmmeter.h>

#include <vm/vm_param.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <kinfo.h>
#include <langinfo.h>
#include <nlist.h>
#include <paths.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "utmpentry.h"
#include <devstat.h>
#include "systat.h"
#include "extern.h"
#include "devs.h"

#define NKVMSW	16

static struct Info {
	struct kinfo_cputime cp_time;
	struct	vmmeter Vmm;
	struct	vmtotal Total;
	struct  vmstats Vms;
	struct	nchstats nchstats;
	long	nchcount;
	long	nchpathcount;
	long	*intrcnt;
	long	bufspace;
	int	maxvnodes;
	int	cachedvnodes;
	int	inactivevnodes;
	int	activevnodes;
	long	dirtybufspace;
	long	physmem;
	struct kvm_swap  kvmsw[NKVMSW];
} s, s1, s2, z;

struct statinfo run;
struct kinfo_cputime cp_time;
static int kvnsw;

#define	vmm s.Vmm
#define	vms s.Vms
#define oldvmm s1.Vmm
#define oldvms s1.Vms
#define	total s.Total
#define	nchtotal s.nchstats
#define	oldnchtotal s1.nchstats

static	enum state { BOOT, TIME, RUN } state = TIME;

static void allocinfo(struct Info *);
static void copyinfo(struct Info *, struct Info *);
static void dinfo(int, int, struct statinfo *, struct statinfo *);
static void getinfo(struct Info *);
static void put64(int64_t, int, int, int, int);
static void putfloat(double, int, int, int, int, int);
static void putlongdouble(long double, int, int, int, int, int);
static void putlongdoublez(long double, int, int, int, int, int);
static int ucount(void);

static	int ncpu;
static	char buf[26];
static	time_t t;
static	double etime;
static	int nintr;
static	int  *intralias;
static	int  *intrsmp;
static	long *intrloc;
static	long *lacc;
static	char **intrname;
static	int nextintsrow;
static  int extended_vm_stats;



WINDOW *
openkre(void)
{

	return (stdscr);
}

void
closekre(WINDOW *w)
{

	if (w == NULL)
		return;
	wclear(w);
	wrefresh(w);
}


static struct nlist namelist[] = {
#define	X_BUFFERSPACE	0
	{ .n_name = "_bufspace" },
#define	X_NCHSTATS	1
	{ .n_name = "_nchstats" },
#define	X_DESIREDVNODES	2
	{ .n_name = "_maxvnodes" },
#define	X_CACHEDVNODES	3
	{ .n_name = "_cachedvnodes" },
#define	X_INACTIVEVNODES 4
	{ .n_name = "_inactivevnodes" },
#define	X_ACTIVEVNODES	5
	{ .n_name = "_activevnodes" },
#define X_NUMDIRTYBUFFERS 6
	{ .n_name = "_dirtybufspace" },
	{ .n_name = "" },
};

/*
 * These constants define where the major pieces are laid out
 */
#define STATROW		 0	/* uses 1 row and 68 cols */
#define STATCOL		 2
#define MEMROW		 2	/* uses 4 rows and 31 cols */
#define MEMCOLA		 0
#define MEMCOLB		 17
#define PAGEROW		 2	/* uses 4 rows and 26 cols */
#define PAGECOL		45
#define INTSROW		 6	/* uses all rows to bottom and 17 cols */
#define INTSCOL		61
#define PROCSROW	 7	/* uses 2 rows and 20 cols */
#define PROCSCOL	 0
#define GENSTATROW	 7	/* uses 2 rows and 30 cols */
#define GENSTATCOL	16
#define VMSTATROW	 6	/* uses 17 rows and 12 cols */
#define VMSTATCOL	50
#define GRAPHROW	10	/* uses 3 rows and 51 cols */
#define GRAPHCOL	 0
#define NAMEIROW	14	/* uses 3 rows and 38 cols */
#define NAMEICOL	 0
#define EXECROW		14	/* uses 2 rows and 5 cols */
#define EXECCOL		38
#define DISKROW		17	/* uses 6 rows and 50 cols (for 9 drives) */
#define DISKCOL		 0

#define	DRIVESPACE	 7	/* max # for space */

#define	MAXDRIVES	DRIVESPACE	 /* max # to display */

static
int
findintralias(const char *name, int limit)
{
	int i;
	size_t nlen;
	size_t ilen;

	nlen = strlen(name);
	for (i = 0; i < limit; ++i) {
		if (strcmp(name, intrname[i]) == 0)
			break;
		ilen = strlen(intrname[i]);
		if (nlen == ilen &&
		    nlen > 1 &&
		    strncmp(name, intrname[i], nlen - 1) == 0 &&
		    strchr(name, ' ') &&
		    isdigit(name[nlen - 1]) &&
		    (isdigit(intrname[i][nlen - 1]) ||
		     intrname[i][nlen - 1] == '*')) {
			intrname[i][nlen - 1] = '*';
			break;
		}
	}
	return i;
}

int
initkre(void)
{
	char *intrnamebuf;
	size_t bytes;
	size_t b;
	size_t i;

	if (namelist[0].n_type == 0) {
		if (kvm_nlist(kd, namelist)) {
			nlisterr(namelist);
			return(0);
		}
		if (namelist[0].n_type == 0) {
			error("No namelist");
			return(0);
		}
	}

	if ((num_devices = getnumdevs()) < 0) {
		warnx("%s", devstat_errbuf);
		return(0);
	}

	cur.dinfo = (struct devinfo *)malloc(sizeof(struct devinfo));
	last.dinfo = (struct devinfo *)malloc(sizeof(struct devinfo));
	run.dinfo = (struct devinfo *)malloc(sizeof(struct devinfo));
	bzero(cur.dinfo, sizeof(struct devinfo));
	bzero(last.dinfo, sizeof(struct devinfo));
	bzero(run.dinfo, sizeof(struct devinfo));

	if (dsinit(MAXDRIVES, &cur, &last, &run) != 1)
		return(0);

	if (nintr == 0) {
		if (sysctlbyname("hw.intrnames", NULL, &bytes, NULL, 0) == 0) {
			intrnamebuf = malloc(bytes);
			sysctlbyname("hw.intrnames", intrnamebuf, &bytes,
					NULL, 0);
			for (i = 0; i < bytes; ++i) {
				if (intrnamebuf[i] == 0)
					++nintr;
			}
			intrname = malloc(nintr * sizeof(char *));
			intrloc = malloc(nintr * sizeof(*intrloc));
			lacc = malloc(nintr * sizeof(*lacc));
			intralias = malloc(nintr * sizeof(*intralias));
			intrsmp = malloc(nintr * sizeof(*intrsmp));
			bzero(intrsmp, nintr * sizeof(*intrsmp));

			nintr = 0;
			for (b = i = 0; i < bytes; ++i) {
				if (intrnamebuf[i] == 0) {
					intrname[nintr] = intrnamebuf + b;
					intrloc[nintr] = 0;
					intralias[nintr] =
					  findintralias(intrname[nintr], nintr);
					++intrsmp[intralias[nintr]];
					b = i + 1;
					++nintr;
				}
			}
		}
		nextintsrow = INTSROW + 2;
		allocinfo(&s);
		allocinfo(&s1);
		allocinfo(&s2);
		allocinfo(&z);
	}
	getinfo(&s2);
	copyinfo(&s2, &s1);
	return(1);
}

void
fetchkre(void)
{
	time_t now;
	struct tm *tp;
	static int d_first = -1;

	if (d_first < 0)
		d_first = (*nl_langinfo(D_MD_ORDER) == 'd');

	time(&now);
	tp = localtime(&now);
	(void) strftime(buf, sizeof(buf),
			d_first ? "%e %b %R" : "%b %e %R", tp);
	getinfo(&s);
}

void
labelkre(void)
{
	int i, j;

	clear();
	mvprintw(STATROW, STATCOL + 4, "users    Load");
	mvprintw(MEMROW + 0, MEMCOLA, "Active ");
	mvprintw(MEMROW + 1, MEMCOLA, "Kernel ");
	mvprintw(MEMROW + 2, MEMCOLA, "Free   ");
	mvprintw(MEMROW + 3, MEMCOLA, "Total  ");

	mvprintw(MEMROW + 2, MEMCOLA + 14, "i+c+f");

	mvprintw(MEMROW + 0, MEMCOLB, "PMAP");
	mvprintw(MEMROW + 0, MEMCOLB + 13, "VMRSS");
	mvprintw(MEMROW + 1, MEMCOLB, "SWAP");
	mvprintw(MEMROW + 1, MEMCOLB + 13, "SWTOT");

	mvprintw(PAGEROW, PAGECOL,     "       VNODE PAGER    SWAP PAGER ");
	mvprintw(PAGEROW + 1, PAGECOL, "          in   out      in   out ");
	mvprintw(PAGEROW + 2, PAGECOL, "bytes");
	mvprintw(PAGEROW + 3, PAGECOL, "count");

	mvprintw(INTSROW, INTSCOL + 3, " Interrupts");
	mvprintw(INTSROW + 1, INTSCOL + 9, "total");

	mvprintw(VMSTATROW + 1, VMSTATCOL + 8, "cow");
	mvprintw(VMSTATROW + 2, VMSTATCOL + 8, "wire");
	mvprintw(VMSTATROW + 3, VMSTATCOL + 8, "act");
	mvprintw(VMSTATROW + 4, VMSTATCOL + 8, "inact");
	mvprintw(VMSTATROW + 5, VMSTATCOL + 8, "cache");
	mvprintw(VMSTATROW + 6, VMSTATCOL + 8, "free");
	mvprintw(VMSTATROW + 7, VMSTATCOL + 8, "daefr");
	mvprintw(VMSTATROW + 8, VMSTATCOL + 8, "prcfr");
	mvprintw(VMSTATROW + 9, VMSTATCOL + 8, "react");
	mvprintw(VMSTATROW + 10, VMSTATCOL + 8, "pdwake");
	mvprintw(VMSTATROW + 11, VMSTATCOL + 8, "pdpgs");
	mvprintw(VMSTATROW + 12, VMSTATCOL + 8, "intrn");
	mvprintw(VMSTATROW + 13, VMSTATCOL + 8, "buf");
	mvprintw(VMSTATROW + 14, VMSTATCOL + 8, "dirtybuf");

	mvprintw(VMSTATROW + 15, VMSTATCOL + 8, "activ-vp");
	mvprintw(VMSTATROW + 16, VMSTATCOL + 8, "cachd-vp");
	mvprintw(VMSTATROW + 17, VMSTATCOL + 8, "inact-vp");

	mvprintw(GENSTATROW, GENSTATCOL, "  Csw  Trp  Sys  Int  Sof  Flt");

	mvprintw(GRAPHROW, GRAPHCOL,
		"  . %%Sys    . %%Intr   . %%User   . %%Nice   . %%Idle");
	mvprintw(PROCSROW, PROCSCOL, "   r   p   d   s");
	mvprintw(GRAPHROW + 1, GRAPHCOL,
		"|    |    |    |    |    |    |    |    |    |    |");

	mvprintw(NAMEIROW, NAMEICOL, "Path-lookups   hits   %%    Components");
	mvprintw(EXECROW, EXECCOL, "Execs");
	mvprintw(DISKROW, DISKCOL, "Disks");
	mvprintw(DISKROW + 1, DISKCOL, "KB/t");
	mvprintw(DISKROW + 2, DISKCOL, "tpr/s");
	mvprintw(DISKROW + 3, DISKCOL, "MBr/s");
	mvprintw(DISKROW + 4, DISKCOL, "tpw/s");
	mvprintw(DISKROW + 5, DISKCOL, "MBw/s");
	mvprintw(DISKROW + 6, DISKCOL, "%% busy");

	/*
	 * For now, we don't support a fourth disk statistic.  So there's
	 * no point in providing a label for it.  If someone can think of a
	 * fourth useful disk statistic, there is room to add it.
	 */
	j = 0;
	for (i = 0; i < num_devices && j < MAXDRIVES; i++)
		if (dev_select[i].selected) {
			char tmpstr[80];
			sprintf(tmpstr, "%s%d", dev_select[i].device_name,
				dev_select[i].unit_number);
			mvprintw(DISKROW, DISKCOL + 5 + 6 * j,
				" %5.5s", tmpstr);
			j++;
		}

	if (j <= 4) {
		/*
		 * room for extended VM stats
		 */
		mvprintw(VMSTATROW + 11, VMSTATCOL - 6, "nzfod");
		mvprintw(VMSTATROW + 12, VMSTATCOL - 6, "ozfod");
		mvprintw(VMSTATROW + 13, VMSTATCOL - 6, "%%zslo");
		mvprintw(VMSTATROW + 14, VMSTATCOL - 6, "pgfre");
		extended_vm_stats = 1;
	} else {
		extended_vm_stats = 0;
		mvprintw(VMSTATROW + 0, VMSTATCOL + 8, "zfod");
	}

	for (i = 0; i < nintr; i++) {
		if (intrloc[i] == 0)
			continue;
		mvprintw(intrloc[i], INTSCOL + 9, "%-10.10s", intrname[i]);
	}
}

#define CP_UPDATE(fld)	do {	\
	uint64_t lt;		\
	lt=s.fld;		\
	s.fld-=s1.fld;		\
	if(state==TIME)		\
		s1.fld=lt;	\
	lt=fld;			\
	fld-=old_##fld;		\
	if(state==TIME)		\
		old_##fld=lt;	\
	etime += s.fld;		\
} while(0)
#define X(fld)	{t=s.fld[i]; s.fld[i]-=s1.fld[i]; if(state==TIME) s1.fld[i]=t;}
#define Y(fld)	{t = s.fld; s.fld -= s1.fld; if(state == TIME) s1.fld = t;}
#define Z(fld)	{t = s.nchstats.fld; s.nchstats.fld -= s1.nchstats.fld; \
	if(state == TIME) s1.nchstats.fld = t;}
#define PUTRATE(fld, l, c, w) \
	Y(fld); \
	put64((int64_t)((float)s.fld/etime + 0.5), l, c, w, 'D')
#define PUTRATE_PGTOB(fld, l, c, w) \
	Y(fld); \
	put64((int64_t)((float)s.fld/etime + 0.5) * PAGE_SIZE, l, c, w, 0)
#define MAXFAIL 5

#define CPUSTATES 5
static	const char cpuchar[5] = { '=' , '+', '>', '-', ' ' };

static	const size_t cpuoffsets[] = {
	offsetof(struct kinfo_cputime, cp_sys),
	offsetof(struct kinfo_cputime, cp_intr),
	offsetof(struct kinfo_cputime, cp_user),
	offsetof(struct kinfo_cputime, cp_nice),
	offsetof(struct kinfo_cputime, cp_idle)
};

void
showkre(void)
{
	float f1, f2;
	int psiz;
	int i, j, lc;
	long inttotal;
	long l;
	static int failcnt = 0;
	double total_time;

	etime = 0;
	CP_UPDATE(cp_time.cp_user);
	CP_UPDATE(cp_time.cp_nice);
	CP_UPDATE(cp_time.cp_sys);
	CP_UPDATE(cp_time.cp_intr);
	CP_UPDATE(cp_time.cp_idle);

	total_time = etime;
	if (total_time == 0.0)
		total_time = 1.0;

	if (etime < 100000.0) {	/* < 100ms ignore this trash */
		if (failcnt++ >= MAXFAIL) {
			clear();
			mvprintw(2, 10, "The alternate system clock has died!");
			mvprintw(3, 10, "Reverting to ``pigs'' display.");
			move(CMDLINE, 0);
			refresh();
			failcnt = 0;
			sleep(5);
			command("pigs");
		}
		return;
	}
	failcnt = 0;
	etime /= 1000000.0;
	etime /= ncpu;
	if (etime == 0)
		etime = 1;
	inttotal = 0;
	bzero(lacc, nintr * sizeof(*lacc));

	for (i = 0; i < nintr; i++) {
		if (s.intrcnt[i] == 0)
			continue;
		j = intralias[i];
		if (intrloc[j] == 0) {
			if (nextintsrow == LINES)
				continue;
			intrloc[j] = nextintsrow++;
			mvprintw(intrloc[j], INTSCOL + 9, "%-10.10s",
				intrname[j]);
		}
		X(intrcnt);
		l = (long)((float)s.intrcnt[i]/etime + 0.5);
		lacc[j] += l;
		inttotal += l;
		put64(lacc[j], intrloc[j], INTSCOL + 3, 5, 'D');
	}
	put64(inttotal, INTSROW + 1, INTSCOL + 3, 5, 'D');
	Z(ncs_goodhits); Z(ncs_badhits); Z(ncs_miss);
	Z(ncs_longhits); Z(ncs_longmiss); Z(ncs_neghits);
	s.nchcount = nchtotal.ncs_goodhits + nchtotal.ncs_badhits +
	    nchtotal.ncs_miss + nchtotal.ncs_neghits;
	s.nchpathcount = nchtotal.ncs_longhits + nchtotal.ncs_longmiss;
	if (state == TIME) {
		s1.nchcount = s.nchcount;
		s1.nchpathcount = s.nchpathcount;
	}

#define LOADCOLS	49	/* Don't but into the 'free' value */
#define LOADRANGE	(100.0 / LOADCOLS)

	psiz = 0;
	f2 = 0.0;
	for (lc = 0; lc < CPUSTATES; lc++) {
		uint64_t val = *(uint64_t *)(((uint8_t *)&s.cp_time) +
			       cpuoffsets[lc]);
		f1 = 100.0 * val / total_time;
		f2 += f1;
		l = (int)((f2 + (LOADRANGE / 2.0)) / LOADRANGE) - psiz;
		if (f1 > 99.9)
			f1 = 99.9;	/* no room to display 100.0 */
		putfloat(f1, GRAPHROW, GRAPHCOL + 10 * lc, 4, 1, 0);
		move(GRAPHROW + 2, psiz);
		psiz += l;
		while (l-- > 0)
			addch(cpuchar[lc]);
	}

	put64(ucount(), STATROW, STATCOL, 3, 'D');
	putfloat(avenrun[0], STATROW, STATCOL + 18, 6, 2, 0);
	putfloat(avenrun[1], STATROW, STATCOL + 25, 6, 2, 0);
	putfloat(avenrun[2], STATROW, STATCOL + 32, 6, 2, 0);
	mvaddstr(STATROW, STATCOL + 53, buf);
#define pgtokb(pg) (int64_t)((intmax_t)(pg) * vms.v_page_size / 1024)
#define pgtomb(pg) (int64_t)((intmax_t)(pg) * vms.v_page_size / (1024 * 1024))
#define pgtob(pg)  (int64_t)((intmax_t)(pg) * vms.v_page_size)

	put64(pgtob(vms.v_active_count), MEMROW + 0, MEMCOLA + 7, 6, 0);
	put64(pgtob(vms.v_wire_count), MEMROW + 1, MEMCOLA + 7, 6, 0); /*XXX*/
	put64(pgtob(vms.v_inactive_count +
		    vms.v_cache_count +
		    vms.v_free_count), MEMROW + 2, MEMCOLA + 7, 6, 0);
	put64(s.physmem, MEMROW + 3, MEMCOLA + 7, 6, 0);
	put64(pgtob(total.t_arm),
			MEMROW + 0, MEMCOLB + 5, 6, 0);
	put64(pgtob(total.t_avm + total.t_avmshr),
			MEMROW + 0, MEMCOLB + 19, 6, 0);
	put64(pgtob(total.t_vm - total.t_rm),
			MEMROW + 1, MEMCOLB + 5, 6, 0);
	put64(pgtob(s.kvmsw[kvnsw].ksw_total),
			MEMROW + 1, MEMCOLB + 19, 6, 0);

#if 0
	put64(pgtob(total.t_arm), MEMROW + 2, MEMCOL + 4, 6, 0);
	put64(pgtob(total.t_armshr), MEMROW + 2, MEMCOL + 11, 6, 0);
	put64(pgtob(total.t_avm), MEMROW + 2, MEMCOL + 19, 6, 0);
	put64(pgtob(total.t_avmshr), MEMROW + 2, MEMCOL + 26, 6, 0);
	put64(pgtob(total.t_rm), MEMROW + 3, MEMCOL + 4, 6, 0);
	put64(pgtob(total.t_rmshr), MEMROW + 3, MEMCOL + 11, 6, 0);
	put64(pgtob(total.t_vm), MEMROW + 3, MEMCOL + 19, 6, 0);
	put64(pgtob(total.t_vmshr), MEMROW + 3, MEMCOL + 26, 6, 0);
	put64(pgtob(total.t_free), MEMROW + 2, MEMCOL + 34, 6, 0);
#endif

	put64(total.t_rq - 1, PROCSROW + 1, PROCSCOL + 0, 4, 'D');
	put64(total.t_pw, PROCSROW + 1, PROCSCOL + 4, 4, 'D');
	put64(total.t_dw, PROCSROW + 1, PROCSCOL + 8, 4, 'D');
	put64(total.t_sl, PROCSROW + 1, PROCSCOL + 12, 4, 'D');
	/*put64(total.t_sw, PROCSROW + 1, PROCSCOL + 12, 3, 'D');*/
	if (extended_vm_stats == 0) {
		PUTRATE_PGTOB(Vmm.v_zfod, VMSTATROW + 0, VMSTATCOL, 7);
	}
	PUTRATE_PGTOB(Vmm.v_cow_faults, VMSTATROW + 1, VMSTATCOL, 7);
	put64(pgtob(vms.v_wire_count), VMSTATROW + 2, VMSTATCOL, 7, 0);
	put64(pgtob(vms.v_active_count), VMSTATROW + 3, VMSTATCOL, 7, 0);
	put64(pgtob(vms.v_inactive_count), VMSTATROW + 4, VMSTATCOL, 7, 0);
	put64(pgtob(vms.v_cache_count), VMSTATROW + 5, VMSTATCOL, 7, 0);
	put64(pgtob(vms.v_free_count), VMSTATROW + 6, VMSTATCOL, 7, 0);
	PUTRATE(Vmm.v_dfree, VMSTATROW + 7, VMSTATCOL, 7);
	PUTRATE(Vmm.v_pfree, VMSTATROW + 8, VMSTATCOL, 7);
	PUTRATE(Vmm.v_reactivated, VMSTATROW + 9, VMSTATCOL, 7);
	PUTRATE(Vmm.v_pdwakeups, VMSTATROW + 10, VMSTATCOL, 7);
	PUTRATE(Vmm.v_pdpages, VMSTATROW + 11, VMSTATCOL, 7);
	PUTRATE(Vmm.v_intrans, VMSTATROW + 12, VMSTATCOL, 7);

	if (extended_vm_stats) {
		int64_t orig_zfod = s.Vmm.v_zfod;
		s.Vmm.v_zfod -= s.Vmm.v_ozfod;
		PUTRATE_PGTOB(Vmm.v_zfod, VMSTATROW + 11, VMSTATCOL - 14, 7);
		PUTRATE_PGTOB(Vmm.v_ozfod, VMSTATROW + 12, VMSTATCOL - 14, 7);
#define nz(x)	((x) ? (x) : 1)
		put64((s.Vmm.v_zfod) * 100 / nz(orig_zfod),
		    VMSTATROW + 13, VMSTATCOL - 14, 7, 'D');
#undef nz
		PUTRATE_PGTOB(Vmm.v_tfree, VMSTATROW + 14, VMSTATCOL - 14, 7);
	}

	put64(s.bufspace, VMSTATROW + 13, VMSTATCOL, 7, 0);
	put64(s.dirtybufspace/1024, VMSTATROW + 14, VMSTATCOL, 7, 'K');
	put64(s.activevnodes, VMSTATROW + 15, VMSTATCOL, 7, 'D');
	put64(s.cachedvnodes, VMSTATROW + 16, VMSTATCOL, 7, 'D');
	put64(s.inactivevnodes, VMSTATROW + 17, VMSTATCOL, 7, 'D');
	PUTRATE_PGTOB(Vmm.v_vnodepgsin, PAGEROW + 2, PAGECOL + 7, 5);
	PUTRATE_PGTOB(Vmm.v_vnodepgsout, PAGEROW + 2, PAGECOL + 13, 5);
	PUTRATE_PGTOB(Vmm.v_swappgsin, PAGEROW + 2, PAGECOL + 21, 5);
	PUTRATE_PGTOB(Vmm.v_swappgsout, PAGEROW + 2, PAGECOL + 27, 5);
	PUTRATE(Vmm.v_vnodein, PAGEROW + 3, PAGECOL + 7, 5);
	PUTRATE(Vmm.v_vnodeout, PAGEROW + 3, PAGECOL + 13, 5);
	PUTRATE(Vmm.v_swapin, PAGEROW + 3, PAGECOL + 21, 5);
	PUTRATE(Vmm.v_swapout, PAGEROW + 3, PAGECOL + 27, 5);
	PUTRATE(Vmm.v_swtch, GENSTATROW + 1, GENSTATCOL + 1, 4);
	PUTRATE(Vmm.v_trap, GENSTATROW + 1, GENSTATCOL + 6, 4);
	PUTRATE(Vmm.v_syscall, GENSTATROW + 1, GENSTATCOL + 11, 4);
	PUTRATE(Vmm.v_intr, GENSTATROW + 1, GENSTATCOL + 16, 4);
	PUTRATE(Vmm.v_soft, GENSTATROW + 1, GENSTATCOL + 21, 4);
	PUTRATE(Vmm.v_vm_faults, GENSTATROW + 1, GENSTATCOL + 26, 4);
	mvprintw(DISKROW, DISKCOL + 5, "                              ");
	for (i = 0, lc = 0; i < num_devices && lc < MAXDRIVES; i++)
		if (dev_select[i].selected) {
			char tmpstr[80];
			sprintf(tmpstr, "%s%d", dev_select[i].device_name,
				dev_select[i].unit_number);
			mvprintw(DISKROW, DISKCOL + 5 + 6 * lc,
				" %5.5s", tmpstr);
			switch(state) {
			case TIME:
				dinfo(i, ++lc, &cur, &last);
				break;
			case RUN:
				dinfo(i, ++lc, &cur, &run);
				break;
			case BOOT:
				dinfo(i, ++lc, &cur, NULL);
				break;
			}
		}
#define nz(x)	((x) ? (x) : 1)
	put64(s.nchpathcount, NAMEIROW + 1, NAMEICOL + 6, 6, 'D');
	PUTRATE(Vmm.v_exec, EXECROW + 1, EXECCOL, 5);
	put64(nchtotal.ncs_longhits, NAMEIROW + 1, NAMEICOL + 13, 6, 'D');
	putfloat(nchtotal.ncs_longhits * 100.0 / nz(s.nchpathcount),
	    NAMEIROW + 1, NAMEICOL + 19, 4, 0, 0);

	putfloat((double)s.nchcount / nz(s.nchpathcount),
	    NAMEIROW + 1, NAMEICOL + 27, 5, 2, 1);
#undef nz
}

int
cmdkre(const char *cmd, char *args)
{
	int retval;

	if (prefix(cmd, "run")) {
		retval = 1;
		copyinfo(&s2, &s1);
		switch (getdevs(&run)) {
		case -1:
			errx(1, "%s", devstat_errbuf);
			break;
		case 1:
			num_devices = run.dinfo->numdevs;
			generation = run.dinfo->generation;
			retval = dscmd("refresh", NULL, MAXDRIVES, &cur);
			if (retval == 2)
				labelkre();
			break;
		default:
			break;
		}
		state = RUN;
		return (retval);
	}
	if (prefix(cmd, "boot")) {
		state = BOOT;
		copyinfo(&z, &s1);
		return (1);
	}
	if (prefix(cmd, "time")) {
		state = TIME;
		return (1);
	}
	if (prefix(cmd, "zero")) {
		retval = 1;
		if (state == RUN) {
			getinfo(&s1);
			switch (getdevs(&run)) {
			case -1:
				errx(1, "%s", devstat_errbuf);
				break;
			case 1:
				num_devices = run.dinfo->numdevs;
				generation = run.dinfo->generation;
				retval = dscmd("refresh",NULL, MAXDRIVES, &cur);
				if (retval == 2)
					labelkre();
				break;
			default:
				break;
			}
		}
		return (retval);
	}
	retval = dscmd(cmd, args, MAXDRIVES, &cur);

	if (retval == 2)
		labelkre();

	return(retval);
}

/* calculate number of users on the system */
static int
ucount(void)
{
	struct utmpentry *ep = NULL;	/* avoid gcc warnings */
	int nusers = 0;

	getutentries(NULL, &ep);
	for (; ep; ep = ep->next)
		nusers++;

	return (nusers);
}

static void
put64(intmax_t n, int l, int lc, int w, int type)
{
	char b[128];
	int isneg;
	int i;
	int64_t d;
	int64_t u;

	move(l, lc);
	if (n == 0) {
		while (w-- > 0)
			addch(' ');
		return;
	}
	if (type == 0 || type == 'D')
		snprintf(b, sizeof(b), "%*jd", w, n);
	else
		snprintf(b, sizeof(b), "%*jd%c", w - 1, n, type);
	if (strlen(b) <= (size_t)w) {
		addstr(b);
		return;
	}

	if (type == 'D')
		u = 1000;
	else
		u = 1024;
	if (n < 0) {
		n = -n;
		isneg = 1;
	} else {
		isneg = 0;
	}

	for (d = 1; n / d >= 1000; d *= u) {
		switch(type) {
		case 'D':
		case 0:
			type = 'K';
			break;
		case 'K':
			type = 'M';
			break;
		case 'M':
			type = 'G';
			break;
		case 'G':
			type = 'T';
			break;
		case 'T':
			type = 'X';
			break;
		default:
			type = '?';
			break;
		}
	}

	i = w - isneg;
	if (n / d >= 100)
		i -= 3;
	else if (n / d >= 10)
		i -= 2;
	else
		i -= 1;
	if (i > 4) {
		snprintf(b + 64, sizeof(b) - 64, "%jd.%03jd%c",
			 n / d, n / (d / 1000) % 1000, type);
	} else if (i > 3) {
		snprintf(b + 64, sizeof(b) - 64, "%jd.%02jd%c",
			 n / d, n / (d / 100) % 100, type);
	} else if (i > 2) {
		snprintf(b + 64, sizeof(b) - 64, "%jd.%01jd%c",
			 n / d, n / (d / 10) % 10, type);
	} else {
		snprintf(b + 64, sizeof(b) - 64, "%jd%c",
			 n / d, type);
	}
	w -= strlen(b + 64);
	i = 64;
	if (isneg) {
		b[--i] = '-';
		--w;
	}
	while (w > 0) {
		--w;
		b[--i] = ' ';
	}
	addstr(b + i);
}

static void
putfloat(double f, int l, int lc, int w, int d, int nz)
{
	char b[128];

	move(l, lc);
	if (nz && f == 0.0) {
		while (--w >= 0)
			addch(' ');
		return;
	}
	snprintf(b, sizeof(b), "%*.*f", w, d, f);
	if (strlen(b) > (size_t)w)
		snprintf(b, sizeof(b), "%*.0f", w, f);
	if (strlen(b) > (size_t)w) {
		while (--w >= 0)
			addch('*');
		return;
	}
	addstr(b);
}

static void
putlongdouble(long double f, int l, int lc, int w, int d, int nz)
{
	char b[128];

	move(l, lc);
	if (nz && f == 0.0) {
		while (--w >= 0)
			addch(' ');
		return;
	}
	sprintf(b, "%*.*Lf", w, d, f);
	if (strlen(b) > (size_t)w)
		sprintf(b, "%*.0Lf", w, f);
	if (strlen(b) > (size_t)w) {
		while (--w >= 0)
			addch('*');
		return;
	}
	addstr(b);
}

static void
putlongdoublez(long double f, int l, int lc, int w, int d, int nz)
{
	char b[128];

	if (f == 0.0) {
		move(l, lc);
		sprintf(b, "%*.*s", w, w, "");
		addstr(b);
	} else {
		putlongdouble(f, l, lc, w, d, nz);
	}
}

static void
getinfo(struct Info *ls)
{
	struct devinfo *tmp_dinfo;
	struct nchstats *nch_tmp;
	size_t size;
	size_t vms_size = sizeof(ls->Vms);
	size_t vmm_size = sizeof(ls->Vmm);
	size_t nch_size = sizeof(ls->nchstats) * SMP_MAXCPU;
	size_t phys_size = sizeof(ls->physmem);

        kvnsw = kvm_getswapinfo(kd, ls->kvmsw, NKVMSW, 0);

        if (sysctlbyname("vm.vmstats", &ls->Vms, &vms_size, NULL, 0)) {
                perror("sysctlbyname: vm.vmstats");
                exit(1);
        }
        if (sysctlbyname("vm.vmmeter", &ls->Vmm, &vmm_size, NULL, 0)) {
                perror("sysctlbyname: vm.vmstats");
                exit(1);
        }
        if (sysctlbyname("hw.physmem", &ls->physmem, &phys_size, NULL, 0)) {
                perror("sysctlbyname: hw.physmem");
                exit(1);
	}

	if (kinfo_get_sched_cputime(&ls->cp_time))
		err(1, "kinfo_get_sched_cputime");
	if (kinfo_get_sched_cputime(&cp_time))
		err(1, "kinfo_get_sched_cputime");
	NREAD(X_BUFFERSPACE, &ls->bufspace, sizeof(ls->bufspace));
	NREAD(X_DESIREDVNODES, &ls->maxvnodes, sizeof(ls->maxvnodes));
	NREAD(X_CACHEDVNODES, &ls->cachedvnodes, sizeof(ls->cachedvnodes));
	NREAD(X_INACTIVEVNODES, &ls->inactivevnodes,
						sizeof(ls->inactivevnodes));
	NREAD(X_ACTIVEVNODES, &ls->activevnodes, sizeof(ls->activevnodes));
	NREAD(X_NUMDIRTYBUFFERS, &ls->dirtybufspace, sizeof(ls->dirtybufspace));

	if (nintr) {
		size = nintr * sizeof(ls->intrcnt[0]);
		sysctlbyname("hw.intrcnt_all", ls->intrcnt, &size, NULL, 0);
	}
	size = sizeof(ls->Total);
	if (sysctlbyname("vm.vmtotal", &ls->Total, &size, NULL, 0) < 0) {
		error("Can't get kernel info: %s\n", strerror(errno));
		bzero(&ls->Total, sizeof(ls->Total));
	}

	if ((nch_tmp = malloc(nch_size)) == NULL) {
		perror("malloc");
		exit(1);
	} else {
		if (sysctlbyname("vfs.cache.nchstats", nch_tmp, &nch_size, NULL, 0)) {
			perror("sysctlbyname vfs.cache.nchstats");
			free(nch_tmp);
			exit(1);
		} else {
			if ((nch_tmp = realloc(nch_tmp, nch_size)) == NULL) {
				perror("realloc");
				exit(1);
			}
		}
	}

	if (kinfo_get_cpus(&ncpu))
		err(1, "kinfo_get_cpus");
	kvm_nch_cpuagg(nch_tmp, &ls->nchstats, ncpu);
	free(nch_tmp);

	tmp_dinfo = last.dinfo;
	last.dinfo = cur.dinfo;
	cur.dinfo = tmp_dinfo;

	last.busy_time = cur.busy_time;
	switch (getdevs(&cur)) {
	case -1:
		errx(1, "%s", devstat_errbuf);
		break;
	case 1:
		num_devices = cur.dinfo->numdevs;
		generation = cur.dinfo->generation;
		cmdkre("refresh", NULL);
		break;
	default:
		break;
	}
}

static void
allocinfo(struct Info *ls)
{
	ls->intrcnt = (long *) calloc(nintr, sizeof(long));
	if (ls->intrcnt == NULL)
		errx(2, "out of memory");
}

static void
copyinfo(struct Info *from, struct Info *to)
{
	long *intrcnt;

	/*
	 * time, wds, seek, and xfer are malloc'd so we have to
	 * save the pointers before the structure copy and then
	 * copy by hand.
	 */
	intrcnt = to->intrcnt;
	*to = *from;

	bcopy(from->intrcnt, to->intrcnt = intrcnt, nintr * sizeof (int));
}

static void
dinfo(int dn, int lc, struct statinfo *now, struct statinfo *then)
{
	long double kb_per_transfer;
	long double transfers_per_secondr;
	long double transfers_per_secondw;
	long double mb_per_secondr;
	long double mb_per_secondw;
	long double elapsed_time, device_busy;
	int di;

	di = dev_select[dn].position;

	elapsed_time = compute_etime(now->busy_time, then ?
				     then->busy_time :
				     now->dinfo->devices[di].dev_creation_time);

	device_busy =  compute_etime(now->dinfo->devices[di].busy_time, then ?
				     then->dinfo->devices[di].busy_time :
				     now->dinfo->devices[di].dev_creation_time);

	if (compute_stats(
			  &now->dinfo->devices[di],
			  (then ? &then->dinfo->devices[di] : NULL),
			  elapsed_time,
			  NULL, NULL, NULL,
			  &kb_per_transfer,
			  NULL,
			  NULL,
			  NULL, NULL) != 0)
		errx(1, "%s", devstat_errbuf);

	if (compute_stats_read(
			  &now->dinfo->devices[di],
			  (then ? &then->dinfo->devices[di] : NULL),
			  elapsed_time,
			  NULL, NULL, NULL,
			  NULL,
			  &transfers_per_secondr,
			  &mb_per_secondr,
			  NULL, NULL) != 0)
		errx(1, "%s", devstat_errbuf);

	if (compute_stats_write(
			  &now->dinfo->devices[di],
			  (then ? &then->dinfo->devices[di] : NULL),
			  elapsed_time,
			  NULL, NULL, NULL,
			  NULL,
			  &transfers_per_secondw,
			  &mb_per_secondw,
			  NULL, NULL) != 0)
		errx(1, "%s", devstat_errbuf);

#if 0
	/*
	 * Remove this hack, it no longer works properly and will
	 * report 100% busy in situations where the device is able
	 * to respond to the requests faster than the busy counter's
	 * granularity.
	 */
	if ((device_busy == 0) &&
	    (transfers_per_secondr > 5 || transfers_per_secondw > 5)) {
		/* the device has been 100% busy, fake it because
		 * as long as the device is 100% busy the busy_time
		 * field in the devstat struct is not updated */
		device_busy = elapsed_time;
	}
#endif
	if (device_busy > elapsed_time) {
		/* this normally happens after one or more periods
		 * where the device has been 100% busy, correct it */
		device_busy = elapsed_time;
	}

	lc = DISKCOL + lc * 6;
	putlongdoublez(kb_per_transfer, DISKROW + 1, lc, 5, 2, 0);
	putlongdoublez(transfers_per_secondr, DISKROW + 2, lc, 5, 0, 0);
	putlongdoublez(mb_per_secondr, DISKROW + 3, lc, 5, 2, 0);
	putlongdoublez(transfers_per_secondw, DISKROW + 4, lc, 5, 0, 0);
	putlongdoublez(mb_per_secondw, DISKROW + 5, lc, 5, 2, 0);
	putlongdouble(device_busy * 100 / elapsed_time,
				      DISKROW + 6, lc, 5, 0, 0);
}
