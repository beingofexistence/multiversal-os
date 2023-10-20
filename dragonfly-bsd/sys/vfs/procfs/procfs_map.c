/*
 * Copyright (c) 1993 Jan-Simon Pendry
 * Copyright (c) 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Jan-Simon Pendry.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)procfs_status.c	8.3 (Berkeley) 2/17/94
 *
 * $FreeBSD: src/sys/miscfs/procfs/procfs_map.c,v 1.24.2.1 2001/08/04 13:12:24 rwatson Exp $
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/sbuf.h>
#include <sys/malloc.h>
#include <vfs/procfs/procfs.h>

#include <vm/vm.h>
#include <sys/lock.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_page.h>
#include <vm/vm_object.h>

#include <machine/limits.h>

int
procfs_domap(struct proc *curp, struct lwp *lp, struct pfsnode *pfs,
	     struct uio *uio)
{
	struct proc *p = lp->lwp_proc;
	ssize_t buflen = uio->uio_offset + uio->uio_resid;
	struct vnode *vp;
	char *fullpath, *freepath;
	int error;
	vm_map_t map = &p->p_vmspace->vm_map;
	vm_map_entry_t entry;
	struct sbuf *sb = NULL;
	unsigned int last_timestamp;

	if (uio->uio_rw != UIO_READ)
		return (EOPNOTSUPP);

	error = 0;

	if (uio->uio_offset < 0 || uio->uio_resid < 0 || buflen >= INT_MAX)
		return EINVAL;
	sb = sbuf_new (sb, NULL, buflen+1, 0);
	if (sb == NULL)
		return EIO;

	/*
	 * Lock the map so we can access it.  Release the process token
	 * to avoid unnecessary token stalls while we are processing the
	 * map.
	 */
	vm_map_lock_read(map);
	lwkt_reltoken(&p->p_token);

	RB_FOREACH(entry, vm_map_rb_tree, &map->rb_root) {
		vm_map_backing_t ba;
		vm_object_t obj;
		int ref_count, flags;
		vm_offset_t e_start, e_end;
		vm_eflags_t e_eflags;
		vm_prot_t e_prot;
		int resident;
		char *type;

		switch(entry->maptype) {
		case VM_MAPTYPE_NORMAL:
			ba = &entry->ba;
			break;
		case VM_MAPTYPE_UKSMAP:
			ba = NULL;
			break;
		default:
			/* ignore entry */
			continue;
		}

		e_eflags = entry->eflags;
		e_prot = entry->protection;
		e_start = entry->ba.start;
		e_end = entry->ba.end;

		/*
		 * Don't count resident pages, its impossible on 64-bit.
		 * A single mapping could be gigabytes or terrabytes.
		 */
		resident = -1;
#if 0
		pmap_t pmap = vmspace_pmap(p->p_vmspace);
		vm_offset_t addr;

		resident = 0;
		addr = entry->ba.start;
		while (addr < entry->ba.end) {
			if (pmap_extract(pmap, addr, NULL))
				resident++;
			addr += PAGE_SIZE;
		}
#endif
		if (ba) {
			while (ba->backing_ba)
				ba = ba->backing_ba;
			obj = ba->object;
			if (obj)
				vm_object_hold(obj);
		} else {
			obj = NULL;
		}
		last_timestamp = map->timestamp;
		vm_map_unlock(map);

		freepath = NULL;
		fullpath = "-";
		flags = 0;
		ref_count = 0;

		if (obj) {
			switch(obj->type) {
			default:
			case OBJT_DEFAULT:
				type = "default";
				vp = NULL;
				break;
			case OBJT_VNODE:
				type = "vnode";
				vp = obj->handle;
				vref(vp);
				break;
			case OBJT_SWAP:
				type = "swap";
				vp = NULL;
				break;
			case OBJT_DEVICE:
				type = "device";
				vp = NULL;
				break;
			case OBJT_MGTDEVICE:
				type = "mgtdevice";
				vp = NULL;
				break;
			}
			if (ba->object) {
				flags = ba->object->flags;
				ref_count = ba->object->ref_count;
			}
			vm_object_drop(obj);
			if (vp) {
				vn_fullpath(p, vp, &fullpath, &freepath, 1);
				vrele(vp);
			}
		} else {
			switch(entry->maptype) {
			case VM_MAPTYPE_UNSPECIFIED:
				type = "unspec";
				break;
			case VM_MAPTYPE_NORMAL:
				type = "none";
				break;
			case VM_MAPTYPE_SUBMAP:
				type = "submap";
				break;
			case VM_MAPTYPE_UKSMAP:
				type = "uksmap";
				break;
			default:
				type = "unknown";
				break;
			}
		}

		/*
		 * format:
		 *  start, end, res, priv res, cow, access, type, (fullpath).
		 */
		error = sbuf_printf(sb,
#if LONG_BIT == 64
			  "0x%016lx 0x%016lx %d %d %p %s%s%s %d %d "
#else
			  "0x%08lx 0x%08lx %d %d %p %s%s%s %d %d "
#endif
			  "0x%04x %s%s %s %s\n",
			(u_long)e_start, (u_long)e_end,
			resident, -1, (ba ? ba->object : NULL),
			(e_prot & VM_PROT_READ) ? "r" : "-",
			(e_prot & VM_PROT_WRITE) ? "w" : "-",
			(e_prot & VM_PROT_EXECUTE) ? "x" : "-",
			ref_count, 0, flags,
			(e_eflags & MAP_ENTRY_COW) ? "COW" : "NCOW",
			(e_eflags & MAP_ENTRY_NEEDS_COPY) ?" NC" : " NNC",
			type, fullpath);

		if (freepath != NULL) {
			kfree(freepath, M_TEMP);
			freepath = NULL;
		}

		vm_map_lock_read(map);
		if (error == -1) {
			error = 0;
			break;
		}

		if (last_timestamp != map->timestamp) {
			vm_map_entry_t reentry;
			vm_map_lookup_entry(map, e_start, &reentry);
			entry = reentry;
		}
	}
	vm_map_unlock_read(map);
	if (sbuf_finish(sb) == 0)
		buflen = sbuf_len(sb);
	error = uiomove_frombuf(sbuf_data(sb), buflen, uio);
	sbuf_delete(sb);

	lwkt_gettoken(&p->p_token);	/* re-acquire */

	return error;
}

int
procfs_validmap(struct lwp *lp)
{
	return ((lp->lwp_proc->p_flags & P_SYSTEM) == 0);
}
