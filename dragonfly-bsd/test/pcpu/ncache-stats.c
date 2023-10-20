/*
 * Copyright (c) 2003, 2004 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Hiten Pandya <hmp@backplane.com>.
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
 *
 * $DragonFly: src/test/pcpu/ncache-stats.c,v 1.6 2005/05/01 03:01:20 hmp Exp $
 */

#include <sys/param.h>
#include <sys/nchstats.h>
#include <sys/sysctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <err.h>
#include <kvm.h>

#define _NCH_ENT(t, n, tt)                       \
    printf("%-20s", t);                          \
    for (i = 1; i <= ncpus; ++i) {               \
        printf("%-9ld%s", nch[i-1].n, "\t");   \
		if (i == ncpus)                          \
			printf("(%-9ld)\n", tt.n);         \
	}                                            \


int main(void)
{
	int i, ncpus;
	struct nchstats *nch, total;
	size_t nch_len = SMP_MAXCPU * sizeof(struct nchstats);
	u_long nchtotal;

	nch = malloc(nch_len);
	if (nch == NULL)
		exit(-1);

	/* retrieve the statistics */
	if (sysctlbyname("vfs.cache.nchstats", nch, &nch_len, NULL, 0) < 0) {
		warn("sysctl");
		exit(-1);
	} else {
		nch = reallocf(nch, nch_len);
		if (nch == NULL)
			exit(-1);
	}

	ncpus = nch_len / sizeof(struct nchstats);

#if defined(DEBUG)
	printf("Number of processors = %d\n", ncpus);
#endif

	kvm_nch_cpuagg(nch, &total, ncpus);
	nchtotal = total.ncs_goodhits + total.ncs_neghits +
	    total.ncs_badhits + total.ncs_falsehits +
	    total.ncs_miss + total.ncs_long;

	printf("VFS Name Cache Effectiveness Statistics\n");
	printf("%9ld total name lookups\n",nchtotal);

	printf("%-20s", "COUNTER");
	
	for (i = 1; i <= ncpus; ++i) {
		printf("%3s-%-2d%s", "CPU", i, "\t");
		if (i == ncpus)
			printf("\t%-9s\n", "TOTAL");
	}

	_NCH_ENT("goodhits", ncs_goodhits, total);
	_NCH_ENT("neghits", ncs_neghits, total);
	_NCH_ENT("badhits", ncs_badhits, total);
	_NCH_ENT("falsehits", ncs_falsehits, total);
	_NCH_ENT("misses", ncs_miss, total);
	_NCH_ENT("longnames", ncs_long, total);
	_NCH_ENT("passes 2", ncs_pass2, total);
	_NCH_ENT("2-passes", ncs_2passes, total);

	free(nch);

	return 0;
}
