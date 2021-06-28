/*
 *  linux/kernel/hd.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * This is the low-level hd interrupt support. It traverses the
 * request-list, using interrupts to jump between functions. As
 * all the functions are called within interrupts, we may not
 * sleep. Special care is recommended.
 * 
 *  modified by Drew Eckhardt to check nr of hd's from the CMOS.
 */

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/hdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#define MAJOR_NR 3
#include "blk.h"

#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \
inb_p(0x71); \
})

/* Max read/write errors/sector */
#define MAX_ERRORS	7
#define MAX_HD		2

static void recal_intr(void);

static int recalibrate = 1;
static int reset = 1;

/*
 *  This struct defines the HD's and their types.
 */
//定义硬盘参数及类型
//硬盘信息结构
struct hd_i_struct {
	int head,sect,cyl,wpcom,lzone,ctl;
	};
#ifdef HD_TYPE
struct hd_i_struct hd_info[] = { HD_TYPE };
#define NR_HD ((sizeof (hd_info))/(sizeof (struct hd_i_struct)))
#else
//先默认设置为0
struct hd_i_struct hd_info[] = { {0,0,0,0,0,0},{0,0,0,0,0,0} };
static int NR_HD = 0;
#endif

//定义硬盘分区结构
//给出每个分区从硬盘0道开始算起的物理起始扇区号和分区扇区总数
//其中5的倍数处的项(例如hd[0]和hd[5]等)代表整个硬盘的参数
static struct hd_struct {
	long start_sect;		//起始扇区号
	long nr_sects;			//总扇区数
} hd[5*MAX_HD]={{0,0},};

//读端口嵌入汇编宏
//读端口port，共读nr个字，保存在buf中
//insw指令从DX指定的外设端口输入一个字到有ES:DI指定的存储器中
#define port_read(port,buf,nr) \
__asm__("cld;rep;insw"::"d" (port),"D" (buf),"c" (nr):"cx","di")

#define port_write(port,buf,nr) \
__asm__("cld;rep;outsw"::"d" (port),"S" (buf),"c" (nr):"cx","si")

extern void hd_interrupt(void);
extern void rd_load(void);

