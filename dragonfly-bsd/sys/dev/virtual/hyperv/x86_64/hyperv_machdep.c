/*-
 * Copyright (c) 2016 Microsoft Corp.
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
 */

#include <sys/param.h>
#include <sys/systimer.h>
#include <sys/systm.h>

#include <machine/cpufunc.h>
#include <machine/specialreg.h>
#include <machine/msi_machdep.h>

#include <dev/virtual/hyperv/hyperv_busdma.h>
#include <dev/virtual/hyperv/hyperv_machdep.h>
#include <dev/virtual/hyperv/hyperv_reg.h>
#include <dev/virtual/hyperv/hyperv_var.h>

struct hyperv_reftsc_ctx {
	struct hyperv_reftsc	*tsc_ref;
	struct hyperv_dma	tsc_ref_dma;
};

static void		hyperv_tsc_cputimer_construct(struct cputimer *,
			    sysclock_t);
static sysclock_t	hyperv_tsc_cputimer_count_mfence(void);
static sysclock_t	hyperv_tsc_cputimer_count_lfence(void);

static struct hyperv_reftsc_ctx	hyperv_ref_tsc;
static hyperv_tc64_t	hyperv_tc64_saved;

static struct cputimer	hyperv_tsc_cputimer = {
	.next		= SLIST_ENTRY_INITIALIZER,
	.name		= "Hyper-V-TSC",
	.pri		= CPUTIMER_PRI_VMM_HI,
	.type		= CPUTIMER_VMM1,
	.count		= NULL,	/* determined later */
	.fromhz		= cputimer_default_fromhz,
	.fromus		= cputimer_default_fromus,
	.construct	= hyperv_tsc_cputimer_construct,
	.destruct	= cputimer_default_destruct,
	.freq		= HYPERV_TIMER_FREQ
};

static struct cpucounter hyperv_tsc_cpucounter = {
	.freq		= HYPERV_TIMER_FREQ,
	.count		= NULL, /* determined later */
	.flags		= CPUCOUNTER_FLAG_MPSYNC,
	.prio		= CPUCOUNTER_PRIO_VMM_HI,
	.type		= CPUCOUNTER_VMM1
};

uint64_t
hypercall_md(volatile void *hc_addr, uint64_t in_val,
    uint64_t in_paddr, uint64_t out_paddr)
{
	uint64_t status;

	__asm__ __volatile__ ("mov %0, %%r8" : : "r" (out_paddr): "r8");
	__asm__ __volatile__ ("call *%3" : "=a" (status) :
	    "c" (in_val), "d" (in_paddr), "m" (hc_addr));
	return (status);
}

int
hyperv_msi2vector(uint64_t msi_addr __unused, uint32_t msi_data)
{
	return (msi_data & MSI_X86_DATA_INTVEC);
}

#define HYPERV_TSC(fence)					\
static uint64_t							\
hyperv_tsc_##fence(void)					\
{								\
	struct hyperv_reftsc *tsc_ref = hyperv_ref_tsc.tsc_ref;	\
	uint32_t seq;						\
								\
	while ((seq = tsc_ref->tsc_seq) != 0) {			\
		uint64_t disc, ret, tsc;			\
		uint64_t scale;					\
		int64_t ofs;					\
								\
		cpu_ccfence();					\
		scale = tsc_ref->tsc_scale;			\
		ofs = tsc_ref->tsc_ofs;				\
								\
		cpu_##fence();					\
		tsc = rdtsc();					\
								\
		/* ret = ((tsc * scale) >> 64) + ofs */		\
		__asm__ __volatile__ ("mulq %3" :		\
		    "=d" (ret), "=a" (disc) :			\
		    "a" (tsc), "r" (scale));			\
		ret += ofs;					\
								\
		cpu_ccfence();					\
		if (tsc_ref->tsc_seq == seq)			\
			return (ret);				\
								\
		/* Sequence changed; re-sync. */		\
	}							\
	/* Fallback to the generic rdmsr. */			\
	return (rdmsr(MSR_HV_TIME_REF_COUNT));			\
}								\
struct __hack

