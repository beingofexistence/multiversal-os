/*
 * Copyright (c) 1997, 1998 John S. Dyson.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *	notice immediately at the beginning of the file, without modification,
 *	this list of conditions, and the following disclaimer.
 * 2. Absolutely no warranty of function or purpose is made by the author
 *	John S. Dyson.
 *
 * $FreeBSD: src/sys/vm/vm_zone.c,v 1.30.2.6 2002/10/10 19:50:16 dillon Exp $
 *
 * Copyright (c) 2003-2017,2019 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com>
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

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/sysctl.h>
#include <sys/vmmeter.h>

#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_map.h>
#include <vm/vm_kern.h>
#include <vm/vm_extern.h>
#include <vm/vm_zone.h>

#include <sys/spinlock2.h>
#include <vm/vm_page2.h>

static MALLOC_DEFINE(M_ZONE, "ZONE", "Zone header");

#define	ZONE_ERROR_INVALID 0
#define	ZONE_ERROR_NOTFREE 1
#define	ZONE_ERROR_ALREADYFREE 2

#define ZONE_ROUNDING	32

#define	ZENTRY_FREE	0x12342378

long zone_burst = 128;

static void *zget(vm_zone_t z, int *tryagainp);

/*
 * Return an item from the specified zone.   This function is non-blocking for
 * ZONE_INTERRUPT zones.
 *
 * No requirements.
 */
void *
zalloc(vm_zone_t z)
{
	globaldata_t gd = mycpu;
	vm_zpcpu_t *zpcpu;
	void *item;
	int tryagain;
	long n;

#ifdef INVARIANTS
	if (z == NULL)
		zerror(ZONE_ERROR_INVALID);
#endif
	zpcpu = &z->zpcpu[gd->gd_cpuid];
retry:
	/*
	 * Avoid spinlock contention by allocating from a per-cpu queue
	 */
	if (zpcpu->zfreecnt > 0) {
		crit_enter_gd(gd);
		if (zpcpu->zfreecnt > 0) {
			item = zpcpu->zitems;
#ifdef INVARIANTS
			KASSERT(item != NULL,
				("zitems_pcpu unexpectedly NULL"));
			if (((void **)item)[1] != (void *)ZENTRY_FREE)
				zerror(ZONE_ERROR_NOTFREE);
			((void **)item)[1] = NULL;
#endif
			zpcpu->zitems = ((void **) item)[0];
			--zpcpu->zfreecnt;
			++zpcpu->znalloc;
			crit_exit_gd(gd);

			return item;
		}
		crit_exit_gd(gd);
	}

	/*
	 * Per-zone spinlock for the remainder.  Always load at least one
	 * item.
	 */
	spin_lock(&z->zspin);
	if (z->zfreecnt > z->zfreemin) {
		n = zone_burst;
		do {
			item = z->zitems;
#ifdef INVARIANTS
			KASSERT(item != NULL, ("zitems unexpectedly NULL"));
			if (((void **)item)[1] != (void *)ZENTRY_FREE)
				zerror(ZONE_ERROR_NOTFREE);
#endif
			z->zitems = ((void **)item)[0];
			--z->zfreecnt;
			((void **)item)[0] = zpcpu->zitems;
			zpcpu->zitems = item;
			++zpcpu->zfreecnt;
		} while (--n > 0 && z->zfreecnt > z->zfreemin);
		spin_unlock(&z->zspin);
		goto retry;
	} else {
		spin_unlock(&z->zspin);
		tryagain = 0;
		item = zget(z, &tryagain);
		if (tryagain)
			goto retry;

		/*
		 * PANICFAIL allows the caller to assume that the zalloc()
		 * will always succeed.  If it doesn't, we panic here.
		 */
		if (item == NULL && (z->zflags & ZONE_PANICFAIL))
			panic("zalloc(%s) failed", z->zname);
	}
	return item;
}

/*
 * Free an item to the specified zone.   
 *
 * No requirements.
 */
