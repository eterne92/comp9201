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

/*
 * Add your file-related functions here ...
 */
struct globalfd *gfd;

/* init a file */
struct file *file_init(struct vnode *v, int flags)
{
    struct file *ret;

    if (v == NULL)
    {
        return NULL;
    }

    ret = kmalloc(sizeof(struct file));
    if (ret == NULL)
    {
        return NULL;
    }

    ret->f_lock = lock_create("file lock");
    if (ret->f_lock == NULL)
    {
        kfree(ret);
        return NULL;
    }

    ret->f_vnode = v;
    ret->pos = 0;
    ret->flags = flags;
    ret->opencount = 1;
    return ret;
}

void file_destroy(struct file *f)
{
    kfree(f->f_lock);
    kfree(f);
}

/* create a filetable, set all value -1 */
int filetable_create(struct filetable **fd)
{
    struct filetable *ret = kmalloc(sizeof(struct filetable));
    if (ret == NULL)
    {
        return ENOMEM;
    }
    for (int i = 0; i < OPEN_MAX; i++)
    {
        ret->openfiles[i] = -1;
    }
    *fd = ret;
    return 0;
}

/* set fd 0, 1, 2 all to console */
int filetable_init(struct filetable *fd)
{
    struct vnode *v;
    int result;

    /* open stdin, stdout, stderr */
    for (int i = 0; i < 3; i++)
    {
        /* open console */
        char path[] = "con:";
        result = vfs_open(path, O_RDWR, 0, &v);
        if (result)
        {
            return result;
        }

        struct file *f = file_init(v, O_RDWR);
        if (f == NULL)
        {
            return ENOMEM;
        }

        int gfd_num = 0;
        /* insert file into global filetable */
        lock_acquire(gfd->gfd_lock);
        for (gfd_num = 0; gfd_num < GOPEN_MAX; gfd_num++)
        {
            if (gfd->openfiles[gfd_num] == NULL)
            {
                gfd->openfiles[gfd_num] = f;
                break;
            }
        }
        lock_release(gfd->gfd_lock);

        if (gfd_num == GOPEN_MAX)
        {
            return ENFILE;
        }
        fd->openfiles[i] = gfd_num;
    }

    for (int i = 3; i < OPEN_MAX; i++)
    {
        fd->openfiles[i] = -1;
    }

    return 0;
}

void filetable_copy(struct filetable *src, struct filetable *dest)
{
    struct file *f;

    KASSERT(src != NULL && dest != NULL);

    for (int i = 0; i < OPEN_MAX; i++)
    {
        if (src->openfiles[i] != -1)
        {
            dest->openfiles[i] = src->openfiles[i];
            /* fetch file */
            lock_acquire(gfd->gfd_lock);
            f = gfd->openfiles[dest->openfiles[i]];
            lock_release(gfd->gfd_lock);
            lock_acquire(f->f_lock);
            f->opencount++;
            VOP_INCREF(f->f_vnode);
            lock_release(f->f_lock);
        }
    }
}

void filetable_destroy(struct filetable *fd)
{
    for (int i = 0; i < OPEN_MAX; i++)
    {
        if (fd->openfiles[i] >= 0)
        {
            _sys_close(i, fd);
        }
    }
    kfree(fd);
}

void gfd_init(void)
{
    gfd = kmalloc(sizeof(struct globalfd));
    /* since we only init gfd at boot I think it's ok to panic */
    if (gfd == NULL)
    {
        panic("gfd create failed");
    }
    for (int i = 0; i < OPEN_MAX; i++)
    {
        gfd->openfiles[i] = NULL;
    }
    gfd->gfd_lock = lock_create("gfd lock");
    if (gfd->gfd_lock == NULL)
    {
        panic("gfd lock create failed");
    }
}

void gfd_destroy(void)
{
    lock_destroy(gfd->gfd_lock);
    kfree(gfd);
}

