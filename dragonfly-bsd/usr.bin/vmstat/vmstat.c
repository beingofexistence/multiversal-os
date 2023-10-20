/*
 * Copyright (c) 1980, 1986, 1991, 1993
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
 * @(#) Copyright (c) 1980, 1986, 1991, 1993 The Regents of the University of California.  All rights reserved.
 * @(#)vmstat.c	8.1 (Berkeley) 6/6/93
 * $FreeBSD: src/usr.bin/vmstat/vmstat.c,v 1.38.2.4 2001/07/31 19:52:41 tmm Exp $
 */

#include <sys/user.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/namei.h>
#include <sys/objcache.h>
#include <sys/signal.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <sys/vmmeter.h>
#include <sys/interrupt.h>

#include <vm/vm_param.h>
#include <vm/vm_zone.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <kinfo.h>
#include <kvm.h>
#include <limits.h>
#include <nlist.h>
#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>
#include <devstat.h>

static struct nlist namelist[] = {
#define	X_BOOTTIME	0
	{ "_boottime",	0, 0, 0, 0 },
#define X_NCHSTATS	1
	{ "_nchstats",	0, 0, 0, 0 },
#define	X_KMEMSTATISTICS 2
	{ "_kmemstatistics",	0, 0, 0, 0 },
#define	X_NCPUS		3
	{ "_ncpus",	0, 0, 0, 0 },
#define	X_ZLIST		4
	{ "_zlist",	0, 0, 0, 0 },
#define	X_KSLAB_DUMMY	5
	{ "_kslab_dummy", 0, 0, 0, 0 },
#ifdef notyet
#define	X_DEFICIT	6
	{ "_deficit",	0, 0, 0, 0 },
#define	X_FORKSTAT	7
	{ "_forkstat",	0, 0, 0, 0 },
#define X_REC		8
	{ "_rectime",	0, 0, 0, 0 },
#define X_PGIN		9
	{ "_pgintime",	0, 0, 0, 0 },
#define	X_XSTATS	10
	{ "_xstats",	0, 0, 0, 0 },
#define X_END		11
#else
#define X_END		6
#endif
	{ "", 0, 0, 0, 0 },
};

#define ONEMB	(1024L * 1024L)
#define ONEKB	(1024L)

LIST_HEAD(zlist, vm_zone);

struct statinfo cur, last;
int num_devices, maxshowdevs;
long generation;
struct device_selection *dev_select;
int num_selected;
struct devstat_match *matches;
int num_matches = 0;
int num_devices_specified, num_selections;
long select_generation;
char **specified_devices;
devstat_select_mode select_mode;

struct	vmmeter vmm, ovmm;
struct	vmstats vms, ovms;

int	winlines = 20;
int	nflag = 0;
int	mflag = 0;
int	verbose = 0;
int	unformatted_opt = 0;
int	brief_opt = 0;
int	ncpus;

kvm_t *kd;

struct kinfo_cputime cp_time, old_cp_time, diff_cp_time;

#define	FORKSTAT	0x01
#define	INTRSTAT	0x02
#define	MEMSTAT		0x04
#define	SUMSTAT		0x08
#define	TIMESTAT	0x10
#define	VMSTAT		0x20
#define ZMEMSTAT	0x40
#define OCSTAT		0x80

static void cpustats(void);
static void dointr(void);
static void domem(void);
static void dooc(void);
static void dosum(void);
static void dozmem(u_int interval, int reps);
static void dovmstat(u_int, int);
static void kread(int, void *, size_t);
static void usage(void);
static char **getdrivedata(char **);
static long getuptime(void);
static void needhdr(int);
static long pct(long, long);

#ifdef notyet
static void dotimes(void); /* Not implemented */
static void doforkst(void);
#endif
static void printhdr(void);
static const char *formatnum(intmax_t value, int width, int do10s);
static void devstats(int dooutput);

