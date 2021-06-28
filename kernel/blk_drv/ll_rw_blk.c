/*
 *  linux/kernel/blk_dev/ll_rw.c
 *
 * (C) 1991 Linus Torvalds
 */

/*
 * This handles all read/write requests to block devices
 */
#include <errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include "blk.h"

/*
 * The request-struct contains all necessary data
 * to load a nr of sectors into memory
 */
//请求项数组队列，共有NR_REQUEST=32个请求项
struct request request[NR_REQUEST];

/*
 * used to wait on when there are no free requests
 */
struct task_struct * wait_for_request = NULL;

/* blk_dev_struct is:
 *	do_request-address
 *	next-request
 */
//块设备数组
//该数组使用主设备号作为索引，实际内容将在各块设备驱动程序初始化时填入
struct blk_dev_struct blk_dev[NR_BLK_DEV] = {
	{ NULL, NULL },		/* no_dev */		//0-无设备
	{ NULL, NULL },		/* dev mem */		//1-内存
	{ NULL, NULL },		/* dev fd */			//2-软驱设备
	{ NULL, NULL },		/* dev hd */		//3-硬盘设备
	{ NULL, NULL },		/* dev ttyx */		//4-ttyx设备
	{ NULL, NULL },		/* dev tty */		//5-tty设备
	{ NULL, NULL }		/* dev lp */		//6-lp打印机设备
};

//锁定指定缓冲区
//如果指定的缓冲块已经被其他任务锁定，则使自己睡眠(不可中断的等待)
//直到被执行解锁缓冲块的任务明确被唤醒
static inline void lock_buffer(struct buffer_head * bh)
{
	cli();								//关中断
	//如果缓冲区已锁定则睡眠，直到缓冲区解锁
	while (bh->b_lock)
		sleep_on(&bh->b_wait);
	//锁定该缓冲区
	bh->b_lock=1;
	sti();							//开中断
}

//解除锁定的缓冲区
static inline void unlock_buffer(struct buffer_head * bh)
{
	if (!bh->b_lock)
		printk("ll_rw_block.c: buffer not locked\n\r");
	bh->b_lock = 0;				//清锁定标志
	wake_up(&bh->b_wait);		//唤醒等待该缓冲区的任务
}

/*
 * add-request adds a request to the linked list.
 * It disables interrupts so that it can muck with the
 * request-lists in peace.
 */
//将链表中加入请求项
//本函数把已经设置好的请求项req添加到指定设备的请求项链表中
//如果该设备的当前请求项指针为空，则可以设置req为当前请求项并立刻调用设备请求项处理函数
//否则就把req请求项插入到该请求项链表中
static void add_request(struct blk_dev_struct * dev, struct request * req)
{
	struct request * tmp;

	//首先再进一步对参数提供的请求项和标志作初始设置
	req->next = NULL;						//置空请求项中的下一请求项指针
	cli();										//关中断
	if (req->bh)
		req->bh->b_dirt = 0;					//清除请求项相关缓冲区"脏"标志
	//然后查看指定设备是否正忙
	//如果指定设备dev当前请求项(current_request)子段为空，则表示目前该设备没有请求项
	//本次是第1个请求项，也是唯一一个请求项
	if (!(tmp = dev->current_request)) {
		dev->current_request = req;			//块设备当前指针直接指向该请求项
		sti();									//开中断
		(dev->request_fn)();					//指向请求函数，对于硬盘是do_hd_request()
		return;
	}
	for ( ; tmp->next ; tmp=tmp->next)
		if ((IN_ORDER(tmp,req) ||
		    !IN_ORDER(tmp,tmp->next)) &&
		    IN_ORDER(req,tmp->next))
			break;
	req->next=tmp->next;
	tmp->next=req;
	sti();
}

//创建请求项并插入请求队列
static void make_request(int major,int rw, struct buffer_head * bh)
{
	struct request * req;
	int rw_ahead;

/* WRITEA/READA is special case - it is not really needed, so if the */
/* buffer is locked, we just forget about it, else it's a normal read */
	//处理READA和WRITEA情况
	if (rw_ahead = (rw == READA || rw == WRITEA)) {
		if (bh->b_lock)
			return;
		if (rw == READA)
			rw = READ;
		else
			rw = WRITE;
	}
	if (rw!=READ && rw!=WRITE)
		panic("Bad block dev command, must be R/W/RA/WA");
	//锁定缓冲区
	lock_buffer(bh);
	//有两种情况可以不必添加请求项
	//一是当命令是写(WRITE)，但缓冲区中的数据在读入后并没有修改过
	//二是当命令是读(READ)，但缓冲区中的数据已经是更新过的，即与块设备上的完全一样
	if ((rw == WRITE && !bh->b_dirt) || (rw == READ && bh->b_uptodate)) {
		unlock_buffer(bh);
		return;
	}
repeat:
/* we don't allow the write-requests to fill up the queue completely:
 * we want some room for reads: they take precedence. The last third
 * of the requests are only for reads.
 */
	//读请求从尾端开始，写请求从2/3开始
	if (rw == READ)
		req = request+NR_REQUEST;
	else
		req = request+((NR_REQUEST*2)/3);
/* find an empty request */
	//从后向前搜索空闲请求项
	while (--req >= request)
		if (req->dev<0)			//dev初始化为-1，即空闲项
			break;
/* if none found, sleep on new requests: check for rw_ahead */
	//如果没有找到空闲项，则让该次新请求操作睡眠
	if (req < request) {
		if (rw_ahead) {
			unlock_buffer(bh);
			return;
		}
		sleep_on(&wait_for_request);
		goto repeat;
	}
/* fill up the request-info, and add it to the queue */
	//项空闲请求项中填写请求信息，并将其加入队列中
	req->dev = bh->b_dev;				//设备号
	req->cmd = rw;						//命令(READ/WRITE)
	req->errors=0;							//操作时产生的错误次数
	req->sector = bh->b_blocknr<<1;		//起始扇区。块号转换成扇区号(1块=2扇区)
	req->nr_sectors = 2;					//本请求项需要读写的扇区数
	req->buffer = bh->b_data;			//请求项缓冲区指针指向需读写的数据缓冲区
	req->waiting = NULL;					//任务等待操作执行完成的地方
	req->bh = bh;							//缓冲块头指针
	req->next = NULL;					//指向下一请求队列
	//将请求项加入队列中
	add_request(major+blk_dev,req);		
}

//低层读写数据块函数(Low Level Read Write Block)
//主要功能是创建块设备读写请求项并插入到指定块设备请求队列中，实际读写操作由设备的request_fn()函数完成
//对于硬盘操作，该函数是do_hd_request();对于软盘操作，该函数是do_fd_request();对于虚拟盘则是do_rd_request();
void ll_rw_block(int rw, struct buffer_head * bh)
{
	unsigned int major;

	//如果主设备号不存在或者该设备号的请求操作函数不存在，则显示出错信息并返回
	if ((major=MAJOR(bh->b_dev)) >= NR_BLK_DEV ||
	!(blk_dev[major].request_fn)) {
		printk("Trying to read nonexistent block-device\n\r");
		return;
	}
	make_request(major,rw,bh);
}

//块设备初始化函数
//初始化请求数组，将所有请求项置为空闲项(dev=-1)，共有32项(NR_REQUEST=32)
void blk_dev_init(void)
{
	int i;

	for (i=0 ; i<NR_REQUEST ; i++) {
		request[i].dev = -1;		//设置为空闲
		request[i].next = NULL;		//互不挂接
	}
}
