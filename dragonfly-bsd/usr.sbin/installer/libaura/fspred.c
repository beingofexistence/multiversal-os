/*
 * Copyright (c) 2004 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Chris Pressey <cpressey@catseye.mine.nu>.
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
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
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


/*
 * fspred.c
 * Filesystem predicates.
 * $Id: fspred.c,v 1.3 2005/02/10 03:33:49 cpressey Exp $
 */

#include <sys/stat.h>
#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/mount.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fspred.h"

/** PREDICATES **/

static void
vstatmod(mode_t *m, int *error, const char *fmt, va_list args)
{
	char *filename;
	struct stat sb;

	memset(&sb, 0, sizeof(sb));

	vasprintf(&filename, fmt, args);

	*error = stat(filename, &sb);
	free(filename);

	if (*error)
		*m = 0;	/* Do not leak fake mode */
	else
		*m = sb.st_mode;
}

int
is_dir(const char *fmt, ...)
{
	va_list args;
	int error;
	mode_t m;

	va_start(args, fmt);
	vstatmod(&m, &error, fmt, args);
	va_end(args);

	if (error == 0)
		return(S_ISDIR(m));
	else
		return(0);
}

int
is_file(const char *fmt, ...)
{
	va_list args;
	int error;
	mode_t m;

	va_start(args, fmt);
	vstatmod(&m, &error, fmt, args);
	va_end(args);

	if (error == 0)
		return(S_ISREG(m) );
	else
		return(0);

}

int
is_program(const char *fmt, ...)
{
	char *filename;
	struct stat sb;
	va_list args;
	int error;
	uid_t uid;
	gid_t gid;

	va_start(args, fmt);
	vasprintf(&filename, fmt, args);
	va_end(args);

	error = stat(filename, &sb);
	free(filename);

	uid = getuid();
	gid = getgid();

	if (error == 0) {
		/* Try to be more precise when identifying executable programs.
		 * Still this is subject to race conditions where the regular file
		 * might have its permissions/ownership changed during the test and
		 * thus provide inaccurate results.
		 * Also, effective uid/gid is not being checked.
		 */
		if ((S_ISREG(sb.st_mode)) &&
		    ((sb.st_uid == uid && sb.st_mode & S_IXUSR) ||
			(sb.st_gid == gid && sb.st_mode & S_IXGRP) ||
			(sb.st_mode & S_IXOTH))) {
			return 1;
		}
	}

	return 0;
}

int
is_device(const char *fmt, ...)
{
	va_list args;
	int error;
	mode_t m;

	va_start(args, fmt);
	vstatmod(&m, &error, fmt, args);
	va_end(args);

	if (error == 0)
		return(S_ISBLK(m) || S_ISCHR(m));
	else
		return(0);
}

int
is_named_pipe(const char *fmt, ...)
{
	va_list args;
	int error;
	mode_t m;

	va_start(args, fmt);
	vstatmod(&m, &error, fmt, args);
	va_end(args);

	if (error == 0)
		return(S_ISFIFO(m));
	else
		return(0);
}

int
is_mountpoint_mounted(const char *mtpt)
{
	struct statfs *mt_array, *mt_ptr;
	int count;

	count = getmntinfo(&mt_array, MNT_WAIT);
	for (mt_ptr = mt_array; count > 0; mt_ptr++, count--) {
		if (strncmp(mt_ptr->f_mntonname, mtpt, PATH_MAX) == 0)
			return(1);
	}
	return(0);
}

int
is_device_mounted(const char *device)
{
	struct statfs *mt_array, *mt_ptr;
	int count;

	count = getmntinfo(&mt_array, MNT_WAIT);
	for (mt_ptr = mt_array; count > 0; mt_ptr++, count--) {
		if (strncmp(mt_ptr->f_mntfromname, device, PATH_MAX) == 0)
			return(1);
	}
	return(0);
}

int
is_any_slice_mounted(const char *diskdev)
{
	struct statfs *mt_array, *mt_ptr;
	int count;

	count = getmntinfo(&mt_array, MNT_WAIT);
	for (mt_ptr = mt_array; count > 0; mt_ptr++, count--) {
		if (strstr(mt_ptr->f_mntfromname, diskdev) ==
		    mt_ptr->f_mntfromname)
			return(1);
	}
	return(0);
}
