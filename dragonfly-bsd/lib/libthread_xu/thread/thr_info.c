/*
 * Copyright (c) 1995-1998 John Birrell <jb@cimlogic.com.au>
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
 * 3. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY JOHN BIRRELL AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: src/lib/libthr/thread/thr_info.c,v 1.10 2007/04/05 07:20:31 davidxu Exp $
 */

#include "namespace.h"
#include <sys/lwp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <pthread_np.h>
#include "un-namespace.h"

#include "thr_private.h"

/* Set the thread name for debug. */
int
_pthread_setname_np(pthread_t thread, const char *name)
{
	pthread_t curthread = tls_get_curthread();
	int error;
	int result;

	result = 0;
	if (curthread == thread) {
		if (lwp_setname(thread->tid, name) == -1)
			result = errno;
	} else {
		if ((error = _thr_ref_add(curthread, thread, 0)) == 0) {
			THR_THREAD_LOCK(curthread, thread);
			if (thread->state != PS_DEAD) {
				if (lwp_setname(thread->tid, name) == -1)
					result = errno;
			}
			THR_THREAD_UNLOCK(curthread, thread);
			_thr_ref_delete(curthread, thread);
		} else {
			result = error;
		}
	}
	return (result);
}

void
_pthread_set_name_np(pthread_t thread, const char *name)
{
	(void)_pthread_setname_np(thread, name);
}

/* Set the thread name for debug. */
int
_pthread_getname_np(pthread_t thread, char *name, size_t len)
{
	pthread_t curthread = tls_get_curthread();
	int error;
	int result;

	result = 0;
	if (curthread == thread) {
		if (lwp_getname(thread->tid, name, len) == -1)
			result = errno;
	} else {
		if ((error = _thr_ref_add(curthread, thread, 0)) == 0) {
			THR_THREAD_LOCK(curthread, thread);
			if (thread->state != PS_DEAD) {
				if (lwp_getname(thread->tid, name, len) == -1)
					result = errno;
			} else if (len) {
				name[0] = 0;
			}
			THR_THREAD_UNLOCK(curthread, thread);
			_thr_ref_delete(curthread, thread);
		} else if (len) {
			result = errno = EINVAL;
			name[0] = 0;
		} else {
			result = error;
		}
	}
	return (result);
}

void
_pthread_get_name_np(pthread_t thread, char *buf, size_t len)
{
	(void)_pthread_getname_np(thread, buf, len);
}

__strong_reference(_pthread_get_name_np, pthread_get_name_np);
__strong_reference(_pthread_getname_np, pthread_getname_np);
__strong_reference(_pthread_set_name_np, pthread_set_name_np);
__strong_reference(_pthread_setname_np, pthread_setname_np);
