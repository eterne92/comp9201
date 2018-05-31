/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *        The President and Fellows of Harvard College.
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
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>
#include <vfs.h>
#include <vnode.h>
#include <uio.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 *
 * UNSW: If you use ASST3 config as required, then this file forms
 * part of the VM subsystem.
 *
 */

struct addrspace *
as_create(void)
{
    struct addrspace *as;

    as = kmalloc(sizeof(struct addrspace));
    if (as == NULL)
    {
        return NULL;
    }
    // as->top is always fixed size in asst3
    // which is as_max_page pages
    as->region = NULL;
    as->heap_base = 0;
    as->heap_size = 0;
    as->unused_top = USERSPACETOP;

    return as;
}

int as_copy(struct addrspace *old, struct addrspace **ret)
{
    struct addrspace *newas;
    struct region_entry *old_region, *new_region, *tmp;
    uint32_t entryhi, entrylo, entrylo_old;
    int result;

    newas = as_create();
    if (newas == NULL)
    {
        return ENOMEM;
    }
    newas->heap_base = old->heap_base;
    newas->heap_size = old->heap_size;

    old_region = old->region;
    new_region = newas->region;
    tmp = NULL;
    while (old_region != NULL)
    {
        new_region = kmalloc(sizeof(struct region_entry));
        if (newas->region == NULL)
        {
            newas->region = new_region;
            tmp = new_region;
        }
        else
        {
            tmp->next = new_region;
            tmp = tmp->next;
        }
        *new_region = *old_region;
        if (old_region->vn)
        {
            VOP_INCREF(old_region->vn);
        }
        // check old_region's region have a entry or not
        for (uint32_t i = 0; i < old_region->npages; i++)
        {
            entrylo_old = vm_lookup(old, old_region->vbase + i * PAGE_SIZE);
            entryhi = (old_region->vbase + i * PAGE_SIZE) & TLBHI_VPAGE;
            // if we do have a entry, we need to insert a new one for newas
            if (entrylo_old)
            {
                // mark it readonly
                entrylo = entrylo_old & ~TLBLO_DIRTY;
                result = vm_insert(newas, new_region->vbase + i * PAGE_SIZE, entrylo);
                if (result)
                {
                    as_destroy(newas);
                    return result;
                }
                vm_update(old, old_region->vbase + i * PAGE_SIZE, entrylo);
                share_page(entrylo_old & PAGE_FRAME);
                tlb_update(entryhi, entrylo_old, entrylo);
            }
        }

        old_region = old_region->next;
    }
    *ret = newas;
    return 0;
}

void region_destroy_munmap(struct addrspace *as, struct region_entry *region)
{
    
    if (region->vn)
    {
        if (region->flags & RG_W)
        {
            struct uio useruio;
            struct iovec iov;
            int result;
            uio_uinit(&iov, &useruio, (userptr_t)region->vaddr,
                      region->npages * PAGE_SIZE, region->offset, UIO_WRITE);
            result = VOP_WRITE(region->vn, &useruio);
            if(result){
                panic("write went wrong");
            }
        }
        vfs_close(region->vn);
    }

    for (uint32_t i = 0; i < region->npages; i++)
    {
        vaddr_t vbase = region->vbase + i * PAGE_SIZE;
        if (vm_lookup(as, vbase))
        {
            vm_delete(as, vbase);
        }
    }
    kfree(region);
}
static void region_destroy(struct addrspace *as, struct region_entry *region)
{
    
    if (region->vn)
    {
        vfs_close(region->vn);
    }

    for (uint32_t i = 0; i < region->npages; i++)
    {
        vaddr_t vbase = region->vbase + i * PAGE_SIZE;
        if (vm_lookup(as, vbase))
        {
            vm_delete(as, vbase);
        }
    }
    kfree(region);
}



void as_destroy(struct addrspace *as)
{
    /*
         * Clean up as needed.
         */
    struct region_entry *region = as->region;
    while (region)
    {
        struct region_entry *tmp_region = region->next;
        region_destroy(as, region);
        region = tmp_region;
    }

    kfree(as);
}

void as_activate(void)
{
    struct addrspace *as;

    as = proc_getas();
    if (as == NULL)
    {
        /*
        * Kernel thread without an address space; leave the
        * prior address space in place.
        */
        return;
    }

    tlb_flush();
}

