/*        $NetBSD: dm.h,v 1.17 2009/12/29 23:37:48 haad Exp $      */

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

#ifndef _DM_DEV_H_
#define _DM_DEV_H_

#ifdef _KERNEL

#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/kernel.h>

#include <sys/types.h>
#include <cpu/inttypes.h>
#include <sys/condvar.h>
#include <sys/lock.h>
#include <sys/queue.h>

#include <sys/vnode.h>
#include <sys/buf.h>
#include <sys/device.h>
#include <sys/devicestat.h>
#include <sys/diskslice.h>
#include <sys/disklabel.h>
#include <sys/vfscache.h>

#include <libprop/proplib.h>

#define DM_VERSION_MAJOR	4
#define DM_VERSION_MINOR	16
#define DM_VERSION_PATCHLEVEL	0

#define DM_MAX_TYPE_NAME 16
#define DM_MAX_DEV_NAME 32
#define DM_MAX_PARAMS_SIZE 1024

#define DM_NAME_LEN 128
#define DM_UUID_LEN 129

#define DM_TABLE_ACTIVE 0
#define DM_TABLE_INACTIVE 1

typedef struct dm_mapping {
	union {
		struct dm_pdev *pdev;
	} data;
	TAILQ_ENTRY(dm_mapping) next;
} dm_mapping_t;

/*
 * A device mapper table is a list of physical ranges plus the mapping target
 * applied to them.
 */
typedef struct dm_table_entry {
	struct dm_dev *dev;		/* backlink */
	uint64_t start;
	uint64_t length;

	struct dm_target *target;      /* Link to table target. */
	void *target_config;           /* Target specific data. */
	TAILQ_ENTRY(dm_table_entry) next;

	TAILQ_HEAD(, dm_mapping) pdev_maps;
} dm_table_entry_t;

TAILQ_HEAD(dm_table, dm_table_entry);

typedef struct dm_table dm_table_t;

typedef struct dm_table_head {
	/* Current active table is selected with this. */
	int cur_active_table;
	struct dm_table tables[2];

	struct lock   table_mtx;

	int	 io_cnt;
} dm_table_head_t;

/*
 * This structure is used to store opened vnodes for disk with name.
 * I need this because devices can be opened only once, but I can
 * have more then one device on one partition.
 */
typedef struct dm_pdev {
	char name[DM_MAX_DEV_NAME];
	char udev_name[DM_MAX_DEV_NAME];
	dev_t udev;
	struct partinfo pdev_pinfo; /* partinfo of the underlying device */

	struct vnode *pdev_vnode;
	int ref_cnt; /* reference counter for users ofthis pdev */

	TAILQ_ENTRY(dm_pdev) next_pdev;
} dm_pdev_t;

/*
 * This structure is called for every device-mapper device.
 * It points to TAILQ of device tables and mirrored, snapshoted etc. devices.
 */
typedef struct dm_dev {
	char name[DM_NAME_LEN];
	char uuid[DM_UUID_LEN];

	cdev_t devt; /* pointer to autoconf device_t structure */
	uint64_t minor;
	uint32_t flags; /* store communication protocol flags */

	struct lock dev_mtx; /* mutex for general device lock */
	struct cv dev_cv; /* cv for between ioctl synchronization */

	/* uint32_t event_nr; */
	uint32_t ref_cnt;

	uint32_t is_open;

	dm_table_head_t table_head;

	struct disk *diskp;

	struct devstat stats;

	TAILQ_ENTRY(dm_dev) next_devlist; /* Major device list. */
} dm_dev_t;

/*
 * Target config is initiated with dm_target_init function.
 */

/* constant dm_target structures for error, zero, linear, stripes etc. */
typedef struct dm_target {
	char name[DM_MAX_TYPE_NAME];

	int (*init)(dm_table_entry_t *, int, char **);
	int (*destroy)(dm_table_entry_t *);
	int (*strategy)(dm_table_entry_t *, struct buf *); /* Mandatory */
	char *(*table)(void *);	/* DM_STATUS_TABLE_FLAG */
	char *(*info)(void *);	/* !DM_STATUS_TABLE_FLAG */
	int (*dump)(dm_table_entry_t *, void *data, size_t length, off_t offset);
	int (*message)(dm_table_entry_t *, char *);

	uint32_t version[3];
	int ref_cnt;
	int max_argc;

	TAILQ_ENTRY(dm_target) dm_target_next;
} dm_target_t;

