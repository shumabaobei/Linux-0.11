/*
 *  linux/kernel/fork.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also system_call.s), and some misc functions ('verify_area').
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/mm.c': 'copy_page_tables()'
 */
#include <errno.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <asm/system.h>

extern void write_verify(unsigned long address);

//最新进程号，其值会有get_empty_process()生成
long last_pid=0;

//进程空间区域写前验证函数
void verify_area(void * addr,int size)
{
	unsigned long start;

	start = (unsigned long) addr;
	size += start & 0xfff;
	start &= 0xfffff000;
	start += get_base(current->ldt[2]);
	while (size>0) {
		size -= 4096;
		write_verify(start);
		start += 4096;
	}
}

//复制内存页表
//参数nr是新任务号，p是新任务数据结构指针
//该函数为新任务在线性地址空间中设置代码段和数据段基址、限长，并复制页表
int copy_mem(int nr,struct task_struct * p)
{
	unsigned long old_data_base,new_data_base,data_limit;
	unsigned long old_code_base,new_code_base,code_limit;

	//取出当前局部描述符表中代码段描述符和数据段描述符项中的段限长(字节数)
	code_limit=get_limit(0x0f);
	data_limit=get_limit(0x17);
	//取当前进程代码段和数据段在线性地址空间中的基地址
	old_code_base = get_base(current->ldt[1]);
	old_data_base = get_base(current->ldt[2]);
	if (old_data_base != old_code_base)
		panic("We don't support separate I&D");
	if (data_limit < code_limit)
		panic("Bad data_limit");
	//设置创建中的新进程在线性地址空间中的基地址等于(64M*其任务号)
	new_data_base = new_code_base = nr * 0x4000000;
	p->start_code = new_code_base;
	//并用该值设置新进程局部描述符表中段描述符中的基地址
	set_base(p->ldt[1],new_code_base);
	set_base(p->ldt[2],new_data_base);
	//复制当前进程的(父进程)的页目录表和页表项
	if (copy_page_tables(old_data_base,new_data_base,data_limit)) {
		//如果出错则释放刚申请的页表项
		free_page_tables(new_data_base,data_limit);
		return -ENOMEM;
	}
	return 0;
}

/*
 *  Ok, this is the main fork-routine. It copies the system process
 * information (task[nr]) and sets up the necessary registers. It
 * also copies the data segment in it's entirety.
 */
//copy_process()用于创建并复制进程的代码段和数据段以及环境
//其中参数nr是调用find_empty_process()分配的任务数组项号
int copy_process(int nr,long ebp,long edi,long esi,long gs,long none,
		long ebx,long ecx,long edx,
		long fs,long es,long ds,
		long eip,long cs,long eflags,long esp,long ss)
{
	struct task_struct *p;
	int i;
	struct file *f;

	//首先为新任务数据结构分配内存
	p = (struct task_struct *) get_free_page();
	if (!p)
		return -EAGAIN;
	//将新任务数据结构指针放入任务数组的nr中
	task[nr] = p;
	//把当前进程任务结构内容复制到刚申请到的内存页面p开始处
	*p = *current;	/* NOTE! this doesn't copy the supervisor stack */
	//对复制来的进程结构内容进行一些修改，作为新进程的任务结构

	//先将新进程的状态置为不可中断等待状态，以防止内核调度其执行
	p->state = TASK_UNINTERRUPTIBLE;
	//设置新进程的pid和父进程号father，初始化进程运行时间片值等于其priority(一般为15个滴答)
	//接着复位新进程的信号位图、报警定时值、会话领导标志leader、
	//进程及进程在内核和用户态运行时间统计值，还设置进程开始运行的系统时间start_time
	p->pid = last_pid;			//新进程号
	p->father = current->pid;
	p->counter = p->priority;
	p->signal = 0;
	p->alarm = 0;
	p->leader = 0;		/* process leadership doesn't inherit */
	p->utime = p->stime = 0;
	p->cutime = p->cstime = 0;
	p->start_time = jiffies;

	//修改任务状态段TSS数据
	p->tss.back_link = 0;
	//esp0(任务内核态栈指针)用作程序在内核态执行的栈
	//ss:esp0用作程序在内核态执行的栈
	p->tss.esp0 = PAGE_SIZE + (long) p;
	p->tss.ss0 = 0x10;
	p->tss.eip = eip;			//指令代码指针
	p->tss.eflags = eflags;		//标志寄存器
	//创建新进程时新进程返回值应为0，所以设置tss.eax=0
	p->tss.eax = 0;				
	p->tss.ecx = ecx;
	p->tss.edx = edx;
	p->tss.ebx = ebx;
	p->tss.esp = esp;
	p->tss.ebp = ebp;
	p->tss.esi = esi;
	p->tss.edi = edi;
	p->tss.es = es & 0xffff;
	p->tss.cs = cs & 0xffff;
	p->tss.ss = ss & 0xffff;
	p->tss.ds = ds & 0xffff;
	p->tss.fs = fs & 0xffff;
	p->tss.gs = gs & 0xffff;
	//把GDT中本任务LDT描述符的选择符保存在本任务的TSS段中
	//当CPU执行切换任务时，会自动从TSS中把LDT段描述符的选择符加载到ldtr寄存器中
	p->tss.ldt = _LDT(nr);
	p->tss.trace_bitmap = 0x80000000;
	//协处理器相关
	if (last_task_used_math == current)
		__asm__("clts ; fnsave %0"::"m" (p->tss.i387));
	//复制进程页表
	//在线性地址空间中设置新任务代码段和数据段描述符中的基址和限长，并复制页表
	if (copy_mem(nr,p)) {
		//如果出错(返回值不是0)，
		//则复位任务数组中相应项并释放为该新任务分配的用于任务结构的内存页
		task[nr] = NULL;
		free_page((long) p);
		return -EAGAIN;
	}
	//如果父进程中有文件是打开的，则将对应文件的打开次数增1
	for (i=0; i<NR_OPEN;i++)
		if (f=p->filp[i])
			f->f_count++;
	//将当前进程(父进程)的pwd、root和executable引用次数均增1
	if (current->pwd)
		current->pwd->i_count++;
	if (current->root)
		current->root->i_count++;
	if (current->executable)
		current->executable->i_count++;
	//在GDT表中设置新任务TSS段和LDT段描述符项
	set_tss_desc(gdt+(nr<<1)+FIRST_TSS_ENTRY,&(p->tss));
	set_ldt_desc(gdt+(nr<<1)+FIRST_LDT_ENTRY,&(p->ldt));
	//然后把新进程设置为就绪态
	p->state = TASK_RUNNING;	/* do this last, just in case */
	//最后返回新进程号
	return last_pid;
}

//为新进程取得不重复的进程号last_pid，函数返回在任务数组中的任务号
int find_empty_process(void)
{
	int i;

	repeat:
		//如果last_pid增1后超出进程号的正数表示范围，则重新从1开始使用pid号
		if ((++last_pid)<0) last_pid=1;
		//在任务数组中搜索刚设置的pid号是否被任何任务使用
		//如果是则跳转到函数开始处重新获得一个pid号
		for(i=0 ; i<NR_TASKS ; i++)
			if (task[i] && task[i]->pid == last_pid) goto repeat;
	//在任务数组中为新任务寻找一个空闲项，并返回项号
	for(i=1 ; i<NR_TASKS ; i++)
		if (!task[i])
			return i;
	return -EAGAIN;
}
