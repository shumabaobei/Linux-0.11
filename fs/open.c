/*
 *  linux/fs/open.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <utime.h>
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/kernel.h>
#include <asm/segment.h>

int sys_ustat(int dev, struct ustat * ubuf)
{
	return -ENOSYS;
}

int sys_utime(char * filename, struct utimbuf * times)
{
	struct m_inode * inode;
	long actime,modtime;

	if (!(inode=namei(filename)))
		return -ENOENT;
	if (times) {
		actime = get_fs_long((unsigned long *) &times->actime);
		modtime = get_fs_long((unsigned long *) &times->modtime);
	} else
		actime = modtime = CURRENT_TIME;
	inode->i_atime = actime;
	inode->i_mtime = modtime;
	inode->i_dirt = 1;
	iput(inode);
	return 0;
}

/*
 * XXX should we use the real or effective uid?  BSD uses the real uid,
 * so as to make this call useful to setuid programs.
 */
int sys_access(const char * filename,int mode)
{
	struct m_inode * inode;
	int res, i_mode;

	mode &= 0007;
	if (!(inode=namei(filename)))
		return -EACCES;
	i_mode = res = inode->i_mode & 0777;
	iput(inode);
	if (current->uid == inode->i_uid)
		res >>= 6;
	else if (current->gid == inode->i_gid)
		res >>= 6;
	if ((res & 0007 & mode) == mode)
		return 0;
	/*
	 * XXX we are doing this test last because we really should be
	 * swapping the effective with the real user id (temporarily),
	 * and then calling suser() routine.  If we do call the
	 * suser() routine, it needs to be called last. 
	 */
	if ((!current->uid) &&
	    (!(mode & 1) || (i_mode & 0111)))
		return 0;
	return -EACCES;
}

int sys_chdir(const char * filename)
{
	struct m_inode * inode;

	if (!(inode = namei(filename)))
		return -ENOENT;
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		return -ENOTDIR;
	}
	iput(current->pwd);
	current->pwd = inode;
	return (0);
}

int sys_chroot(const char * filename)
{
	struct m_inode * inode;

	if (!(inode=namei(filename)))
		return -ENOENT;
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		return -ENOTDIR;
	}
	iput(current->root);
	current->root = inode;
	return (0);
}

int sys_chmod(const char * filename,int mode)
{
	struct m_inode * inode;

	if (!(inode=namei(filename)))
		return -ENOENT;
	if ((current->euid != inode->i_uid) && !suser()) {
		iput(inode);
		return -EACCES;
	}
	inode->i_mode = (mode & 07777) | (inode->i_mode & ~07777);
	inode->i_dirt = 1;
	iput(inode);
	return 0;
}

int sys_chown(const char * filename,int uid,int gid)
{
	struct m_inode * inode;

	if (!(inode=namei(filename)))
		return -ENOENT;
	if (!suser()) {
		iput(inode);
		return -EACCES;
	}
	inode->i_uid=uid;
	inode->i_gid=gid;
	inode->i_dirt=1;
	iput(inode);
	return 0;
}

