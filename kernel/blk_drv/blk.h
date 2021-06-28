#ifndef _BLK_H
#define _BLK_H

#define NR_BLK_DEV	7
/*
 * NR_REQUEST is the number of entries in the request-queue.
 * NOTE that writes may use only the low 2/3 of these: reads
 * take precedence.
 *
 * 32 seems to be a reasonable number: enough to get some benefit
 * from the elevator-mechanism, but not so much as to lock a lot of
 * buffers when they are in the queue. 64 seems to be too many (easily
 * long pauses in reading when heavy writing/syncing is going on)
 */
#define NR_REQUEST	32

/*
 * Ok, this is an expanded form so that we can use the same
 * request for paging requests when that is implemented. In
 * paging, 'bh' is NULL, and 'waiting' is used to wait for
 * read/write completion.
 */
//下面是请求队列中项的结构，如果字段dev=-1表示队列中该项没有被使用
struct request {
	int dev;		/* -1 if no request */	//发请求的设备号
	int cmd;		/* READ or WRITE */		//Read或Write命令
	int errors;						//操作时产生的错误次数
	unsigned long sector;			//起始扇区(1块=2扇区)
	unsigned long nr_sectors;		//读/写扇区数
	char * buffer;					//数据缓冲区
	struct task_struct * waiting;	//任务等待操作执行完成的地方???
	struct buffer_head * bh;		//缓冲区头指针
	struct request * next;			//指向下一个请求项
};

/*
 * This is used in the elevator algorithm: Note that
 * reads always go before writes. This is natural: reads
 * are much more time-critical than writes.
 */
#define IN_ORDER(s1,s2) \
((s1)->cmd<(s2)->cmd || (s1)->cmd==(s2)->cmd && \
((s1)->dev < (s2)->dev || ((s1)->dev == (s2)->dev && \
(s1)->sector < (s2)->sector)))

//块设备结构
struct blk_dev_struct {
	void (*request_fn)(void);			//请求操作的函数指针
	struct request * current_request;	//当前正在处理的请求信息结构
};

extern struct blk_dev_struct blk_dev[NR_BLK_DEV];
extern struct request request[NR_REQUEST];
extern struct task_struct * wait_for_request;

#ifdef MAJOR_NR

/*
 * Add entries as needed. Currently the only block devices
 * supported are hard-disks and floppies.
 */

#if (MAJOR_NR == 1)
/* ram disk */
#define DEVICE_NAME "ramdisk"
#define DEVICE_REQUEST do_rd_request
#define DEVICE_NR(device) ((device) & 7)
#define DEVICE_ON(device) 
#define DEVICE_OFF(device)

#elif (MAJOR_NR == 2)
/* floppy */
#define DEVICE_NAME "floppy"
#define DEVICE_INTR do_floppy
#define DEVICE_REQUEST do_fd_request
#define DEVICE_NR(device) ((device) & 3)
#define DEVICE_ON(device) floppy_on(DEVICE_NR(device))
#define DEVICE_OFF(device) floppy_off(DEVICE_NR(device))

#elif (MAJOR_NR == 3)
/* harddisk */
#define DEVICE_NAME "harddisk"
#define DEVICE_INTR do_hd
#define DEVICE_REQUEST do_hd_request
#define DEVICE_NR(device) (MINOR(device)/5)
#define DEVICE_ON(device)
#define DEVICE_OFF(device)

#elif
/* unknown blk device */
#error "unknown blk device"

#endif

#define CURRENT (blk_dev[MAJOR_NR].current_request)
#define CURRENT_DEV DEVICE_NR(CURRENT->dev)

#ifdef DEVICE_INTR
void (*DEVICE_INTR)(void) = NULL;
#endif
static void (DEVICE_REQUEST)(void);

//解锁指定的缓冲区
extern inline void unlock_buffer(struct buffer_head * bh)
{
	if (!bh->b_lock)
		printk(DEVICE_NAME ": free buffer being unlocked\n");
	bh->b_lock=0;					//解锁
	wake_up(&bh->b_wait);		//唤醒等待该缓冲区的进程
}

//结束请求处理
//首先关闭指定块设备，然后检查此次读写缓冲区是否有效
//如果有效则根据参数值设置缓冲区数据更新标志，并解锁该缓冲区
//最后唤醒等待该请求项的进程以及等待空闲请求项出现的进程，
//释放并从请求项链表中删除本请求项，并把当前请求项指针指向下一请求项
extern inline void end_request(int uptodate)
{
	DEVICE_OFF(CURRENT->dev);				//关闭设备
	if (CURRENT->bh) {
		CURRENT->bh->b_uptodate = uptodate;		//置更新标志为1
		unlock_buffer(CURRENT->bh);				//解锁缓冲区
	}
	if (!uptodate) {
		printk(DEVICE_NAME " I/O error\n\r");
		printk("dev %04x, block %d\n\r",CURRENT->dev,
			CURRENT->bh->b_blocknr);
	}
	wake_up(&CURRENT->waiting);
	wake_up(&wait_for_request);
	CURRENT->dev = -1;
	CURRENT = CURRENT->next;
}

//定义初始化请求项宏
#define INIT_REQUEST \
repeat: \
	if (!CURRENT) \							//如果当前请求结构指针为NULL则返回
		return; \
	if (MAJOR(CURRENT->dev) != MAJOR_NR) \			//如果当前设备的主设备号不对则死机
		panic(DEVICE_NAME ": request list destroyed"); \
	if (CURRENT->bh) { \
		if (!CURRENT->bh->b_lock) \						//如果请求项的缓冲区没锁定则死机
			panic(DEVICE_NAME ": block not locked"); \
	}

#endif

#endif
