/*
 *  linux/fs/buffer.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'buffer.c' implements the buffer-cache functions. Race-conditions have
 * been avoided by NEVER letting a interrupt change a buffer (except for the
 * data, of course), but instead letting the caller do it. NOTE! As interrupts
 * can wake up a caller, some cli-sti sequences are needed to check for
 * sleep-on-calls. These should be extremely quick, though (I hope).
 */

/*
 * NOTE! There is one discordant note here: checking floppies for
 * disk change. This is where it fits best, I think, as it should
 * invalidate changed floppy-disk-caches.
 */

#include <stdarg.h>
 
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <asm/io.h>

extern int end;
struct buffer_head * start_buffer = (struct buffer_head *) &end;
struct buffer_head * hash_table[NR_HASH];	//NR_HASH=307
static struct buffer_head * free_list;				//空闲缓冲块链表头指针
static struct task_struct * buffer_wait = NULL;
int NR_BUFFERS = 0;

//等待指定缓冲区解锁
static inline void wait_on_buffer(struct buffer_head * bh)
{
	cli();								//关中断
	while (bh->b_lock)				//如果已被上锁则进程进入睡眠，等待其解锁
		sleep_on(&bh->b_wait);
	sti();								//开中断
}

int sys_sync(void)
{
	int i;
	struct buffer_head * bh;

	sync_inodes();		/* write out inodes into buffers */
	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		wait_on_buffer(bh);
		if (bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	return 0;
}

int sync_dev(int dev)
{
	int i;
	struct buffer_head * bh;

	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	sync_inodes();
	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	return 0;
}

void inline invalidate_buffers(int dev)
{
	int i;
	struct buffer_head * bh;

	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev)
			bh->b_uptodate = bh->b_dirt = 0;
	}
}

/*
 * This routine checks whether a floppy has been changed, and
 * invalidates all buffer-cache-entries in that case. This
 * is a relatively slow routine, so we have to try to minimize using
 * it. Thus it is called only upon a 'mount' or 'open'. This
 * is the best way of combining speed and utility, I think.
 * People changing diskettes in the middle of an operation deserve
 * to loose :-)
 *
 * NOTE! Although currently this is only for floppies, the idea is
 * that any additional removable block-device will use this routine,
 * and that mount/open needn't know that floppies/whatever are
 * special.
 */
void check_disk_change(int dev)
{
	int i;

	if (MAJOR(dev) != 2)
		return;
	if (!floppy_change(dev & 0x03))
		return;
	for (i=0 ; i<NR_SUPER ; i++)
		if (super_block[i].s_dev == dev)
			put_super(super_block[i].s_dev);
	invalidate_inodes(dev);
	invalidate_buffers(dev);
}

//hash函数和hash表项的计算宏定义
#define _hashfn(dev,block) (((unsigned)(dev^block))%NR_HASH)	//取307的模
#define hash(dev,block) hash_table[_hashfn(dev,block)]

//从hash队列和空闲缓冲队列中移走缓冲块
//hash队列是双向链表结构，空闲缓冲块队列是双向循环链表结构
static inline void remove_from_queues(struct buffer_head * bh)
{
/* remove from hash-queue */
	//从hash队列中移除缓冲块
	if (bh->b_next)
		bh->b_next->b_prev = bh->b_prev;
	if (bh->b_prev)
		bh->b_prev->b_next = bh->b_next;
	//如果该缓冲区是该队列的头一个块，则让hash表的对应项指向本队列中的下一个缓冲区
	if (hash(bh->b_dev,bh->b_blocknr) == bh)
		hash(bh->b_dev,bh->b_blocknr) = bh->b_next;
/* remove from free list */
	//从空闲缓冲块表中移除缓冲块
	if (!(bh->b_prev_free) || !(bh->b_next_free))
		panic("Free block list corrupted");
	bh->b_prev_free->b_next_free = bh->b_next_free;
	bh->b_next_free->b_prev_free = bh->b_prev_free;
	//如果空闲链表头指向本缓冲区，则让其指向下一个缓冲区
	if (free_list == bh)
		free_list = bh->b_next_free;
}

