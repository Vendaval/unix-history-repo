/*
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 1994 John S. Dyson
 * All rights reserved.
 * Copyright (c) 1994 David Greenman
 * All rights reserved.
 *
 *
 * This code is derived from software contributed to Berkeley by
 * The Mach Operating System project at Carnegie-Mellon University.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
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
 *	from: @(#)vm_fault.c	8.4 (Berkeley) 1/12/94
 *
 *
 * Copyright (c) 1987, 1990 Carnegie-Mellon University.
 * All rights reserved.
 *
 * Authors: Avadis Tevanian, Jr., Michael Wayne Young
 *
 * Permission to use, copy, modify and distribute this software and
 * its documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 *
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND
 * FOR ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 *
 * Carnegie Mellon requests users of this software to return to
 *
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 *
 * any improvements or extensions that they make and grant Carnegie the
 * rights to redistribute these changes.
 *
 * $Id: vm_fault.c,v 1.35 1995/11/02 06:42:47 davidg Exp $
 */

/*
 *	Page fault handling module.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/resource.h>
#include <sys/signalvar.h>
#include <sys/resourcevar.h>

#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/vm_pageout.h>
#include <vm/vm_kern.h>
#include <vm/vm_pager.h>
#include <vm/vnode_pager.h>
#include <vm/swap_pager.h>

int vm_fault_additional_pages __P((vm_page_t, int, int, vm_page_t *, int *));

#define VM_FAULT_READ_AHEAD 4
#define VM_FAULT_READ_BEHIND 3
#define VM_FAULT_READ (VM_FAULT_READ_AHEAD+VM_FAULT_READ_BEHIND+1)

/*
 *	vm_fault:
 *
 *	Handle a page fault occuring at the given address,
 *	requiring the given permissions, in the map specified.
 *	If successful, the page is inserted into the
 *	associated physical map.
 *
 *	NOTE: the given address should be truncated to the
 *	proper page address.
 *
 *	KERN_SUCCESS is returned if the page fault is handled; otherwise,
 *	a standard error specifying why the fault is fatal is returned.
 *
 *
 *	The map in question must be referenced, and remains so.
 *	Caller may hold no locks.
 */
int
vm_fault(map, vaddr, fault_type, change_wiring)
	vm_map_t map;
	vm_offset_t vaddr;
	vm_prot_t fault_type;
	boolean_t change_wiring;
{
	vm_object_t first_object;
	vm_offset_t first_offset;
	vm_map_entry_t entry;
	register vm_object_t object;
	register vm_offset_t offset;
	vm_page_t m;
	vm_page_t first_m;
	vm_prot_t prot;
	int result;
	boolean_t wired;
	boolean_t su;
	boolean_t lookup_still_valid;
	boolean_t page_exists;
	vm_page_t old_m;
	vm_object_t next_object;
	vm_page_t marray[VM_FAULT_READ];
	int spl;
	int hardfault = 0;
	struct vnode *vp = NULL;

	cnt.v_vm_faults++;	/* needs lock XXX */
/*
 *	Recovery actions
 */
#define	FREE_PAGE(m)	{				\
	PAGE_WAKEUP(m);					\
	vm_page_free(m);				\
}

#define	RELEASE_PAGE(m)	{				\
	PAGE_WAKEUP(m);					\
	if ((m->flags & PG_ACTIVE) == 0) vm_page_activate(m);		\
}

#define	UNLOCK_MAP	{				\
	if (lookup_still_valid) {			\
		vm_map_lookup_done(map, entry);		\
		lookup_still_valid = FALSE;		\
	}						\
}

#define	UNLOCK_THINGS	{				\
	vm_object_pip_wakeup(object); \
	if (object != first_object) {			\
		FREE_PAGE(first_m);			\
		vm_object_pip_wakeup(first_object); \
	}						\
	UNLOCK_MAP;					\
	if (vp != NULL) VOP_UNLOCK(vp);			\
}

#define	UNLOCK_AND_DEALLOCATE	{			\
	UNLOCK_THINGS;					\
	vm_object_deallocate(first_object);		\
}