/* device-mapper.c */
void dmsetdiskinfo(struct disk *, dm_table_head_t *);
uint64_t atoi64(const char *);
char *dm_alloc_string(int len);
void dm_builtin_init(void *);
void dm_builtin_uninit(void *);
#ifdef MALLOC_DECLARE
MALLOC_DECLARE(M_DM);
#endif
extern int dm_debug_level;

/* dm_ioctl.c */
int dm_list_versions_ioctl(prop_dictionary_t);
int dm_dev_create_ioctl(prop_dictionary_t);
int dm_dev_list_ioctl(prop_dictionary_t);
int dm_dev_rename_ioctl(prop_dictionary_t);
int dm_dev_remove_ioctl(prop_dictionary_t);
int dm_dev_remove_all_ioctl(prop_dictionary_t);
int dm_dev_status_ioctl(prop_dictionary_t);
int dm_dev_suspend_ioctl(prop_dictionary_t);
int dm_dev_resume_ioctl(prop_dictionary_t);
int dm_table_clear_ioctl(prop_dictionary_t);
int dm_table_deps_ioctl(prop_dictionary_t);
int dm_table_load_ioctl(prop_dictionary_t);
int dm_table_status_ioctl(prop_dictionary_t);
int dm_message_ioctl(prop_dictionary_t);
int dm_check_version(prop_dictionary_t);

/* dm_target.c */
void dm_target_busy(dm_target_t *);
void dm_target_unbusy(dm_target_t *);
dm_target_t* dm_target_autoload(const char *);
dm_target_t* dm_target_lookup(const char *);
int dm_target_insert(dm_target_t *);
int dm_target_remove(char *);
dm_target_t* dm_target_alloc(const char *);
int dm_target_free(dm_target_t *);
prop_array_t dm_target_prop_list(void);
int dm_target_init(void);
int dm_target_uninit(void);

/* dm_table.c  */
dm_table_t *dm_table_get_entry(dm_table_head_t *, uint8_t);
void dm_table_release(dm_table_head_t *, uint8_t s);
void dm_table_switch_tables(dm_table_head_t *);
int dm_table_destroy(dm_table_head_t *, uint8_t);
uint64_t dm_table_size(dm_table_head_t *);
uint64_t dm_inactive_table_size(dm_table_head_t *);
int dm_table_get_target_count(dm_table_head_t *, uint8_t);
void dm_table_head_init(dm_table_head_t *);
void dm_table_head_destroy(dm_table_head_t *);
void dm_table_init_target(dm_table_entry_t *table_en, void *cfg);
int dm_table_add_deps(dm_table_entry_t *table_en, dm_pdev_t *pdev);
void dm_table_free_deps(dm_table_entry_t *table_en);

/* dm_dev.c */
dm_dev_t* dm_dev_lookup(const char *, const char *, int);
int dm_dev_insert(dm_dev_t *);
#if 0
dm_dev_t* dm_dev_lookup_evict(const char *, const char *, int);
#endif
int dm_dev_create(dm_dev_t **, const char *, const char *, int);
int dm_dev_remove(dm_dev_t *);
int dm_dev_remove_all(int);
void dm_dev_busy(dm_dev_t *);
void dm_dev_unbusy(dm_dev_t *);
prop_array_t dm_dev_prop_list(void);
int dm_dev_init(void);
int dm_dev_uninit(void);

/* dm_pdev.c */
off_t dm_pdev_correct_dump_offset(dm_pdev_t *, off_t);
dm_pdev_t* dm_pdev_insert(const char *);
int dm_pdev_decr(dm_pdev_t *);
uint64_t dm_pdev_get_udev(dm_pdev_t *);
int dm_pdev_get_vattr(dm_pdev_t *, struct vattr *);
int dm_pdev_init(void);
int dm_pdev_uninit(void);

#define dmdebug(format, ...)	\
    do { if (dm_debug_level) kprintf("%s: "format, __func__, ## __VA_ARGS__); } while(0)

#define DM_TARGET_MODULE(name, evh)				\
    static moduledata_t name##_mod = {				\
	    #name,						\
	    evh,						\
	    NULL						\
    };								\
    DECLARE_MODULE(name, name##_mod, SI_SUB_DM_TARGETS,		\
		   SI_ORDER_ANY);				\
    MODULE_DEPEND(name, dm, 1, 1, 1)

#define DM_TARGET_BUILTIN(name, evh)				\
    SYSINIT(name##module, SI_SUB_DM_TARGETS, SI_ORDER_ANY,	\
	dm_builtin_init, evh);					\
    SYSUNINIT(name##module, SI_SUB_DM_TARGETS, SI_ORDER_ANY,	\
	dm_builtin_uninit, evh)

#endif /*_KERNEL*/

#endif /*_DM_DEV_H_*/