void
zfree(vm_zone_t z, void *item)
{
	globaldata_t gd = mycpu;
	vm_zpcpu_t *zpcpu;
	void *tail_item;
	long count;
	long zmax;

	zpcpu = &z->zpcpu[gd->gd_cpuid];

	/*
	 * Avoid spinlock contention by freeing into a per-cpu queue
	 */
	zmax = z->zmax_pcpu;
	if (zmax < 1024)
		zmax = 1024;

	/*
	 * Add to pcpu cache
	 */
	crit_enter_gd(gd);
	((void **)item)[0] = zpcpu->zitems;
#ifdef INVARIANTS
	if (((void **)item)[1] == (void *)ZENTRY_FREE)
		zerror(ZONE_ERROR_ALREADYFREE);
	((void **)item)[1] = (void *)ZENTRY_FREE;
#endif
	zpcpu->zitems = item;
	++zpcpu->zfreecnt;

	if (zpcpu->zfreecnt < zmax) {
		crit_exit_gd(gd);
		return;
	}

	/*
	 * Hystereis, move (zmax) (calculated below) items to the pool.
	 */
	zmax = zmax / 2;
	if (zmax > zone_burst)
		zmax = zone_burst;
	tail_item = item;
	count = 1;

	while (count < zmax) {
		tail_item = ((void **)tail_item)[0];
		++count;
	}
	zpcpu->zitems = ((void **)tail_item)[0];
	zpcpu->zfreecnt -= count;

	/*
	 * Per-zone spinlock for the remainder.
	 *
	 * Also implement hysteresis by freeing a number of pcpu
	 * entries.
	 */
	spin_lock(&z->zspin);
	((void **)tail_item)[0] = z->zitems;
	z->zitems = item;
	z->zfreecnt += count;
	spin_unlock(&z->zspin);

	crit_exit_gd(gd);
}

/*
 * This file comprises a very simple zone allocator.  This is used
 * in lieu of the malloc allocator, where needed or more optimal.
 *
 * Note that the initial implementation of this had coloring, and
 * absolutely no improvement (actually perf degradation) occurred.
 *
 * Note also that the zones are type stable.  The only restriction is
 * that the first two longwords of a data structure can be changed
 * between allocations.  Any data that must be stable between allocations
 * must reside in areas after the first two longwords.
 *
 * zinitna, zinit, zbootinit are the initialization routines.
 * zalloc, zfree, are the allocation/free routines.
 */

LIST_HEAD(zlist, vm_zone) zlist = LIST_HEAD_INITIALIZER(zlist);
static int sysctl_vm_zone(SYSCTL_HANDLER_ARGS);
static vm_pindex_t zone_kmem_pages, zone_kern_pages;
static long zone_kmem_kvaspace;

/*
 * Create a zone, but don't allocate the zone structure.  If the
 * zone had been previously created by the zone boot code, initialize
 * various parts of the zone code.
 *
 * If waits are not allowed during allocation (e.g. during interrupt
 * code), a-priori allocate the kernel virtual space, and allocate
 * only pages when needed.
 *
 * Arguments:
 * z		pointer to zone structure.
 * obj		pointer to VM object (opt).
 * name		name of zone.
 * size		size of zone entries.
 * nentries	number of zone entries allocated (only ZONE_INTERRUPT.)
 * flags	ZONE_INTERRUPT -- items can be allocated at interrupt time.
 * zalloc	number of pages allocated when memory is needed.
 *
 * Note that when using ZONE_INTERRUPT, the size of the zone is limited
 * by the nentries argument.  The size of the memory allocatable is
 * unlimited if ZONE_INTERRUPT is not set.
 *
 * No requirements.
 */
