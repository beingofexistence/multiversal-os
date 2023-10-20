/*
 * Copyright (c) 2005 Jeffrey M. Hsu.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Jeffrey M. Hsu.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
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
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/callout.h>
#include <sys/globaldata.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/objcache.h>
#include <sys/spinlock.h>
#include <sys/thread.h>
#include <sys/thread2.h>
#include <sys/spinlock2.h>
#include <sys/sysctl.h>

static MALLOC_DEFINE(M_OBJCACHE, "objcache", "Object Cache");
static MALLOC_DEFINE(M_OBJMAG, "objcache mag", "Object Cache Magazine");

#define	INITIAL_MAG_CAPACITY	64

struct magazine {
	int			 rounds;
	int			 capacity;
	SLIST_ENTRY(magazine)	 nextmagazine;
	void			*objects[];
};

SLIST_HEAD(magazinelist, magazine);

#define MAGAZINE_HDRSIZE	__offsetof(struct magazine, objects[0])
#define MAGAZINE_CAPACITY_MAX	4096
#define MAGAZINE_CAPACITY_MIN	4

/*
 * per-cluster cache of magazines
 *
 * All fields in this structure are protected by the spinlock.
 */
struct magazinedepot {
	/*
	 * The per-cpu object caches only exchanges completely full or
	 * completely empty magazines with the depot layer, so only have
	 * to cache these two types of magazines.
	 */
	struct magazinelist	fullmagazines;
	struct magazinelist	emptymagazines;
	int			magcapacity;

	/* protect this structure */
	struct spinlock		spin;

	/* magazines not yet allocated towards limit */
	int			unallocated_objects;
	int			cluster_limit;	/* ref for adjustments */

	/* infrequently used fields */
	int			waiting;	/* waiting for another cpu to
						 * return a full magazine to
						 * the depot */
	int			contested;	/* depot contention count */
} __cachealign;

/*
 * per-cpu object cache
 * All fields in this structure are protected by crit_enter().
 */
struct percpu_objcache {
	struct magazine	*loaded_magazine;	/* active magazine */
	struct magazine	*previous_magazine;	/* backup magazine */

	/* statistics */
	u_long		gets_cumulative;	/* total calls to get */
	u_long		gets_null;		/* objcache_get returned NULL */
	u_long		allocs_cumulative;	/* total calls to alloc */
	u_long		puts_cumulative;	/* total calls to put */
	u_long		gets_exhausted;		/* # of gets hit exhaustion */
#ifdef notyet
	u_long		puts_othercluster;	/* returned to other cluster */
#endif

	/* infrequently used fields */
	int		waiting;		/* waiting for a thread on this
						 * cpu to return an obj to the
						 * per-cpu cache */
} __cachealign;

/* only until we have NUMA cluster topology information XXX */
#define MAXCLUSTERS 1
#define myclusterid 0
#define CLUSTER_OF(obj) 0

/*
 * Rarely accessed but useful bits of objcache.
 */
struct objcache_desc {
	LIST_ENTRY(objcache_desc)	next;
	struct objcache			*objcache;
	int				total_objects;
	int				reserved;
	char				name[OBJCACHE_NAMELEN];
};

/*
 * Two-level object cache consisting of NUMA cluster-level depots of
 * fully loaded or completely empty magazines and cpu-level caches of
 * individual objects.
 */
struct objcache {
	/* object constructor and destructor from blank storage */
	objcache_ctor_fn	*ctor;
	objcache_dtor_fn	*dtor;
	void			*privdata;

	/* interface to underlying allocator */
	objcache_alloc_fn	*alloc;
	objcache_free_fn	*free;
	void			*allocator_args;

	struct objcache_desc	*desc;

	/* NUMA-cluster level caches */
	struct magazinedepot	depot[MAXCLUSTERS];

	struct percpu_objcache	cache_percpu[];	/* per-cpu caches */
};

SYSCTL_NODE(_kern, OID_AUTO, objcache, CTLFLAG_RW, 0, "objcache");