//将缓冲块插入空闲链表尾部，同时放入hash队列中
static inline void insert_into_queues(struct buffer_head * bh)
{
/* put at end of free list */
	//放在空闲链表末尾处
	bh->b_next_free = free_list;
	bh->b_prev_free = free_list->b_prev_free;
	free_list->b_prev_free->b_next_free = bh;
	free_list->b_prev_free = bh;
/* put the buffer in new hash-queue if it has a device */
	//如果该缓冲块对应一个设备，则将其插入新hash队列中
	bh->b_prev = NULL;
	bh->b_next = NULL;
	if (!bh->b_dev)
		return;
	bh->b_next = hash(bh->b_dev,bh->b_blocknr);
	hash(bh->b_dev,bh->b_blocknr) = bh;
	bh->b_next->b_prev = bh;
}

//利用hash表在高速缓冲中寻找给定设备和指定块号的缓冲区块
//如果找到则返回缓冲区块的指针，否则返回NULL
static struct buffer_head * find_buffer(int dev, int block)
{		
	struct buffer_head * tmp;

	//搜索hash表，寻找指定设备号和块号的缓冲块
	for (tmp = hash(dev,block) ; tmp != NULL ; tmp = tmp->b_next)
		if (tmp->b_dev==dev && tmp->b_blocknr==block)
			return tmp;
	return NULL;
}

/*
 * Why like this, I hear you say... The reason is race-conditions.
 * As we don't lock buffers (unless we are readint them, that is),
 * something might happen to it while we sleep (ie a read-error
 * will force it bad). This shouldn't really happen currently, but
 * the code is ready.
 */
//利用hash表在高速缓冲区中寻找指定的缓冲块，若找到则对该缓冲块上锁并返回块头指针
struct buffer_head * get_hash_table(int dev, int block)
{
	struct buffer_head * bh;

	for (;;) {
		//在高速缓冲中寻找给定的设备和指定块的缓冲区块，如果没有找到则返回NULL，退出
		if (!(bh=find_buffer(dev,block)))
			return NULL;
		bh->b_count++;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_blocknr == block)
			return bh;
		bh->b_count--;
	}
}

/*
 * Ok, this is getblk, and it isn't very clear, again to hinder
 * race-conditions. Most of the code is seldom used, (ie repeating),
 * so it should be much more efficient than it looks.
 *
 * The algoritm is changed: hopefully better, and an elusive bug removed.
 */
//用于同时判断缓冲区的修改标志和锁定标志，并且定义修改标志的权重要比锁定标志大
#define BADNESS(bh) (((bh)->b_dirt<<1)+(bh)->b_lock)
//取高速缓冲中指定的缓冲块
struct buffer_head * getblk(int dev,int block)
{
	struct buffer_head * tmp, * bh;

repeat:
	//搜索hash表，如果指定块已经在高速缓冲中，则返回对应缓冲区头指针
	if (bh = get_hash_table(dev,block))
		return bh;

	//扫描空闲数据块链表，寻找空闲缓冲区
	//首先让tmp指向空闲链表的第一个空闲缓冲区头
	tmp = free_list;
	do {
		//如果该缓冲区正被使用(引用计数不等于0)，则继续扫描下一项
		if (tmp->b_count)
			continue;
		//如果缓冲头指针bh为空或者tmp所指缓冲头的标志(修改、锁定)权重小于bh头标志的权重
		//则让bh指向tmp缓冲块头
		if (!bh || BADNESS(tmp)<BADNESS(bh)) {
			bh = tmp;
			//如果该tmp缓冲块头表明缓冲块既没有修改也没有锁定标志置位
			//则说明已为指定设备上的块取得对应的高速缓冲块，则退出循环
			if (!BADNESS(tmp))
				break;
		}
/* and repeat until we find something good */
	//重复操作直到找到合适的缓冲块
	} while ((tmp = tmp->b_next_free) != free_list);
	//如果循环检查发现所有缓冲块都正在被使用，则睡眠等待有空闲缓冲块可用
	//当有空闲缓冲块可用时本进程会被明确唤醒，然后跳转到函数开始处重新查找空闲缓冲块
	if (!bh) {
		sleep_on(&buffer_wait);
		goto repeat;
	}
	wait_on_buffer(bh);
	if (bh->b_count)
		goto repeat;
	while (bh->b_dirt) {
		sync_dev(bh->b_dev);
		wait_on_buffer(bh);
		if (bh->b_count)
			goto repeat;
	}
/* NOTE!! While we slept waiting for this block, somebody else might */
/* already have added "this" block to the cache. check it */
	if (find_buffer(dev,block))
		goto repeat;
/* OK, FINALLY we know that this buffer is the only one of it's kind, */
/* and that it's unused (b_count=0), unlocked (b_lock=0), and clean */
	//最终我们直到该缓冲块是指定参数的唯一一块
	//而且还没有被使用，也未被上锁，并且是干净的
	//于是我们占用此缓冲块，置引用计数为1，复位修改标志和有效标志
	bh->b_count=1;
	bh->b_dirt=0;
	bh->b_uptodate=0;
	//从hash队列和空闲链表中移除该缓冲区头，让该缓冲区用于指定设备和其上的指定块
	remove_from_queues(bh);
	//根据新设备号和块号重新插入空闲链表和hash队列新位置处
	bh->b_dev=dev;
	bh->b_blocknr=block;
	insert_into_queues(bh);
	return bh;
}

