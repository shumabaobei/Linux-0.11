/*
 *  linux/kernel/blk_drv/ramdisk.c
 *
 *  Written by Theodore Ts'o, 12/2/91
 */

#include <string.h>

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <asm/segment.h>
#include <asm/memory.h>

#define MAJOR_NR 1
#include "blk.h"

char	*rd_start;
int	rd_length = 0;			//虚拟盘所占内存大小(字节)

void do_rd_request(void)
{
	int	len;
	char	*addr;

	INIT_REQUEST;
	addr = rd_start + (CURRENT->sector << 9);
	len = CURRENT->nr_sectors << 9;
	if ((MINOR(CURRENT->dev) != 1) || (addr+len > rd_start+rd_length)) {
		end_request(0);
		goto repeat;
	}
	if (CURRENT-> cmd == WRITE) {
		(void ) memcpy(addr,
			      CURRENT->buffer,
			      len);
	} else if (CURRENT->cmd == READ) {
		(void) memcpy(CURRENT->buffer, 
			      addr,
			      len);
	} else
		panic("unknown ramdisk-command");
	end_request(1);
	goto repeat;
}

/*
 * Returns amount of memory which needs to be reserved.
 */
//返回内存虚拟盘ramdisk所需的内存量
//虚拟盘初始化函数
long rd_init(long mem_start, int length)
{
	int	i;
	char	*cp;

	//首先设置虚拟盘设备的请求项处理函数指针指向do_rd_request()
	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;
	rd_start = (char *) mem_start;		//对于16MB系统该值为4MB
	rd_length = length;			//虚拟盘长度
	cp = rd_start;
	for (i=0; i < length; i++)
		*cp++ = '\0';			//盘区清零
	return(length);
}

/*
 * If the root device is the ram disk, try to load it.
 * In order to do this, the root device is originally set to the
 * floppy, and we later change it to be ram disk.
 */
//尝试把根文件系统加载到虚拟盘
//1磁盘块=1024字节
void rd_load(void)
{
	struct buffer_head *bh;			//高速缓冲块头指针
	struct super_block	s;		//文件超级块结构
	int		block = 256;	/* Start at block 256 */ //根文件系统映像文件被存储在boot盘第256磁盘块开始处
	int		i = 1;
	int		nblocks;		//文件系统盘块总数
	char		*cp;		/* Move pointer */
	
	//如果ramdisk长度为零则退出，否则显示ramdisk的大小以及内存位置
	if (!rd_length)
		return;
	printk("Ram disk: %d bytes, starting at 0x%x\n", rd_length,
		(int) rd_start);
	//如果根文件设备不是软盘设备则退出
	if (MAJOR(ROOT_DEV) != 2)
		return;
	//读根文件系统的基本参数
	//即读软盘块256+1,256和256+2,这里block+1是指磁盘上的超级块 
	bh = breada(ROOT_DEV,block+1,block,block+2,-1);
	if (!bh) {
		printk("Disk error while looking for ramdisk!\n");
		return;
	}
	//把缓冲区中的磁盘超级块(d_super_block是磁盘超级块结构)复制到s变量，并释放缓冲区
	*((struct d_super_block *) &s) = *((struct d_super_block *) bh->b_data);
	brelse(bh);
	//如果不等，说明不是minix文件系统
	if (s.s_magic != SUPER_MAGIC)
		/* No ram disk image present, assume normal floppy boot */
		//磁盘中没有ramdisk映像文件，退出去执行通常的软盘引导
		return;
	//文件系统中数据块总数大于内存虚拟盘所能容纳的开始，则不能执行加载操作，显示储蓄哦信息并返回
	nblocks = s.s_nzones << s.s_log_zone_size;
	if (nblocks > (rd_length >> BLOCK_SIZE_BITS)) {
		printk("Ram disk image too big!  (%d blocks, %d avail)\n", 
			nblocks, rd_length >> BLOCK_SIZE_BITS);
		return;
	}
	//若虚拟盘能容纳下文件系统总数据块数，则显示加载数据块信息
	printk("Loading %d bytes into ram disk... 0000k", 
		nblocks << BLOCK_SIZE_BITS);
	//cp指向内存虚拟盘起始处
	cp = rd_start;
	//执行循环操作将磁盘上根文件系统映像文件加载到虚拟盘上
	while (nblocks) {
		if (nblocks > 2) 
			bh = breada(ROOT_DEV, block, block+1, block+2, -1);
		else
			bh = bread(ROOT_DEV, block);
		if (!bh) {
			printk("I/O error on block %d, aborting load\n", 
				block);
			return;
		}
		(void) memcpy(cp, bh->b_data, BLOCK_SIZE);
		brelse(bh);
		printk("\010\010\010\010\010%4dk",i);
		cp += BLOCK_SIZE;
		block++;
		nblocks--;
		i++;
	}
	//当boot盘中从256盘块开始的整个根文件系统加载完毕后，显示"done"
	printk("\010\010\010\010\010done \n");
	//把目前根文件设备号修改成虚拟盘的设备号0x0101
	ROOT_DEV=0x0101;
}