int
zinitna(vm_zone_t z, char *name, size_t size, long nentries, uint32_t flags)
{
	size_t totsize;

	/*
	 * Only zones created with zinit() are destroyable.
	 */
	if (z->zflags & ZONE_DESTROYABLE)
		panic("zinitna: can't create destroyable zone");

	/*
	 * NOTE: We can only adjust zsize if we previously did not
	 * 	 use zbootinit().
	 */
	if ((z->zflags & ZONE_BOOT) == 0) {
		z->zsize = roundup2(size, ZONE_ROUNDING);
		spin_init(&z->zspin, "zinitna");
		lockinit(&z->zgetlk, "zgetlk", 0, LK_CANRECURSE);

		z->zfreecnt = 0;
		z->ztotal = 0;
		z->zmax = 0;
		z->zname = name;
		z->zitems = NULL;

		lwkt_gettoken(&vm_token);
		LIST_INSERT_HEAD(&zlist, z, zlink);
		lwkt_reltoken(&vm_token);

		bzero(z->zpcpu, sizeof(z->zpcpu));
	}

	z->zkmvec = NULL;
	z->zkmcur = z->zkmmax = 0;
	z->zflags |= flags;

	/*
	 * If we cannot wait, allocate KVA space up front, and we will fill
	 * in pages as needed.  This is particularly required when creating
	 * an allocation space for map entries in kernel_map, because we
	 * do not want to go into a recursion deadlock with 
	 * vm_map_entry_reserve().
	 */
	if (z->zflags & ZONE_INTERRUPT) {
		totsize = round_page((size_t)z->zsize * nentries);
		atomic_add_long(&zone_kmem_kvaspace, totsize);

		z->zkva = kmem_alloc_pageable(kernel_map, totsize,
					      VM_SUBSYS_ZALLOC);
		if (z->zkva == 0) {
			LIST_REMOVE(z, zlink);
			return 0;
		}

		z->zpagemax = totsize / PAGE_SIZE;
		z->zallocflag = VM_ALLOC_SYSTEM | VM_ALLOC_INTERRUPT |
				VM_ALLOC_NORMAL | VM_ALLOC_RETRY;
		z->zmax += nentries;

		/*
		 * Set reasonable pcpu cache bounds.  Low-memory systems
		 * might try to cache too little, large-memory systems
		 * might try to cache more than necessarsy.
		 *
		 * In particular, pvzone can wind up being excessive and
		 * waste memory unnecessarily.
		 */
		z->zmax_pcpu = z->zmax / ncpus / 64;
		if (z->zmax_pcpu < 1024)
			z->zmax_pcpu = 1024;
		if (z->zmax_pcpu * z->zsize > 16*1024*1024)
			z->zmax_pcpu = 16*1024*1024 / z->zsize;
	} else {
		z->zallocflag = VM_ALLOC_NORMAL | VM_ALLOC_SYSTEM;
		z->zmax = 0;
		z->zmax_pcpu = 8192;
	}


	if (z->zsize > PAGE_SIZE)
		z->zfreemin = 1;
	else
		z->zfreemin = PAGE_SIZE / z->zsize;

	z->zpagecount = 0;

	/*
	 * Reduce kernel_map spam by allocating in chunks.
	 */
	z->zalloc = ZONE_MAXPGLOAD;

	/*
	 * Populate the interrrupt zone at creation time rather than
	 * on first allocation, as this is a potentially long operation.
	 */
	if (z->zflags & ZONE_INTERRUPT) {
		void *buf;

		buf = zget(z, NULL);
		if (buf)
			zfree(z, buf);
	}

	return 1;
}

/*
 * Subroutine same as zinitna, except zone data structure is allocated
 * automatically by malloc.  This routine should normally be used, except
 * in certain tricky startup conditions in the VM system -- then
 * zbootinit and zinitna can be used.  Zinit is the standard zone
 * initialization call.
 *
 * No requirements.
 */
vm_zone_t
zinit(char *name, size_t size, long nentries, uint32_t flags)
{
	vm_zone_t z;

	z = (vm_zone_t) kmalloc(sizeof (struct vm_zone), M_ZONE, M_NOWAIT);
	if (z == NULL)
		return NULL;

	z->zflags = 0;
	if (zinitna(z, name, size, nentries, flags & ~ZONE_DESTROYABLE) == 0) {
		kfree(z, M_ZONE);
		return NULL;
	}

	if (flags & ZONE_DESTROYABLE)
		z->zflags |= ZONE_DESTROYABLE;

	return z;
}

/*
 * Initialize a zone before the system is fully up.  This routine should
 * only be called before full VM startup.
 *
 * Called from the low level boot code only.
 */
void
zbootinit(vm_zone_t z, char *name, size_t size, void *item, long nitems)
{
	long i;

	spin_init(&z->zspin, "zbootinit");
	lockinit(&z->zgetlk, "zgetlk", 0, LK_CANRECURSE);
	bzero(z->zpcpu, sizeof(z->zpcpu));
	z->zname = name;
	z->zsize = size;
	z->zpagemax = 0;
	z->zflags = ZONE_BOOT;
	z->zfreemin = 0;
	z->zallocflag = 0;
	z->zpagecount = 0;
	z->zalloc = 0;

	bzero(item, (size_t)nitems * z->zsize);
	z->zitems = NULL;
	for (i = 0; i < nitems; i++) {
		((void **)item)[0] = z->zitems;
#ifdef INVARIANTS
		((void **)item)[1] = (void *)ZENTRY_FREE;
#endif
		z->zitems = item;
		item = (uint8_t *)item + z->zsize;
	}
	z->zfreecnt = nitems;
	z->zmax = nitems;
	z->ztotal = nitems;

	lwkt_gettoken(&vm_token);
	LIST_INSERT_HEAD(&zlist, z, zlink);
	lwkt_reltoken(&vm_token);
}

