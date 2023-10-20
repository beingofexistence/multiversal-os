/*-
 * Copyright (c) 1996, 1997, 1998
 *	HD Associates, Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by HD Associates, Inc
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY HD ASSOCIATES AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL HD ASSOCIATES OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: src/sys/posix4/posix4.h,v 1.6 1999/12/27 10:22:09 bde Exp $
 */

#ifndef _SYS_POSIX4_H_
#define _SYS_POSIX4_H_

#ifndef _KERNEL

#error "This file should not be included by userland programs."

#else

#include "opt_posix.h"

#include <sys/sched.h>

#ifdef MALLOC_DECLARE
MALLOC_DECLARE(M_P31B);
#endif

struct proc;
struct lwp;

void p31b_setcfg (int, int);

#ifdef _KPOSIX_PRIORITY_SCHEDULING

/*
 * KSCHED_OP_RW is a vector of read/write flags for each entry indexed
 * by the enum ksched_op.
 *
 * 1 means you need write access, 0 means read is sufficient.
 */

enum ksched_op {

#define KSCHED_OP_RW { 1, 0, 1, 0, 0, 0, 0, 0 }

	SCHED_SETPARAM,
	SCHED_GETPARAM,
	SCHED_SETSCHEDULER,
	SCHED_GETSCHEDULER,
	SCHED_YIELD,
	SCHED_GET_PRIORITY_MAX,
	SCHED_GET_PRIORITY_MIN,
	SCHED_RR_GET_INTERVAL,
	SCHED_OP_MAX
};

struct ksched;

int ksched_attach(struct ksched **);
int ksched_detach(struct ksched *);

int ksched_setparam(register_t *, struct ksched *,
	struct lwp *, const struct sched_param *);
int ksched_getparam(register_t *, struct ksched *,
	struct lwp *, struct sched_param *);

int ksched_setscheduler(register_t *, struct ksched *,
	struct lwp *, int, const struct sched_param *);
int ksched_getscheduler(register_t *, struct ksched *, struct lwp *);

int ksched_yield(register_t *, struct ksched *);

int ksched_get_priority_max(register_t *, struct ksched *, int);
int ksched_get_priority_min(register_t *, struct ksched *, int);

int ksched_rr_get_interval(register_t *, struct ksched *,
	struct lwp *, struct timespec *);

#endif /* _KPOSIX_PRIORITY_SCHEDULING */

#endif /* !_KERNEL */
#endif /* !_SYS_POSIX4_H_ */
