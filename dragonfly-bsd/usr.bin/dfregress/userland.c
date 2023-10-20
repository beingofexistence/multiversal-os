/*
 * Copyright (c) 2011 Alex Hornung <alex@alexhornung.com>.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>

#include <err.h>

#include <libprop/proplib.h>

#include "testcase.h"
#include "runlist.h"
#include "userland.h"
#include <dfregress.h>

static void
clean_child(pid_t pid)
{
	kill(pid, SIGKILL);
}

static void
sig_handle(int sig __unused)
{
	return;
}

int
run_userland(const char *binary, int argc, const char **argv, const char *interpreter,
    int need_setuid, uid_t uid, struct timeval *timeout, int rc, int unify_output,
    char *errbuf, size_t errbuf_sz, struct testcase_result *tr)
{
	struct itimerval itim;
	struct sigaction sa;
	pid_t pid = -1, r_pid;
	int r, status;
	int fd_stdout = -1, fd_stderr = -1;
	size_t sz_stdout, sz_stderr;
	char stdout_file[256];
	char stderr_file[256];
	char **argv_copy;

	/* Set sane defaults */
	bzero(tr, sizeof(*tr));
	tr->result = RESULT_NOTRUN;

	strcpy(stdout_file, "/tmp/dfregress.XXXXXXXXXXXX");
	strcpy(stderr_file, "/tmp/dfregress.XXXXXXXXXXXX");
	fd_stdout = mkostemp(stdout_file, O_SYNC);
	if (fd_stdout == -1) {
		if (errbuf)
			snprintf(errbuf, errbuf_sz, "Could not mkostemp(): "
			    "%s\n", strerror(errno));
		return -1;
	}

	if (!unify_output) {
		fd_stderr = mkostemp(stderr_file, O_SYNC);
		if (fd_stderr == -1) {
			if (errbuf)
				snprintf(errbuf, errbuf_sz, "Could not mkostemp(): "
				"%s\n", strerror(errno));
			return -1;
		}
	}


	if ((pid = fork()) == -1) {
		if (errbuf)
			snprintf(errbuf, errbuf_sz, "Could not fork to run "
			    "binary %s: %s\n", binary, strerror(errno));

		goto err_out;
	} else if (pid > 0) {
		/* parent */

		if (timeout != NULL) {
			/* Ignore SIGALRM */
			bzero(&sa, sizeof(sa));
			sa.sa_handler = sig_handle;
			sigaction(SIGALRM, &sa, NULL);

			/* Set up timeout */
			itim.it_interval.tv_sec = 0;
			itim.it_interval.tv_usec = 0;
			itim.it_value = *timeout;
			r = setitimer(ITIMER_REAL, &itim, NULL);
			if (r == -1) {
				if (errbuf)
					snprintf(errbuf, errbuf_sz, "Could not "
					    "set up timer: %s", strerror(errno));

				/* Clean up child process! */
				goto err_out;
			}
		}

		r_pid = wait4(pid, &status, 0, &tr->rusage);
		if (r_pid == -1) {
			if (errno == EINTR) {
				/* Alarm timed out */
				tr->result = RESULT_TIMEOUT;

				/* Clean up child process! */
				clean_child(pid);
			} else if (errno == ECHILD) {
				/* Child already exited somehow */
				tr->result = RESULT_UNKNOWN;
			} else {
				/* EFAULT */
				if (errbuf)
					snprintf(errbuf, errbuf_sz, "Could not "
					    "wait4(): %s", strerror(errno));

				goto err_out;
			}
		} else {
			if (WIFEXITED(status)) {
				tr->result = (WEXITSTATUS(status) == rc) ?
				    RESULT_PASS :
				    (WEXITSTATUS(status) == EXIT_NOTRUN) ?
				    RESULT_NOTRUN : RESULT_FAIL;

				tr->exit_value = WEXITSTATUS(status);
			} else if (WIFSIGNALED(status)) {
				tr->result = RESULT_SIGNALLED;
				tr->signal = WTERMSIG(status);
				tr->core_dumped = (WCOREDUMP(status)) ? 1 : 0;
			} else {
				tr->result = RESULT_UNKNOWN;
			}
		}

		if (timeout != NULL) {
			/* Disable timer */
			itim.it_value.tv_sec = 0;
			itim.it_value.tv_usec = 0;
			setitimer(ITIMER_REAL, &itim, NULL);
		}
	} else {
		/* pid == 0, so we are the child */

		/* Redirect stdout and stderr */
		if (fd_stdout >= 0) {
			dup2(fd_stdout, 1);
			setvbuf(stdout, NULL, _IONBF, 0);
		}

		if ((fd_stderr >= 0) || (unify_output && fd_stdout >= 0)) {
			dup2((unify_output) ? fd_stdout : fd_stderr, 2);
			setvbuf((unify_output) ? stdout : stderr,
			    NULL, _IONBF, 0);
		}

		/* Set uid if necessary */
		if (need_setuid) {
			r = setuid(uid);
			if (r == -1) {
				fprintf(stderr, "ERR: NOT RUN (setuid): %s",
				    strerror(errno));
				exit(EXIT_NOTRUN);
			}
		}

		if (interpreter) {
			/*
			 * Allocate argc + 3 arguments more as shown below:
			 * argv_copy[0] = interpreter
			 * argv_copy[1] = argv[0]
			 * argv_copy[argc+2] = NULL
			 *
			 * execvp requires the array to end with NULL.
			 */
			argv_copy = (char **)calloc(argc + 3, sizeof(char *));
			if (argv_copy == NULL) {
				err(1, "could not calloc argv_copy memory");

			}
			/* Insert the interpreter at pos 0 */
			argv_copy[0] = malloc(strlen(interpreter) + 1);
			snprintf(argv_copy[0], strlen(interpreter) + 1, "%s",
			    interpreter);

			/* We still need argv[0] when argc is 0 */
			for (int i = 0; i <= argc; i++) {
				size_t len;
				len = strlen(argv[i]) + 1; /* NULL-terminated */

				argv_copy[i + 1] = malloc(len);
				if (argv_copy[i] == NULL)
					err(1, "could not malloc memory");

				snprintf(argv_copy[i + 1], len, "%s",
					argv[i]);

			}
			/* Null terminate the array */
			argv_copy[argc + 2] = NULL;
			r = execvp(interpreter, argv_copy);
		} else {
			/* Try to exec() */
			r = execvp(binary, __DECONST(char **, argv));
		}
		if (r == -1) {
			/*
			 * If we couldn't exec(), notify parent that we didn't
			 * run.
			 */
			fprintf(stderr, "ERR: NOT RUN: %s", strerror(errno));
			exit(EXIT_NOTRUN);
		}
	}

	/* Read stdout and stderr redirected file contents into memory */
	sz_stdout = (size_t)lseek(fd_stdout, 0, SEEK_END);
	lseek(fd_stdout, 0, SEEK_SET);

	tr->stdout_buf = malloc(sz_stdout + 1);
	if (tr->stdout_buf == NULL)
		err(1, "could not malloc fd buf memory");

	read(fd_stdout, tr->stdout_buf, sz_stdout);
	tr->stdout_buf[sz_stdout] = '\0';

	close(fd_stdout);
	unlink(stdout_file);

	if (!unify_output) {
		sz_stderr = (size_t)lseek(fd_stderr, 0, SEEK_END);
		lseek(fd_stderr, 0, SEEK_SET);

		tr->stderr_buf = malloc(sz_stderr + 1);
		if (tr->stderr_buf == NULL)
			err(1, "could not malloc fd buf memory");

		read(fd_stderr, tr->stderr_buf, sz_stderr);
		tr->stderr_buf[sz_stderr] = '\0';

		close(fd_stderr);
		unlink(stderr_file);
	}


	return 0;
	/* NOTREACHED */

err_out:
	if (pid != -1)
		clean_child(pid);

	if (fd_stdout >= 0) {
		close(fd_stdout);
		unlink(stdout_file);
	}

	if (fd_stderr >= 0) {
		close(fd_stderr);
		unlink(stderr_file);
	}

	return -1;
}

int
run_simple_cmd(const char *binary, const char *arg, char *errbuf,
    size_t errbuf_sz, struct testcase_result *tr)
{
	const char *argv[3];
	char *s;

	s = strrchr(binary, '/');

	argv[0] = (s == NULL) ? __DECONST(char *, binary) : s+1;
	argv[1] = __DECONST(char *, arg);
	argv[2] = NULL;

	return run_userland(binary, /* executable */
	    1,    /* argc */
	    argv, /* argv */
	    NULL, /* interpreter */
	    0,    /* needs_setuid */
	    0,    /* runas_uid */
	    NULL, /* timeout */
	    0,    /* rc */
	    1,    /* unify_output */
	    errbuf,    /* errbuf */
	    errbuf_sz, /* errbuf_size */
	    tr);       /* testcase_result */
}