/*
 * Release all resources owned by zone created with zinit().
 *
 * No requirements.
 */
void
zdestroy(vm_zone_t z)
{
	vm_pindex_t i;

	if (z == NULL)
		panic("zdestroy: null zone");
	if ((z->zflags & ZONE_DESTROYABLE) == 0)
		panic("zdestroy: undestroyable zone");

	lwkt_gettoken(&vm_token);
	LIST_REMOVE(z, zlink);
	lwkt_reltoken(&vm_token);

	/*
	 * Release virtual mappings, physical memory and update sysctl stats.
	 */
	KKASSERT((z->zflags & ZONE_INTERRUPT) == 0);
	for (i = 0; i < z->zkmcur; i++) {
		kmem_free(kernel_map, z->zkmvec[i],
			  (size_t)z->zalloc * PAGE_SIZE);
		atomic_subtract_long(&zone_kern_pages, z->zalloc);
	}
	if (z->zkmvec != NULL)
		kfree(z->zkmvec, M_ZONE);

	spin_uninit(&z->zspin);
	kfree(z, M_ZONE);
}


/*
 * void *zalloc(vm_zone_t zone) --
 *	Returns an item from a specified zone.  May not be called from a
 *	FAST interrupt or IPI function.
 *
 * void zfree(vm_zone_t zone, void *item) --
 *	Frees an item back to a specified zone.  May not be called from a
 *	FAST interrupt or IPI function.
 */

/*
 * Internal zone routine.  Not to be called from external (non vm_zone) code.
 *
 * This function may return NULL.
 *
 * No requirements.
 */