//释放指定缓冲块
//等待该缓冲区解锁，然后引用计数递减1，并明确地唤醒等待缓冲块的进程
void brelse(struct buffer_head * buf)
{
	if (!buf)
		return;
	wait_on_buffer(buf);
	if (!(buf->b_count--))
		panic("Trying to free free buffer");
	wake_up(&buffer_wait);
}

/*
 * bread() reads a specified block and returns the buffer that contains
 * it. It returns NULL if the block was unreadable.
 */
//从设备上读取指定的数据块并返回含有数据的缓冲区，如果指定的块不存在则返回NULL
struct buffer_head * bread(int dev,int block)
{
	struct buffer_head * bh;

	//在高速缓冲区中申请一块缓冲块，如果返回是NULL则表示内核出错停机。
	if (!(bh=getblk(dev,block)))
		panic("bread: getblk returned NULL\n");
	//如果该缓冲区中数据是有效的(已更新的)可以直接使用则返回
	if (bh->b_uptodate)
		return bh;
	//调用底层块设备读写ll_rw_block()函数，产生读设备请求
	ll_rw_block(READ,bh);
	//等待指定数据块被读入，并等待缓冲区解锁
	wait_on_buffer(bh);
	//在睡眠醒来之后，如果该缓冲区已更新，则返回缓冲区头指针
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return NULL;
}

//复制内存块
//从from地址复制一块(1024字节)数据到to位置
#define COPYBLK(from,to) \
__asm__("cld\n\t" \
	"rep\n\t" \
	"movsl\n\t" \
	::"c" (BLOCK_SIZE/4),"S" (from),"D" (to) \
	:"cx","di","si")

/*
 * bread_page reads four buffers into memory at the desired address. It's
 * a function of its own, as there is some speed to be got by reading them
 * all at the same time, not waiting for one to be read, and then another
 * etc.
 */
//读设备上一个页面(4个缓冲块)的内容到指定内存地址处
//参数address是保存页面数据的地址 dev是指定的设备号 b[4]是含有4个设备数据块号
void bread_page(unsigned long address,int dev,int b[4])
{
	struct buffer_head * bh[4];
	int i;

	//该函数循环执行4次，根据放在数组b[]中的4个块号从设备dev中读取一页内容放到指定内存位置address处
	for (i=0 ; i<4 ; i++)
		//对于参数b[i]给出的有效块号，函数首先从高速缓冲中取指定设备和块号的缓冲块
		if (b[i]) {
			if (bh[i] = getblk(dev,b[i]))
				//如果缓冲块中数据无效(未更新)则产生读设备请求从设备上读取相应数据块
				if (!bh[i]->b_uptodate)
					ll_rw_block(READ,bh[i]);
		} else
			bh[i] = NULL;
	//随后将4个缓冲块上的内容顺序复制到指定地址处
	for (i=0 ; i<4 ; i++,address += BLOCK_SIZE)
		if (bh[i]) {
			//在进行复制缓冲块之前先要睡眠等待缓冲块解锁(若被上锁的话)
			wait_on_buffer(bh[i]);
			//因为可能睡眠过了，所以我们还需要在复制之前再检查一下缓冲块中的数据是否是有效的
			if (bh[i]->b_uptodate)
				COPYBLK((unsigned long) bh[i]->b_data,address);
			brelse(bh[i]);			//释放该缓冲块
		}
}