static struct spinlock objcachelist_spin;
static LIST_HEAD(objcachelist, objcache_desc) allobjcaches;
static int magazine_capmin;
static int magazine_capmax;

static struct magazine *
mag_alloc(int capacity)
{
	struct magazine *mag;
	int size;

	size = __offsetof(struct magazine, objects[capacity]);
	KASSERT(size > 0 && (size & __VM_CACHELINE_MASK) == 0,
	    ("magazine size is not multiple cache line size"));

	mag = kmalloc(size, M_OBJMAG, M_INTWAIT | M_ZERO | M_CACHEALIGN);
	mag->capacity = capacity;
	mag->rounds = 0;
	return (mag);
}

static int
mag_capacity_align(int mag_capacity)
{
	int mag_size;

	mag_size = __VM_CACHELINE_ALIGN(
	    __offsetof(struct magazine, objects[mag_capacity]));
	mag_capacity = (mag_size - MAGAZINE_HDRSIZE) / sizeof(void *);

	return mag_capacity;
}

/*
 * Utility routine for objects that don't require any de-construction.
 */

static void
null_dtor(void *obj, void *privdata)
{
	/* do nothing */
}

static boolean_t
null_ctor(void *obj, void *privdata, int ocflags)
{
	return TRUE;
}

/*
 * Create an object cache.
 */
struct objcache *
objcache_create(const char *name, int cluster_limit, int nom_cache,
		objcache_ctor_fn *ctor, objcache_dtor_fn *dtor, void *privdata,
		objcache_alloc_fn *alloc, objcache_free_fn *free,
		void *allocator_args)
{
	struct objcache_desc *desc;
	struct objcache *oc;
	struct magazinedepot *depot;
	int cpuid;
	int nmagdepot;
	int mag_capacity;
	int i;

	/*
	 * Allocate objcache descriptor.
	 */
	desc = kmalloc(sizeof(*desc), M_OBJCACHE, M_WAITOK | M_ZERO);

	/*
	 * Allocate object cache structure
	 */
	oc = kmalloc(__offsetof(struct objcache, cache_percpu[ncpus]),
		     M_OBJCACHE,
		     M_WAITOK | M_ZERO | M_CACHEALIGN);
	oc->ctor = ctor ? ctor : null_ctor;
	oc->dtor = dtor ? dtor : null_dtor;
	oc->privdata = privdata;
	oc->alloc = alloc;
	oc->free = free;
	oc->allocator_args = allocator_args;

	/*
	 * Link objcache and its descriptor.
	 */
	oc->desc = desc;
	desc->objcache = oc;
	strlcpy(desc->name, name, sizeof(desc->name));

	/*
	 * Initialize depot list(s).
	 */
	depot = &oc->depot[0];

	spin_init(&depot->spin, "objcachedepot");
	SLIST_INIT(&depot->fullmagazines);
	SLIST_INIT(&depot->emptymagazines);

	/*
	 * Figure out the nominal number of free objects to cache and
	 * the magazine capacity.  By default we want to cache up to
	 * half the cluster_limit.  If there is no cluster_limit then
	 * we want to cache up to 128 objects.
	 */
	if (nom_cache == 0)
		nom_cache = cluster_limit / 2;
	if (cluster_limit && nom_cache > cluster_limit)
		nom_cache = cluster_limit;
	if (nom_cache == 0)
		nom_cache = INITIAL_MAG_CAPACITY * 2;

	/*
	 * Magazine capacity for 2 active magazines per cpu plus 2
	 * magazines in the depot.
	 */
	mag_capacity = mag_capacity_align(nom_cache / (ncpus + 1) / 2 + 1);
	if (mag_capacity > magazine_capmax)
		mag_capacity = magazine_capmax;
	else if (mag_capacity < magazine_capmin)
		mag_capacity = magazine_capmin;
	depot->magcapacity = mag_capacity;

	/*
	 * The cluster_limit must be sufficient to have two magazines per
	 * cpu plus at least two magazines in the depot.  However, because
	 * partial magazines can stay on the cpus what we really need here
	 * is to specify the number of extra magazines we allocate for the
	 * depot.
	 *
	 * Use ~1B objects to mean 'unlimited'.  A negative unallocated
	 * object count is possible due to dynamic adjustments so we can't
	 * use a negative number to mean 'unlimited'.  We need some overflow
	 * capacity too due to the preallocated mags.
	 */
	if (cluster_limit == 0) {
		depot->unallocated_objects = OBJCACHE_UNLIMITED;
	} else {
		depot->unallocated_objects = ncpus * mag_capacity * 2 +
					     cluster_limit;
	}

	/* Save # of total objects. */
	desc->total_objects = depot->unallocated_objects;

	/*
	 * This is a dynamic adjustment aid initialized to the callers
	 * expectations of the current limit.
	 */
	depot->cluster_limit = cluster_limit;

	/*
	 * Initialize per-cpu caches
	 */
	for (cpuid = 0; cpuid < ncpus; cpuid++) {
		struct percpu_objcache *cache_percpu = &oc->cache_percpu[cpuid];

		cache_percpu->loaded_magazine = mag_alloc(mag_capacity);
		cache_percpu->previous_magazine = mag_alloc(mag_capacity);
	}

	/*
	 * Compute how many empty magazines to place in the depot.  This
	 * determines the retained cache size and is based on nom_cache.
	 *
	 * The actual cache size is larger because there are two magazines
	 * for each cpu as well but those can be in any fill state so we
	 * just can't count them.
	 *
	 * There is a minimum of two magazines in the depot.
	 */
	nmagdepot = nom_cache / mag_capacity + 1;
	if (nmagdepot < 2)
		nmagdepot = 2;

	/*
	 * Put empty magazines in depot
	 */
	for (i = 0; i < nmagdepot; i++) {
		struct magazine *mag = mag_alloc(mag_capacity);
		SLIST_INSERT_HEAD(&depot->emptymagazines, mag, nextmagazine);
	}

	spin_lock(&objcachelist_spin);
	LIST_INSERT_HEAD(&allobjcaches, desc, next);
	spin_unlock(&objcachelist_spin);

	return (oc);
}

