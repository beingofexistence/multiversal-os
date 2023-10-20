/*
 * Copyright (c) 2005, David Xu <davidxu@freebsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
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
 *
 */

#include "namespace.h"
#include <machine/tls.h>
#include <pthread.h>
#include "un-namespace.h"

#include "thr_private.h"

int
_pthread_cancel(pthread_t pthread)
{
	pthread_t curthread = tls_get_curthread();
	int oldval, newval = 0;
	int oldtype;
	int ret;

	/*
	 * POSIX says _pthread_cancel should be async cancellation safe,
	 * so we temporarily disable async cancellation.
	 */
	_pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &oldtype);
	if ((ret = _thr_ref_add(curthread, pthread, 0)) != 0) {
		_pthread_setcanceltype(oldtype, NULL);
		return (ret);
	}

	do {
		oldval = pthread->cancelflags;
		if (oldval & THR_CANCEL_NEEDED)
			break;
		newval = oldval | THR_CANCEL_NEEDED;
	} while (!atomic_cmpset_acq_int(&pthread->cancelflags, oldval, newval));

	if (!(oldval & THR_CANCEL_NEEDED) && SHOULD_ASYNC_CANCEL(newval))
		_thr_send_sig(pthread, SIGCANCEL);

	_thr_ref_delete(curthread, pthread);
	_pthread_setcanceltype(oldtype, NULL);
	return (0);
}

static inline void
testcancel(pthread_t curthread)
{
	int newval;

	newval = curthread->cancelflags;
	if (SHOULD_CANCEL(newval))
		_pthread_exit(PTHREAD_CANCELED);
}

int
_pthread_setcancelstate(int state, int *oldstate)
{
	pthread_t curthread = tls_get_curthread();
	int oldval;

	oldval = curthread->cancelflags;
	if (oldstate != NULL)
		*oldstate = ((oldval & THR_CANCEL_DISABLE) ?
		    PTHREAD_CANCEL_DISABLE : PTHREAD_CANCEL_ENABLE);
	switch (state) {
	case PTHREAD_CANCEL_DISABLE:
		atomic_set_int(&curthread->cancelflags, THR_CANCEL_DISABLE);
		break;
	case PTHREAD_CANCEL_ENABLE:
		atomic_clear_int(&curthread->cancelflags, THR_CANCEL_DISABLE);
		testcancel(curthread);
		break;
	default:
		return (EINVAL);
	}

	return (0);
}

int
_pthread_setcanceltype(int type, int *oldtype)
{
	pthread_t curthread = tls_get_curthread();
	int oldval;

	oldval = curthread->cancelflags;
	if (oldtype != NULL)
		*oldtype = ((oldval & THR_CANCEL_AT_POINT) ?
				 PTHREAD_CANCEL_ASYNCHRONOUS :
				 PTHREAD_CANCEL_DEFERRED);
	switch (type) {
	case PTHREAD_CANCEL_ASYNCHRONOUS:
		atomic_set_int(&curthread->cancelflags, THR_CANCEL_AT_POINT);
		testcancel(curthread);
		break;
	case PTHREAD_CANCEL_DEFERRED:
		atomic_clear_int(&curthread->cancelflags, THR_CANCEL_AT_POINT);
		break;
	default:
		return (EINVAL);
	}

	return (0);
}

void
_pthread_testcancel(void)
{
	testcancel(tls_get_curthread());
}

int
_thr_cancel_enter(pthread_t curthread)
{
	int oldval;

	oldval = curthread->cancelflags;
	if (!(oldval & THR_CANCEL_AT_POINT)) {
		atomic_set_int(&curthread->cancelflags, THR_CANCEL_AT_POINT);
		testcancel(curthread);
	}
	return (oldval);
}

void
_thr_cancel_leave(pthread_t curthread, int previous)
{
	if (!(previous & THR_CANCEL_AT_POINT))
		atomic_clear_int(&curthread->cancelflags, THR_CANCEL_AT_POINT);
}

__strong_reference(_pthread_cancel, pthread_cancel);
__strong_reference(_pthread_setcancelstate, pthread_setcancelstate);
__strong_reference(_pthread_setcanceltype, pthread_setcanceltype);
__strong_reference(_pthread_testcancel, pthread_testcancel);
