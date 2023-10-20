/*-
 * Copyright (c) 1990, 1993
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
 * @(#)exit.c	8.1 (Berkeley) 6/4/93
 * $FreeBSD: src/lib/libc/stdlib/exit.c,v 1.9 2007/01/09 00:28:09 imp Exp $
 */

#include "namespace.h"
#include <sys/single_threaded.h>
#include <stdlib.h>
#include <unistd.h>
#include "un-namespace.h"

#include "atexit.h"
#include "libc_private.h"

void (*__cleanup)(void);

/*
 * Publicly exported __isthreaded inverted sticky version for use in
 * third-party libraries to optimize non-threaded programs.
 */
char __libc_single_threaded = 1;

/*
 * This variable is zero until a process has created a thread.
 * It is used to avoid calling locking functions in libc when they
 * are not required. By default, libc is intended to be(come)
 * thread-safe, but without a (significant) penalty to non-threaded
 * processes.
 */
int	__isthreaded	= 0;

/*
 * Allows code to test if the whole process is exiting, to avoid
 * unnecessary overhead such as freeing zones in nmalloc.
 */
int	__isexiting	= 0;

/*
 * Exit, flushing stdio buffers if necessary.
 */
void
exit(int status)
{
	/* Ensure that the auto-initialization routine is linked in: */
	extern int _thread_autoinit_dummy_decl;

	__isexiting = 1;
	_thread_autoinit_dummy_decl = 1;

	/* Call TLS destructors, if any. */
	_thread_finalize();
	__cxa_finalize(NULL);
	if (__cleanup)
		(*__cleanup)();
	_exit(status);
}

/*
 * ISO C99 added the _Exit() function that results in immediate program
 * termination without triggering signals or atexit()-registered functions.
 * In POSIX this is equivalent to the _exit() function.
 */
void
_Exit(int status)
{
	_exit(status);
}