/*
 * Adjust the cluster limit.  This is allowed to cause unallocated_objects
 * to go negative.  Note that due to the magazine hysteresis there is a
 * limit to how much of the objcache can be reclaimed using this API to
 * reduce its size.
 */
void
objcache_set_cluster_limit(struct objcache *oc, int cluster_limit)
{
	struct magazinedepot *depot;

	depot = &oc->depot[myclusterid];
	if (depot->cluster_limit != cluster_limit) {
		int delta;

		spin_lock(&depot->spin);
		delta = cluster_limit - depot->cluster_limit;
		depot->unallocated_objects += delta;
		depot->cluster_limit = cluster_limit;
		spin_unlock(&depot->spin);
		wakeup(depot);

		oc->desc->total_objects += delta;
	}
}

struct objcache *
objcache_create_simple(malloc_type_t mtype, size_t objsize)
{
	struct objcache_malloc_args *margs;
	struct objcache *oc;

	margs = kmalloc(sizeof(*margs), M_OBJCACHE, M_WAITOK|M_ZERO);
	margs->objsize = objsize;
	margs->mtype = mtype;
	oc = objcache_create(mtype->ks_shortdesc, 0, 0,
			     NULL, NULL, NULL,
			     objcache_malloc_alloc, objcache_malloc_free,
			     margs);
	return (oc);
}

struct objcache *
objcache_create_mbacked(malloc_type_t mtype, size_t objsize,
			int cluster_limit, int nom_cache,
			objcache_ctor_fn *ctor, objcache_dtor_fn *dtor,
			void *privdata)
{
	struct objcache_malloc_args *margs;
	struct objcache *oc;

	margs = kmalloc(sizeof(*margs), M_OBJCACHE, M_WAITOK|M_ZERO);
	margs->objsize = objsize;
	margs->mtype = mtype;
	oc = objcache_create(mtype->ks_shortdesc,
			     cluster_limit, nom_cache,
			     ctor, dtor, privdata,
			     objcache_malloc_alloc, objcache_malloc_free,
			     margs);
	return(oc);
}