//打开(或创建)文件系统调用
//参数filename是文件名，flag是打开文件标志
//如果调用操作成功，则返回文件句柄(文件描述符)，否则返回出错码
int sys_open(const char * filename,int flag,int mode)
{
	struct m_inode * inode;
	struct file * f;
	int i,fd;

	//将用户设置的文件模式和进程模式屏蔽码相与，产生许可的文件模式
	mode &= 0777 & ~current->umask;
	//搜索进程结构中文件结构指针数组，以查找一个空闲项
	//空闲项的索引号fd即是句柄值
	for(fd=0 ; fd<NR_OPEN ; fd++)
		if (!current->filp[fd])
			break;
	if (fd>=NR_OPEN)
		return -EINVAL;
	//设置当前进程的执行时关闭文件句柄位图，复位对应的比特位
	current->close_on_exec &= ~(1<<fd);
	//然后为打开文件在文件表中寻找一个空闲结构项
	//令f指向文件表数组开始处，搜索空闲文件结构项(引用计数为0的项)
	f=0+file_table;
	for (i=0 ; i<NR_FILE ; i++,f++)
		if (!f->f_count) break;
	if (i>=NR_FILE)
		return -EINVAL;
	//让进程对应文件句柄fd的文件结构指针指向搜索到的文件结构，并令文件引用计数递增1
	(current->filp[fd]=f)->f_count++;
	//执行打开操作，若返回值小于0，则说明出错，于是释放刚申请到的文件结构，返回出错码i
	//若文件打开操作成功，则inode是已打开文件的i节点指针
	if ((i=open_namei(filename,flag,mode,&inode))<0) {
		current->filp[fd]=NULL;
		f->f_count=0;
		return i;
	}
/* ttys are somewhat special (ttyxx major==4, tty major==5) */
	//根据已打开文件i节点的属性字段，我们可以知道文件的类型
	if (S_ISCHR(inode->i_mode))
		//对于设备特殊文件名对应的i节点，其zone[0]中是该文件名指明的设备的设备号
		//如果打开的是字符设备文件
		//对于设备号是4的文件，如果当前进程是进程组首领并且当前进程的tty字段小于0(没有设备)
		//则设置当前进程的tty号为该i节点的子设备号，并设置当前进程tty对应的tty表项的父进程组号
		//等于当前进程的进程组号，表示为该进程组(会话期)分配控制终端
		if (MAJOR(inode->i_zone[0])==4) {
			if (current->leader && current->tty<0) {
				current->tty = MINOR(inode->i_zone[0]);
				tty_table[current->tty].pgrp = current->pgrp;
			}
		//对于设备号是5的字符文件
		} else if (MAJOR(inode->i_zone[0])==5)
			if (current->tty<0) {
				iput(inode);
				current->filp[fd]=NULL;
				f->f_count=0;
				return -EPERM;
			}
/* Likewise with block-devices: check for floppy_change */
	//如果打开的是块设备文件，则检查盘片是否更换过
	//若更换过则需要让高速缓冲区中该设备的所有缓冲区失效
	if (S_ISBLK(inode->i_mode))
		check_disk_change(inode->i_zone[0]);
	//初始化打开文件的文件结构
	f->f_mode = inode->i_mode;	//用该i节点属性，设置文件属性
	f->f_flags = flag;				//用flag参数，设置文件标识
	f->f_count = 1;					//将文件引用计数加1
	f->f_inode = inode;				//文件与i节点建立关系
	f->f_pos = 0;						//将文件读写指针设置为0
	//返回文件句柄号
	return (fd);
}

//创建文件系统调用
//成功返回文件句柄 
int sys_creat(const char * pathname, int mode)
{
	return sys_open(pathname, O_CREAT | O_TRUNC, mode);
}

//关闭文件系统调用
//参数fd是文件句柄
//成功则返回0，否则返回出错码
int sys_close(unsigned int fd)
{	
	struct file * filp;

	//首先检查参数有效性
	if (fd >= NR_OPEN)
		return -EINVAL;
	current->close_on_exec &= ~(1<<fd);
	if (!(filp = current->filp[fd]))
		return -EINVAL;
	//置该文件句柄的文件结构指针为NULL
	current->filp[fd] = NULL;
	//若在关闭文件之前，对应文件结构中的句柄要引用计数已经为0
	//则说明内核出错，停机
	if (filp->f_count == 0)
		panic("Close: file count is 0");
	//否则将对应文件结构的引用计数减1
	//此时如果它还不为0，则说明有其他进程正在使用该文件，于是返回0(成功)
	if (--filp->f_count)
		return (0);
	//如果引用计数已等于0，说明该文件已经没有进程引用，该文件结构已变为空闲
	//则释放该文件i节点，返回0
	iput(filp->f_inode);
	return (0);
}