int sys_open(const_userptr_t filename, int flags, int *retval)
{
    char path[NAME_MAX];
    struct vnode *v;
    struct filetable *fd;
    struct file *f;
    size_t got;
    int fd_num;
    int result;
    int gfd_num = 0;

    /* copy userlevel string into kernel */
    result = copyinstr(filename, path, NAME_MAX, &got);
    if (result)
    {
        return result;
    }
    result = vfs_open(path, flags, 0, &v);
    /* handle vfs_open errs */
    if (result)
    {
        return result;
    }

    fd = curproc->p_fd;
    /* If we assume there is only one thread inside process 
     * there should be no race condition over process's file
     * table.
     */
    for (fd_num = 0; fd_num < OPEN_MAX; fd_num++)
    {
        if (fd->openfiles[fd_num] == -1)
        {
            break;
        }
    }

    if (fd_num == OPEN_MAX)
    {
        return EMFILE;
    }

    /* create a file using vnode */
    f = file_init(v, flags);
    if (f == NULL)
    {
        return ENOMEM;
    }

    /* find smallest slot, may have race condition */
    lock_acquire(gfd->gfd_lock);
    for (gfd_num = 0; gfd_num < GOPEN_MAX; gfd_num++)
    {
        if (gfd->openfiles[gfd_num] == NULL)
        {
            break;
        }
    }
    if (gfd_num == GOPEN_MAX)
    {
        file_destroy(f);
        lock_release(gfd->gfd_lock);
        return ENFILE;
    }
    gfd->openfiles[gfd_num] = f;
    lock_release(gfd->gfd_lock);

    fd->openfiles[fd_num] = gfd_num;
    VOP_INCREF(f->f_vnode);

    *retval = fd_num;

    return 0;
}

int sys_read(int filehandle, void *buf, size_t size, ssize_t *retval)
{
    struct vnode *v;
    struct filetable *fd;
    struct file *f;
    struct uio auio;
    struct iovec aiov;
    void *kbuf;
    int result;
    int gfd_num;

    fd = curproc->p_fd;
    /* check valid fd number */
    if (filehandle < 0)
    {
        return EBADF;
    }
    if (filehandle >= OPEN_MAX || fd->openfiles[filehandle] < 0)
    {
        return EBADF;
    }

    gfd_num = fd->openfiles[filehandle];

    /* fetch file from gfd */
    lock_acquire(gfd->gfd_lock);
    f = gfd->openfiles[gfd_num];
    lock_release(gfd->gfd_lock);

    if ((f->flags & O_ACCMODE) == O_WRONLY)
    {
        return EBADF;
    }

    /* do read, race may happen between two processes
     * operate the same file at the same time.
     */
    kbuf = kmalloc(size);
    lock_acquire(f->f_lock);
    v = f->f_vnode;
    uio_kinit(&aiov, &auio, kbuf, size, f->pos, UIO_READ);
    result = VOP_READ(v, &auio);
    if (result)
    {

        lock_release(f->f_lock);
        return result;
    }

    *retval = size - auio.uio_resid;
    f->pos = f->pos + *retval;
    lock_release(f->f_lock);

    result = copyout(kbuf, buf, size);
    if (result)
    {
        kfree(kbuf);
        return result;
    }

    kfree(kbuf);
    return 0;
}

int sys_write(int filehandle, const void *buf, size_t size, ssize_t *retval)
{
    struct vnode *v;
    struct filetable *fd;
    struct file *f;
    struct uio auio;
    struct iovec aiov;
    void *kbuf;
    int result;
    int gfd_num;

    fd = curproc->p_fd;
    /* check valid fd number */
    if (filehandle < 0)
    {
        return EBADF;
    }

    if (filehandle >= OPEN_MAX || fd->openfiles[filehandle] < 0)
    {
        return EBADF;
    }

    gfd_num = fd->openfiles[filehandle];
    /* fetch file from gfd */
    lock_acquire(gfd->gfd_lock);
    f = gfd->openfiles[gfd_num];
    lock_release(gfd->gfd_lock);

    if ((f->flags & O_ACCMODE) == O_RDONLY)
    {
        return EBADF;
    }

    /* copy in buf */
    kbuf = kmalloc(size);
    result = copyin((const_userptr_t)buf, kbuf, size);
    if (result)
    {
        kfree(kbuf);
        return result;
    }

    lock_acquire(f->f_lock);
    v = f->f_vnode;
    uio_kinit(&aiov, &auio, kbuf, size, f->pos, UIO_WRITE);
    result = VOP_WRITE(v, &auio);
    if (result)
    {
        lock_release(f->f_lock);
        return result;
    }

    *retval = size - auio.uio_resid;
    f->pos = f->pos + *retval;
    lock_release(f->f_lock);
    kfree(kbuf);
    return 0;
}