#define MAGAZINE_EMPTY(mag)	(mag->rounds == 0)
#define MAGAZINE_NOTEMPTY(mag)	(mag->rounds != 0)
#define MAGAZINE_FULL(mag)	(mag->rounds == mag->capacity)

#define	swap(x, y)	({ struct magazine *t = x; x = y; y = t; })

/*
 * Get an object from the object cache.
 *
 * WARNING!  ocflags are only used when we have to go to the underlying
 * allocator, so we cannot depend on flags such as M_ZERO.
 */
void *
objcache_get(struct objcache *oc, int ocflags)
{
	struct percpu_objcache *cpucache = &oc->cache_percpu[mycpuid];
	struct magazine *loadedmag;
	struct magazine *emptymag;
	void *obj;
	struct magazinedepot *depot;

	KKASSERT((ocflags & M_ZERO) == 0);
	crit_enter();
	++cpucache->gets_cumulative;

retry:
	/*
	 * Loaded magazine has an object.  This is the hot path.
	 * It is lock-free and uses a critical section to block
	 * out interrupt handlers on the same processor.
	 */
	loadedmag = cpucache->loaded_magazine;
	if (MAGAZINE_NOTEMPTY(loadedmag)) {
		obj = loadedmag->objects[--loadedmag->rounds];
		crit_exit();
		return (obj);
	}

	/* Previous magazine has an object. */
	if (MAGAZINE_NOTEMPTY(cpucache->previous_magazine)) {
		swap(cpucache->loaded_magazine, cpucache->previous_magazine);
		loadedmag = cpucache->loaded_magazine;
		obj = loadedmag->objects[--loadedmag->rounds];
		crit_exit();
		return (obj);
	}

	/*
	 * Both magazines empty.  Get a full magazine from the depot and
	 * move one of the empty ones to the depot.
	 *
	 * Obtain the depot spinlock.
	 *
	 * NOTE: Beyond this point, M_* flags are handled via oc->alloc()
	 */
	depot = &oc->depot[myclusterid];
	spin_lock(&depot->spin);

	/*
	 * Recheck the cpucache after obtaining the depot spinlock.  This
	 * shouldn't be necessary now but don't take any chances.
	 */
	if (MAGAZINE_NOTEMPTY(cpucache->loaded_magazine) ||
	    MAGAZINE_NOTEMPTY(cpucache->previous_magazine)
	) {
		spin_unlock(&depot->spin);
		goto retry;
	}

	/* Check if depot has a full magazine. */
	if (!SLIST_EMPTY(&depot->fullmagazines)) {
		emptymag = cpucache->previous_magazine;
		cpucache->previous_magazine = cpucache->loaded_magazine;
		cpucache->loaded_magazine = SLIST_FIRST(&depot->fullmagazines);
		SLIST_REMOVE_HEAD(&depot->fullmagazines, nextmagazine);

		/*
		 * Return emptymag to the depot.
		 */
		KKASSERT(MAGAZINE_EMPTY(emptymag));
		SLIST_INSERT_HEAD(&depot->emptymagazines,
				  emptymag, nextmagazine);
		spin_unlock(&depot->spin);
		goto retry;
	}

	/*
	 * The depot does not have any non-empty magazines.  If we have
	 * not hit our object limit we can allocate a new object using
	 * the back-end allocator.
	 *
	 * NOTE: unallocated_objects can wind up being negative due to
	 *	 objcache_set_cluster_limit() calls.
	 */
	if (__predict_true(depot->unallocated_objects > 0)) {
		--depot->unallocated_objects;
		spin_unlock(&depot->spin);
		++cpucache->allocs_cumulative;
		crit_exit();

		obj = oc->alloc(oc->allocator_args, ocflags);
		if (obj) {
			if (oc->ctor(obj, oc->privdata, ocflags))
				return (obj);
			oc->free(obj, oc->allocator_args);
			obj = NULL;
		}
		if (obj == NULL) {
			spin_lock(&depot->spin);
			++depot->unallocated_objects;
			spin_unlock(&depot->spin);
			if (depot->waiting)
				wakeup(depot);

			crit_enter();
			/*
			 * makes debugging easier when gets_cumulative does
			 * not include gets_null.
			 */
			++cpucache->gets_null;
			--cpucache->gets_cumulative;
			crit_exit();
		}
		return(obj);
	}
	if (__predict_false(cpucache->gets_exhausted++ == 0)) {
		kprintf("Warning: objcache(%s) exhausted on cpu%d!\n",
		    oc->desc->name, mycpuid);
	}

	/*
	 * Otherwise block if allowed to.
	 */
	if ((ocflags & (M_WAITOK|M_NULLOK)) == M_WAITOK) {
		++cpucache->waiting;
		++depot->waiting;
		ssleep(depot, &depot->spin, 0, "objcache_get", 0);
		--cpucache->waiting;
		--depot->waiting;
		spin_unlock(&depot->spin);
		goto retry;
	}

	/*
	 * Otherwise fail
	 */
	++cpucache->gets_null;
	--cpucache->gets_cumulative;
	crit_exit();
	spin_unlock(&depot->spin);
	return (NULL);
}