void as_deactivate(void)
{
    /*
    * Write this. For many designs it won't need to actually do
    * anything. See proc.c for an explanation of why it (might)
    * be needed.
    */
    tlb_flush();
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
static int create_region(struct region_entry *region,
                         vaddr_t vaddr, size_t memsize,
                         int readable, int writeable, int executable)
{
    size_t npages;

    // align region
    memsize += vaddr & ~(vaddr_t)PAGE_FRAME;
    vaddr &= PAGE_FRAME;
    // align memsize
    memsize = (memsize + PAGE_SIZE - 1) & PAGE_FRAME;
    // set npages
    npages = memsize / PAGE_SIZE;

    // check userspace top
    if (vaddr + memsize > USERSPACETOP)
    {
        // what should we return? ENOMEM? ENOSYS?
        return ENOSYS;
    }
    // setup region
    if (region == NULL)
    {
        return ENOMEM;
    }
    region->next = NULL;
    // setup flags
    region->flags = 0;
    region->flags |= readable | writeable | executable;
    region->vbase = vaddr;
    region->npages = npages;
    // default with out vn
    region->vn = NULL;
    region->offset = 0;
    return 0;
}

static int insert_region(struct addrspace *as, struct region_entry *region)
{
    vaddr_t vaddr;
    vaddr = region->vbase;

    if (as->region == NULL)
    {
        // first region
        as->region = region;
    }
    else
    {
        struct region_entry *tmp = as->region;
        // check overlap
        while (1)
        {
            // overlap
            if ((vaddr > tmp->vbase &&
                 vaddr - tmp->vbase < tmp->npages * PAGE_SIZE))
            {
                kfree(region);
                return ENOSYS;
            }
            // last region
            if (tmp->next == NULL)
            {
                tmp->next = region;
                break;
            }
            // not last region
            else
            {
                // we are in the right position
                KASSERT(vaddr > tmp->vbase);
                if (vaddr < tmp->next->vbase)
                {
                    // check overlap
                    if (vaddr + region->npages * PAGE_SIZE > tmp->next->vbase)
                    {
                        kfree(region);
                        return ENOSYS;
                    }
                    region->next = tmp->next;
                    tmp->next = region;
                    break;
                }
            }
            tmp = tmp->next;
        }
    }

    return 0;
}

int as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
                     int readable, int writeable, int executable)
{
    int result;
    struct region_entry *region;

    // Should NEVER call this with a NULL as
    KASSERT(as != NULL);

    region = kmalloc(sizeof(struct region_entry));
    result = create_region(region, vaddr, memsize, readable, writeable, executable);
    if (result)
    {
        return result;
    }

    result = insert_region(as, region);
    if (result)
    {
        return result;
    }
    return 0; /* Unimplemented */
}

// made all regions writeable
int as_prepare_load(struct addrspace *as)
{
    struct region_entry *region = as->region;
    while (region)
    {
        // set old bit to write bit
        region->flags |= (region->flags & RG_W) << 2;
        // set it to writeable
        region->flags |= RG_W;
        region = region->next;
    }
    return 0;
}

int as_complete_load(struct addrspace *as)
{
    struct region_entry *region = as->region;
    while (region)
    {
        // reset those cant be write
        if (!(region->flags | RG_OLD))
        {
            region->flags &= ~RG_W;
        }
        region = region->next;
    }
    return 0;
}

int as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
    /* Initial user-level stack pointer */
    int result;
    int stacksize = PAGE_SIZE * USERSTACKPAGES;
    result = as_define_region(as,
                              USERSTACK - stacksize,
                              stacksize,
                              RG_R, RG_W, 0);
    if (result)
    {
        return result;
    }

    *stackptr = USERSTACK;
    as->unused_top = USERSTACK - stacksize;
    return 0;
}

int as_define_heap(struct addrspace *as)
{
    int result;
    vaddr_t vbase, vtop;
    struct region_entry *region;

    region = as->region;
    while (region != NULL)
    {
        vbase = region->vbase;
        vtop = vbase + region->npages * PAGE_SIZE;
        // we don't count stack
        if (vbase >= as->unused_top)
        {
            break;
        }
        // now regions are in increase order
        // should aligned already
        as->heap_base = vtop;
        // when init, heap can't currupt stack,
        // otherwise vm will corrupt already
        region = region->next;
    }
    result = as_define_region(as, as->heap_base,
                              0 /* heap init with size 0 */,
                              RG_R, RG_W, 0);
    if (result)
    {
        return result;
    }
    return 0;
}

// wrapper of as_define_region
int as_define_mmap(struct addrspace *as, vaddr_t vaddr, size_t memsize,
                   int readable, int writeable, int executable, /* these are same as define_region */
                   struct vnode *vn, off_t offset, size_t filesize)
{
    int result;
    struct region_entry *region;
    region = kmalloc(sizeof(struct region_entry));
    if (region == NULL)
    {
        return ENOMEM;
    }
    // setup region
    result = create_region(region, vaddr, memsize, readable, writeable, executable);
    if (result)
    {
        return result;
    }
    // this is a mmap region, so we need to setup vn and offset
    // vn problem should been handled
    KASSERT(vn);
    region->vn = vn;
    region->offset = offset;
    region->filesize = filesize;
    region->vaddr = vaddr;
    // insert region
    result = insert_region(as, region);
    if (result)
    {
        return result;
    }
    return 0;
}

int find_mmap_place(struct addrspace *as, size_t length, vaddr_t *vaddr)
{
    struct region_entry *region;
    vaddr_t vtop, vbase, proper;
    // we should have as, this would be handle by file syscall
    KASSERT(as);
    region = as->region;
    // first fit
    proper = 0;
    while (region)
    {
        vbase = region->vbase + region->npages * PAGE_SIZE;
        if (region->next)
        {
            vtop = region->next->vbase;
        }
        else
        {
            // should not happen , we always have stack with us
            vtop = USERSPACETOP;
        }
        if (vtop >= vbase && vtop - vbase >= length)
        {
            // we find a fit
            proper = vbase;
            break;
        }
        region = region->next;
    }
    if (proper == 0)
    {
        // we run ot of virtual mem
        return ENOMEM;
    }
    *vaddr = proper;
    return 0;
}