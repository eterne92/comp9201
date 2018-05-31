#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>
#include <proc.h>
#include <current.h>
#include <spl.h>
#include <uio.h>
#include <vnode.h>
#include <vfs.h>

static struct hpt_entry *page_table;
static int hpt_size;
static struct spinlock hpt_lock = SPINLOCK_INITIALIZER;
int entry_size = 0;

static uint32_t hpt_hash(struct addrspace *as, vaddr_t faultaddr)
{
    uint32_t index;

    index = (((uint32_t)as) ^ (faultaddr >> PAGE_BITS)) % hpt_size;
    return index;
}

void vm_bootstrap(void)
{
    // fix hpt size to 2 times of frame entries
    // malloc hpt first, so the bump allocator will do it
    hpt_size = ram_getsize() / PAGE_SIZE * 2;
    page_table = kmalloc(sizeof(struct hpt_entry) * hpt_size);
    // then we init the frame table and let it take control
    frame_table_init();
    // set all of page_table 0, not sure whether needed or not
    bzero(page_table, sizeof(struct hpt_entry) * hpt_size);
}

uint32_t vm_lookup(struct addrspace *as, vaddr_t vaddr)
{
    struct hpt_entry *page;

    vaddr_t vpn = vaddr & PAGE_FRAME;
    KASSERT(as != NULL);
    uint32_t hash = hpt_hash(as, vaddr);
    page = &page_table[hash];
    // try to find the entrylo
    spinlock_acquire(&hpt_lock);
    while (page != NULL)
    {
        // find, just return
        if (page->as == as && page->vpn == vpn)
        {
            spinlock_release(&hpt_lock);
            return page->entrylo;
        }
        page = page->next;
    }
    spinlock_release(&hpt_lock);
    return 0;
}

void vm_delete(struct addrspace *as, vaddr_t vaddr)
{
    struct hpt_entry *page;
    uint32_t entrylo, hash;
    vaddr_t vpn;

    vpn = vaddr & PAGE_FRAME;
    KASSERT(as != NULL);
    hash = hpt_hash(as, vaddr);
    page = &page_table[hash];
    // try to find the entrylo
    spinlock_acquire(&hpt_lock);

    while (page != NULL)
    {
        // delete the entry (Not really delete delete)
        // just set the as to NULL and free the frame
        if (page->as == as && page->vpn == vpn)
        {
            page->as = NULL;
            entrylo = page->entrylo;
            // deshare this frame, others will be done by frametable
            deshare_page(entrylo & PAGE_FRAME);
            spinlock_release(&hpt_lock);
            return;
        }
        page = page->next;
    }

    spinlock_release(&hpt_lock);
    // We should never get here, since we only delete something
    // that is in the pagetable
    KASSERT(false);
    return;
}

int vm_insert(struct addrspace *as, vaddr_t vaddr, uint32_t entrylo)
{
    vaddr_t vpn;
    uint32_t hash;
    struct hpt_entry *page;

    vpn = vaddr & PAGE_FRAME;
    hash = hpt_hash(as, vaddr);

    page = &page_table[hash];
    // now we start to real touch the page table
    spinlock_acquire(&hpt_lock);

    // This shitty loop just insert a page_table_entry
    // into the page table. I promise it will end.
    while (1)
    {
        // find a ext chain node available
        if (page->as == NULL)
        {
            break;
        }
        // We are at the back of the chain
        // still no available node
        if (page->next == NULL)
        {
            // create a new chain node at the end
            page->next = kmalloc(sizeof(struct hpt_entry));

            page = page->next;
            // run out of mem
            if (page == NULL)
            {
                spinlock_release(&hpt_lock);
                return ENOMEM;
            }
            page->next = NULL;
            break;
        }
        // We are not at the back , just keep on going
        else
        {
            page = page->next;
        }
    }
    // maybe redundant?
    if (page == NULL)
    {
        // run out of mem
        spinlock_release(&hpt_lock);
        return ENOMEM;
    }

    page->as = as;
    page->vpn = vpn;
    page->entrylo = entrylo;

    spinlock_release(&hpt_lock);
    return 0;
}

void vm_update(struct addrspace *as, vaddr_t vaddr, uint32_t entrylo)
{
    struct hpt_entry *page;
    uint32_t hash;
    vaddr_t vpn;
    vpn = vaddr & PAGE_FRAME;
    KASSERT(as != NULL);
    hash = hpt_hash(as, vaddr);
    page = &page_table[hash];
    // try to find the entrylo
    spinlock_acquire(&hpt_lock);
    while (page != NULL)
    {
        // find, update
        if (page->as == as && page->vpn == vpn)
        {
            page->entrylo = entrylo;
            spinlock_release(&hpt_lock);
            return;
        }
        page = page->next;
    }
    // not found? unbelieveable
    KASSERT(false);
}

