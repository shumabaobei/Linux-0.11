/*
 *  linux/init/main.c
 *
 *  (C) 1991  Linus Torvalds
 */

#define __LIBRARY__
#include <unistd.h>
#include <time.h>

/*
 * we need this inline - forking from kernel space will result
 * in NO COPY ON WRITE (!!!), until an execve is executed. This
 * is no problem, but for the stack. This is handled by not letting
 * main() use the stack at all after fork(). Thus, no function
 * calls - which means inline code for fork too, as otherwise we
 * would use the stack upon exit from 'fork()'.
 *
 * Actually only pause and fork are needed inline, so that there
 * won't be any messing with the stack from main(), but we define
 * some others too.
 */

//_syscall0()是unistd.h中的内嵌宏代码
/*展开为 static inline int fork(void){
	long __res;
	__asm__ volatile ("int $0x80" : "=a" (__res) : "0" (__NR_##name)); 
	if (__res >= 0) 
	return (type) __res; 
	errno = -__res; 
	return -1; 	
}
*/
static inline _syscall0(int,fork)
static inline _syscall0(int,pause)
static inline _syscall1(int,setup,void *,BIOS)
static inline _syscall0(int,sync)

#include <linux/tty.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <asm/system.h>
#include <asm/io.h>

#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include <linux/fs.h>

static char printbuf[1024];

extern int vsprintf();
extern void init(void);
extern void blk_dev_init(void);
extern void chr_dev_init(void);
extern void hd_init(void);
extern void floppy_init(void);
extern void mem_init(long start, long end);
extern long rd_init(long mem_start, int length);
extern long kernel_mktime(struct tm * tm);
extern long startup_time;

/*
 * This is set up by the setup-routine at boot-time
 */
#define EXT_MEM_K (*(unsigned short *)0x90002)		//1MB以后的扩展内存大小(KB)
#define DRIVE_INFO (*(struct drive_info *)0x90080)	//硬盘参数表的32字节内容
#define ORIG_ROOT_DEV (*(unsigned short *)0x901FC)	//根文件系统所在的设备号

/*
 * Yeah, yeah, it's ugly, but I cannot find how to do this correctly
 * and this seems to work. I anybody has more info on the real-time
 * clock I'd be interested. Most of this was trial and error, and some
 * bios-listing reading. Urghh.
 */
//宏定义读取CMOS实时钟信息
#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \		//0x70是写端口，0x80|addr是要读取的CMOS内存地址
inb_p(0x71); \					//0x71是读端口号
})

//将BCD码转换成二进制数值
#define BCD_TO_BIN(val) ((val)=((val)&15) + ((val)>>4)*10)

static void time_init(void)
{
	struct tm time;

	do {
		time.tm_sec = CMOS_READ(0);			//当前时间秒数(BCD码)
		time.tm_min = CMOS_READ(2);
		time.tm_hour = CMOS_READ(4);
		time.tm_mday = CMOS_READ(7);
		time.tm_mon = CMOS_READ(8);
		time.tm_year = CMOS_READ(9);		//当前年份
	} while (time.tm_sec != CMOS_READ(0));
	BCD_TO_BIN(time.tm_sec);		//转换成二进制数值
	BCD_TO_BIN(time.tm_min);
	BCD_TO_BIN(time.tm_hour);
	BCD_TO_BIN(time.tm_mday);
	BCD_TO_BIN(time.tm_mon);
	BCD_TO_BIN(time.tm_year);
	time.tm_mon--;
	startup_time = kernel_mktime(&time);	//计算开机时间
}

static long memory_end = 0;			//机器具有的物理内存容量(字节数)
static long buffer_memory_end = 0;	//高速缓冲区末端地址
static long main_memory_start = 0;	//主内存(将用于分页)开始的位置

struct drive_info { char dummy[32]; } drive_info;	//用于存放硬盘参数表信息