RetryFault:;

	/*
	 * Find the backing store object and offset into it to begin the
	 * search.
	 */

	if ((result = vm_map_lookup(&map, vaddr,
		fault_type, &entry, &first_object,
		&first_offset, &prot, &wired, &su)) != KERN_SUCCESS) {
		return (result);
	}

	vp = vnode_pager_lock(first_object);

	lookup_still_valid = TRUE;

	if (wired)
		fault_type = prot;

	first_m = NULL;

	/*
	 * Make a reference to this object to prevent its disposal while we
	 * are messing with it.  Once we have the reference, the map is free
	 * to be diddled.  Since objects reference their shadows (and copies),
	 * they will stay around as well.
	 */

	first_object->ref_count++;
	first_object->paging_in_progress++;

	/*
	 * INVARIANTS (through entire routine):
	 *
	 * 1)	At all times, we must either have the object lock or a busy
	 * page in some object to prevent some other process from trying to
	 * bring in the same page.
	 *
	 * Note that we cannot hold any locks during the pager access or when
	 * waiting for memory, so we use a busy page then.
	 *
	 * Note also that we aren't as concerned about more than one thead
	 * attempting to pager_data_unlock the same page at once, so we don't
	 * hold the page as busy then, but do record the highest unlock value
	 * so far.  [Unlock requests may also be delivered out of order.]
	 *
	 * 2)	Once we have a busy page, we must remove it from the pageout
	 * queues, so that the pageout daemon will not grab it away.
	 *
	 * 3)	To prevent another process from racing us down the shadow chain
	 * and entering a new page in the top object before we do, we must
	 * keep a busy page in the top object while following the shadow
	 * chain.
	 *
	 * 4)	We must increment paging_in_progress on any object for which
	 * we have a busy page, to prevent vm_object_collapse from removing
	 * the busy page without our noticing.
	 */

	/*
	 * Search for the page at object/offset.
	 */

	object = first_object;
	offset = first_offset;

	/*
	 * See whether this page is resident
	 */

	while (TRUE) {
		m = vm_page_lookup(object, offset);
		if (m != NULL) {
			/*
			 * If the page is being brought in, wait for it and
			 * then retry.
			 */
			if ((m->flags & PG_BUSY) || m->busy) {
				int s;

				UNLOCK_THINGS;
				s = splhigh();
				if ((m->flags & PG_BUSY) || m->busy) {
					m->flags |= PG_WANTED | PG_REFERENCED;
					cnt.v_intrans++;
					tsleep(m, PSWP, "vmpfw", 0);
				}
				splx(s);
				vm_object_deallocate(first_object);
				goto RetryFault;
			}

			/*
			 * Mark page busy for other processes, and the pagedaemon.
			 */
			m->flags |= PG_BUSY;
			if ((m->flags & PG_CACHE) &&
			    (cnt.v_free_count + cnt.v_cache_count) < cnt.v_free_reserved) {
				UNLOCK_AND_DEALLOCATE;
				VM_WAIT;
				PAGE_WAKEUP(m);
				goto RetryFault;
			}

			if (m->valid && ((m->valid & VM_PAGE_BITS_ALL) != VM_PAGE_BITS_ALL) &&
				 m->object != kernel_object && m->object != kmem_object) {
				goto readrest;
			}
			break;
		}
		if (((object->type != OBJT_DEFAULT) && (!change_wiring || wired))
		    || (object == first_object)) {

			if (offset >= object->size) {
				UNLOCK_AND_DEALLOCATE;
				return (KERN_PROTECTION_FAILURE);
			}

			/*
			 * Allocate a new page for this object/offset pair.
			 */
			m = vm_page_alloc(object, offset,
				vp?VM_ALLOC_NORMAL:(VM_ALLOC_NORMAL|VM_ALLOC_ZERO));

			if (m == NULL) {
				UNLOCK_AND_DEALLOCATE;
				VM_WAIT;
				goto RetryFault;
			}
		}
readrest:
		if (object->type != OBJT_DEFAULT && (!change_wiring || wired)) {
			int rv;
			int faultcount;
			int reqpage;

			/*
			 * now we find out if any other pages should be paged
			 * in at this time this routine checks to see if the
			 * pages surrounding this fault reside in the same
			 * object as the page for this fault.  If they do,
			 * then they are faulted in also into the object.  The
			 * array "marray" returned contains an array of
			 * vm_page_t structs where one of them is the
			 * vm_page_t passed to the routine.  The reqpage
			 * return value is the index into the marray for the
			 * vm_page_t passed to the routine.
			 */
			faultcount = vm_fault_additional_pages(
			    m, VM_FAULT_READ_BEHIND, VM_FAULT_READ_AHEAD,
			    marray, &reqpage);

			/*
			 * Call the pager to retrieve the data, if any, after
			 * releasing the lock on the map.
			 */
			UNLOCK_MAP;

			rv = faultcount ?
			    vm_pager_get_pages(object, marray, faultcount,
				reqpage) : VM_PAGER_FAIL;

			if (rv == VM_PAGER_OK) {
				/*
				 * Found the page. Leave it busy while we play
				 * with it.
				 */

				/*
				 * Relookup in case pager changed page. Pager
				 * is responsible for disposition of old page
				 * if moved.
				 */
				m = vm_page_lookup(object, offset);
				if( !m) {
					UNLOCK_AND_DEALLOCATE;
					goto RetryFault;
				}

				hardfault++;
				break;
			}
			/*
			 * Remove the bogus page (which does not exist at this
			 * object/offset); before doing so, we must get back
			 * our object lock to preserve our invariant.
			 *
			 * Also wake up any other process that may want to bring
			 * in this page.
			 *
			 * If this is the top-level object, we must leave the
			 * busy page to prevent another process from rushing
			 * past us, and inserting the page in that object at
			 * the same time that we are.
			 */

			if (rv == VM_PAGER_ERROR)
				printf("vm_fault: pager input (probably hardware) error, PID %d failure\n",
				    curproc->p_pid);
			/*
			 * Data outside the range of the pager or an I/O error
			 */
			/*
			 * XXX - the check for kernel_map is a kludge to work
			 * around having the machine panic on a kernel space
			 * fault w/ I/O error.
			 */
			if (((map != kernel_map) && (rv == VM_PAGER_ERROR)) || (rv == VM_PAGER_BAD)) {
				FREE_PAGE(m);
				UNLOCK_AND_DEALLOCATE;
				return ((rv == VM_PAGER_ERROR) ? KERN_FAILURE : KERN_PROTECTION_FAILURE);
			}
			if (object != first_object) {
				FREE_PAGE(m);
				/*
				 * XXX - we cannot just fall out at this
				 * point, m has been freed and is invalid!
				 */
			}
		}
		/*
		 * We get here if the object has default pager (or unwiring) or the
		 * pager doesn't have the page.
		 */
		if (object == first_object)
			first_m = m;

		/*
		 * Move on to the next object.  Lock the next object before
		 * unlocking the current one.
		 */

		offset += object->backing_object_offset;
		next_object = object->backing_object;
		if (next_object == NULL) {
			/*
			 * If there's no object left, fill the page in the top
			 * object with zeros.
			 */
			if (object != first_object) {
				vm_object_pip_wakeup(object);

				object = first_object;
				offset = first_offset;
				m = first_m;
			}
			first_m = NULL;

			if ((m->flags & PG_ZERO) == 0)
				vm_page_zero_fill(m);
			m->valid = VM_PAGE_BITS_ALL;
			cnt.v_zfod++;
			break;
		} else {
			if (object != first_object) {
				vm_object_pip_wakeup(object);
			}
			object = next_object;
			object->paging_in_progress++;
		}
	}

	if ((m->flags & PG_BUSY) == 0)
		panic("vm_fault: not busy after main loop");

	/*
	 * PAGE HAS BEEN FOUND. [Loop invariant still holds -- the object lock
	 * is held.]
	 */

	old_m = m;	/* save page that would be copied */

	/*
	 * If the page is being written, but isn't already owned by the
	 * top-level object, we have to copy it into a new page owned by the
	 * top-level object.
	 */

	if (object != first_object) {
		/*
		 * We only really need to copy if we want to write it.
		 */

		if (fault_type & VM_PROT_WRITE) {

			/*
			 * If we try to collapse first_object at this point,
			 * we may deadlock when we try to get the lock on an
			 * intermediate object (since we have the bottom
			 * object locked).  We can't unlock the bottom object,
			 * because the page we found may move (by collapse) if
			 * we do.
			 *
			 * Instead, we first copy the page.  Then, when we have
			 * no more use for the bottom object, we unlock it and
			 * try to collapse.
			 *
			 * Note that we copy the page even if we didn't need
			 * to... that's the breaks.
			 */

			/*
			 * We already have an empty page in first_object - use
			 * it.
			 */

			vm_page_copy(m, first_m);
			first_m->valid = VM_PAGE_BITS_ALL;

			/*
			 * If another map is truly sharing this page with us,
			 * we have to flush all uses of the original page,
			 * since we can't distinguish those which want the
			 * original from those which need the new copy.
			 *
			 * XXX If we know that only one map has access to this
			 * page, then we could avoid the pmap_page_protect()
			 * call.
			 */

			if ((m->flags & PG_ACTIVE) == 0)
				vm_page_activate(m);
			vm_page_protect(m, VM_PROT_NONE);

			/*
			 * We no longer need the old page or object.
			 */
			PAGE_WAKEUP(m);
			vm_object_pip_wakeup(object);

			/*
			 * Only use the new page below...
			 */

			cnt.v_cow_faults++;
			m = first_m;
			object = first_object;
			offset = first_offset;

			/*
			 * Now that we've gotten the copy out of the way,
			 * let's try to collapse the top object.
			 *
			 * But we have to play ugly games with
			 * paging_in_progress to do that...
			 */
			vm_object_pip_wakeup(object);
			vm_object_collapse(object);
			object->paging_in_progress++;
		} else {
			prot &= ~VM_PROT_WRITE;
		}
	}

	/*
	 * We must verify that the maps have not changed since our last
	 * lookup.
	 */

	if (!lookup_still_valid) {
		vm_object_t retry_object;
		vm_offset_t retry_offset;
		vm_prot_t retry_prot;

		/*
		 * Since map entries may be pageable, make sure we can take a
		 * page fault on them.
		 */

		/*
		 * To avoid trying to write_lock the map while another process
		 * has it read_locked (in vm_map_pageable), we do not try for
		 * write permission.  If the page is still writable, we will
		 * get write permission.  If it is not, or has been marked
		 * needs_copy, we enter the mapping without write permission,
		 * and will merely take another fault.
		 */
		result = vm_map_lookup(&map, vaddr, fault_type & ~VM_PROT_WRITE,
		    &entry, &retry_object, &retry_offset, &retry_prot, &wired, &su);

		/*
		 * If we don't need the page any longer, put it on the active
		 * list (the easiest thing to do here).  If no one needs it,
		 * pageout will grab it eventually.
		 */

		if (result != KERN_SUCCESS) {
			RELEASE_PAGE(m);
			UNLOCK_AND_DEALLOCATE;
			return (result);
		}
		lookup_still_valid = TRUE;

		if ((retry_object != first_object) ||
		    (retry_offset != first_offset)) {
			RELEASE_PAGE(m);
			UNLOCK_AND_DEALLOCATE;
			goto RetryFault;
		}
		/*
		 * Check whether the protection has changed or the object has
		 * been copied while we left the map unlocked. Changing from
		 * read to write permission is OK - we leave the page
		 * write-protected, and catch the write fault. Changing from
		 * write to read permission means that we can't mark the page
		 * write-enabled after all.
		 */
		prot &= retry_prot;
	}
	/*
	 * (the various bits we're fiddling with here are locked by the
	 * object's lock)
	 */

	/*
	 * It's critically important that a wired-down page be faulted only
	 * once in each map for which it is wired.
	 */

	/*
	 * Put this page into the physical map. We had to do the unlock above
	 * because pmap_enter may cause other faults.   We don't put the page
	 * back on the active queue until later so that the page-out daemon
	 * won't find us (yet).
	 */

	if (prot & VM_PROT_WRITE) {
		m->flags |= PG_WRITEABLE;
		m->object->flags |= OBJ_WRITEABLE|OBJ_MIGHTBEDIRTY;
		/*
		 * If the fault is a write, we know that this page is being
		 * written NOW. This will save on the pmap_is_modified() calls
		 * later.
		 */
		if (fault_type & VM_PROT_WRITE) {
			m->dirty = VM_PAGE_BITS_ALL;
		}
	}

	m->flags |= PG_MAPPED|PG_REFERENCED;
	m->flags &= ~PG_ZERO;

	pmap_enter(map->pmap, vaddr, VM_PAGE_TO_PHYS(m), prot, wired);