/*
 * Wrapper for malloc allocation routines.
 */
void *
objcache_malloc_alloc(void *allocator_args, int ocflags)
{
	struct objcache_malloc_args *alloc_args = allocator_args;

	return (kmalloc(alloc_args->objsize, alloc_args->mtype,
		       ocflags & OC_MFLAGS));
}

/*
 * Wrapper for malloc allocation routines, with initial zeroing
 * (but objects are not zerod on reuse from cache).
 */
void *
objcache_malloc_alloc_zero(void *allocator_args, int ocflags)
{
	struct objcache_malloc_args *alloc_args = allocator_args;

	return (kmalloc(alloc_args->objsize, alloc_args->mtype,
		       (ocflags & OC_MFLAGS) | M_ZERO));
}


void
objcache_malloc_free(void *obj, void *allocator_args)
{
	struct objcache_malloc_args *alloc_args = allocator_args;

	kfree(obj, alloc_args->mtype);
}

/*
 * Wrapper for allocation policies that pre-allocate at initialization time
 * and don't do run-time allocation.
 */
void *
objcache_nop_alloc(void *allocator_args, int ocflags)
{
	return (NULL);
}

void
objcache_nop_free(void *obj, void *allocator_args)
{
}

/*
 * Return an object to the object cache.
 */
