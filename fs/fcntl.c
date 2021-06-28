/*
 *  linux/fs/fcntl.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h>
#include <errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#include <fcntl.h>
#include <sys/stat.h>

extern int sys_close(int fd);

//复制文件句柄(文件描述符)
//参数fd是欲复制的文件句柄，arg指定新文件句柄的最小数值
//返回新文件句柄或出错码
static int dupfd(unsigned int fd, unsigned int arg)
{
	//文件句柄就是进程文件结构指针数组索引号
	//首先检查函数参数的有效性
	if (fd >= NR_OPEN || !current->filp[fd])
		return -EBADF;
	if (arg >= NR_OPEN)
		return -EINVAL;
	//然后在当前进程的文件结构指针数组中寻找索引号等于或大于arg，但还没有使用的项
	while (arg < NR_OPEN)
		if (current->filp[arg])
			arg++;
		else
			break;
	if (arg >= NR_OPEN)
		return -EMFILE;
	//针对找到的空闲项(句柄)，在执行时关闭标志位图close_on_exec中复位该句柄位
	//即在运行exec()类函数时，不会关闭用dup()创建的句柄
	current->close_on_exec &= ~(1<<arg);
	//令该文件结构指针等于原句柄fd的指针，并将文件引用计数增1
	(current->filp[arg] = current->filp[fd])->f_count++;
	//最后返回新的文件句柄arg
	return arg;
}

int sys_dup2(unsigned int oldfd, unsigned int newfd)
{
	sys_close(newfd);
	return dupfd(oldfd,newfd);
}

//复制文件句柄系统调用
//复制指定文件句柄oldfd，新句柄的值是当前最小的未用句柄值
//参数：fildes-被复制的文件句柄
//返回新文件句柄值
int sys_dup(unsigned int fildes)
{
	return dupfd(fildes,0);
}

int sys_fcntl(unsigned int fd, unsigned int cmd, unsigned long arg)
{	
	struct file * filp;

	if (fd >= NR_OPEN || !(filp = current->filp[fd]))
		return -EBADF;
	switch (cmd) {
		case F_DUPFD:
			return dupfd(fd,arg);
		case F_GETFD:
			return (current->close_on_exec>>fd)&1;
		case F_SETFD:
			if (arg&1)
				current->close_on_exec |= (1<<fd);
			else
				current->close_on_exec &= ~(1<<fd);
			return 0;
		case F_GETFL:
			return filp->f_flags;
		case F_SETFL:
			filp->f_flags &= ~(O_APPEND | O_NONBLOCK);
			filp->f_flags |= arg & (O_APPEND | O_NONBLOCK);
			return 0;
		case F_GETLK:	case F_SETLK:	case F_SETLKW:
			return -1;
		default:
			return -1;
	}
}