#if 0
	if (change_wiring == 0 && wired == 0)
		pmap_prefault(map->pmap, vaddr, entry, first_object);
#endif

	/*
	 * If the page is not wired down, then put it where the pageout daemon
	 * can find it.
	 */
	if (change_wiring) {
		if (wired)
			vm_page_wire(m);
		else
			vm_page_unwire(m);
	} else {
		if ((m->flags & PG_ACTIVE) == 0)
			vm_page_activate(m);
	}

	if (curproc && (curproc->p_flag & P_INMEM) && curproc->p_stats) {
		if (hardfault) {
			curproc->p_stats->p_ru.ru_majflt++;
		} else {
			curproc->p_stats->p_ru.ru_minflt++;
		}
	}

	if ((m->flags & PG_BUSY) == 0)
		printf("page not busy: %d\n", m->offset);
	/*
	 * Unlock everything, and return
	 */

	PAGE_WAKEUP(m);
	UNLOCK_AND_DEALLOCATE;

	return (KERN_SUCCESS);

}

/*
 *	vm_fault_wire:
 *
 *	Wire down a range of virtual addresses in a map.
 */
int
vm_fault_wire(map, start, end)
	vm_map_t map;
	vm_offset_t start, end;
{

	register vm_offset_t va;
	register pmap_t pmap;
	int rv;

	pmap = vm_map_pmap(map);

	/*
	 * Inform the physical mapping system that the range of addresses may
	 * not fault, so that page tables and such can be locked down as well.
	 */

	pmap_pageable(pmap, start, end, FALSE);

	/*
	 * We simulate a fault to get the page and enter it in the physical
	 * map.
	 */

	for (va = start; va < end; va += PAGE_SIZE) {

		while( curproc != pageproc &&
			(cnt.v_free_count <= cnt.v_pageout_free_min))
			VM_WAIT;

		rv = vm_fault(map, va, VM_PROT_READ|VM_PROT_WRITE, TRUE);
		if (rv) {
			if (va != start)
				vm_fault_unwire(map, start, va);
			return (rv);
		}
	}
	return (KERN_SUCCESS);
}