void
objcache_put(struct objcache *oc, void *obj)
{
	struct percpu_objcache *cpucache = &oc->cache_percpu[mycpuid];
	struct magazine *loadedmag;
	struct magazinedepot *depot;

	crit_enter();
	++cpucache->puts_cumulative;

	if (CLUSTER_OF(obj) != myclusterid) {
#ifdef notyet
		/* use lazy IPI to send object to owning cluster XXX todo */
		++cpucache->puts_othercluster;
		crit_exit();
		return;
#endif
	}

retry:
	/*
	 * Free slot available in loaded magazine.  This is the hot path.
	 * It is lock-free and uses a critical section to block out interrupt
	 * handlers on the same processor.
	 */
	loadedmag = cpucache->loaded_magazine;
	if (!MAGAZINE_FULL(loadedmag)) {
		loadedmag->objects[loadedmag->rounds++] = obj;
		if (cpucache->waiting)
			wakeup_mycpu(&oc->depot[myclusterid]);
		crit_exit();
		return;
	}

	/*
	 * Current magazine full, but previous magazine has room.  XXX
	 */
	if (!MAGAZINE_FULL(cpucache->previous_magazine)) {
		swap(cpucache->loaded_magazine, cpucache->previous_magazine);
		loadedmag = cpucache->loaded_magazine;
		loadedmag->objects[loadedmag->rounds++] = obj;
		if (cpucache->waiting)
			wakeup_mycpu(&oc->depot[myclusterid]);
		crit_exit();
		return;
	}

	/*
	 * Both magazines full.  Get an empty magazine from the depot and
	 * move a full loaded magazine to the depot.  Even though the
	 * magazine may wind up with space available after we block on
	 * the spinlock, we still cycle it through to avoid the non-optimal
	 * corner-case.
	 *
	 * Obtain the depot spinlock.
	 */
	depot = &oc->depot[myclusterid];
	spin_lock(&depot->spin);

	/*
	 * If an empty magazine is available in the depot, cycle it
	 * through and retry.
	 */
	if (!SLIST_EMPTY(&depot->emptymagazines)) {
		loadedmag = cpucache->previous_magazine;
		cpucache->previous_magazine = cpucache->loaded_magazine;
		cpucache->loaded_magazine = SLIST_FIRST(&depot->emptymagazines);
		SLIST_REMOVE_HEAD(&depot->emptymagazines, nextmagazine);

		/*
		 * Return loadedmag to the depot.  Due to blocking it may
		 * not be entirely full and could even be empty.
		 */
		if (MAGAZINE_EMPTY(loadedmag)) {
			SLIST_INSERT_HEAD(&depot->emptymagazines,
					  loadedmag, nextmagazine);
			spin_unlock(&depot->spin);
		} else {
			SLIST_INSERT_HEAD(&depot->fullmagazines,
					  loadedmag, nextmagazine);
			spin_unlock(&depot->spin);
			if (depot->waiting)
				wakeup(depot);
		}
		goto retry;
	}

	/*
	 * An empty mag is not available.  This is a corner case which can
	 * occur due to cpus holding partially full magazines.  Do not try
	 * to allocate a mag, just free the object.
	 */
	++depot->unallocated_objects;
	spin_unlock(&depot->spin);
	if (depot->waiting)
		wakeup(depot);
	crit_exit();
	oc->dtor(obj, oc->privdata);
	oc->free(obj, oc->allocator_args);
}

/*
 * The object is being put back into the cache, but the caller has
 * indicated that the object is not in any shape to be reused and should
 * be dtor'd immediately.
 */
void
objcache_dtor(struct objcache *oc, void *obj)
{
	struct magazinedepot *depot;

	depot = &oc->depot[myclusterid];
	spin_lock(&depot->spin);
	++depot->unallocated_objects;
	spin_unlock(&depot->spin);
	if (depot->waiting)
		wakeup(depot);
	oc->dtor(obj, oc->privdata);
	oc->free(obj, oc->allocator_args);
}

/*
 * Deallocate all objects in a magazine and free the magazine if requested.
 * When freeit is TRUE the magazine must already be disassociated from the
 * depot.
 *
 * Must be called with a critical section held when called with a per-cpu
 * magazine.  The magazine may be indirectly modified during the loop.
 *
 * If the magazine moves during a dtor the operation is aborted.  This is
 * only allowed when freeit is FALSE.
 *
 * The number of objects freed is returned.
 */
static int
mag_purge(struct objcache *oc, struct magazine **magp, int freeit)
{
	struct magazine *mag = *magp;
	int count;
	void *obj;

	count = 0;
	while (mag->rounds) {
		obj = mag->objects[--mag->rounds];
		oc->dtor(obj, oc->privdata);		/* MAY BLOCK */
		oc->free(obj, oc->allocator_args);	/* MAY BLOCK */
		++count;

		/*
		 * Cycle for interrupts.
		 */
		if ((count & 15) == 0) {
			crit_exit();
			crit_enter();
		}

		/*
		 * mag may have become invalid either due to dtor/free
		 * blocking or interrupt cycling, do not derefernce it
		 * until we check.
		 */
		if (*magp != mag) {
			kprintf("mag_purge: mag ripped out\n");
			break;
		}
	}
	if (freeit) {
		KKASSERT(*magp == mag);
		*magp = NULL;
		kfree(mag, M_OBJMAG);
	}
	return(count);
}