static void *
zget(vm_zone_t z, int *tryagainp)
{
	vm_page_t pgs[ZONE_MAXPGLOAD];
	vm_page_t m;
	long nitems;
	long savezpc;
	size_t nbytes;
	size_t noffset;
	void *item;
	vm_pindex_t npages;
	vm_pindex_t nalloc;
	vm_pindex_t i;

	if (z == NULL)
		panic("zget: null zone");

	/*
	 * We need an encompassing per-zone lock for zget() refills.
	 *
	 * Without this we wind up locking on the vm_map inside kmem_alloc*()
	 * prior to any entries actually being added to the zone, potentially
	 * exhausting the per-cpu cache of vm_map_entry's when multiple threads
	 * are blocked on the same lock on the same cpu.
	 */
	if ((z->zflags & ZONE_INTERRUPT) == 0) {
		if (lockmgr(&z->zgetlk, LK_EXCLUSIVE | LK_SLEEPFAIL)) {
			*tryagainp = 1;
			return NULL;
		}
	}

	if (z->zflags & ZONE_INTERRUPT) {
		/*
		 * Interrupt zones do not mess with the kernel_map, they
		 * simply populate an existing mapping.
		 *
		 * First allocate as many pages as we can, stopping at
		 * our limit or if the page allocation fails.  Try to
		 * avoid exhausting the interrupt free minimum by backing
		 * off to normal page allocations after a certain point.
		 */
		for (i = 0; i < ZONE_MAXPGLOAD && i < z->zalloc; ++i) {
			if (i < 4) {
				m = vm_page_alloc(NULL,
						  mycpu->gd_rand_incr++,
						  z->zallocflag);
			} else {
				m = vm_page_alloc(NULL,
						  mycpu->gd_rand_incr++,
						  VM_ALLOC_NORMAL |
						  VM_ALLOC_SYSTEM);
			}
			if (m == NULL)
				break;
			pgs[i] = m;
		}
		nalloc = i;

		/*
		 * Account for the pages.
		 *
		 * NOTE! Do not allow overlap with a prior page as it
		 *	 may still be undergoing allocation on another
		 *	 cpu.
		 */
		spin_lock(&z->zspin);
		noffset = (size_t)z->zpagecount * PAGE_SIZE;
		/* noffset -= noffset % z->zsize; */
		savezpc = z->zpagecount;

		/*
		 * Track total memory use and kmem offset.
		 */
		if (z->zpagecount + nalloc > z->zpagemax)
			z->zpagecount = z->zpagemax;
		else
			z->zpagecount += nalloc;

		item = (char *)z->zkva + noffset;
		npages = z->zpagecount - savezpc;
		nitems = ((size_t)(savezpc + npages) * PAGE_SIZE - noffset) /
			 z->zsize;
		atomic_add_long(&zone_kmem_pages, npages);
		spin_unlock(&z->zspin);

		/*
		 * Enter the pages into the reserved KVA space.
		 */
		for (i = 0; i < npages; ++i) {
			vm_offset_t zkva;

			m = pgs[i];
			KKASSERT(m->queue == PQ_NONE);
			m->valid = VM_PAGE_BITS_ALL;
			vm_page_wire(m);
			vm_page_wakeup(m);

			zkva = z->zkva + (size_t)(savezpc + i) * PAGE_SIZE;
			pmap_kenter(zkva, VM_PAGE_TO_PHYS(m));
			bzero((void *)zkva, PAGE_SIZE);
		}
		for (i = npages; i < nalloc; ++i) {
			m = pgs[i];
			vm_page_free(m);
		}
	} else if (z->zflags & ZONE_SPECIAL) {
		/*
		 * The special zone is the one used for vm_map_entry_t's.
		 * We have to avoid an infinite recursion in 
		 * vm_map_entry_reserve() by using vm_map_entry_kreserve()
		 * instead.  The map entries are pre-reserved by the kernel
		 * by vm_map_entry_reserve_cpu_init().
		 */
		nbytes = (size_t)z->zalloc * PAGE_SIZE;
		z->zpagecount += z->zalloc;	/* Track total memory use */

		item = (void *)kmem_alloc3(kernel_map, nbytes,
					   VM_SUBSYS_ZALLOC, KM_KRESERVE);

		/* note: z might be modified due to blocking */
		if (item != NULL) {
			atomic_add_long(&zone_kern_pages, z->zalloc);
			bzero(item, nbytes);
		} else {
			nbytes = 0;
		}
		nitems = nbytes / z->zsize;
	} else {
		/*
		 * Otherwise allocate KVA from the kernel_map.
		 */
		nbytes = (size_t)z->zalloc * PAGE_SIZE;
		z->zpagecount += z->zalloc;	/* Track total memory use */

		item = (void *)kmem_alloc3(kernel_map, nbytes,
					   VM_SUBSYS_ZALLOC, 0);

		/* note: z might be modified due to blocking */
		if (item != NULL) {
			atomic_add_long(&zone_kern_pages, z->zalloc);
			bzero(item, nbytes);

			if (z->zflags & ZONE_DESTROYABLE) {
				if (z->zkmcur == z->zkmmax) {
					z->zkmmax =
						z->zkmmax==0 ? 1 : z->zkmmax*2;
					z->zkmvec = krealloc(z->zkmvec,
					    z->zkmmax * sizeof(z->zkmvec[0]),
					    M_ZONE, M_WAITOK);
				}
				z->zkmvec[z->zkmcur++] = (vm_offset_t)item;
			}
		} else {
			nbytes = 0;
		}
		nitems = nbytes / z->zsize;
	}

	/*
	 * Enter any new pages into the pool, reserving one, or get the
	 * item from the existing pool.
	 */
	spin_lock(&z->zspin);
	z->ztotal += nitems;

	/*
	 * The zone code may need to allocate kernel memory, which can
	 * recurse zget() infinitely if we do not handle it properly.
	 * We deal with this by directly repopulating the pcpu vm_map_entry
	 * cache.
	 */
	if (nitems > 1 && (z->zflags & ZONE_SPECIAL)) {
		struct globaldata *gd = mycpu;
		vm_map_entry_t entry;

		/*
		 * Make sure we have enough structures in gd_vme_base to handle
		 * the reservation request.
		 *
		 * The critical section protects access to the per-cpu gd.
		 */
		crit_enter();
		while (gd->gd_vme_avail < 2 && nitems > 1) {
			entry = item;
			MAPENT_FREELIST(entry) = gd->gd_vme_base;
			gd->gd_vme_base = entry;
			atomic_add_int(&gd->gd_vme_avail, 1);
			item = (uint8_t *)item + z->zsize;
			--nitems;
		}
		crit_exit();
	}

	if (nitems != 0) {
		/*
		 * Enter pages into the pool saving one for immediate
		 * allocation.
		 */
		nitems -= 1;
		for (i = 0; i < nitems; i++) {
			((void **)item)[0] = z->zitems;
#ifdef INVARIANTS
			((void **)item)[1] = (void *)ZENTRY_FREE;
#endif
			z->zitems = item;
			item = (uint8_t *)item + z->zsize;
		}
		z->zfreecnt += nitems;
		++z->znalloc;
	} else if (z->zfreecnt > 0) {
		/*
		 * Get an item from the existing pool.
		 */
		item = z->zitems;
		z->zitems = ((void **)item)[0];
#ifdef INVARIANTS
		if (((void **)item)[1] != (void *)ZENTRY_FREE)
			zerror(ZONE_ERROR_NOTFREE);
		((void **) item)[1] = NULL;
#endif
		--z->zfreecnt;
		++z->znalloc;
	} else {
		/*
		 * No items available.
		 */
		item = NULL;
	}
	spin_unlock(&z->zspin);

	/*
	 * Release the per-zone global lock after the items have been
	 * added.  Any other threads blocked in zget()'s zgetlk will
	 * then retry rather than potentially exhaust the per-cpu cache
	 * of vm_map_entry structures doing their own kmem_alloc() calls,
	 * or allocating excessive amounts of space unnecessarily.
	 */
	if ((z->zflags & ZONE_INTERRUPT) == 0)
		lockmgr(&z->zgetlk, LK_RELEASE);

	return item;
}