static int load_mmap(struct addrspace *as, 
                     struct region_entry *region, vaddr_t vaddr, paddr_t paddr){
    struct iovec iov;
    struct uio u;
    int result;
    off_t offset;
    vaddr_t baseaddr;
    size_t loadsize;

    loadsize = 0;
    baseaddr = vaddr & PAGE_FRAME;
    // if we are loading the first page
    if(baseaddr == region->vbase){
        baseaddr = region->vaddr;
        // calculate how much do we need to read
        if(region->filesize >= PAGE_SIZE){
            loadsize = PAGE_SIZE - (baseaddr - region->vbase);
        }
        else{
            loadsize = region->filesize;
        }
    }
    // we are loading second or later page
    else{
        baseaddr = vaddr & PAGE_FRAME;
        // calculate how much do we need to read
        if(region->filesize + region->vaddr - baseaddr < PAGE_SIZE){
            // we are in last page?
            loadsize = region->filesize + region->vaddr - baseaddr;
        }
        else{
            loadsize = PAGE_SIZE;
        }
    }
    // calc offset
    offset = baseaddr - region->vaddr + region->offset;
	iov.iov_ubase = (void *)baseaddr;
	iov.iov_len = PAGE_SIZE;		 // length of the memory space
	u.uio_iov = &iov;
	u.uio_iovcnt = 1;
    u.uio_resid = loadsize;
	u.uio_offset = offset;
	u.uio_segflg = (region->flags & RG_X) ? UIO_USERISPACE : UIO_USERSPACE;
	u.uio_rw = UIO_READ;
	u.uio_space = as;

    uint32_t entrylo = (paddr & TLBLO_PPAGE) | TLBLO_VALID | TLBLO_DIRTY;
    vm_insert(as, vaddr, entrylo);

    result = VOP_READ(region->vn, &u);
    if(result){
        return result;
    }


    return 0;
}

/* get a new fresh frame */
int get_frame(paddr_t *frame_addr)
{
    vaddr_t vaddr;
    vaddr = alloc_kpages(1);
    // no more memory
    if (vaddr == 0)
    {
        return ENOMEM;
    }
    bzero((void *)vaddr, PAGE_SIZE);
    *frame_addr = KVADDR_TO_PADDR(vaddr);
    return 0;
}


static struct region_entry *get_region(struct addrspace *as, vaddr_t faultaddress)
{
    struct region_entry *region;
    vaddr_t vbase;
    vaddr_t vtop;
    region = as->region;
    // we shouldn't have as without region
    KASSERT(region);
    while (region != NULL)
    {
        vbase = region->vbase;
        vtop = vbase + region->npages * PAGE_SIZE;
        if (faultaddress >= vbase && faultaddress < vtop)
        {
            break;
        }
        region = region->next;
    }
    return region;
}

int vm_fault(int faulttype, vaddr_t faultaddress)
{
    struct addrspace *as;
    struct region_entry *region;
    uint32_t entrylo, entryhi, entrylo_old;
    paddr_t paddr;
    vaddr_t newframe;
    int result;

    // no process
    if (faultaddress == 0)
    {
        return EFAULT;
    }

    if (curproc == NULL)
    {
        return EFAULT;
    }

    as = proc_getas();
    if (as == NULL)
    {
        // no address space
        return EFAULT;
    }

    // READONLY could happen when shared memory been modified
    if (faulttype == VM_FAULT_READONLY)
    {
        region = get_region(as, faultaddress);
        if (region == NULL || !(region->flags & RG_W))
        {
            return EFAULT;
        }

        // There should be a entry, otherwise we wont get a READONLY fault
        entrylo = vm_lookup(as, faultaddress);
        KASSERT(entrylo);

        entryhi = faultaddress & TLBHI_VPAGE;
        entrylo_old = entrylo;

        // duplicate a frame
        newframe = modify_frame(PADDR_TO_KVADDR(entrylo & PAGE_FRAME));
        entrylo = KVADDR_TO_PADDR(newframe) & TLBLO_PPAGE;
        entrylo |= TLBLO_VALID | TLBLO_DIRTY;

        vm_update(as, faultaddress, entrylo);
        tlb_update(entryhi, entrylo_old, entrylo);

        return 0;
    }

    entrylo = vm_lookup(as, faultaddress);
    if (entrylo == 0)
    {
        region = get_region(as, faultaddress);
        if (region == NULL)
        {
            // not valid vaddr
            return EFAULT;
        }
        if (faulttype == VM_FAULT_WRITE && !(region->flags & RG_W))
        {
            // not valid region
            return EFAULT;
        }
        // try to insert a page
        // get a fresh frame first
        // now we implement mmap, so we need to do more than get a fresh frame
        result = get_frame(&paddr);
        if (result)
        {
            return result;
        }
        // it's a file mapped region, shit!!!!
        if(region->vn != NULL){
            as_prepare_load(as);
            // load it
            result = load_mmap(as, region, faultaddress & PAGE_FRAME, paddr);
            if(result){
                return result;
            }
            // dont forget to set the region
            as_complete_load(as);
            tlb_flush();
            return 0;
        }

        // insert it into page table
        else{
            entrylo = paddr & TLBLO_PPAGE;
            entrylo |= TLBLO_VALID;
            if (faulttype == VM_FAULT_WRITE)
            {
                entrylo |= TLBLO_DIRTY;
            }

            result = vm_insert(as, faultaddress, entrylo);
            if (result)
            {
                return result;
            }
        }
    }

    entryhi = faultaddress & TLBHI_VPAGE;
    int spl = splhigh();

    tlb_random(entryhi, entrylo);

    splx(spl);

    return 0;
}

void tlb_flush(void)
{
    int spl, i;
    spl = splhigh();

    for (i = 0; i < NUM_TLB; i++)
    {
        tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
    }

    splx(spl);
}

void tlb_update(uint32_t entryhi, uint32_t entrylo_old, uint32_t entrylo_new)
{
    int spl, index;
    spl = splhigh();

    index = tlb_probe(entryhi, entrylo_old);
    tlb_write(entryhi, entrylo_new, index);

    splx(spl);
}

/*
 *
 * SMP-specific functions.  Unused in our configuration.
 */

void vm_tlbshootdown(const struct tlbshootdown *ts)
{
    (void)ts;
    panic("vm tried to do tlb shootdown?!\n");
}
