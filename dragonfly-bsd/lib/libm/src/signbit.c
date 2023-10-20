/*	$OpenBSD: signbit.c,v 1.7 2013/07/03 04:46:36 espie Exp $	*/
/*
 * Copyright (c) 2008 Martynas Venckus <martynas@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <machine/ieee.h>
#include <float.h>
#include <math.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"

__sym_compat(__signbit, past___signbit, DF306.0);
int
past___signbit(double d)
{
	struct ieee_double *p = (struct ieee_double *)&d;

	return p->dbl_sign;
}

__sym_compat(__signbitf, past___signbitf, DF306.0);
int
past___signbitf(float f)
{
	struct ieee_single *p = (struct ieee_single *)&f;

	return p->sng_sign;
}

#pragma GCC diagnostic pop
