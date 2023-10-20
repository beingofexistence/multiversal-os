/*
 * Copyright (c) 2005, David Xu <davidxu@freebsd.org>
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "namespace.h"
#include <machine/tls.h>
#include <errno.h>
#include <pthread.h>
#include "un-namespace.h"

#include "thr_private.h"

#define cpu_ccfence()       __asm __volatile("" : : : "memory")

int _pthread_timedjoin_np(pthread_t pthread, void **thread_return,
	const struct timespec *abstime);

static int join_common(pthread_t, void **, const struct timespec *);

static void backout_join(void *arg)
{
	pthread_t curthread = tls_get_curthread();
	pthread_t pthread = (pthread_t)arg;

	THREAD_LIST_LOCK(curthread);
	pthread->joiner = NULL;
	THREAD_LIST_UNLOCK(curthread);
}

int
_pthread_join(pthread_t pthread, void **thread_return)
{
	return (join_common(pthread, thread_return, NULL));
}

int
_pthread_timedjoin_np(pthread_t pthread, void **thread_return,
	const struct timespec *abstime)
{
	if (abstime == NULL || abstime->tv_sec < 0 || abstime->tv_nsec < 0 ||
	    abstime->tv_nsec >= 1000000000)
		return (EINVAL);

	return (join_common(pthread, thread_return, abstime));
}

static int
join_common(pthread_t pthread, void **thread_return,
	const struct timespec *abstime)
{
	pthread_t curthread = tls_get_curthread();
	struct timespec ts, ts2, *tsp;
	void *tmp;
	long state;
	int oldcancel;
	int ret = 0;

	if (pthread == NULL)
		return (EINVAL);

	if (pthread == curthread)
		return (EDEADLK);

	THREAD_LIST_LOCK(curthread);
	if ((ret = _thr_find_thread(curthread, pthread, 1)) != 0) {
		ret = ESRCH;
	} else if ((pthread->tlflags & TLFLAGS_DETACHED) != 0) {
		ret = EINVAL;
	} else if (pthread->joiner != NULL) {
		/* Multiple joiners are not supported. */
		ret = ENOTSUP;
	}
	if (ret) {
		THREAD_LIST_UNLOCK(curthread);
		return (ret);
	}
	/* Set the running thread to be the joiner: */
	pthread->joiner = curthread;

	THREAD_LIST_UNLOCK(curthread);

	THR_CLEANUP_PUSH(curthread, backout_join, pthread);
	oldcancel = _thr_cancel_enter(curthread);

	while ((state = pthread->state) != PS_DEAD) {
		cpu_ccfence();
		if (abstime != NULL) {
			clock_gettime(CLOCK_REALTIME, &ts);
			timespecsub(abstime, &ts, &ts2);
			if (ts2.tv_sec < 0) {
				ret = ETIMEDOUT;
				break;
			}
			tsp = &ts2;
		} else
			tsp = NULL;
		ret = _thr_umtx_wait(&pthread->state, state, tsp,
				     CLOCK_REALTIME);
		if (ret == ETIMEDOUT)
			break;
	}

	_thr_cancel_leave(curthread, oldcancel);
	THR_CLEANUP_POP(curthread, 0);

	if (ret == ETIMEDOUT) {
		THREAD_LIST_LOCK(curthread);
		pthread->joiner = NULL;
		THREAD_LIST_UNLOCK(curthread);
	} else {
		ret = 0;
		tmp = pthread->ret;
		THREAD_LIST_LOCK(curthread);
		pthread->tlflags |= TLFLAGS_DETACHED;
		pthread->joiner = NULL;
		THR_GCLIST_ADD(pthread);
		THREAD_LIST_UNLOCK(curthread);

		if (thread_return != NULL)
			*thread_return = tmp;
	}
	return (ret);
}

__strong_reference(_pthread_join, pthread_join);
__strong_reference(_pthread_timedjoin_np, pthread_timedjoin_np);