/*
 * Disassociate zero or more magazines from a magazine list associated with
 * the depot, update the depot, and move the magazines to a temporary
 * list.
 *
 * The caller must check the depot for waiters and wake it up, typically
 * after disposing of the magazines this function loads onto the temporary
 * list.
 */
static void
maglist_disassociate(struct magazinedepot *depot, struct magazinelist *maglist,
		     struct magazinelist *tmplist, boolean_t purgeall)
{
	struct magazine *mag;

	while ((mag = SLIST_FIRST(maglist)) != NULL) {
		SLIST_REMOVE_HEAD(maglist, nextmagazine);
		SLIST_INSERT_HEAD(tmplist, mag, nextmagazine);
		depot->unallocated_objects += mag->rounds;
	}
}
			
/*
 * Deallocate all magazines and their contents from the passed temporary
 * list.  The magazines have already been accounted for by their depots.
 *
 * The total number of rounds freed is returned.  This number is typically
 * only used to determine whether a wakeup on the depot is needed or not.
 */
static int
maglist_purge(struct objcache *oc, struct magazinelist *maglist)
{
	struct magazine *mag;
	int count = 0;

	/*
	 * can't use SLIST_FOREACH because blocking releases the depot
	 * spinlock 
	 */
	crit_enter();
	while ((mag = SLIST_FIRST(maglist)) != NULL) {
		SLIST_REMOVE_HEAD(maglist, nextmagazine);
		count += mag_purge(oc, &mag, TRUE);
	}
	crit_exit();
	return(count);
}

/*
 * De-allocates all magazines on the full and empty magazine lists.
 *
 * Because this routine is called with a spinlock held, the magazines
 * can only be disassociated and moved to a temporary list, not freed.
 *
 * The caller is responsible for freeing the magazines.
 */
static void
depot_disassociate(struct magazinedepot *depot, struct magazinelist *tmplist)
{
	maglist_disassociate(depot, &depot->fullmagazines, tmplist, TRUE);
	maglist_disassociate(depot, &depot->emptymagazines, tmplist, TRUE);
}

/*
 * Try to free up some memory.  Return as soon as some free memory is found.
 * For each object cache on the reclaim list, first try the current per-cpu
 * cache, then the full magazine depot.
 */
boolean_t
objcache_reclaimlist(struct objcache *oclist[], int nlist, int ocflags)
{
	struct objcache *oc;
	struct percpu_objcache *cpucache;
	struct magazinedepot *depot;
	struct magazinelist tmplist;
	int i, count;

	SLIST_INIT(&tmplist);

	for (i = 0; i < nlist; i++) {
		oc = oclist[i];
		cpucache = &oc->cache_percpu[mycpuid];
		depot = &oc->depot[myclusterid];

		crit_enter();
		count = mag_purge(oc, &cpucache->loaded_magazine, FALSE);
		if (count == 0)
			count += mag_purge(oc, &cpucache->previous_magazine, FALSE);
		crit_exit();
		if (count > 0) {
			spin_lock(&depot->spin);
			depot->unallocated_objects += count;
			spin_unlock(&depot->spin);
			if (depot->waiting)
				wakeup(depot);
			return (TRUE);
		}
		spin_lock(&depot->spin);
		maglist_disassociate(depot, &depot->fullmagazines,
				     &tmplist, FALSE);
		spin_unlock(&depot->spin);
		count = maglist_purge(oc, &tmplist);
		if (count > 0) {
			if (depot->waiting)
				wakeup(depot);
			return (TRUE);
		}
	}
	return (FALSE);
}

/*
 * Destroy an object cache.  Must have no existing references.
 */
