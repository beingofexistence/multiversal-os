/*        $NetBSD: dm_target_linear.c,v 1.9 2010/01/04 00:14:41 haad Exp $      */

/*
 * Copyright (c) 2008 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Adam Hamsik.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


/*
 * This file implements initial version of device-mapper dklinear target.
 */

#include <dev/disk/dm/dm.h>
#include <sys/malloc.h>		/* for malloc macros, dm.h includes sys/param.h */

MALLOC_DEFINE(M_DMLINEAR, "dm_linear", "Device Mapper Target Linear");

typedef struct target_linear_config {
	dm_pdev_t *pdev;
	uint64_t offset;
} dm_target_linear_config_t;

/*
 * Allocate target specific config data, and link them to table.
 * This function is called only when, flags is not READONLY and
 * therefore we can add things to pdev list. This should not a
 * problem because this routine is called only from dm_table_load_ioctl.
 * @argv[0] is name,
 * @argv[1] is physical data offset.
 */
static int
dm_target_linear_init(dm_table_entry_t *table_en, int argc, char **argv)
{
	dm_target_linear_config_t *tlc;
	dm_pdev_t *dmp;

	if (argc != 2) {
		kprintf("Linear target takes 2 args\n");
		return EINVAL;
	}

	/* Insert dmp to global pdev list */
	if ((dmp = dm_pdev_insert(argv[0])) == NULL)
		return ENOENT;

	if ((tlc = kmalloc(sizeof(dm_target_linear_config_t), M_DMLINEAR, M_WAITOK))
	    == NULL)
		return ENOMEM;

	tlc->pdev = dmp;
	tlc->offset = atoi64(argv[1]);

	dm_table_add_deps(table_en, dmp);

	dm_table_init_target(table_en, tlc);

	return 0;
}

/*
 * Table routine is called to get params string, which is target
 * specific. When dm_table_status_ioctl is called with flag
 * DM_STATUS_TABLE_FLAG I have to sent params string back.
 */
static char *
dm_target_linear_table(void *target_config)
{
	dm_target_linear_config_t *tlc;
	char *params;
	tlc = target_config;

	params = dm_alloc_string(DM_MAX_PARAMS_SIZE);

	ksnprintf(params, DM_MAX_PARAMS_SIZE, "%s %" PRIu64,
	    tlc->pdev->udev_name, tlc->offset);

	return params;
}

/*
 * Do IO operation, called from dmstrategy routine.
 */
static int
dm_target_linear_strategy(dm_table_entry_t *table_en, struct buf *bp)
{
	dm_target_linear_config_t *tlc;

	tlc = table_en->target_config;

	bp->b_bio1.bio_offset += tlc->offset * DEV_BSIZE;

	vn_strategy(tlc->pdev->pdev_vnode, &bp->b_bio1);

	return 0;

}

static int
dm_target_linear_dump(dm_table_entry_t *table_en, void *data, size_t length, off_t offset)
{
	dm_target_linear_config_t *tlc;

	tlc = table_en->target_config;

	offset += tlc->offset * DEV_BSIZE;
	offset = dm_pdev_correct_dump_offset(tlc->pdev, offset);

	if (tlc->pdev->pdev_vnode->v_rdev == NULL)
		return ENXIO;

	return dev_ddump(tlc->pdev->pdev_vnode->v_rdev, data, 0, offset, length);
}

/*
 * Destroy target specific data. Decrement table pdevs.
 */
static int
dm_target_linear_destroy(dm_table_entry_t *table_en)
{
	dm_target_linear_config_t *tlc;

	/*
	 * Destroy function is called for every target even if it
	 * doesn't have target_config.
	 */

	if (table_en->target_config == NULL)
		return 0;

	tlc = table_en->target_config;

	/* Decrement pdev ref counter if 0 remove it */
	dm_pdev_decr(tlc->pdev);

	kfree(table_en->target_config, M_DMLINEAR);

	return 0;
}

static int
dmtl_mod_handler(module_t mod, int type, void *unused)
{
	dm_target_t *dmt = NULL;
	int err = 0;

	switch(type) {
	case MOD_LOAD:
		if ((dmt = dm_target_lookup("linear")) != NULL) {
			dm_target_unbusy(dmt);
			return EEXIST;
		}
		dmt = dm_target_alloc("linear");
		dmt->version[0] = 1;
		dmt->version[1] = 0;
		dmt->version[2] = 2;
		dmt->init = &dm_target_linear_init;
		dmt->destroy = &dm_target_linear_destroy;
		dmt->strategy = &dm_target_linear_strategy;
		dmt->table = &dm_target_linear_table;
		dmt->dump = &dm_target_linear_dump;

		err = dm_target_insert(dmt);
		if (err == 0)
			kprintf("dm_target_linear: Successfully initialized\n");
		break;

	case MOD_UNLOAD:
		err = dm_target_remove("linear");
		if (err == 0)
			kprintf("dm_target_linear: unloaded\n");
		break;
	}

	return err;
}

DM_TARGET_MODULE(dm_target_linear, dmtl_mod_handler);