/*
 *	vm_fault_unwire:
 *
 *	Unwire a range of virtual addresses in a map.
 */
void
vm_fault_unwire(map, start, end)
	vm_map_t map;
	vm_offset_t start, end;
{

	register vm_offset_t va, pa;
	register pmap_t pmap;

	pmap = vm_map_pmap(map);

	/*
	 * Since the pages are wired down, we must be able to get their
	 * mappings from the physical map system.
	 */

	for (va = start; va < end; va += PAGE_SIZE) {
		pa = pmap_extract(pmap, va);
		if (pa == (vm_offset_t) 0) {
			panic("unwire: page not in pmap");
		}
		pmap_change_wiring(pmap, va, FALSE);
		vm_page_unwire(PHYS_TO_VM_PAGE(pa));
	}

	/*
	 * Inform the physical mapping system that the range of addresses may
	 * fault, so that page tables and such may be unwired themselves.
	 */

	pmap_pageable(pmap, start, end, TRUE);

}

/*
 *	Routine:
 *		vm_fault_copy_entry
 *	Function:
 *		Copy all of the pages from a wired-down map entry to another.
 *
 *	In/out conditions:
 *		The source and destination maps must be locked for write.
 *		The source map entry must be wired down (or be a sharing map
 *		entry corresponding to a main map entry that is wired down).
 */

