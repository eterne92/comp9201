/*
 * Declarations for file handle and file table management.
 */

#ifndef _FILE_H_
#define _FILE_H_

/*
 * Contains some file-related maximum length constants
 */
#include <limits.h>
/* globale filetable size */
#define GOPEN_MAX 128
struct vnode;

/* abstract file */
struct file
{
    struct vnode *f_vnode;
    off_t pos;
    int flags;
    int opencount;
    struct lock *f_lock;
};

/* file table inside a process */
struct filetable
{
    int openfiles[OPEN_MAX];
};

/* global file table */
struct globalfd
{
    struct file *openfiles[GOPEN_MAX];
    struct lock *gfd_lock;
};

extern struct globalfd *gfd;

int filetable_create(struct filetable **fd);
int filetable_init(struct filetable *fd);
void filetable_copy(struct filetable *src, struct filetable *dest);
void file_destroy(struct file *f);
struct file *file_init(struct vnode *v, int flags);
void filetable_destroy(struct filetable *fd);

void gfd_init(void);
void gfd_destroy(void);

/* syscalls about files */
int sys_open(const_userptr_t filename, int flags, int *retval);
int sys_read(int filehandle, void *buf, size_t size, ssize_t *retval);
int sys_write(int filehandle, const void *buf, size_t size, ssize_t *retval);
int sys_lseek(int filehandle, off_t pos, int code, off_t *retval);
int sys_close(int filehandle);
int sys_dup2(int filehandle, int newhandle, int *retval);

#endif /* _FILE_H_ */