void
objcache_destroy(struct objcache *oc)
{
	struct objcache_desc *desc = oc->desc;
	struct percpu_objcache *cache_percpu;
	struct magazinedepot *depot;
	int clusterid, cpuid;
	struct magazinelist tmplist;

	spin_lock(&objcachelist_spin);
	LIST_REMOVE(desc, next);
	spin_unlock(&objcachelist_spin);

	SLIST_INIT(&tmplist);
	for (clusterid = 0; clusterid < MAXCLUSTERS; clusterid++) {
		depot = &oc->depot[clusterid];
		spin_lock(&depot->spin);
		depot_disassociate(depot, &tmplist);
		spin_unlock(&depot->spin);
	}
	maglist_purge(oc, &tmplist);

	for (cpuid = 0; cpuid < ncpus; cpuid++) {
		cache_percpu = &oc->cache_percpu[cpuid];

		crit_enter();
		mag_purge(oc, &cache_percpu->loaded_magazine, TRUE);
		mag_purge(oc, &cache_percpu->previous_magazine, TRUE);
		crit_exit();
		cache_percpu->loaded_magazine = NULL;
		cache_percpu->previous_magazine = NULL;
		/* don't bother adjusting depot->unallocated_objects */
	}

	kfree(desc, M_OBJCACHE);
	kfree(oc, M_OBJCACHE);
}

static int
sysctl_ocstats(SYSCTL_HANDLER_ARGS)
{
	struct objcache_stats stat;
	struct objcache_desc marker, *desc;
	int error;

	memset(&marker, 0, sizeof(marker));

	spin_lock(&objcachelist_spin);

	LIST_INSERT_HEAD(&allobjcaches, &marker, next);
	while ((desc = LIST_NEXT(&marker, next)) != NULL) {
		u_long puts, unalloc;
		int cpu;

		LIST_REMOVE(&marker, next);
		LIST_INSERT_AFTER(desc, &marker, next);

		if (desc->total_objects == 0) {
			/* Marker inserted by another thread. */
			continue;
		}

		memset(&stat, 0, sizeof(stat));
		strlcpy(stat.oc_name, desc->name, sizeof(stat.oc_name));
		stat.oc_limit = desc->total_objects;
		/* XXX domain aware */
		unalloc = desc->objcache->depot[0].unallocated_objects;

		puts = 0;
		for (cpu = 0; cpu < ncpus; ++cpu) {
			const struct percpu_objcache *cache;

			cache = &desc->objcache->cache_percpu[cpu];
			puts += cache->puts_cumulative;

			stat.oc_requested += cache->gets_cumulative;
			stat.oc_exhausted += cache->gets_exhausted;
			stat.oc_failed += cache->gets_null;
			stat.oc_allocated += cache->allocs_cumulative;
		}
		spin_unlock(&objcachelist_spin);

		/*
		 * Apply fixup.
		 */
		if (stat.oc_requested > puts)
			stat.oc_used = stat.oc_requested - puts;
		if (stat.oc_limit > unalloc + stat.oc_used) {
			stat.oc_cached = stat.oc_limit -
			    (unalloc + stat.oc_used);
		}
		stat.oc_requested += stat.oc_failed;

		/* Send out. */
		error = SYSCTL_OUT(req, &stat, sizeof(stat));

		/* Hold the lock before we return. */
		spin_lock(&objcachelist_spin);

		if (error)
			break;
	}
	LIST_REMOVE(&marker, next);

	spin_unlock(&objcachelist_spin);

	return error;
}
SYSCTL_PROC(_kern_objcache, OID_AUTO, stats, (CTLTYPE_OPAQUE | CTLFLAG_RD),
    0, 0, sysctl_ocstats, "S,objcache_stats", "objcache statistics");

static void
objcache_init(void)
{
	spin_init(&objcachelist_spin, "objcachelist");

	magazine_capmin = mag_capacity_align(MAGAZINE_CAPACITY_MIN);
	magazine_capmax = mag_capacity_align(MAGAZINE_CAPACITY_MAX);
	if (bootverbose) {
		kprintf("objcache: magazine cap [%d, %d]\n",
		    magazine_capmin, magazine_capmax);
	}
#if 0
	callout_init_mp(&objcache_callout);
	objcache_rebalance_period = 60 * hz;
	callout_reset(&objcache_callout, objcache_rebalance_period,
		      objcache_timer, NULL);
#endif
}
SYSINIT(objcache, SI_BOOT2_OBJCACHE, SI_ORDER_FIRST, objcache_init, 0);