int sys_lseek(int filehandle, off_t pos, int code, off_t *retval)
{
    struct vnode *v;
    struct filetable *fd;
    struct file *f;
    off_t newpos;
    int result;
    struct stat statbuf;
    int gfd_num;

    fd = curproc->p_fd;
    /* check valid fd number */
    if (filehandle < 0)
    {
        return EBADF;
    }
    if (filehandle >= OPEN_MAX || fd->openfiles[filehandle] < 0)
    {
        return EBADF;
    }

    gfd_num = fd->openfiles[filehandle];
    /* fetch file from gfd */
    lock_acquire(gfd->gfd_lock);
    f = gfd->openfiles[gfd_num];
    lock_release(gfd->gfd_lock);

    v = f->f_vnode;
    if (!VOP_ISSEEKABLE(v))
    {
        return ESPIPE;
    }

    /* lock file up when we change position */
    lock_acquire(f->f_lock);
    switch (code)
    {
    case SEEK_SET:
        newpos = pos;
        break;

    case SEEK_CUR:
        newpos = f->pos + pos;
        break;

    case SEEK_END:
        result = VOP_STAT(v, &statbuf);
        if (result)
        {
            lock_release(f->f_lock);
            return result;
        }
        newpos = statbuf.st_size + pos;
        break;

    default:
        lock_release(f->f_lock);
        return EINVAL;
    }

    if (newpos < 0)
    {
        lock_release(f->f_lock);
        return EINVAL;
    }

    f->pos = newpos;
    lock_release(f->f_lock);
    *retval = newpos;
    return 0;
}

int sys_close(int filehandle)
{
    return _sys_close(filehandle, curproc->p_fd);
}

/* close a file in a process level filetable.
 * a spcified filetable is given so we we can 
 * close another process's filetable.
 */
int _sys_close(int filehandle, struct filetable *fd)
{
    struct vnode *v;
    struct file *f;
    int gfd_num;

    /* check valid fd number */
    if (filehandle < 0)
    {
        return EBADF;
    }
    if (filehandle >= OPEN_MAX || fd->openfiles[filehandle] < 0)
    {
        return EBADF;
    }

    gfd_num = fd->openfiles[filehandle];
    lock_acquire(gfd->gfd_lock);
    f = gfd->openfiles[gfd_num];
    lock_release(gfd->gfd_lock);

    fd->openfiles[filehandle] = -1;

    v = f->f_vnode;
    lock_acquire(f->f_lock);
    /* check vnode count first */
    if (v->vn_refcount > 1)
    {
        /* vnode exist */
        vfs_close(v);
    }
    else if (v->vn_refcount == 1)
    {
        /* vnode not exist anymore*/
        vnode_cleanup(v);
    }

    /* check file count */
    if (f->opencount == 1)
    {
        lock_release(f->f_lock);
        file_destroy(f);
        /* set gfd table slot to NULL */
        lock_acquire(gfd->gfd_lock);
        gfd->openfiles[gfd_num] = NULL;
        lock_release(gfd->gfd_lock);
    }
    else
    {
        f->opencount--;
        lock_release(f->f_lock);
    }

    return 0;
}

int sys_dup2(int filehandle, int newhandle, int *retval)
{
    struct filetable *fd;
    struct file *f;
    int result;
    int gfd_num;

    fd = curproc->p_fd;
    /* check valid fd number */
    if (filehandle < 0 || newhandle < 0)
    {
        return EBADF;
    }
    if (filehandle >= OPEN_MAX || fd->openfiles[filehandle] < 0)
    {
        return EBADF;
    }

    gfd_num = fd->openfiles[filehandle];
    /* fetch file from gfd */
    lock_acquire(gfd->gfd_lock);
    f = gfd->openfiles[gfd_num];
    lock_release(gfd->gfd_lock);

    /* check newhandle */
    if (newhandle >= OPEN_MAX)
    {
        return EBADF;
    }
    if (fd->openfiles[newhandle] != -1)
    {
        /* newhandle opened */
        result = sys_close(newhandle);
        if (result)
        {
            return result;
        }
    }
    /* now the newhandle is available */
    fd->openfiles[newhandle] = gfd_num;

    lock_acquire(f->f_lock);
    f->opencount++;
    VOP_INCREF(f->f_vnode);
    lock_release(f->f_lock);

    *retval = newhandle;

    return 0;
}