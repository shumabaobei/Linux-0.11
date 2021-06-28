/*
 *  linux/kernel/sched.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'sched.c' is the main kernel file. It contains scheduling primitives
 * (sleep_on, wakeup, schedule etc) as well as a number of simple system
 * call functions (type getpid(), which just extracts a field from
 * current-task
 */
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/sys.h>
#include <linux/fdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#include <signal.h>

#define _S(nr) (1<<((nr)-1))
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))

void show_task(int nr,struct task_struct * p)
{
	int i,j = 4096-sizeof(struct task_struct);

	printk("%d: pid=%d, state=%d, ",nr,p->pid,p->state);
	i=0;
	while (i<j && !((char *)(p+1))[i])
		i++;
	printk("%d (of %d) chars free in kernel stack\n\r",i,j);
}

void show_stat(void)
{
	int i;

	for (i=0;i<NR_TASKS;i++)
		if (task[i])
			show_task(i,task[i]);
}

#define LATCH (1193180/HZ)

extern void mem_use(void);

extern int timer_interrupt(void);
extern int system_call(void);

//定义任务的内核态堆栈结构
union task_union {
	struct task_struct task;
	char stack[PAGE_SIZE];
};

//初始任务的数据
static union task_union init_task = {INIT_TASK,};

long volatile jiffies=0;
long startup_time=0;
//当前任务指针，初始化指向任务0
struct task_struct *current = &(init_task.task);
struct task_struct *last_task_used_math = NULL;

//定义任务指针数组。第一项被初始化指向初始任务(任务0)的任务数据结构
struct task_struct * task[NR_TASKS] = {&(init_task.task), };

//定义用户堆栈，共1K项，容量4K字节 
//在运行任务0之前它是内核代码段，以后用作任务0和任务1的用户态栈
long user_stack [ PAGE_SIZE>>2 ] ;

//用于设置堆栈ss:esp
//ss被设置为内核数据段选择符(0x10)，esp指在user_stack数组最后一项后面
struct {
	long * a;
	short b;
	} stack_start = { & user_stack [PAGE_SIZE>>2] , 0x10 };
/*
 *  'math_state_restore()' saves the current math information in the
 * old math state array, and gets the new ones from the current task
 */
void math_state_restore()
{
	if (last_task_used_math == current)
		return;
	__asm__("fwait");
	if (last_task_used_math) {
		__asm__("fnsave %0"::"m" (last_task_used_math->tss.i387));
	}
	last_task_used_math=current;
	if (current->used_math) {
		__asm__("frstor %0"::"m" (current->tss.i387));
	} else {
		__asm__("fninit"::);
		current->used_math=1;
	}
}

/*
 *  'schedule()' is the scheduler function. This is GOOD CODE! There
 * probably won't be any reason to change this, as it should work well
 * in all circumstances (ie gives IO-bound processes good response etc).
 * The one thing you might take a look at is the signal-handler code here.
 *
 *   NOTE!!  Task 0 is the 'idle' task, which gets called when no other
 * tasks can run. It can not be killed, and it cannot sleep. The 'state'
 * information in task[0] is never used.
 */
//调度程序
void schedule(void)
{
	int i,next,c;
	struct task_struct ** p;

/* check alarm, wake up any interruptible tasks that have got a signal */
	//检测alarm(进程的报警定时值),唤醒任何也得到信号的可中断任务
	//从任务数组中最后一个任务开始循环检测alarm。在循环时跳过空指针项
	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
		if (*p) {
			//如果设置过任务的定时值alarm并且已经过期(alarm<jiffies)
			//则在信号位图中置SIGALRM信号，即向任务发送SIGALARM信号，然后清alarm
			//该信号的默认操作是终止进程。jiffies是系统从开机开始算起的滴答数(10ms/滴答)
			if ((*p)->alarm && (*p)->alarm < jiffies) {
					(*p)->signal |= (1<<(SIGALRM-1));
					(*p)->alarm = 0;
				}
			//如果信号位图中除被阻塞的信号外还有其他信号，并且任务处于可中断状态，则置任务为就绪状态
			//其中'~(_BLOCKABLE & (*p)->blocked)'用于忽略被阻塞的信号，但SIGKILL和SIGSTOP不能被阻塞
			if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&
			(*p)->state==TASK_INTERRUPTIBLE)
				(*p)->state=TASK_RUNNING;
		}