void
vm_fault_copy_entry(dst_map, src_map, dst_entry, src_entry)
	vm_map_t dst_map;
	vm_map_t src_map;
	vm_map_entry_t dst_entry;
	vm_map_entry_t src_entry;
{
	vm_object_t dst_object;
	vm_object_t src_object;
	vm_offset_t dst_offset;
	vm_offset_t src_offset;
	vm_prot_t prot;
	vm_offset_t vaddr;
	vm_page_t dst_m;
	vm_page_t src_m;

#ifdef	lint
	src_map++;
#endif	/* lint */

	src_object = src_entry->object.vm_object;
	src_offset = src_entry->offset;

	/*
	 * Create the top-level object for the destination entry. (Doesn't
	 * actually shadow anything - we copy the pages directly.)
	 */
	dst_object = vm_object_allocate(OBJT_DEFAULT,
	    (vm_size_t) (dst_entry->end - dst_entry->start));

	dst_entry->object.vm_object = dst_object;
	dst_entry->offset = 0;

	prot = dst_entry->max_protection;

	/*
	 * Loop through all of the pages in the entry's range, copying each
	 * one from the source object (it should be there) to the destination
	 * object.
	 */
	for (vaddr = dst_entry->start, dst_offset = 0;
	    vaddr < dst_entry->end;
	    vaddr += PAGE_SIZE, dst_offset += PAGE_SIZE) {

		/*
		 * Allocate a page in the destination object
		 */
		do {
			dst_m = vm_page_alloc(dst_object, dst_offset, VM_ALLOC_NORMAL);
			if (dst_m == NULL) {
				VM_WAIT;
			}
		} while (dst_m == NULL);

		/*
		 * Find the page in the source object, and copy it in.
		 * (Because the source is wired down, the page will be in
		 * memory.)
		 */
		src_m = vm_page_lookup(src_object, dst_offset + src_offset);
		if (src_m == NULL)
			panic("vm_fault_copy_wired: page missing");

		vm_page_copy(src_m, dst_m);

		/*
		 * Enter it in the pmap...
		 */

		dst_m->flags |= PG_WRITEABLE|PG_MAPPED;
		dst_m->flags &= ~PG_ZERO;
		pmap_enter(dst_map->pmap, vaddr, VM_PAGE_TO_PHYS(dst_m),
		    prot, FALSE);

		/*
		 * Mark it no longer busy, and put it on the active list.
		 */
		vm_page_activate(dst_m);
		PAGE_WAKEUP(dst_m);
	}
}