void main(void)		/* This really IS void, no error here. */
{			/* The startup routine assumes (well, ...) this */
//此时中断仍被禁止着，做完必要的设置后就将其开启
/*
 * Interrupts are still disabled. Do necessary setups, then
 * enable them
 */
	//根设备号->ROOT_DEV			高速缓冲末端地址->buffer_memory_end(4MB)
	//机器内存数->memory_end(16MB) 	主内存开始地址->main_memory_start(4MB)	
 	ROOT_DEV = ORIG_ROOT_DEV;
 	drive_info = DRIVE_INFO;
	memory_end = (1<<20) + (EXT_MEM_K<<10);
	memory_end &= 0xfffff000;
	if (memory_end > 16*1024*1024)
		memory_end = 16*1024*1024;
	if (memory_end > 12*1024*1024) 
		buffer_memory_end = 4*1024*1024;
	else if (memory_end > 6*1024*1024)
		buffer_memory_end = 2*1024*1024;
	else
		buffer_memory_end = 1*1024*1024;
	main_memory_start = buffer_memory_end;
#ifdef RAMDISK
	main_memory_start += rd_init(main_memory_start, RAMDISK*1024);
#endif
	//以下是内核进行所有方面的初始化工作
	//主内存区初始化
	mem_init(main_memory_start,memory_end);
	//中断向量初始化
	trap_init();
	//块设备初始化
	blk_dev_init();
	//字符设备初始化(为空函数)
	chr_dev_init();
	//tty初始化(实际进行初始化字符设备的功能)
	tty_init();
	//设置开机启动时间->startup_time
	time_init();
	//调度程序初始化(加载任务0的tr,ldtr)
	sched_init();
	//缓冲管理初始化，建内存链表
	buffer_init(buffer_memory_end);
	//初始化硬盘
	hd_init();
	//初始化软盘
	floppy_init();
	//开启中断
	sti();
	//通过在堆栈中设置的参数，利用中断返回指令启动任务０运行
	move_to_user_mode();	//移到用户模式下执行
	if (!fork()) {		/* we count on this going ok */
		init();			//在新建的子进程(任务１即init进程)执行
	}
/*
 *   NOTE!!   For any other task 'pause()' would mean we have to get a
 * signal to awaken, but task0 is the sole exception (see 'schedule()')
 * as task 0 gets activated at every idle moment (when no other tasks
 * can run). For task0 'pause()' just means we go check if some other
 * task can run, and if not we return here.
 */
	for(;;) pause();
}

static int printf(const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	write(1,printbuf,i=vsprintf(printbuf, fmt, args));
	va_end(args);
	return i;
}

//读取并执行/etc/rc文件时所使用的命令行参数和环境参数
static char * argv_rc[] = { "/bin/sh", NULL };
static char * envp_rc[] = { "HOME=/", NULL };

static char * argv[] = { "-/bin/sh",NULL };
static char * envp[] = { "HOME=/usr/root", NULL };

//init()函数运行在任务0第一次创建的子进程(任务1)中
//首先对第一个将要执行的程序(shell)的环境进行初始化
//然后以登录shell方式加载该程序并执行之
void init(void)
{
	int pid,i;

	//setup()是一个系统调用
	//用于读取硬盘参数，并加载虚拟盘(若存在的话)，安装根文件系统设备
	setup((void *) &drive_info);
	//下面以读写访问方式打开设备"/dev/tty0"，它对应终端控制台
	//由于这是第一次打开文件操作，因此产生的文件句柄号(文件描述符)肯定是0
	//该句柄是Unix类操作系统默认的控制台标准 输入句柄stdin
	//这里把它以读和写的方式打开是为了复制产生标准输出(写)句柄stdout和标准出错输出句柄stderr
	(void) open("/dev/tty0",O_RDWR,0);			//创建标准输入设备，其中/dev/tty0是该文件的路径名
	//复制句柄，产生句柄1号---stdout标准输出设备
	(void) dup(0);					//创建标准输出设备
	//复制句柄，产生句柄2号---stderr标准出错输出设备
	(void) dup(0);					//创建标准错误输出设备
	printf("%d buffers = %d bytes buffer space\n\r",NR_BUFFERS,
		NR_BUFFERS*BLOCK_SIZE);
	printf("Free mem: %d bytes\n\r",memory_end-main_memory_start);
	//进程1创建进程2
	if (!(pid=fork())) {	
		//该子进程关闭句柄0(stdin)
		close(0);
		//以只读方式打开/etc/rc文件
		if (open("/etc/rc",O_RDONLY,0))
			_exit(1);
		//使用execve()函数将进程自身替换成/bin/sh程序(即shell程序)，然后执行/bin/sh程序
		//argv_rc和envp_rc分别是参数及环境变量
		execve("/bin/sh",argv_rc,envp_rc);
		_exit(2);
	}
	//下面还是父进程(进程1)执行的语句
	if (pid>0)	
		//进程1等待子进程退出，最终会切换到进程2执行
		while (pid != wait(&i))		//此时执行结果为2!=2为假，跳出循环，去下面while(1)中执行
			/* nothing */;
	//如果执行到这里，说明刚创建的子进程的执行已停止或终止了
	while (1) {			//重启shell进程
		if ((pid=fork())<0) {		//进程1创建进程4，即重建shell进程
			printf("Fork failed in init\r\n");
			continue;
		}
		if (!pid) {
			//新的shell进程关闭所有打开的问价
			close(0);close(1);close(2);
			//创建新的会话
			setsid();
			//重新打开标准输入设备文件
			(void) open("/dev/tty0",O_RDWR,0);
			//重新打开标准输出设备文件
			(void) dup(0);
			//重新打开标准错误输出设备文件
			(void) dup(0);
			//加载shell进程
			_exit(execve("/bin/sh",argv,envp));
		}
		while (1)
			if (pid == wait(&i))
				break;
		printf("\n\rchild %d died with code %04x\n\r",pid,i);
		sync();
	}
	_exit(0);	/* NOTE! _exit, not exit() */
}