/* this is the scheduler proper: */
//这是调度程序的主要部分
	while (1) {
		c = -1;
		next = 0;
		i = NR_TASKS;
		p = &task[NR_TASKS];
		//从任务数组最后一个任务开始循环处理，并跳过不含任务的数组槽
		//比较进程的状态和时间片，找出处在就绪态且counter最大的进程
		while (--i) {
			if (!*--p)
				continue;
			if ((*p)->state == TASK_RUNNING && (*p)->counter > c)
				c = (*p)->counter, next = i;
		}
		//如果比较得出有counter值不等于0的结果，或者系统没有一个可运行的任务存在(c仍然为-1,next=0)
		//则退出循环，执行任务切换操作
		if (c) break;
		//如果有任务可运行，但时间片用完，则根据每个任务的优先权值，更新每个任务的counter，然后重新比较
		for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
			if (*p)
				(*p)->counter = ((*p)->counter >> 1) +
						(*p)->priority;
	}
	//用下面的宏把当前任务指针current指向任务号为next的任务，并切换到该任务运行
	//前面next被初始化为0，因此若系统中没有任何其他任务可运行，则调度函数会在系统空闲时取执行任务0
	switch_to(next);
}

int sys_pause(void)
{
	//将当前进程设置为可中断等待状态，如果产生某种中断或其他进程给这个进程发送特定信号
	//才能将该进程的状态改为就绪态
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return 0;
}

//将当前任务置为不可中断的等待状态，并让睡眠队列头指针指向当前任务
void sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp = *p;
	*p = current;
	current->state = TASK_UNINTERRUPTIBLE;
	schedule();
	if (tmp)
		tmp->state=0;
}

void interruptible_sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp=*p;
	*p=current;
repeat:	current->state = TASK_INTERRUPTIBLE;
	schedule();
	if (*p && *p != current) {
		(**p).state=0;
		goto repeat;
	}
	*p=NULL;
	if (tmp)
		tmp->state=0;
}

//唤醒*p指向的任务，*p是任务等待队列头指针
void wake_up(struct task_struct **p)
{
	if (p && *p) {
		(**p).state=0;			//置为就绪(可运行)状态
		*p=NULL;
	}
}

/*
 * OK, here are some floppy things that shouldn't be in the kernel
 * proper. They are here because the floppy needs a timer, and this
 * was the easiest way of doing it.
 */
static struct task_struct * wait_motor[4] = {NULL,NULL,NULL,NULL};
static int  mon_timer[4]={0,0,0,0};
static int moff_timer[4]={0,0,0,0};
unsigned char current_DOR = 0x0C;

int ticks_to_floppy_on(unsigned int nr)
{
	extern unsigned char selected;
	unsigned char mask = 0x10 << nr;

	if (nr>3)
		panic("floppy_on: nr>3");
	moff_timer[nr]=10000;		/* 100 s = very big :-) */
	cli();				/* use floppy_off to turn it off */
	mask |= current_DOR;
	if (!selected) {
		mask &= 0xFC;
		mask |= nr;
	}
	if (mask != current_DOR) {
		outb(mask,FD_DOR);
		if ((mask ^ current_DOR) & 0xf0)
			mon_timer[nr] = HZ/2;
		else if (mon_timer[nr] < 2)
			mon_timer[nr] = 2;
		current_DOR = mask;
	}
	sti();
	return mon_timer[nr];
}

void floppy_on(unsigned int nr)
{
	cli();
	while (ticks_to_floppy_on(nr))
		sleep_on(nr+wait_motor);
	sti();
}

void floppy_off(unsigned int nr)
{
	moff_timer[nr]=3*HZ;
}

void do_floppy_timer(void)
{
	int i;
	unsigned char mask = 0x10;

	for (i=0 ; i<4 ; i++,mask <<= 1) {
		if (!(mask & current_DOR))
			continue;
		if (mon_timer[i]) {
			if (!--mon_timer[i])
				wake_up(i+wait_motor);
		} else if (!moff_timer[i]) {
			current_DOR &= ~mask;
			outb(current_DOR,FD_DOR);
		} else
			moff_timer[i]--;
	}
}

#define TIME_REQUESTS 64

static struct timer_list {
	long jiffies;
	void (*fn)();
	struct timer_list * next;
} timer_list[TIME_REQUESTS], * next_timer = NULL;

void add_timer(long jiffies, void (*fn)(void))
{
	struct timer_list * p;

	if (!fn)
		return;
	cli();
	if (jiffies <= 0)
		(fn)();
	else {
		for (p = timer_list ; p < timer_list + TIME_REQUESTS ; p++)
			if (!p->fn)
				break;
		if (p >= timer_list + TIME_REQUESTS)
			panic("No more time requests free");
		p->fn = fn;
		p->jiffies = jiffies;
		p->next = next_timer;
		next_timer = p;
		while (p->next && p->next->jiffies < p->jiffies) {
			p->jiffies -= p->next->jiffies;
			fn = p->fn;
			p->fn = p->next->fn;
			p->next->fn = fn;
			jiffies = p->jiffies;
			p->jiffies = p->next->jiffies;
			p->next->jiffies = jiffies;
			p = p->next;
		}
	}
	sti();
}

