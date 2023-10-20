/*	$OpenBSD: isfinite.c,v 1.8 2013/07/03 04:46:36 espie Exp $	*/
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

__sym_compat(__isfinite, past___isfinite, DF306.0);
int
past___isfinite(double d)
{
	struct ieee_double *p = (struct ieee_double *)&d;

	return (p->dbl_exp != DBL_EXP_INFNAN);
}

__sym_compat(__isfinitef, past___isfinitef, DF306.0);
int
past___isfinitef(float f)
{
	struct ieee_single *p = (struct ieee_single *)&f;

	return (p->sng_exp != SNG_EXP_INFNAN);
}

#pragma GCC diagnostic pop
