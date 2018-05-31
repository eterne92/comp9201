#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>

/* Place your frametable data-structures here 
 * You probably also want to write a frametable initialisation
 * function and call it from vm_bootstrap
 */

struct frame_table_entry
{
    int shared;
    int next;
};

struct frame_table_entry *frame_table = 0;
int first_free;
int table_size;

static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
static struct spinlock share_lock = SPINLOCK_INITIALIZER;

/* put frame table at the bottom of the ram */
void frame_table_init(void)
{
    int ramsize = ram_getsize();
    table_size = ramsize / PAGE_SIZE;
    frame_table = kmalloc(sizeof(struct frame_table_entry) * table_size);
    /* frame_table[i] maps paddr i * PAGE_SIZE to (i + 1) * PAGE_SIZE 
     * first free frame should start from first_free_addr / PAGE_SIZE + 1
     * last free frame should be at tablesize
     */
    first_free = ram_getfirstfree() / PAGE_SIZE + 1;
    /* link frametable */
    for (int i = first_free; i < table_size - 1; i++)
    {
        frame_table[i].next = i + 1;
    }
    /* end of link */
    frame_table[table_size - 1].next = -1;
}

/* Note that this function returns a VIRTUAL address, not a physical 
 * address
 * WARNING: this function gets called very early, before
 * vm_bootstrap().  You may wish to modify main.c to call your
 * frame table initialisation function, or check to see if the
 * frame table has been initialised and call ram_stealmem() otherwise.
 */

vaddr_t alloc_kpages(unsigned int npages)
{
    /*
    * IMPLEMENT ME.  You should replace this code with a proper
    *                implementation.
    */

    paddr_t addr;
    if (frame_table == 0)
    {
        spinlock_acquire(&stealmem_lock);
        addr = ram_stealmem(npages);
        spinlock_release(&stealmem_lock);
    }
    else
    {
        KASSERT(npages == 1);
        if (npages > 1)
        {
            return 0;
        }
        spinlock_acquire(&stealmem_lock);
        // Run out of mem
        if (first_free == -1)
        {
            addr = 0;
        }
        else
        {
            // get the mem addr
            addr = first_free * PAGE_SIZE;
            // first used
            frame_table[first_free].shared = 0;
            // set frame_table
            first_free = frame_table[first_free].next;
        }
        spinlock_release(&stealmem_lock);
    }

    if (addr == 0)
        return 0;
    vaddr_t vaddr = PADDR_TO_KVADDR(addr);
    bzero((void *)vaddr, PAGE_SIZE * npages);
    return vaddr;
}

void free_kpages(vaddr_t addr)
{
    paddr_t paddr = KVADDR_TO_PADDR(addr);
    // get page number
    int page_num = (paddr & PAGE_FRAME) / PAGE_SIZE;
    spinlock_acquire(&stealmem_lock);
    frame_table[page_num].next = first_free;
    first_free = page_num;
    spinlock_release(&stealmem_lock);
}

/* share a page */
void share_page(paddr_t addr)
{
    int page_num = addr / PAGE_SIZE;

    spinlock_acquire(&share_lock);
    // increase sharing
    frame_table[page_num].shared++;
    spinlock_release(&share_lock);
}

/* deshare a page */
void deshare_page(paddr_t addr)
{
    int page_num = addr / PAGE_SIZE;

    spinlock_acquire(&share_lock);
    // decrease sharing
    frame_table[page_num].shared--;

    // nobody using this frame any more
    // it's gone forever
    if (frame_table[page_num].shared == -1)
    {
        free_kpages(PADDR_TO_KVADDR(addr));
    }
    spinlock_release(&share_lock);
}

vaddr_t dup_frame(vaddr_t addr)
{
    vaddr_t newframe;
    // get a new frame
    newframe = alloc_kpages(1);
    if (newframe == 0)
    {
        return 0;
    }
    // copy memory
    memmove((void *)newframe, (void *)addr, PAGE_SIZE);

    return newframe;
}

// try to modify a shared frame
vaddr_t modify_frame(vaddr_t addr)
{
    paddr_t paddr = KVADDR_TO_PADDR(addr);
    int page_num = paddr / PAGE_SIZE;

    spinlock_acquire(&share_lock);
    // it's not shared, just return previous addr
    if (frame_table[page_num].shared == 0)
    {
        spinlock_release(&share_lock);
        return addr;
    }
    // copy a new frame
    vaddr_t newframe = dup_frame(addr & PAGE_FRAME);
    if (newframe == 0)
    {
        spinlock_release(&share_lock);
        return 0;
    }
    // the original frame could be unshared
    // or even not used anymore? maybe not
    frame_table[page_num].shared--;
    if (frame_table[page_num].shared == -1)
    {
        free_kpages(addr);
    }
    spinlock_release(&share_lock);
    return newframe;
}