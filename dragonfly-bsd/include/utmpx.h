/*-
 * Copyright (c) 2002 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Christos Zoulas.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef	_UTMPX_H_
#define	_UTMPX_H_

#include <sys/cdefs.h>
#include <sys/socket.h>
#include <sys/time.h>

#ifndef _PID_T_DECLARED
typedef	__pid_t		pid_t;		/* process id */
#define	_PID_T_DECLARED
#endif

#define	_PATH_UTMPX		"/var/run/utmpx"
#define	_PATH_WTMPX		"/var/log/wtmpx"
#define	_PATH_LASTLOGX		"/var/log/lastlogx"
#define	_PATH_UTMP_UPDATE	"/usr/libexec/utmp_update"

#define _UTX_USERSIZE	32
#define _UTX_LINESIZE	32
#define	_UTX_IDSIZE	4
#define _UTX_HOSTSIZE	256

#if __BSD_VISIBLE
#define UTX_USERSIZE	_UTX_USERSIZE
#define UTX_LINESIZE	_UTX_LINESIZE
#define	UTX_IDSIZE	_UTX_IDSIZE
#define UTX_HOSTSIZE	_UTX_HOSTSIZE
#endif


#define EMPTY		0	/* No valid user accounting information. */
#if __BSD_VISIBLE
#define RUN_LVL		1
#endif
#define BOOT_TIME	2	/* Time of system boot. */
#define OLD_TIME	3	/* Time when system clock changed. */
#define NEW_TIME	4	/* Time after system clock changed. */
#define INIT_PROCESS	5	/* A process spawned by the init process. */
#define LOGIN_PROCESS	6	/* The session leader of a logged-in user. */
#define USER_PROCESS	7	/* A process. */
#define DEAD_PROCESS	8	/* A session leader who has exited. */

#if __BSD_VISIBLE
#define ACCOUNTING	9
#define SIGNATURE	10
#define DOWN_TIME	11

/*
 * Strings placed in the ut_line field to indicate special type entries
 */
#define	RUNLVL_MSG	"run-level %c"
#define	BOOT_MSG	"system boot"
#define	OTIME_MSG	"old time"
#define	NTIME_MSG	"new time"
#define	DOWN_MSG	"system down"

typedef enum {
	UTX_DB_UTMPX,
	UTX_DB_WTMPX,
	UTX_DB_LASTLOGX
} utx_db_t;
#endif

struct exit_status
{
	__uint16_t e_termination;	/* termination status */
	__uint16_t e_exit;		/* exit status */
};

/*
 * The following structure describes the fields of the utmpx entries
 * stored in _PATH_UTMPX or _PATH_WTMPX. This is not necessarily the
 * format the entries are stored in the files, and application should
 * only access entries using routines described in getutxent(3).
 */

#define ut_user ut_name
#define ut_xtime ut_tv.tv_sec

struct utmpx {
	char ut_name[_UTX_USERSIZE];	/* login name */
	char ut_id[_UTX_IDSIZE];	/* inittab id */
	char ut_line[_UTX_LINESIZE];	/* tty name */
	char ut_host[_UTX_HOSTSIZE];	/* host name */
	__uint8_t ut_unused[16];	/* reserved for future use */
	__uint16_t ut_session;		/* session id used for windowing */
	short ut_type;			/* type of this entry */
	pid_t ut_pid;			/* process id creating the entry */
	struct exit_status ut_exit;	/* process termination/exit status */
	struct sockaddr_storage ut_ss;	/* address where entry was made from */
	struct timeval ut_tv;		/* time entry was created */
	__uint8_t ut_unused2[16];	/* reserved for future use */
};

#if __BSD_VISIBLE
struct lastlogx {
	struct timeval ll_tv;		/* time entry was created */
	char ll_line[_UTX_LINESIZE];	/* tty name */
	char ll_host[_UTX_HOSTSIZE];	/* host name */
	struct sockaddr_storage ll_ss;	/* address where entry was made from */
};
#endif

__BEGIN_DECLS
void          endutxent(void);
struct utmpx *getutxent(void);
struct utmpx *getutxid(const struct utmpx *);
struct utmpx *getutxline(const struct utmpx *);
struct utmpx *pututxline(const struct utmpx *);
void          setutxent(void);

#if __BSD_VISIBLE
int _updwtmpx(const char *, const struct utmpx *);
void updwtmpx(const char *, const struct utmpx *);
struct lastlogx *getlastlogx(const char *, uid_t, struct lastlogx *);
int updlastlogx(const char *, uid_t, struct lastlogx *);
int utmpxname(const char *);
struct utmpx *getutxuser(const char *);
int setutxdb(utx_db_t, const char *);
#endif
__END_DECLS

#endif /* _UTMPX_H_ */