/*
 * Ok, breada can be used as bread, but additionally to mark other
 * blocks for reading as well. End the argument list with a negative
 * number.
 */
//从指定设备读取指定的一些块
//函数参数个数可变，是一系列指定的块号
//成功时返回第1块的缓冲块头指针，否则返回NULL
struct buffer_head * breada(int dev,int first, ...)
{
	va_list args;
	struct buffer_head * bh, *tmp;

	//首先取可变参数表中的第1个参数(块号)
	//接着从高速缓冲区中取指定设备和块号的缓冲块
	//如果该缓冲块数据无效，则发出读设备数据块请求
	va_start(args,first);
	if (!(bh=getblk(dev,first)))
		panic("bread: getblk returned NULL\n");
	if (!bh->b_uptodate)
		ll_rw_block(READ,bh);
	//然后顺序取可变参数表中其他预读块号，对其进行同样处理
	while ((first=va_arg(args,int))>=0) {
		tmp=getblk(dev,first);
		if (tmp) {
			if (!tmp->b_uptodate)
				ll_rw_block(READA,bh);
			//因为上句是预读随后的数据块，只需读进高速缓冲区但并不是马上就使用
			//所以需要将其引用计数递减释放掉(因为在getblk()函数中会增加引用计数值)
			tmp->b_count--;
		}
	}
	//此时可变参数表中所有参数处理完毕
	va_end(args);
	//等待第1个缓冲区解锁(如果已被上锁)
	//在等待退出之后如果缓冲区中数据仍然有效，则返回缓冲区头指针退出
	wait_on_buffer(bh);
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return (NULL);
}

//缓冲区初始化函数
//参数buffer_end是缓冲区内存末端，对于具有16MB内存的系统，缓冲区末端被设置为4MB
//从缓冲区开始位置start_buffer处和缓冲区末端buffer_end处分别同时设置缓冲块头结构和
//对应的数据块，直到缓冲区中所有内存被分配完毕
void buffer_init(long buffer_end)
{
	struct buffer_head * h = start_buffer;
	void * b;
	int i;

	//b=4MB
	if (buffer_end == 1<<20)
		b = (void *) (640*1024);
	else
		b = (void *) buffer_end;
	//初始化缓冲区，建立空闲缓冲块循环链表，并获取系统中缓冲块数目
	//从缓冲区高端开始划分1KB大小的缓冲块，与此同时在缓冲区低端建立描述该缓冲块的结构buffer_header
	//并将这些buffer_header组成双向链表
	//h是指向缓冲头结构的指针，而h+1是指向内存地址连续的下一个缓冲头地址，也可以说是指向h缓冲头的末端外
	//为了保证有足够长度的内存来存储一个缓冲头结构，需要b所指向的内存块地址>=h缓冲头的末端，即>=h+1
	while ( (b -= BLOCK_SIZE) >= ((void *) (h+1)) ) {	//BLOCK_SIZE=1KB
		h->b_dev = 0;
		h->b_dirt = 0;
		h->b_count = 0;
		h->b_lock = 0;
		h->b_uptodate = 0;
		h->b_wait = NULL;
		h->b_next = NULL;
		h->b_prev = NULL;
		h->b_data = (char *) b;			//指向对应缓冲块数据块(1024字节)
		h->b_prev_free = h-1;
		h->b_next_free = h+1;
		h++;							
		NR_BUFFERS++;				//缓冲区块数累加
		if (b == (void *) 0x100000)		//若b递减到等于1MB，则让b指向地址640KB处
			b = (void *) 0xA0000;
	}
	h--;								//让h指向最后一个有效缓冲块头
	free_list = start_buffer;			//让空闲链表头指向第一个缓冲块
	free_list->b_prev_free = h;		//前后形成循环链表
	h->b_next_free = free_list;
	//初始化hash表，置表中所有指针为NULL
	for (i=0;i<NR_HASH;i++)			//NR_HASH=307
		hash_table[i]=NULL;
}	