/* This may be used only once, enforced by 'static int callable' */
//下面该函数只在初始化时被调用一次。用静态变量callablle作为可调用标志
//系统设置函数
//函数参数BIOS指向内存0x90080处，此处存放着setup.s程序从BIOS中取得的2个硬盘的参数表(共32字节)
//本函数主要功能是读取CMOS和硬盘参数表信息，用于设置硬盘分区结构hd,并尝试架子啊RAM虚拟盘和根文件系统
int sys_setup(void * BIOS)
{
	static int callable = 1;				//限制本函数只能被调用一次的标志
	int i,drive;
	unsigned char cmos_disks;
	struct partition *p;
	struct buffer_head * bh;

	//设置callable标志，使得本函数只能被调用一次
	if (!callable)
		return -1;
	callable = 0;
#ifndef HD_TYPE						//如果没有定义HD_TYPE,则读取(Linux0.11内核无定义)
	for (drive=0 ; drive<2 ; drive++) {
		hd_info[drive].cyl = *(unsigned short *) BIOS;				//柱面数
		hd_info[drive].head = *(unsigned char *) (2+BIOS);		//磁头数
		hd_info[drive].wpcom = *(unsigned short *) (5+BIOS);		//写前预补偿柱面号
		hd_info[drive].ctl = *(unsigned char *) (8+BIOS);			//控制字节
		hd_info[drive].lzone = *(unsigned short *) (12+BIOS);		//磁头着陆区柱面号
		hd_info[drive].sect = *(unsigned char *) (14+BIOS);		//每磁道扇区数
		BIOS += 16;		//指向下一个表
	}
	//setup.s程序在取BIOS硬盘参数信息时，如果系统只有1个硬盘，就会将对应第2个硬盘的16字节全部清零
	//因此这里只要判断第2个硬盘柱面数是否为0就可以知道是否有第2个硬盘了
	if (hd_info[1].cyl)
		NR_HD=2;			//硬盘数置为2
	else
		NR_HD=1;
#endif
	//设置硬盘分区结构数组hd[]
	//一个物理盘最多可以分4个逻辑盘，0是物理盘，1-4是逻辑盘
	//该数组的项0和项5分别表示两个硬盘的整体参数，而项1-4和6-9分别表示两个硬盘的4个分区的参数
	//这里仅设置表示硬盘整体信息的两项(项0和项5)
	for (i=0 ; i<NR_HD ; i++) {
		hd[i*5].start_sect = 0;						//硬盘起始扇区号
		hd[i*5].nr_sects = hd_info[i].head*
				hd_info[i].sect*hd_info[i].cyl;		//硬盘总扇区数
	}

	/*
		We querry CMOS about hard disks : it could be that 
		we have a SCSI/ESDI/etc controller that is BIOS
		compatable with ST-506, and thus showing up in our
		BIOS table, but not register compatable, and therefore
		not present in CMOS.

		Furthurmore, we will assume that our ST-506 drives
		<if any> are the primary drives in the system, and 
		the ones reflected as drive 1 or 2.

		The first drive is stored in the high nibble of CMOS
		byte 0x12, the second in the low nibble.  This will be
		either a 4 bit drive type or 0xf indicating use byte 0x19 
		for an 8 bit type, drive 1, 0x1a for drive 2 in CMOS.

		Needless to say, a non-zero value means we have 
		an AT controller hard disk for that drive.

		
	*/
	//检测硬盘到底是不是AT控制器兼容的
	if ((cmos_disks = CMOS_READ(0x12)) & 0xf0)
		if (cmos_disks & 0x0f)
			NR_HD = 2;
		else
			NR_HD = 1;
	else
		NR_HD = 0;
	for (i = NR_HD ; i < 2 ; i++) {
		hd[i*5].start_sect = 0;
		hd[i*5].nr_sects = 0;
	}
	//读取每个硬盘上第1个扇区中的分区表信息，用来设置分区结构数组hd[]中硬盘各分区信息
	for (drive=0 ; drive<NR_HD ; drive++) {
		//硬盘的逻辑设备号=主设备号×256+次设备号 即dev_no=(major<<8)+minor
		//利用读块函数bread()读硬盘第1个数据块
		//第1个参数(0x300、0x305)分别是两个硬盘的设备号,第2个参数(0)是所需读取的块号
		//若读操作成功，则数据会被存放在缓冲块bh的数据区中，若缓冲块头指针bh为0说明操作失败，则显示出错信息并停机
		if (!(bh = bread(0x300 + drive*5,0))) {
			printk("Unable to read partition table of drive %d\n\r",
				drive);
			panic("");
		}
		//根据硬盘第1个扇区最后两个字节应该是0xAA55；来判断扇区中数据的有效性
		if (bh->b_data[510] != 0x55 || (unsigned char)
		    bh->b_data[511] != 0xAA) {
			printk("Bad partition table on drive %d\n\r",drive);
			panic("");
		}
		//指向硬盘分区表(位于第1扇区0x1BE处)
		p = 0x1BE + (void *)bh->b_data;
		for (i=1;i<5;i++,p++) {
			hd[i+5*drive].start_sect = p->start_sect;
			hd[i+5*drive].nr_sects = p->nr_sects;
		}
		//释放为存放硬盘块而申请的缓冲区
		brelse(bh);
	}
	if (NR_HD)
		printk("Partition table%s ok.\n\r",(NR_HD>1)?"s":"");
	//尝试并创建加载虚拟盘
	rd_load();
	//安装根文件系统
	mount_root();
	return (0);
}

static int controller_ready(void)
{
	int retries=10000;

	while (--retries && (inb_p(HD_STATUS)&0xc0)!=0x40);
	return (retries);
}

static int win_result(void)
{
	int i=inb_p(HD_STATUS);

	if ((i & (BUSY_STAT | READY_STAT | WRERR_STAT | SEEK_STAT | ERR_STAT))
		== (READY_STAT | SEEK_STAT))
		return(0); /* ok */
	if (i&1) i=inb(HD_ERROR);
	return (1);
}