int
main(int argc, char **argv)
{
	int c, todo;
	u_int interval;		/* milliseconds */
	int reps;
	char *memf, *nlistf;
	char errbuf[_POSIX2_LINE_MAX];

	memf = nlistf = NULL;
	interval = reps = todo = 0;
	maxshowdevs = 2;
	while ((c = getopt(argc, argv, "bc:fiM:mN:n:op:stuvw:z")) != -1) {
		switch (c) {
		case 'b':
			brief_opt = 1;
			break;
		case 'c':
			reps = atoi(optarg);
			break;
		case 'f':
#ifdef notyet
			todo |= FORKSTAT;
#else
			errx(EX_USAGE, "sorry, -f is not (re)implemented yet");
#endif
			break;
		case 'i':
			todo |= INTRSTAT;
			break;
		case 'M':
			memf = optarg;
			break;
		case 'm':
			++mflag;
			todo |= MEMSTAT;
			break;
		case 'N':
			nlistf = optarg;
			break;
		case 'n':
			nflag = 1;
			maxshowdevs = atoi(optarg);
			if (maxshowdevs < 0)
				errx(1, "number of devices %d is < 0",
				     maxshowdevs);
			break;
		case 'o':
			todo |= OCSTAT;
			break;
		case 'p':
			if (buildmatch(optarg, &matches, &num_matches) != 0)
				errx(1, "%s", devstat_errbuf);
			break;
		case 's':
			todo |= SUMSTAT;
			break;
		case 't':
#ifdef notyet
			todo |= TIMESTAT;
#else
			errx(EX_USAGE, "sorry, -t is not (re)implemented yet");
#endif
			break;
		case 'u':
			unformatted_opt = 1;
			break;
		case 'v':
			++verbose;
			break;
		case 'w':
			interval = (u_int)(strtod(optarg, NULL) * 1000.0);
			break;
		case 'z':
			todo |= ZMEMSTAT;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (todo == 0)
		todo = VMSTAT;

	/*
	 * Discard setgid privileges if not the running kernel so that bad
	 * guys can't print interesting stuff from kernel memory.
	 */
	if (nlistf != NULL || memf != NULL) {
		setgid(getgid());
		if (todo & OCSTAT) {
			errx(1, "objcache stats can only be gathered on "
			    "the running system");
		}
	}

	kd = kvm_openfiles(nlistf, memf, NULL, O_RDONLY, errbuf);
	if (kd == NULL)
		errx(1, "kvm_openfiles: %s", errbuf);

	if ((c = kvm_nlist(kd, namelist)) != 0) {
		if (c > 0) {
			warnx("undefined symbols:");
			for (c = 0; c < (int)NELEM(namelist); c++)
				if (namelist[c].n_type == 0)
					fprintf(stderr, " %s",
					    namelist[c].n_name);
			fputc('\n', stderr);
		} else
			warnx("kvm_nlist: %s", kvm_geterr(kd));
		exit(1);
	}

	kread(X_NCPUS, &ncpus, sizeof(ncpus));

	if (todo & VMSTAT) {
		struct winsize winsize;

		/*
		 * Make sure that the userland devstat version matches the
		 * kernel devstat version.  If not, exit and print a
		 * message informing the user of his mistake.
		 */
		if (checkversion() < 0)
			errx(1, "%s", devstat_errbuf);


		argv = getdrivedata(argv);
		winsize.ws_row = 0;
		ioctl(STDOUT_FILENO, TIOCGWINSZ, (char *)&winsize);
		if (winsize.ws_row > 0)
			winlines = winsize.ws_row;

	}

#define	BACKWARD_COMPATIBILITY
#ifdef	BACKWARD_COMPATIBILITY
	if (*argv) {
		interval = (u_int)(strtod(*argv, NULL) * 1000.0);
		if (*++argv)
			reps = atoi(*argv);
	}
#endif

	if (interval) {
		if (!reps)
			reps = -1;
	} else if (reps) {
		interval = 1000;
	}

#ifdef notyet
	if (todo & FORKSTAT)
		doforkst();
#endif
	if (todo & MEMSTAT)
		domem();
	if (todo & ZMEMSTAT)
		dozmem(interval, reps);
	if (todo & SUMSTAT)
		dosum();
#ifdef notyet
	if (todo & TIMESTAT)
		dotimes();
#endif
	if (todo & INTRSTAT)
		dointr();
	if (todo & VMSTAT)
		dovmstat(interval, reps);
	if (todo & OCSTAT)
		dooc();
	exit(0);
}

static char **
getdrivedata(char **argv)
{
	if ((num_devices = getnumdevs()) < 0)
		errx(1, "%s", devstat_errbuf);

	cur.dinfo = (struct devinfo *)malloc(sizeof(struct devinfo));
	last.dinfo = (struct devinfo *)malloc(sizeof(struct devinfo));
	bzero(cur.dinfo, sizeof(struct devinfo));
	bzero(last.dinfo, sizeof(struct devinfo));

	if (getdevs(&cur) == -1)
		errx(1, "%s", devstat_errbuf);

	num_devices = cur.dinfo->numdevs;
	generation = cur.dinfo->generation;

	specified_devices = (char **)malloc(sizeof(char *));
	for (num_devices_specified = 0; *argv; ++argv) {
		if (isdigit(**argv))
			break;
		num_devices_specified++;
		specified_devices = (char **)realloc(specified_devices,
						     sizeof(char *) *
						     num_devices_specified);
		specified_devices[num_devices_specified - 1] = *argv;
	}
	dev_select = NULL;

	if (nflag == 0 && maxshowdevs < num_devices_specified)
			maxshowdevs = num_devices_specified;

	/*
	 * People are generally only interested in disk statistics when
	 * they're running vmstat.  So, that's what we're going to give
	 * them if they don't specify anything by default.  We'll also give
	 * them any other random devices in the system so that we get to
	 * maxshowdevs devices, if that many devices exist.  If the user
	 * specifies devices on the command line, either through a pattern
	 * match or by naming them explicitly, we will give the user only
	 * those devices.
	 */
	if ((num_devices_specified == 0) && (num_matches == 0)) {
		if (buildmatch("da", &matches, &num_matches) != 0)
			errx(1, "%s", devstat_errbuf);

		select_mode = DS_SELECT_ADD;
	} else
		select_mode = DS_SELECT_ONLY;

	/*
	 * At this point, selectdevs will almost surely indicate that the
	 * device list has changed, so we don't look for return values of 0
	 * or 1.  If we get back -1, though, there is an error.
	 */
	if (selectdevs(&dev_select, &num_selected, &num_selections,
		       &select_generation, generation, cur.dinfo->devices,
		       num_devices, matches, num_matches, specified_devices,
		       num_devices_specified, select_mode,
		       maxshowdevs, 0) == -1)
		errx(1, "%s", devstat_errbuf);

	return(argv);
}

static long
getuptime(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_UPTIME, &ts);

	return ts.tv_sec;
}

int	hdrcnt;

static void
dovmstat(u_int interval, int reps)
{
	struct vmtotal total;
	struct devinfo *tmp_dinfo;
	size_t vmm_size = sizeof(vmm);
	size_t vms_size = sizeof(vms);
	size_t vmt_size = sizeof(total);
	int initial = 1;
	int dooutput = 1;

	signal(SIGCONT, needhdr);
	if (reps != 0)
		dooutput = 0;

	for (hdrcnt = 1;;) {
		if (!--hdrcnt)
			printhdr();
		if (kinfo_get_sched_cputime(&cp_time))
			err(1, "kinfo_get_sched_cputime");

		tmp_dinfo = last.dinfo;
		last.dinfo = cur.dinfo;
		cur.dinfo = tmp_dinfo;
		last.busy_time = cur.busy_time;

		/*
		 * Here what we want to do is refresh our device stats.
		 * getdevs() returns 1 when the device list has changed.
		 * If the device list has changed, we want to go through
		 * the selection process again, in case a device that we
		 * were previously displaying has gone away.
		 */
		switch (getdevs(&cur)) {
		case -1:
			errx(1, "%s", devstat_errbuf);
			break;
		case 1: {
			int retval;

			num_devices = cur.dinfo->numdevs;
			generation = cur.dinfo->generation;

			retval = selectdevs(&dev_select, &num_selected,
					    &num_selections, &select_generation,
					    generation, cur.dinfo->devices,
					    num_devices, matches, num_matches,
					    specified_devices,
					    num_devices_specified, select_mode,
					    maxshowdevs, 0);
			switch (retval) {
			case -1:
				errx(1, "%s", devstat_errbuf);
				break;
			case 1:
				printhdr();
				break;
			default:
				break;
			}
		}
		default:
			break;
		}

		if (sysctlbyname("vm.vmstats", &vms, &vms_size, NULL, 0)) {
			perror("sysctlbyname: vm.vmstats");
			exit(1);
		}
		if (sysctlbyname("vm.vmmeter", &vmm, &vmm_size, NULL, 0)) {
			perror("sysctlbyname: vm.vmmeter");
			exit(1);
		}
		if (sysctlbyname("vm.vmtotal", &total, &vmt_size, NULL, 0)) {
			perror("sysctlbyname: vm.vmtotal");
			exit(1);
		}

		/*
		 * Be a little inventive so we can squeeze everything into
		 * 80 columns.  These days the run queue can trivially be
		 * into the three digits and under heavy paging loads the
		 * blocked (d+p) count can as well.
		 */
		if (dooutput) {
			char b1[4];
			char b2[4];
			char b3[2];

			strcpy(b1, "***");
			strcpy(b2, "***");
			strcpy(b3, "*");
			if (total.t_rq - 1 < 1000) {
				snprintf(b1, sizeof(b1),
					 "%3ld", total.t_rq - 1);
			}
			if (total.t_dw + total.t_pw < 1000) {
				snprintf(b2, sizeof(b2),
					 "%3ld", total.t_dw + total.t_pw);
			}
			if (total.t_sw < 10) {
				snprintf(b3, sizeof(b3), "%ld", total.t_sw);
			}
			printf("%s %s %s", b1, b2, b3);
		}

#define rate(x)		\
	(intmax_t)(initial ? (x) : ((intmax_t)(x) * 1000 + interval / 2) \
				   / interval)

		if (dooutput) {
			printf(" %s ",
			       formatnum((int64_t)total.t_free *
					 vms.v_page_size,
					 5, 1));
			printf("%s ",
			       formatnum(rate(vmm.v_vm_faults -
					      ovmm.v_vm_faults),
					 5, 1));
			printf("%s ",
			       formatnum(rate((vmm.v_reactivated -
					      ovmm.v_reactivated) *
					      vms.v_page_size),
					 4, 1));
			printf("%s ",
			       formatnum(rate((vmm.v_swappgsin +
					       vmm.v_vnodepgsin -
					       ovmm.v_swappgsin -
					       ovmm.v_vnodepgsin) *
					      vms.v_page_size),
					 4, 1));
			printf("%s ",
			       formatnum(rate((vmm.v_swappgsout +
					       vmm.v_vnodepgsout -
					       ovmm.v_swappgsout -
					       ovmm.v_vnodepgsout) *
					      vms.v_page_size),
					 4, 1));
			printf("%s ",
			       formatnum(rate((vmm.v_tfree - ovmm.v_tfree) *
					      vms.v_page_size), 4, 1));
		}
		devstats(dooutput);
		if (dooutput) {
			printf("%s ",
			       formatnum(rate(vmm.v_intr - ovmm.v_intr),
					 5, 1));
			printf("%s ",
			       formatnum(rate(vmm.v_syscall -
					      ovmm.v_syscall),
					 5, 1));
			printf("%s ",
			       formatnum(rate(vmm.v_swtch -
					      ovmm.v_swtch),
					 5, 1));
			cpustats();
			printf("\n");
			fflush(stdout);
		}
		if (reps >= 0 && --reps <= 0)
			break;
		ovmm = vmm;
		usleep(interval * 1000);
		initial = 0;
		dooutput = 1;
	}
}

static const char *
formatnum(intmax_t value, int width, int do10s)
{
	static char buf[16][64];
	static int bi;
	const char *fmt;
	double d;

	if (brief_opt)
		do10s = 0;

	bi = (bi + 1) % 16;

	if (unformatted_opt) {
		switch(width) {
		case 4:
			snprintf(buf[bi], sizeof(buf[bi]), "%4jd", value);
			break;
		case 5:
			snprintf(buf[bi], sizeof(buf[bi]), "%5jd", value);
			break;
		default:
			snprintf(buf[bi], sizeof(buf[bi]), "%jd", value);
			break;
		}
		return buf[bi];
	}

	d = (double)value;
	fmt = "n/a";

	switch(width) {
	case 4:
		if (value < 1024) {
			fmt = "%4.0f";
		} else if (value < 10*1024) {
			fmt = "%3.1fK";
			d = d / 1024;
		} else if (value < 1000*1024) {
			fmt = "%3.0fK";
			d = d / 1024;
		} else if (value < 10*1024*1024) {
			fmt = "%3.1fM";
			d = d / (1024 * 1024);
		} else if (value < 1000*1024*1024) {
			fmt = "%3.0fM";
			d = d / (1024 * 1024);
		} else {
			fmt = "%3.1fG";
			d = d / (1024.0 * 1024.0 * 1024.0);
		}
		break;
	case 5:
		if (value < 1024) {
			fmt = "%5.0f";
		} else if (value < 10*1024) {
			fmt = "%4.2fK";
			d = d / 1024;
		} else if (value < 100*1024 && do10s) {
			fmt = "%4.1fK";
			d = d / 1024;
		} else if (value < 1000*1024) {
			fmt = "%4.0fK";
			d = d / 1024;
		} else if (value < 10*1024*1024) {
			fmt = "%4.2fM";
			d = d / (1024 * 1024);
		} else if (value < 100*1024*1024 && do10s) {
			fmt = "%4.1fM";
			d = d / (1024 * 1024);
		} else if (value < 1000*1024*1024) {
			fmt = "%4.0fM";
			d = d / (1024 * 1024);
		} else if (value < 10LL*1024*1024*1024) {
			fmt = "%4.2fG";
			d = d / (1024.0 * 1024.0 * 1024.0);
		} else if (value < 100LL*1024*1024*1024 && do10s) {
			fmt = "%4.1fG";
			d = d / (1024.0 * 1024.0 * 1024.0);
		} else if (value < 1000LL*1024*1024*1024) {
			fmt = "%4.0fG";
			d = d / (1024.0 * 1024.0 * 1024.0);
		} else {
			fmt = "%4.2fT";
			d = d / (1024.0 * 1024.0 * 1024.0 * 1024.0);
		}
		break;
	default:
		fprintf(stderr, "formatnum: unsupported width %d\n", width);
		exit(1);
		break;
	}
	snprintf(buf[bi], sizeof(buf[bi]), fmt, d);
	return buf[bi];
}

static void
printhdr(void)
{
	int i, num_shown;

	num_shown = (num_selected < maxshowdevs) ? num_selected : maxshowdevs;
	printf("--procs-- ---memory-- -------paging------ ");
	if (num_shown > 1)
		printf("--disks%.*s",
		       num_shown * 4 - 6,
		       "---------------------------------");
	else if (num_shown == 1)
		printf("disk");
	printf(" -----faults------ ---cpu---\n");
	printf("  r   b w   fre   flt   re   pi   po   fr ");
	for (i = 0; i < num_devices; i++)
		if ((dev_select[i].selected)
		 && (dev_select[i].selected <= maxshowdevs))
			printf(" %c%c%d ", dev_select[i].device_name[0],
				     dev_select[i].device_name[1],
				     dev_select[i].unit_number);
	printf("  int   sys   ctx us sy id\n");
	hdrcnt = winlines - 2;
}

/*
 * Force a header to be prepended to the next output.
 */
static void
needhdr(__unused int signo)
{

	hdrcnt = 1;
}

static long
pct(long top, long bot)
{
	long ans;

	if (bot == 0)
		return(0);
	ans = (quad_t)top * 100 / bot;
	return (ans);
}

#define	PCT(top, bot) pct((long)(top), (long)(bot))

static void
dosum(void)
{
	struct nchstats *nch_tmp, nchstats;
	size_t vms_size = sizeof(vms);
	size_t vmm_size = sizeof(vmm);
	int cpucnt;
	u_long nchtotal;
	u_long nchpathtotal;
	size_t nch_size = sizeof(struct nchstats) * SMP_MAXCPU;

	if (sysctlbyname("vm.vmstats", &vms, &vms_size, NULL, 0)) {
		perror("sysctlbyname: vm.vmstats");
		exit(1);
	}
	if (sysctlbyname("vm.vmmeter", &vmm, &vmm_size, NULL, 0)) {
		perror("sysctlbyname: vm.vmstats");
		exit(1);
	}
	printf("%9u cpu context switches\n", vmm.v_swtch);
	printf("%9u device interrupts\n", vmm.v_intr);
	printf("%9u software interrupts\n", vmm.v_soft);
	printf("%9u traps\n", vmm.v_trap);
	printf("%9u system calls\n", vmm.v_syscall);
	printf("%9u kernel threads created\n", vmm.v_kthreads);
	printf("%9u  fork() calls\n", vmm.v_forks);
	printf("%9u vfork() calls\n", vmm.v_vforks);
	printf("%9u rfork() calls\n", vmm.v_rforks);
	printf("%9u exec() calls\n", vmm.v_exec);
	printf("%9u swap pager pageins\n", vmm.v_swapin);
	printf("%9u swap pager pages paged in\n", vmm.v_swappgsin);
	printf("%9u swap pager pageouts\n", vmm.v_swapout);
	printf("%9u swap pager pages paged out\n", vmm.v_swappgsout);
	printf("%9u vnode pager pageins\n", vmm.v_vnodein);
	printf("%9u vnode pager pages paged in\n", vmm.v_vnodepgsin);
	printf("%9u vnode pager pageouts\n", vmm.v_vnodeout);
	printf("%9u vnode pager pages paged out\n", vmm.v_vnodepgsout);
	printf("%9u page daemon wakeups\n", vmm.v_pdwakeups);
	printf("%9u pages examined by the page daemon\n", vmm.v_pdpages);
	printf("%9u pages reactivated\n", vmm.v_reactivated);
	printf("%9u copy-on-write faults\n", vmm.v_cow_faults);
	printf("%9u copy-on-write optimized faults\n", vmm.v_cow_optim);
	printf("%9u zero fill pages zeroed\n", vmm.v_zfod);
	printf("%9u zero fill pages prezeroed\n", vmm.v_ozfod);
	printf("%9u intransit blocking page faults\n", vmm.v_intrans);
	printf("%9u total VM faults taken\n", vmm.v_vm_faults);
	printf("%9u pages affected by kernel thread creation\n", vmm.v_kthreadpages);
	printf("%9u pages affected by  fork()\n", vmm.v_forkpages);
	printf("%9u pages affected by vfork()\n", vmm.v_vforkpages);
	printf("%9u pages affected by rfork()\n", vmm.v_rforkpages);
	printf("%9u pages freed\n", vmm.v_tfree);
	printf("%9u pages freed by daemon\n", vmm.v_dfree);
	printf("%9u pages freed by exiting processes\n", vmm.v_pfree);
	printf("%9lu pages active\n", vms.v_active_count);
	printf("%9lu pages inactive\n", vms.v_inactive_count);
	printf("%9lu pages in VM cache\n", vms.v_cache_count);
	printf("%9lu pages wired down\n", vms.v_wire_count);
	printf("%9lu pages free\n", vms.v_free_count);
	printf("%9u bytes per page\n", vms.v_page_size);
	printf("%9u global smp invltlbs\n", vmm.v_smpinvltlb);

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

	cpucnt = nch_size / sizeof(struct nchstats);
	kvm_nch_cpuagg(nch_tmp, &nchstats, cpucnt);

	nchtotal = nchstats.ncs_goodhits + nchstats.ncs_neghits +
	    nchstats.ncs_badhits + nchstats.ncs_falsehits +
	    nchstats.ncs_miss;
	nchpathtotal = nchstats.ncs_longhits + nchstats.ncs_longmiss;
	printf("%9ld total path lookups\n", nchpathtotal);
	printf("%9ld total component lookups\n", nchtotal);
	printf(
	    "%9s cache hits (%ld%% pos + %ld%% neg)\n",
	    "", PCT(nchstats.ncs_goodhits, nchtotal),
	    PCT(nchstats.ncs_neghits, nchtotal));
	printf("%9s deletions %ld%%, falsehits %ld%%\n", "",
	    PCT(nchstats.ncs_badhits, nchtotal),
	    PCT(nchstats.ncs_falsehits, nchtotal));
	free(nch_tmp);
}

#ifdef notyet
void
doforkst(void)
{
	struct forkstat fks;

	kread(X_FORKSTAT, &fks, sizeof(struct forkstat));
	printf("%d forks, %d pages, average %.2f\n",
	    fks.cntfork, fks.sizfork, (double)fks.sizfork / fks.cntfork);
	printf("%d vforks, %d pages, average %.2f\n",
	    fks.cntvfork, fks.sizvfork, (double)fks.sizvfork / fks.cntvfork);
}
#endif

static void
devstats(int dooutput)
{
	int dn;
	long double transfers_per_second;
	long double busy_seconds;

	diff_cp_time.cp_user = cp_time.cp_user - old_cp_time.cp_user;
	diff_cp_time.cp_nice = cp_time.cp_nice - old_cp_time.cp_nice;
	diff_cp_time.cp_sys = cp_time.cp_sys - old_cp_time.cp_sys;
	diff_cp_time.cp_intr = cp_time.cp_intr - old_cp_time.cp_intr;
	diff_cp_time.cp_idle = cp_time.cp_idle - old_cp_time.cp_idle;
	old_cp_time = cp_time;

	busy_seconds = compute_etime(cur.busy_time, last.busy_time);

	for (dn = 0; dn < num_devices; dn++) {
		int di;

		if ((dev_select[dn].selected == 0)
		 || (dev_select[dn].selected > maxshowdevs))
			continue;

		di = dev_select[dn].position;

		if (compute_stats(&cur.dinfo->devices[di],
				  &last.dinfo->devices[di], busy_seconds,
				  NULL, NULL, NULL,
				  NULL, &transfers_per_second, NULL,
				  NULL, NULL) != 0)
			errx(1, "%s", devstat_errbuf);

		if (dooutput)
			printf("%s ", formatnum(transfers_per_second, 4, 0));
	}
}

static void
cpustats(void)
{
	uint64_t total;
	double totusage;

	total = diff_cp_time.cp_user + diff_cp_time.cp_nice +
	    diff_cp_time.cp_sys + diff_cp_time.cp_intr + diff_cp_time.cp_idle;

	if (total)
		totusage = 100.0 / total;
	else
		totusage = 0;
	printf("%2.0f ",
	       (diff_cp_time.cp_user + diff_cp_time.cp_nice) * totusage);
	printf("%2.0f ",
	       (diff_cp_time.cp_sys + diff_cp_time.cp_intr) * totusage);
	printf("%2.0f",
	       diff_cp_time.cp_idle * totusage);
}

static void
dointr(void)
{
	u_long *intrcnt, uptime;
	u_int64_t inttotal;
	size_t nintr, inamlen, i, size;
	int nwidth;
	char *intrstr;
	char **intrname;

	uptime = getuptime();
	if (sysctlbyname("hw.intrnames", NULL, &inamlen, NULL, 0) != 0)
		errx(1, "sysctlbyname");
	intrstr = malloc(inamlen);
	if (intrstr == NULL)
		err(1, "malloc");
	sysctlbyname("hw.intrnames", intrstr, &inamlen, NULL, 0);
	for (nintr = 0, i = 0; i < inamlen; ++i) {
		if (intrstr[i] == 0)
			nintr++;
	}
	intrname = malloc(nintr * sizeof(char *));
	for (i = 0; i < nintr; ++i) {
		intrname[i] = intrstr;
		intrstr += strlen(intrstr) + 1;
	}

	size = nintr * sizeof(*intrcnt);
	intrcnt = calloc(nintr, sizeof(*intrcnt));
	if (intrcnt == NULL)
		err(1, "malloc");
	sysctlbyname("hw.intrcnt", intrcnt, &size, NULL, 0);

	nwidth = 21;
	for (i = 0; i < nintr; ++i) {
		if (nwidth < (int)strlen(intrname[i]))
			nwidth = (int)strlen(intrname[i]);
	}
	if (verbose) nwidth += 12;

	printf("%-*.*s %11s %10s\n",
		nwidth, nwidth, "interrupt", "total", "rate");
	inttotal = 0;
	for (i = 0; i < nintr; ++i) {
		int named;
		char *infop, irqinfo[72];

		if ((named = strncmp(intrname[i], "irq", 3)) != 0 ||
		    intrcnt[i] > 0) {
			infop = intrname[i];
			if (verbose) {
				ssize_t irq, cpu;

				irq = i % MAX_INTS;
				cpu = i / MAX_INTS;
				if (named) {
					snprintf(irqinfo, sizeof(irqinfo),
						 "irq%-3zd %3zd: %s",
						 irq, cpu, intrname[i]);
				} else {
					snprintf(irqinfo, sizeof(irqinfo),
						 "irq%-3zd %3zd: ", irq, cpu);
				}
				infop = irqinfo;
			}
			printf("%-*.*s %11lu %10lu\n",
				nwidth, nwidth, infop,
				intrcnt[i], intrcnt[i] / uptime);
		}
		inttotal += intrcnt[i];
	}
	printf("%-*.*s %11llu %10llu\n",
		nwidth, nwidth, "Total",
		(long long)inttotal, (long long)(inttotal / uptime));
}

#define	MAX_KMSTATS	16384

enum ksuse { KSINUSE, KSMEMUSE, KSOBJUSE, KSCALLS };

static long
cpuagg(const struct malloc_type *ks, enum ksuse use)
{
    int i;
    long ttl;

    ttl = 0;

    switch(use) {
    case KSINUSE:
	for (i = 0; i < ncpus; ++i)
	    ttl += ks->ks_use[i].inuse;
	break;
    case KSMEMUSE:
	for (i = 0; i < ncpus; ++i)
	    ttl += ks->ks_use[i].memuse;
	break;
    case KSOBJUSE:
	ttl = (ks->ks_mgt.npartial + ks->ks_mgt.nfull + ks->ks_mgt.nempty) *
	      KMALLOC_SLAB_SIZE;
	for (i = 0; i < ncpus; ++i) {
	    struct kmalloc_use *kuse = &ks->ks_use[i];

	    if (kuse->mgt.active &&
		kuse->mgt.active != (void *)namelist[X_KSLAB_DUMMY].n_value)
	    {
		ttl += KMALLOC_SLAB_SIZE;
	    }
	    if (kuse->mgt.alternate &&
		kuse->mgt.alternate != (void *)namelist[X_KSLAB_DUMMY].n_value)
	    {
		ttl += KMALLOC_SLAB_SIZE;
	    }
	}
	break;
    case KSCALLS:
	for (i = 0; i < ncpus; ++i)
	    ttl += ks->ks_use[i].calls;
    	break;
    }
    return(ttl);
}

static int
mcompare(const void *arg1, const void *arg2)
{
	const struct malloc_type *m1 = arg1;
	const struct malloc_type *m2 = arg2;
	long total1;
	long total2;

	total1 = cpuagg(m1, KSMEMUSE) + cpuagg(m1, KSOBJUSE);
	total2 = cpuagg(m2, KSMEMUSE) + cpuagg(m2, KSOBJUSE);
	if (total1 < total2)
		return -1;
	if (total1 > total2)
		return 1;
	return 0;
}

static void
domem(void)
{
	struct malloc_type *ks;
	int nkms;
	int i;
	int n;
	long totuse = 0;
	long totreq = 0;
	long totobj = 0;
	struct malloc_type kmemstats[MAX_KMSTATS], *kmsp;
	char buf[1024];

	/*
	 * Collect
	 */
	kread(X_KMEMSTATISTICS, &kmsp, sizeof(kmsp));
	for (nkms = 0; nkms < MAX_KMSTATS && kmsp != NULL; nkms++) {
		struct malloc_type *ss;

		ss = &kmemstats[nkms];

		if (sizeof(kmemstats[0]) != kvm_read(kd, (u_long)kmsp, ss,
						     sizeof(kmemstats[0])))
		{
			err(1, "kvm_read(%p)", (void *)kmsp);
		}
		if (sizeof(buf) != kvm_read(kd, (u_long)ss->ks_shortdesc,
					    buf, sizeof(buf)))
		{
			err(1, "kvm_read(%p)", kmemstats[nkms].ks_shortdesc);
		}
		buf[sizeof(buf) - 1] = '\0';
		ss->ks_shortdesc = strdup(buf);

		if (ss->ks_use) {
			size_t usebytes;
			void *use;

			usebytes = ncpus * sizeof(ss->ks_use[0]);
			use = malloc(usebytes);
			if (kvm_read(kd, (u_long)ss->ks_use, use, usebytes) !=
			    (ssize_t)usebytes)
			{
				err(1, "kvm_read(%p)", ss->ks_use);
			}
			ss->ks_use = use;
		}
		kmsp = ss->ks_next;
	}
	if (kmsp != NULL)
		warnx("truncated to the first %d memory types", nkms);

	/*
	 * Sort (-mm)
	 */
	if (mflag > 1) {
		qsort(kmemstats, nkms, sizeof(struct malloc_type), mcompare);
	}

	/*
	 * Dump output
	 */
	printf(
	    "\nMemory statistics by type\n");
	printf("\t       Type   Count  MemUse SlabUse   Limit Requests\n");
	for (i = 0, ks = &kmemstats[0]; i < nkms; i++, ks++) {
		long ks_inuse;
		long ks_memuse;
		long ks_objuse;
		long ks_calls;
		char idbuf[64];

		ks_calls = cpuagg(ks, KSCALLS);
		if (ks_calls == 0 && verbose == 0)
			continue;

		ks_inuse = cpuagg(ks, KSINUSE);
		ks_memuse = cpuagg(ks, KSMEMUSE);
		ks_objuse = cpuagg(ks, KSOBJUSE);

		snprintf(idbuf, sizeof(idbuf), "%s", ks->ks_shortdesc);
		for (n = 0; idbuf[n]; ++n) {
			if (idbuf[n] == ' ')
				idbuf[n] = '_';
		}

		printf("%19s   %s   %s   %s   %s    %s\n",
			idbuf,
			formatnum(ks_inuse, 5, 1),
			formatnum(ks_memuse, 5, 1),
			formatnum(ks_objuse, 5, 1),
			formatnum(ks->ks_limit, 5, 1),
			formatnum(ks_calls, 5, 1));

		totuse += ks_memuse;
		totobj += ks_objuse;
		totreq += ks_calls;
	}
	printf("\nMemory Totals:  In-Use  Slab-Use Requests\n");
	printf("                 %s  %s   %s\n",
		formatnum(totuse, 5, 1),
		formatnum(totobj, 5, 1),
		formatnum(totreq, 5, 1));
}

static void
dooc(void)
{
	struct objcache_stats *stat, *s;
	size_t len, count;

	if (sysctlbyname("kern.objcache.stats", NULL, &len, NULL, 0) < 0)
		errx(1, "objcache stats sysctl failed\n");

	/* Add some extra space. */
	stat = malloc(len + (8 * sizeof(*stat)));
	if (sysctlbyname("kern.objcache.stats", stat, &len, NULL, 0) < 0)
		errx(1, "objcache stats sysctl failed\n");

	printf(
	    "\nObjcache statistics by name\n");
	printf("                 Name    Used  Cached   Limit Requests  Allocs Fails  Exhausts\n");
	for (s = stat, count = 0; count < len; ++s) {
		printf("%21s   %s   %s   %s    %s   %s  %s  %s\n",
		    s->oc_name,
		    formatnum(s->oc_used, 5, 1),
		    formatnum(s->oc_cached, 5, 1),
		    s->oc_limit < OBJCACHE_UNLIMITED ?
		    formatnum(s->oc_limit, 5, 1) : "unlim",
		    formatnum(s->oc_requested, 5, 1),
		    formatnum(s->oc_allocated, 5, 1),
		    formatnum(s->oc_failed, 4, 1),
		    formatnum(s->oc_exhausted, 4, 1));

		count += sizeof(*s);
	}
	free(stat);
}

#define MAXSAVE	16

static void
dozmem(u_int interval, int reps)
{
	struct zlist	zlist;
	struct vm_zone	*kz;
	struct vm_zone	zone;
	struct vm_zone	save[MAXSAVE];
	long zfreecnt_prev;
	long znalloc_prev;
	long zfreecnt_next;
	long znalloc_next;
	char name[64];
	size_t namesz;
	int first = 1;
	int i;
	int n;

	bzero(save, sizeof(save));

again:
	kread(X_ZLIST, &zlist, sizeof(zlist));
	kz = LIST_FIRST(&zlist);
	i = 0;

	while (kz) {
		if (kvm_read(kd, (intptr_t)kz, &zone, sizeof(zone)) !=
		    (ssize_t)sizeof(zone)) {
			perror("kvm_read");
			break;
		}
		zfreecnt_prev = save[i].zfreecnt;
		znalloc_prev = save[i].znalloc;
		for (n = 0; n < ncpus; ++n) {
			zfreecnt_prev += save[i].zpcpu[n].zfreecnt;
			znalloc_prev += save[i].zpcpu[n].znalloc;
		}

		zfreecnt_next = zone.zfreecnt;
		znalloc_next = zone.znalloc;
		for (n = 0; n < ncpus; ++n) {
			zfreecnt_next += zone.zpcpu[n].zfreecnt;
			znalloc_next += zone.zpcpu[n].znalloc;
		}
		save[i] = zone;

		namesz = sizeof(name);
		if (kvm_readstr(kd, (intptr_t)zone.zname, name, &namesz) == NULL) {
			perror("kvm_read");
			break;
		}
		if (first && interval) {
			/* do nothing */
		} else if (zone.zmax) {
			printf("%-10s %9ld / %-9ld %5ldM used"
			       " %6.2f%% ",
				name,
				(long)(zone.ztotal - zfreecnt_next),
				(long)zone.zmax,
				(long)zone.zpagecount * 4096 / (1024 * 1024),
				(double)(zone.ztotal - zfreecnt_next) *
					100.0 / (double)zone.zmax);
		} else {
			printf("%-10s %9ld             %5ldM used"
			       "         ",
				name,
				(long)(zone.ztotal - zfreecnt_next),
				(long)(zone.ztotal - zfreecnt_next) *
					zone.zsize / (1024 * 1024));
		}
		if (first == 0) {
			printf("use=%ld\n", znalloc_next - znalloc_prev);
		} else if (interval == 0)
			printf("\n");

		kz = LIST_NEXT(&zone, zlink);
		++i;
	}
	if (reps) {
		first = 0;
		fflush(stdout);
		usleep(interval * 1000);
		--reps;
		printf("\n");
		goto again;
	}
}

/*
 * kread reads something from the kernel, given its nlist index.
 */
static void
kread(int nlx, void *addr, size_t size)
{
	const char *sym;

	if (namelist[nlx].n_type == 0 || namelist[nlx].n_value == 0) {
		sym = namelist[nlx].n_name;
		if (*sym == '_')
			++sym;
		errx(1, "symbol %s not defined", sym);
	}
	if (kvm_read(kd, namelist[nlx].n_value, addr, size) != (ssize_t)size) {
		sym = namelist[nlx].n_name;
		if (*sym == '_')
			++sym;
		errx(1, "%s: %s", sym, kvm_geterr(kd));
	}
}

static void
usage(void)
{
	fprintf(stderr, "%s%s",
		"usage: vmstat [-imsuvz] [-c count] [-M core] "
		"[-N system] [-w wait]\n",
		"              [-n devs] [disks]\n");
	exit(1);
}