//时钟中断C语言处理函数
//参数cpl是当前特权级0或3，是时钟中断发生时正被执行的代码选择符中的特权级
//cpl=0时表示中断发生时正在执行内核代码；cpl=3时表示中断发生时正在执行用户代码
void do_timer(long cpl)
{
	extern int beepcount;					//扬声器发声时间滴答数
	extern void sysbeepstop(void);			//关闭扬声器

	if (beepcount)
		if (!--beepcount)
			sysbeepstop();

	//如果当前特权级(cpl)为0，则将内核代码运行时间stime递增
	//如果cpl>0，则表示是一般用户程序在工作
	if (cpl)
		current->utime++;
	else
		current->stime++;

	if (next_timer) {
		next_timer->jiffies--;
		while (next_timer && next_timer->jiffies <= 0) {
			void (*fn)(void);
			
			fn = next_timer->fn;
			next_timer->fn = NULL;
			next_timer = next_timer->next;
			(fn)();
		}
	}
	if (current_DOR & 0xf0)
		do_floppy_timer();
	//如果进程运行时间还没完，则退出
	if ((--current->counter)>0) return;
	//否则置当前任务运行计数值为0
	current->counter=0;
	//若发生时钟中断时正在内核代码中运行则返回，否则调用执行调度函数
	if (!cpl) return;		//对于内核态程序不依赖counter值进行调度
	schedule();
}

int sys_alarm(long seconds)
{
	int old = current->alarm;

	if (old)
		old = (old - jiffies) / HZ;
	current->alarm = (seconds>0)?(jiffies+HZ*seconds):0;
	return (old);
}

int sys_getpid(void)
{
	return current->pid;
}

int sys_getppid(void)
{
	return current->father;
}

int sys_getuid(void)
{
	return current->uid;
}

int sys_geteuid(void)
{
	return current->euid;
}

int sys_getgid(void)
{
	return current->gid;
}

int sys_getegid(void)
{
	return current->egid;
}

int sys_nice(long increment)
{
	if (current->priority-increment>0)
		current->priority -= increment;
	return 0;
}

//内核调度程序的初始化子程序
void sched_init(void)
{
	int i;
	//描述符表结构指针
	struct desc_struct * p;	

	if (sizeof(struct sigaction) != 16)
		panic("Struct sigaction MUST be 16 bytes");
	//在全局描述符表中设置初始任务(任务０)的任务状态段描述符和局部数据表描述符
	//FIRST_TSS_ENTRY=4 FIRST_LDT_ENTRY=5
	//gdt是一个全局描述符表数组基址，gdt[FIRST_TSS_ENTRY](gdt[4])即gdt数组第4项地址
	set_tss_desc(gdt+FIRST_TSS_ENTRY,&(init_task.task.tss));
	set_ldt_desc(gdt+FIRST_LDT_ENTRY,&(init_task.task.ldt));
	//从gdt[6]开始清空描述符表项
	//从任务1开始清空任务数组
	p = gdt+2+FIRST_TSS_ENTRY;
	for(i=1;i<NR_TASKS;i++) {
		task[i] = NULL;
		p->a=p->b=0;
		p++;
		p->a=p->b=0;
		p++;
	}
/* Clear NT, so that we won't have troubles with that later on */
	__asm__("pushfl ; andl $0xffffbfff,(%esp) ; popfl");
	//将任务０的TSS选择符加载到任务寄存器tr
	ltr(0);
	//将任务０的局部描述符表加载到局部描述符表寄存器ldtr中
	//只明确加载这一次，以后新任务LDT的加载是CPU根据TSS中的LDT项自己加载的
	lldt(0);
	//初始化8253定时器
	//通道0，选择工作方式3，二进制计数方式
	//通道0的输出引脚接在中断控制主芯片的IRQ0上，它每10毫秒发出一个IRQ0请求
	outb_p(0x36,0x43);		/* binary, mode 3, LSB/MSB, ch 0 */
	outb_p(LATCH & 0xff , 0x40);	/* LSB */
	outb(LATCH >> 8 , 0x40);	/* MSB */
	//设置时钟中断门，修改中断控制器屏蔽码，允许时钟中断
	set_intr_gate(0x20,&timer_interrupt);
	outb(inb_p(0x21)&~0x01,0x21);		//允许时钟中断
	//设置系统调用中断门
	set_system_gate(0x80,&system_call);
}