/*
 * No requirements.
 */
static int
sysctl_vm_zone(SYSCTL_HANDLER_ARGS)
{
	vm_zone_t curzone;
	char tmpbuf[128];
	char tmpname[14];
	int error = 0;

	ksnprintf(tmpbuf, sizeof(tmpbuf),
	    "\nITEM            SIZE     LIMIT    USED    FREE  REQUESTS\n");
	error = SYSCTL_OUT(req, tmpbuf, strlen(tmpbuf));
	if (error)
		return (error);

	lwkt_gettoken(&vm_token);
	LIST_FOREACH(curzone, &zlist, zlink) {
		size_t i;
		size_t len;
		int offset;
		long freecnt;
		long znalloc;
		int n;

		len = strlen(curzone->zname);
		if (len >= (sizeof(tmpname) - 1))
			len = (sizeof(tmpname) - 1);
		for(i = 0; i < sizeof(tmpname) - 1; i++)
			tmpname[i] = ' ';
		tmpname[i] = 0;
		memcpy(tmpname, curzone->zname, len);
		tmpname[len] = ':';
		offset = 0;
		if (curzone == LIST_FIRST(&zlist)) {
			offset = 1;
			tmpbuf[0] = '\n';
		}
		freecnt = curzone->zfreecnt;
		znalloc = curzone->znalloc;
		for (n = 0; n < ncpus; ++n) {
			freecnt += curzone->zpcpu[n].zfreecnt;
			znalloc += curzone->zpcpu[n].znalloc;
		}

		ksnprintf(tmpbuf + offset, sizeof(tmpbuf) - offset,
			"%s %6.6lu, %8.8lu, %6.6lu, %6.6lu, %8.8lu\n",
			tmpname, curzone->zsize, curzone->zmax,
			(curzone->ztotal - freecnt),
			freecnt, znalloc);

		len = strlen((char *)tmpbuf);
		if (LIST_NEXT(curzone, zlink) == NULL)
			tmpbuf[len - 1] = 0;

		error = SYSCTL_OUT(req, tmpbuf, len);

		if (error)
			break;
	}
	lwkt_reltoken(&vm_token);
	return (error);
}

#if defined(INVARIANTS)

/*
 * Debugging only.
 */
void
zerror(int error)
{
	char *msg;

	switch (error) {
	case ZONE_ERROR_INVALID:
		msg = "zone: invalid zone";
		break;
	case ZONE_ERROR_NOTFREE:
		msg = "zone: entry not free";
		break;
	case ZONE_ERROR_ALREADYFREE:
		msg = "zone: freeing free entry";
		break;
	default:
		msg = "zone: invalid error";
		break;
	}
	panic("%s", msg);
}
#endif

SYSCTL_OID(_vm, OID_AUTO, zone, CTLTYPE_STRING|CTLFLAG_RD, \
	NULL, 0, sysctl_vm_zone, "A", "Zone Info");

SYSCTL_LONG(_vm, OID_AUTO, zone_kmem_pages,
	CTLFLAG_RD, &zone_kmem_pages, 0, "Number of interrupt safe pages allocated by zone");
SYSCTL_LONG(_vm, OID_AUTO, zone_burst,
	CTLFLAG_RW, &zone_burst, 0, "Burst from depot to pcpu cache");
SYSCTL_LONG(_vm, OID_AUTO, zone_kmem_kvaspace,
	CTLFLAG_RD, &zone_kmem_kvaspace, 0, "KVA space allocated by zone");
SYSCTL_LONG(_vm, OID_AUTO, zone_kern_pages,
	CTLFLAG_RD, &zone_kern_pages, 0, "Number of non-interrupt safe pages allocated by zone");