//向硬盘控制器发送命令块
//参数：drive-硬盘号(0或1) nsect-读写扇区数 sect-起始扇区
//		head-磁头号	cyl-柱面号	cmd-命令码
//		intr_addr()-硬盘中断处理程序中将调用的C处理函数指针
static void hd_out(unsigned int drive,unsigned int nsect,unsigned int sect,
		unsigned int head,unsigned int cyl,unsigned int cmd,
		void (*intr_addr)(void))
{
	register int port asm("dx");			//定义局部寄存器变量并放在指定寄存器dx中

	//对参数进行有效性检查
	if (drive>1 || head>15)
		panic("Trying to write bad sector");
	if (!controller_ready())
		panic("HD controller not ready");
	do_hd = intr_addr;							//do_hd函数会在中断程序中被调用
	outb_p(hd_info[drive].ctl,HD_CMD);			//项控制寄存器输出控制字节
	port=HD_DATA;									//置dx为数据寄存器端口(0x1f0)
	outb_p(hd_info[drive].wpcom>>2,++port);		//参数：写预补偿柱面号
	outb_p(nsect,++port);							//参数：读/写扇区总数
	outb_p(sect,++port);							//参数：起始扇区
	outb_p(cyl,++port);								//参数：柱面号低8位
	outb_p(cyl>>8,++port);							//参数：柱面号高8位
	outb_p(0xA0|(drive<<4)|head,++port);			//参数：驱动器号+磁头号
	outb(cmd,++port);								//命令：硬盘控制命令
}

static int drive_busy(void)
{
	unsigned int i;

	for (i = 0; i < 10000; i++)
		if (READY_STAT == (inb_p(HD_STATUS) & (BUSY_STAT|READY_STAT)))
			break;
	i = inb(HD_STATUS);
	i &= BUSY_STAT | READY_STAT | SEEK_STAT;
	if (i == READY_STAT | SEEK_STAT)
		return(0);
	printk("HD controller times out\n\r");
	return(1);
}

static void reset_controller(void)
{
	int	i;

	outb(4,HD_CMD);
	for(i = 0; i < 100; i++) nop();
	outb(hd_info[0].ctl & 0x0f ,HD_CMD);
	if (drive_busy())
		printk("HD-controller still busy\n\r");
	if ((i = inb(HD_ERROR)) != 1)
		printk("HD-controller reset failed: %02x\n\r",i);
}

static void reset_hd(int nr)
{
	reset_controller();
	hd_out(nr,hd_info[nr].sect,hd_info[nr].sect,hd_info[nr].head-1,
		hd_info[nr].cyl,WIN_SPECIFY,&recal_intr);
}

void unexpected_hd_interrupt(void)
{
	printk("Unexpected HD interrupt\n\r");
}

static void bad_rw_intr(void)
{
	if (++CURRENT->errors >= MAX_ERRORS)
		end_request(0);
	if (CURRENT->errors > MAX_ERRORS/2)
		reset = 1;
}

//读操作中断调用函数
//该函数将在硬盘读命令结束时引发的硬盘中断过程中被调用
//在读命令执行后会产生硬盘中断信号，并执行硬盘中断处理程序，此时在硬盘中断处理程序
//中调用的C函数指针do_hd已经指向read_intr()，因此会在一次读扇区操作完成后执行该函数
static void read_intr(void)
{
	//首先判断此次读命令操作是否出错
	if (win_result()) {					//若控制器忙、读写错或者命令执行错
		bad_rw_intr();					//进行读写硬盘失败处理
		do_hd_request();			//再次请求硬盘作相应处理
		return;
	}
	//从数据寄存器端口把1个扇区的数据读到请求项的缓冲区中，并且递减请求项所需读取的扇区数值
	port_read(HD_DATA,CURRENT->buffer,256);			//HD_DATA=0x1f0   256字=512字节
	CURRENT->errors = 0;
	CURRENT->buffer += 512;
	CURRENT->sector++;
	//若递减后不等于0，表示本项请求还有数据没读取完
	//于是再次置中断调用C函数指针do_hd为read_intr()并直接返回，
	//等待硬盘在独处另一个扇区数据后发出中断并再次调用本函数
	if (--CURRENT->nr_sectors) {
		do_hd = &read_intr;
		return;
	}
	//执行到此，说明本次请求项的全部扇区数据已经读完，则调用end_request()函数取处理请求项结束事宜
	end_request(1);
	//最后再次调用do_hd_request()，去处理其他硬盘请求项，执行其他硬盘请求操作
	do_hd_request();
}