HYPERV_TSC(lfence);
HYPERV_TSC(mfence);

static sysclock_t
hyperv_tsc_cputimer_count_lfence(void)
{
	uint64_t val;

	val = hyperv_tsc_lfence();
	return (val + hyperv_tsc_cputimer.base);
}

static sysclock_t
hyperv_tsc_cputimer_count_mfence(void)
{
	uint64_t val;

	val = hyperv_tsc_mfence();
	return (val + hyperv_tsc_cputimer.base);
}

static void
hyperv_tsc_cputimer_construct(struct cputimer *timer, sysclock_t oldclock)
{
	timer->base = 0;
	timer->base = oldclock - timer->count();
}

void
hyperv_md_init(void)
{
	hyperv_tc64_t tc64 = NULL;
	uint64_t val, orig;

	if ((hyperv_features &
	     (CPUID_HV_MSR_TIME_REFCNT | CPUID_HV_MSR_REFERENCE_TSC)) !=
	    (CPUID_HV_MSR_TIME_REFCNT | CPUID_HV_MSR_REFERENCE_TSC) ||
	    (cpu_feature & CPUID_SSE2) == 0)	/* SSE2 for mfence/lfence */
		return;

	switch (cpu_vendor_id) {
	case CPU_VENDOR_AMD:
		hyperv_tsc_cputimer.count = hyperv_tsc_cputimer_count_mfence;
		tc64 = hyperv_tsc_mfence;
		break;

	case CPU_VENDOR_INTEL:
		hyperv_tsc_cputimer.count = hyperv_tsc_cputimer_count_lfence;
		tc64 = hyperv_tsc_lfence;
		break;

	default:
		/* Unsupport CPU vendors. */
		return;
	}
	KASSERT(tc64 != NULL, ("tc64 is not set"));
	hyperv_tsc_cpucounter.count = tc64;

	hyperv_ref_tsc.tsc_ref = hyperv_dmamem_alloc(NULL, PAGE_SIZE, 0,
	    sizeof(struct hyperv_reftsc), &hyperv_ref_tsc.tsc_ref_dma,
	    BUS_DMA_WAITOK | BUS_DMA_ZERO);
	if (hyperv_ref_tsc.tsc_ref == NULL) {
		kprintf("hyperv: reftsc page allocation failed\n");
		return;
	}

	orig = rdmsr(MSR_HV_REFERENCE_TSC);
	val = MSR_HV_REFTSC_ENABLE | (orig & MSR_HV_REFTSC_RSVD_MASK) |
	    ((hyperv_ref_tsc.tsc_ref_dma.hv_paddr >> PAGE_SHIFT) <<
	     MSR_HV_REFTSC_PGSHIFT);
	wrmsr(MSR_HV_REFERENCE_TSC, val);

	/* Register Hyper-V reference TSC cputimers. */
	cputimer_register(&hyperv_tsc_cputimer);
	cputimer_select(&hyperv_tsc_cputimer, 0);
	cpucounter_register(&hyperv_tsc_cpucounter);
	hyperv_tc64_saved = hyperv_tc64;
	hyperv_tc64 = tc64;
}

void
hyperv_md_uninit(void)
{
	if (hyperv_ref_tsc.tsc_ref != NULL) {
		uint64_t val;

		/* Deregister Hyper-V reference TSC systimer. */
		cputimer_deregister(&hyperv_tsc_cputimer);
		/* Revert tc64 change. */
		hyperv_tc64 = hyperv_tc64_saved;

		val = rdmsr(MSR_HV_REFERENCE_TSC);
		wrmsr(MSR_HV_REFERENCE_TSC, val & MSR_HV_REFTSC_RSVD_MASK);

		hyperv_dmamem_free(&hyperv_ref_tsc.tsc_ref_dma,
		    hyperv_ref_tsc.tsc_ref);
		hyperv_ref_tsc.tsc_ref = NULL;
	}
}
