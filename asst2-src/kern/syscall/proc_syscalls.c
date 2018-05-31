#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/limits.h>
#include <kern/stat.h>
#include <kern/seek.h>
#include <lib.h>
#include <uio.h>
#include <thread.h>
#include <current.h>
#include <synch.h>
#include <vfs.h>
#include <vnode.h>
#include <file.h>
#include <syscall.h>
#include <copyinout.h>
#include <proc.h>
#include <addrspace.h>
#include <mips/trapframe.h>
#include <kern/wait.h>

struct pctable *gpt;
struct trapframe;

int sys_fork(struct trapframe *tf, int *retval)
{
    struct proc *newproc;
    struct trapframe *newtf;
    struct proc_ctl *pc;
    struct proc_ctl *parent_pc;
    pid_t pid;
    int result;

    /* create proc */
    newproc = proc_create_runprogram("child");
    if (newproc == NULL)
    {
        return ENOMEM;
    }

    /* addr space */
    result = as_copy(curproc->p_addrspace, &newproc->p_addrspace);
    if (result)
    {
        proc_destroy(newproc);
        return result;
    }

    /* open filetable */
    filetable_copy(curproc->p_fd, newproc->p_fd);

    /* pid allocate */
    result = pid_allocate(newproc);
    if (result)
    {
        proc_destroy(newproc);
        return result;
    }
    /* process control set parent and child */
    pid = newproc->pid;
    lock_acquire(gpt->p_lock);
    pc = gpt->proc[pid];
    lock_release(gpt->p_lock);

    pc->parent = curproc->pid;

    lock_acquire(gpt->p_lock);
    parent_pc = gpt->proc[pc->parent];
    // set child bitmap
    parent_pc->child = parent_pc->child | (1 << pid);
    lock_release(gpt->p_lock);

    /* copy trapframe */
    newtf = kmalloc(sizeof(struct trapframe));
    if (newtf == NULL)
    {
        proc_destroy(newproc);
        return ENOMEM;
    }
    *newtf = *tf;

    /* fork thread, put it into work */
    result = thread_fork("child", newproc,
                         (void (*)(void *data1, unsigned long data2))enter_forked_process, newtf, 0);
    // kfree(newtf);
    if (result)
    {
        proc_destroy(newproc);
        return result;
    }

    *retval = pid;
    return 0;
}

int pid_allocate(struct proc *newproc)
{
    int i;
    struct proc_ctl *pc = proc_ctl_create(newproc);
    lock_acquire(gpt->p_lock);
    for (i = PID_MIN; i <= PID_MAX; i++)
    {
        if (gpt->proc[i] == NULL)
        {
            gpt->proc[i] = pc;
            pc->pid = i;
            newproc->pid = i;
            break;
        }
    }
    lock_release(gpt->p_lock);
    if (i > PID_MAX)
    {
        return ENPROC;
    }
    return 0;
}

void pid_deallocate(pid_t pid)
{
    struct proc_ctl *pc;

    KASSERT(pid >= PID_MIN && pid <= PID_MAX);

    lock_acquire(gpt->p_lock);
    pc = gpt->proc[pid];
    lock_release(gpt->p_lock);

    KASSERT(pc != NULL);
    proc_ctl_destroy(pc);

    lock_acquire(gpt->p_lock);
    gpt->proc[pid] = NULL;
    lock_release(gpt->p_lock);
}

int sys_getpid(pid_t *retval)
{
    *retval = curproc->pid;
    return 0;
}

int sys_waitpid(pid_t pid, int *status, int options, int *retval)
{
    struct proc_ctl *pc;
    struct proc_ctl *parent_pc;
    pid_t parent;

    /* in os161 we always return pid */
    *retval = pid;
    /* check options */
    if (options != 0)
    {
        return EINVAL;
    }
    /* check pid */
    if (pid < PID_MIN || pid > PID_MAX)
    {
        return ESRCH;
    }

    lock_acquire(gpt->p_lock);
    pc = gpt->proc[pid];
    lock_release(gpt->p_lock);

    if (pc == NULL)
    {
        return ESRCH;
    }

    /* check parent */
    if (pc->parent != curproc->pid)
    {
        return ECHILD;
    }

    /* child not exited, wait for it */
    if (pc->exitstatus == false)
    {
        /* wait at child's waiting list */
        /* this wait chanel always get only 1 proc wating
         * since only parent wait for child
         */
        P(pc->p_sem);
    }

    /* check child status */
    if (pc->exitstatus == true)
    {
        /* child already exited */
        /* collect code and deallocate child*/
        *status = pc->exitcode;
        pid_deallocate(pid);
    }
    else
    {
        /* should never get here */
        panic("someting wrong with waitpid\n");
    }

    parent = curproc->pid;
    /* should not get race condition, lock up for further concern */
    lock_acquire(gpt->p_lock);
    parent_pc = gpt->proc[parent];
    lock_release(gpt->p_lock);
    /* set parent's child to 0 */
    parent_pc->child = parent_pc->child & ~(1 << pid);

    return 0;
}

void sys__exit(int exitcode)
{
    struct proc_ctl *pc;
    struct proc_ctl *init;
    pid_t pid;

    /* get process control struct */
    pid = curproc->pid;
    lock_acquire(gpt->p_lock);
    pc = gpt->proc[pid];
    lock_release(gpt->p_lock);

    /* check we have child or not */
    if (pc->child != 0)
    {
        /* too bad you have child */
        /* let init process have all your child */
        lock_acquire(gpt->p_lock);
        init = gpt->proc[1];
        lock_release(gpt->p_lock);
        init->child = init->child | pc->child;
    }
    /* good to be lonely, you can die now */
    pc->exitcode = _MKWAIT_EXIT(exitcode);
    pc->exitstatus = true;
    V(pc->p_sem);
    thread_exit();
}