static void write_intr(void)
{
	if (win_result()) {
		bad_rw_intr();
		do_hd_request();
		return;
	}
	if (--CURRENT->nr_sectors) {
		CURRENT->sector++;
		CURRENT->buffer += 512;
		do_hd = &write_intr;
		port_write(HD_DATA,CURRENT->buffer,256);
		return;
	}
	end_request(1);
	do_hd_request();
}

static void recal_intr(void)
{
	if (win_result())
		bad_rw_intr();
	do_hd_request();
}

//执行硬盘读写请求操作
//该函数根据当前请求项中的设备号和起始扇区号信息首先计算得到对应硬盘上的柱面号、
//当前磁道中扇区号、磁头号数据，然后再根据请求项中的命令(READ/WRITE)对硬盘发送相应读/写命令
void do_hd_request(void)
{
	int i,r;
	unsigned int block,dev;
	unsigned int sec,head,cyl;
	unsigned int nsect;

	//首先检测请求项的合法性
	INIT_REQUEST;
	//取设备号的子设备号以及设备当前请求项中的起始扇区号
	dev = MINOR(CURRENT->dev);
	block = CURRENT->sector;
	//如果子设备号不存在或者起始扇区大于该分区扇区数-2，则结束该请求项，并跳转到标号repeat处
	if (dev >= 5*NR_HD || block+2 > hd[dev].nr_sects) {
		end_request(0);
		goto repeat;
	}
	block += hd[dev].start_sect;		//block为绝对扇区号
	dev /= 5;						//此时dev代表硬盘号(硬盘0还是硬盘1)
	//计算出对应硬盘中所在的柱面号(cyl)、磁道中扇区号(sec)、磁头号(head)
	__asm__("divl %4":"=a" (block),"=d" (sec):"0" (block),"1" (0),
		"r" (hd_info[dev].sect));
	__asm__("divl %4":"=a" (cyl),"=d" (head):"0" (block),"1" (0),
		"r" (hd_info[dev].head));
	sec++;										//读计算所得当前磁道扇区号进行调整
	nsect = CURRENT->nr_sectors;			//欲读/写的扇区数
	//如果此时复位标志reset是置位的，则需要执行复位操作
	//复位硬盘和控制器，并置需要重新校正标志，返回
	if (reset) {
		reset = 0;
		recalibrate = 1;
		reset_hd(CURRENT_DEV);
		return;
	}
	//如果此时重新校正标志recalibrate是置位的，则首先复位标志，然后向硬盘控制器发送重新校正命令
	//该命令会执行寻道操作，让处于任何地方的磁头移动到0柱面
	if (recalibrate) {
		recalibrate = 0;
		hd_out(dev,hd_info[CURRENT_DEV].sect,0,0,0,
			WIN_RESTORE,&recal_intr);
		return;
	}	
	if (CURRENT->cmd == WRITE) {
		hd_out(dev,nsect,sec,head,cyl,WIN_WRITE,&write_intr);
		for(i=0 ; i<3000 && !(r=inb_p(HD_STATUS)&DRQ_STAT) ; i++)
			/* nothing */ ;
		if (!r) {
			bad_rw_intr();
			goto repeat;
		}
		port_write(HD_DATA,CURRENT->buffer,256);
		//如果当前请求是读硬盘数据，则向硬盘控制器发送读扇区命令
	} else if (CURRENT->cmd == READ) {
		hd_out(dev,nsect,sec,head,cyl,WIN_READ,&read_intr);
	} else			//否则命令无效停机
		panic("unknown hd-command");
}

//硬盘系统初始化
void hd_init(void)
{
	//将硬盘请求项服务程序do_hd_request()与blk_dev控制结构相挂接
	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;		//do_hd_request()
	//将硬盘中断服务程序hd_interrupt()与IDT相挂接
	set_intr_gate(0x2E,&hd_interrupt);
	//复位主8259A int2的屏蔽位，允许从片发出中断请求信号	
	outb_p(inb_p(0x21)&0xfb,0x21);
	//复位硬盘的中断请求屏蔽位(在从片上)，允许硬盘控制器发送中断请求信号
	outb(inb_p(0xA1)&0xbf,0xA1);
}
