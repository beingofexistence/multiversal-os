/*
 * Copyright (c) 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Rick Macklem at The University of Guelph.
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
 * @(#) Copyright (c) 1989, 1993 The Regents of the University of California.  All rights reserved.
 * @(#)nfsiod.c	8.4 (Berkeley) 5/3/95
 * $FreeBSD: src/sbin/nfsiod/nfsiod.c,v 1.9 1999/08/28 00:13:55 peter Exp $
 */

#include <sys/param.h>
#include <sys/syslog.h>
#include <sys/wait.h>
#include <sys/mount.h>

#include <vfs/nfs/rpcv2.h>
#include <vfs/nfs/nfs.h>

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Global defs */
#ifdef DEBUG
int debug = 1;
#else
int debug = 0;
#endif

static void	nonfs(int);
static void	reapchild(int);
static void	usage(void);

/*
 * Nfsiod does asynchronous buffered I/O on behalf of the NFS client.
 * It does not have to be running for correct operation, but will
 * improve throughput.
 */
int
main(int argc, char **argv)
{
	int ch, num_servers;
	struct vfsconf vfc;
	int error;

	error = getvfsbyname("nfs", &vfc);
	if (error && vfsisloadable("nfs")) {
		if (vfsload("nfs"))
			err(1, "vfsload(nfs)");
		endvfsent();	/* flush cache */
		error = getvfsbyname("nfs", &vfc);
	}
	if(error)
		errx(1, "NFS support is not available in the running kernel");

#define	MAXNFSDCNT      20
#define	DEFNFSDCNT       1
	num_servers = DEFNFSDCNT;
	while ((ch = getopt(argc, argv, "n:")) != -1)
		switch (ch) {
		case 'n':
			num_servers = atoi(optarg);
			if (num_servers < 1 || num_servers > MAXNFSDCNT) {
				warnx("nfsiod count %d; reset to %d",
				    num_servers, DEFNFSDCNT);
				num_servers = DEFNFSDCNT;
			}
			break;
		default:
			usage();
		}
	argc -= optind;
	argv += optind;

	/*
	 * XXX
	 * Backward compatibility, trailing number is the count of daemons.
	 */
	if (argc > 1)
		usage();
	if (argc == 1) {
		num_servers = atoi(argv[0]);
		if (num_servers < 1 || num_servers > MAXNFSDCNT) {
			warnx("nfsiod count %d; reset to %d", num_servers,
			    DEFNFSDCNT);
			num_servers = DEFNFSDCNT;
		}
	}

	if (debug == 0) {
		daemon(0, 0);
		signal(SIGHUP, SIG_IGN);
		signal(SIGINT, SIG_IGN);
		signal(SIGQUIT, SIG_IGN);
		signal(SIGSYS, nonfs);
	}
	signal(SIGCHLD, reapchild);

	openlog("nfsiod:", LOG_PID, LOG_DAEMON);

	while (num_servers--)
		switch (fork()) {
		case -1:
			syslog(LOG_ERR, "fork: %m");
			exit (1);
		case 0:
			if (nfssvc(NFSSVC_BIOD, NULL) < 0) {
				syslog(LOG_ERR, "nfssvc: %m");
				exit (1);
			}
			exit(0);
		}
	exit (0);
}

static void
nonfs(int signo __unused)
{
	syslog(LOG_ERR, "missing system call: NFS not available");
}

static void
reapchild(int signo __unused)
{

	while (wait3(NULL, WNOHANG, NULL) > 0)
		;
}

static void
usage(void)
{
	fprintf(stderr, "usage: nfsiod [-n num_servers]\n");
	exit(1);
}