/*
 * This routine checks around the requested page for other pages that
 * might be able to be faulted in.  This routine brackets the viable
 * pages for the pages to be paged in.
 *
 * Inputs:
 *	m, rbehind, rahead
 *
 * Outputs:
 *  marray (array of vm_page_t), reqpage (index of requested page)
 *
 * Return value:
 *  number of pages in marray
 */
int
vm_fault_additional_pages(m, rbehind, rahead, marray, reqpage)
	vm_page_t m;
	int rbehind;
	int rahead;
	vm_page_t *marray;
	int *reqpage;
{
	int i;
	vm_object_t object;
	vm_offset_t offset, startoffset, endoffset, toffset, size;
	vm_page_t rtm;
	int treqpage;
	int cbehind, cahead;

	object = m->object;
	offset = m->offset;

	/*
	 * if the requested page is not available, then give up now
	 */

	if (!vm_pager_has_page(object,
		object->paging_offset + offset, &cbehind, &cahead))
		return 0;

	if ((cbehind == 0) && (cahead == 0)) {
		*reqpage = 0;
		marray[0] = m;
		return 1;
	}

	if (rahead > cahead) {
		rahead = cahead;
	}

	if (rbehind > cbehind) {
		rbehind = cbehind;
	}

	/*
	 * try to do any readahead that we might have free pages for.
	 */
	if ((rahead + rbehind) >
		((cnt.v_free_count + cnt.v_cache_count) - cnt.v_free_reserved)) {
		pagedaemon_wakeup();
		*reqpage = 0;
		marray[0] = m;
		return 1;
	}

	/*
	 * scan backward for the read behind pages -- in memory or on disk not
	 * in same object
	 */
	toffset = offset - PAGE_SIZE;
	if (toffset < offset) {
		if (rbehind * PAGE_SIZE > offset)
			rbehind = offset / PAGE_SIZE;
		startoffset = offset - rbehind * PAGE_SIZE;
		while (toffset >= startoffset) {
			if (vm_page_lookup( object, toffset)) {
				startoffset = toffset + PAGE_SIZE;
				break;
			}
			if (toffset == 0)
				break;
			toffset -= PAGE_SIZE;
		}
	} else {
		startoffset = offset;
	}

	/*
	 * scan forward for the read ahead pages -- in memory or on disk not
	 * in same object
	 */
	toffset = offset + PAGE_SIZE;
	endoffset = offset + (rahead + 1) * PAGE_SIZE;
	if (endoffset > object->size)
		endoffset = object->size;
	while (toffset < endoffset) {
		if ( vm_page_lookup(object, toffset)) {
			break;
		}	
		toffset += PAGE_SIZE;
	}
	endoffset = toffset;

	/* calculate number of bytes of pages */
	size = (endoffset - startoffset) / PAGE_SIZE;

	/* calculate the page offset of the required page */
	treqpage = (offset - startoffset) / PAGE_SIZE;

	/* see if we have space (again) */
	if ((cnt.v_free_count + cnt.v_cache_count) >
		(cnt.v_free_reserved + size)) {
		/*
		 * get our pages and don't block for them
		 */
		for (i = 0; i < size; i++) {
			if (i != treqpage) {
				rtm = vm_page_alloc(object,
					startoffset + i * PAGE_SIZE,
					VM_ALLOC_NORMAL);
				if (rtm == NULL) {
					if (i < treqpage) {
						int j;
						for (j = 0; j < i; j++) {
							FREE_PAGE(marray[j]);
						}
						*reqpage = 0;
						marray[0] = m;
						return 1;
					} else {
						size = i;
						*reqpage = treqpage;
						return size;
					}
				}
				marray[i] = rtm;
			} else {
				marray[i] = m;
			}
		}

		*reqpage = treqpage;
		return size;
	}
	*reqpage = 0;
	marray[0] = m;
	return 1;
}
