/*
 *  linux/fs/file_dev.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>
#include <fcntl.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

//文件读函数-根据i节点和文件结构读取文件中数据
int file_read(struct m_inode * inode, struct file * filp, char * buf, int count)
{
	int left,chars,nr;
	struct buffer_head * bh;

	//首先判断参数的有效性
	if ((left=count)<=0)
		return 0;
	//若读取的字节数不为0就循环执行下面操作，直到数据全部读出或遇到问题
	while (left) {
		//根据i节点和文件表结构信息，
		//并利用bmap()得到包含文件当前读写位置的数据块在设备上对应的逻辑块号nr
		if (nr = bmap(inode,(filp->f_pos)/BLOCK_SIZE)) {
			//从i节点指定设备上读取该逻辑块
			if (!(bh=bread(inode->i_dev,nr)))
				break;
		} else
			bh = NULL;
		//接着计算文件读写指针在数据块中的偏移值nr，则在该数据块中我们希望读取的字节数为(BLOCK_SIZE-nr)
		nr = filp->f_pos % BLOCK_SIZE;
		chars = MIN( BLOCK_SIZE-nr , left );
		//调整读写文件指针
		filp->f_pos += chars;
		left -= chars;
		//若从设备上读到了数据
		if (bh) {
			//p指向缓冲块中开始读取数据的位置，并且复制chars字节到用户缓冲区buf中
			char * p = nr + bh->b_data;
			while (chars-->0)
				put_fs_byte(*(p++),buf++);
			brelse(bh);
		//否则往用户缓冲区中填入chars个0值字节
		} else {
			while (chars-->0)
				put_fs_byte(0,buf++);
		}
	}
	//修改i节点的访问时间为当前时间
	inode->i_atime = CURRENT_TIME;
	//返回读取的字节数
	return (count-left)?(count-left):-ERROR;
}

//文件写函数-根据i节点和文件结构信息，将用户数据写入文件中
//由i节点可以知道设备号，由file结构可以知道文件中当前读写指针位置
//buf指定用户态中缓冲区的位置，count为需要写入的字节数，返回值是实际写入的字节数
int file_write(struct m_inode * inode, struct file * filp, char * buf, int count)
{
	off_t pos;
	int block,c;
	struct buffer_head * bh;
	char * p;
	int i=0;

/*
 * ok, append may not work when many processes are writing at the same time
 * but so what. That way leads to madness anyway.
 */
	//首先确定数据写入文件的位置
	//如果是要向文件后添加数据，则将文件读写指针移到文件尾部，否则就将在文件当前读写指针处写入
	if (filp->f_flags & O_APPEND)
		pos = inode->i_size;
	else
		pos = filp->f_pos;
	// 然后在已写入字节数i(刚开始为0)小于指定写入字节数count时，循环执行以下操作
	while (i<count) {
		//先取文件数据块号(pos/BLOCK_SIZE)在设备上对应的逻辑块号block，如果不存在就创建一块
		if (!(block = create_block(inode,pos/BLOCK_SIZE)))
			break;
		//根据该逻辑块号读取设备上的相应逻辑块
		if (!(bh=bread(inode->i_dev,block)))
			break;
		//求出文件当前读写指针在该数据块中的偏移值c,并将指针p指向缓冲块中开始写入数据的位置
		c = pos % BLOCK_SIZE;
		p = c + bh->b_data;
		bh->b_dirt = 1;
		c = BLOCK_SIZE-c;
		if (c > count-i) c = count-i;
		pos += c;
		if (pos > inode->i_size) {
			inode->i_size = pos;
			inode->i_dirt = 1;
		}
		i += c;
		//从用户缓冲区buf中复制c个字节到高速缓冲块中p指向的开始位置处
		while (c-->0)
			*(p++) = get_fs_byte(buf++);
		brelse(bh);
	}
	inode->i_mtime = CURRENT_TIME;
	if (!(filp->f_flags & O_APPEND)) {
		filp->f_pos = pos;
		inode->i_ctime = CURRENT_TIME;
	}
	return (i?i:-1);
}
