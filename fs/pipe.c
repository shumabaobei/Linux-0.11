/*
 *  linux/fs/pipe.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <signal.h>

#include <linux/sched.h>
#include <linux/mm.h>	/* for get_free_page */
#include <asm/segment.h>

//管道读操作函数
//参数inode是管道对应的i节点，buf是用户数据缓冲区指针，count是读取的字节数
int read_pipe(struct m_inode * inode, char * buf, int count)
{
	int chars, size, read = 0;

	//如果需要读取的字节计数count>0，就循环执行以下操作
	while (count>0) {
		//如果当前管道中没有数据(size=0)，则唤醒等待该节点的进程，这通常是写管道进程
		while (!(size=PIPE_SIZE(*inode))) {
			wake_up(&inode->i_wait);
			//如果已没有写管道者，即i节点引用计数值小于2，则返回已读字节数退出
			if (inode->i_count != 2) /* are there any writers? */
				return read;
			//否则在该i节点上睡眠
			sleep_on(&inode->i_wait);
		}
		//此时说明管道(缓冲区)中有数据
		chars = PAGE_SIZE-PIPE_TAIL(*inode);
		if (chars > count)
			chars = count;
		if (chars > size)
			chars = size;
		count -= chars;
		read += chars;
		size = PIPE_TAIL(*inode);
		PIPE_TAIL(*inode) += chars;
		PIPE_TAIL(*inode) &= (PAGE_SIZE-1);
		while (chars-->0)
			put_fs_byte(((char *)inode->i_size)[size++],buf++);
	}
	wake_up(&inode->i_wait);
	return read;
}

//管道写操作函数
//参数inode是管道对应的i节点，buf是用户数据缓冲区指针，count是将写入管道的字节数
int write_pipe(struct m_inode * inode, char * buf, int count)
{
	int chars, size, written = 0;

	//如果需要写入的字节计数count>0，就循环执行以下操作
	while (count>0) {
		//如果当前管道已经满了，则唤醒等待该节点的进程，通常唤醒的是读管道进程
		while (!(size=(PAGE_SIZE-1)-PIPE_SIZE(*inode))) {
			wake_up(&inode->i_wait);
			//如果已没有读管道进程，即i节点引用计数值小于2，
			//则向当前进程发送SIGPIPE信号，并返回已写入的字节数退出
			if (inode->i_count != 2) { /* no readers */
				current->signal |= (1<<(SIGPIPE-1));
				return written?written:-1;
			}
			//否则让当前进程在该i节点上睡眠，以等待读管道进程读取数据，从而让管道腾出空间
			sleep_on(&inode->i_wait);
		}
		//程序执行到这里表示管道缓冲区中有可写空间size
		//取管道头指针到缓冲区末端空间字节数chars，写管道操作是从管道头指针处开始写的
		chars = PAGE_SIZE-PIPE_HEAD(*inode);
		//如果cahrs大于还需要写入的字节数count，则令其等于count
		if (chars > count)
			chars = count;
		//如果chars大于当前管道中空闲空间长度size，则令其等于size
		if (chars > size)
			chars = size;
		//然后把需要写入字节数count减去此次可写入的字节数chars，并把写入自己恩数累加到written中
		count -= chars;
		written += chars;
		//再令size指向管道数据头指针处，并调整当前管道数据头部指针(前移chars字节)
		size = PIPE_HEAD(*inode);
		PIPE_HEAD(*inode) += chars;
		//若头指针差超过管道末端则绕回
		PIPE_HEAD(*inode) &= (PAGE_SIZE-1);
		//然后从用户缓冲区复制chars个字节到管道头指针开始处
		while (chars-->0)
			((char *)inode->i_size)[size++]=get_fs_byte(buf++);
	}
	//此次写管道操作结束，则唤醒等待管道的进程，返回已写入的字节数
	wake_up(&inode->i_wait);
	return written;
}

//创建管道系统调用
//在fildes所指的数组中创建一对文件句柄(描述符)。这对文件句柄指向一管道i节点
//参数：fildes-文件句柄数组。fildes[0]用于读管道数据，fildes[1]用于向管道写入数据
int sys_pipe(unsigned long * fildes)
{
	struct m_inode * inode;
	struct file * f[2];		//文件结构数组
	int fd[2];				//文件句柄数组
	int i,j;

	//首先从系统文件表中取两个空闲项(引用计数字段为0的项)，并分别设置引用计数为1
	j=0;
	for(i=0;j<2 && i<NR_FILE;i++)
		if (!file_table[i].f_count)
			(f[j++]=i+file_table)->f_count++;
	//若只有一个空闲项，则释放该该项(引用计数复位)
	if (j==1)
		f[0]->f_count=0;
	//若没有找到两个空闲项则返回-1
	if (j<2)
		return -1;
	//针对上面取得的两个文件表结构，分别分配一文件句柄号，并使进程文件结构指针数组的两项分别指向这两个文件结构
	//文件句柄即该数组的索引号
	j=0;
	for(i=0;j<2 && i<NR_OPEN;i++)
		if (!current->filp[i]) {
			current->filp[ fd[j]=i ] = f[j];
			j++;
		}
	if (j==1)
		current->filp[fd[0]]=NULL;
	if (j<2) {
		f[0]->f_count=f[1]->f_count=0;
		return -1;
	}
	//然后利用函数get_pipe_inode()申请一个管道使用的i节点，并为管道分配一页内存作为缓冲区
	if (!(inode=get_pipe_inode())) {
		current->filp[fd[0]] =
			current->filp[fd[1]] = NULL;
		f[0]->f_count = f[1]->f_count = 0;
		return -1;
	}
	//如果管道i节点申请成功，则对两个文件结构进行初始化操作，让它们都指向同一个管道i节点，并把读写指针都置零
	f[0]->f_inode = f[1]->f_inode = inode;
	f[0]->f_pos = f[1]->f_pos = 0;
	//第一个文件结构的文件模式置为读，第2个文件结构的文件模式置为写
	f[0]->f_mode = 1;		/* read */
	f[1]->f_mode = 2;		/* write */
	//最后将文件句柄数组复制到对应的用户空间数组中，成功返回0
	put_fs_long(fd[0],0+fildes);
	put_fs_long(fd[1],1+fildes);
	return 0;
}
