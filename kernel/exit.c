1`/*
 *  linux/kernel/exit.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/tty.h>
#include <asm/segment.h>

int sys_pause(void);
int sys_close(int fd);

//释放指定进程占用的任务槽及其任务数据结构占用的内存页面
void release(struct task_struct * p)
{
	int i;

	if (!p)
		return;
	//扫描任务指针数据表task[]以寻找指定的任务
	for (i=1 ; i<NR_TASKS ; i++)
		//如果找到则首先清空任务槽，然后释放该任务结构数据所占用的内存页面
		//最后执行调度函数并返回时立即退出
		if (task[i]==p) {
			task[i]=NULL;
			free_page((long)p);
			schedule();
			return;
		}
	panic("trying to release non-existent task");
}

static inline int send_sig(long sig,struct task_struct * p,int priv)
{
	if (!p || sig<1 || sig>32)
		return -EINVAL;
	if (priv || (current->euid==p->euid) || suser())
		p->signal |= (1<<(sig-1));
	else
		return -EPERM;
	return 0;
}

static void kill_session(void)
{
	struct task_struct **p = NR_TASKS + task;
	
	while (--p > &FIRST_TASK) {
		if (*p && (*p)->session == current->session)
			(*p)->signal |= 1<<(SIGHUP-1);
	}
}

/*
 * XXX need to check permissions needed to send signals to process
 * groups, etc. etc.  kill() permissions semantics are tricky!
 */
int sys_kill(int pid,int sig)
{
	struct task_struct **p = NR_TASKS + task;
	int err, retval = 0;

	if (!pid) while (--p > &FIRST_TASK) {
		if (*p && (*p)->pgrp == current->pid) 
			if (err=send_sig(sig,*p,1))
				retval = err;
	} else if (pid>0) while (--p > &FIRST_TASK) {
		if (*p && (*p)->pid == pid) 
			if (err=send_sig(sig,*p,0))
				retval = err;
	} else if (pid == -1) while (--p > &FIRST_TASK)
		if (err = send_sig(sig,*p,0))
			retval = err;
	else while (--p > &FIRST_TASK)
		if (*p && (*p)->pgrp == -pid)
			if (err = send_sig(sig,*p,0))
				retval = err;
	return retval;
}

//通知父进程---向进程pid发送信号SIGCHLD：默认情况下子进程将停止或终止
//如果没有找到父进程则自己释放
//但根据POSIX.1要求，若父进程已先行终止，则子进程应该别初始进程1收容s
static void tell_father(int pid)
{
	int i;

	if (pid)
		//扫描进程数组表寻找指定进程pid，并向其发送子进程将停止或终止信号SIGCHLD
		for (i=0;i<NR_TASKS;i++) {
			if (!task[i])
				continue;
			if (task[i]->pid != pid)
				continue;
			task[i]->signal |= (1<<(SIGCHLD-1));
			return;
		}
/* if we don't find any fathers, we just release ourselves */
/* This is not really OK. Must change it to make father 1 */
	//
	printk("BAD BAD - no father found\n\r");
	release(current);
}

//程序退出处理函数
//该函数将把当前进程置为TASK_ZOMBIA状态，然后取执行调度函数schedule()，不再返回
//参数code是错误码
int do_exit(long code)
{
	int i;

	//释放当前进程代码段和数据段所占的页表
	free_page_tables(get_base(current->ldt[1]),get_limit(0x0f));
	free_page_tables(get_base(current->ldt[2]),get_limit(0x17));
	//如果当前进程有子进程，就将子进程的father置为1(其父进程改为进程1，即init进程)
	for (i=0 ; i<NR_TASKS ; i++)
		if (task[i] && task[i]->father == current->pid) {
			task[i]->father = 1;
			//如果子进程已经处于僵死(ZOMBIE)状态，则向进程1发送子进程终止信号SIGCHLD
			if (task[i]->state == TASK_ZOMBIE)
				/* assumption task[1] is always init */
				(void) send_sig(SIGCHLD, task[1], 1);
		}
	//关闭当前进程打开着的所有文件
	for (i=0 ; i<NR_OPEN ; i++)
		if (current->filp[i])
			sys_close(i);
	//对当前进程的工作目录pwd、根目录root以及执行程序文件的i节点进行同步操作
	//放回各个i节点并分别置空(释放)
	iput(current->pwd);
	current->pwd=NULL;
	iput(current->root);
	current->root=NULL;
	iput(current->executable);
	current->executable=NULL;
	//如果当前进程是回话头领(leader)进程并且其有控制终端，则释放该终端
	if (current->leader && current->tty >= 0)
		tty_table[current->tty].pgrp = 0;
	//如果当前进程上次使用过协处理器，则将last_task_used_math置空
	if (last_task_used_math == current)
		last_task_used_math = NULL;
	//如果当前进程是leader进程，则终止该会话的所有相关进程
	if (current->leader)
		kill_session();
	//把当前进程置为僵死状态，表明当前进程已经释放了资源，并保存将由父进程读取的退出码
	current->state = TASK_ZOMBIE;
	current->exit_code = code;
	//通知父进程，即向父进程发送信号SIGCHLD---子进程将停止或终止
	tell_father(current->father);
	schedule();
	return (-1);	/* just to suppress warnings */
}

//系统调用exit()。终止进程
//参数error_code是用户程序提供的退出状态信息，只有低字节有效
//把error_code左移8比特是wait()或waitpid()函数的要求，低字节用来保存wait()的状态信息
int sys_exit(int error_code)
{
	return do_exit((error_code&0xff)<<8);
}

//wait()函数最终会映射到系统调用函数sys_waitpid()中执行
//挂起当前进程，直到PID指定的子进程退出或者收到要求终止该进程的信号或者是需要调用一个信号句柄(信号处理程序)
//如果pid所指的子进程早已退出(已成所谓的僵死进程)，则本调用将立刻返回，子进程使用的所有资源将释放
//如果pid>0，表示等待进程号等于pid的子进程
//如果pid=0，表示等待进程组号等于当前进程组号的任何子进程
//如果pid<-1，表示等待进程组号等于pid绝对值的任何子进程
//如果pid=-1，表示等待任何子进程
//若option=WUNTRACED=2，表示如果子进程是停止的，也马上返回(无需跟踪)
//若option=WHONANG=1，表示如果没有子进程退出或终止就马上返回
//如果返回状态指针stat_addr不为空，则就将状态信息保存到那里
//参数pid是进程号，*stat_addr是保存状态信息位置的指针，option是waitpid选项
int sys_waitpid(pid_t pid,unsigned long * stat_addr, int options)
{
	int flag, code;				//flag标志用于后面表示所选出的子进程处于就绪或睡眠态
	struct task_struct ** p;

	verify_area(stat_addr,4);
repeat:
	flag=0;
	//从任务数组末端开始扫描所有任务，跳过空项、本进程项以及非当前进程的子进程项
	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p) {
		if (!*p || *p == current)
			continue;
		if ((*p)->father != current->pid)
			continue;
		//此时扫描选择的进程p肯定是当前进程的子进程
		//如果等待的子进程号pid>0，但与被扫描子进程p的pid不相等，说明它是当前进程另外的子进程
		//于是跳过该进程，接着扫描下一个进程
		if (pid>0) {
			if ((*p)->pid != pid)
				continue;
		//否则如果指定等待进程的pid=0，表示正在等待进程组号等于当前进程组号的任何子进程
		//如果此时被扫描进程p的进程组号与当前进程的组号不等则跳过
		} else if (!pid) {
			if ((*p)->pgrp != current->pgrp)
				continue;
		//否则如果指定的pid<-1，表示正在等待进程组号等于pid绝对值的任何子进程
		//如果此时被扫描进程p的组号与pid的绝对值不等则跳过
		} else if (pid != -1) {
			if ((*p)->pgrp != -pid)
				continue;
		}
		//如果前3个对pid的判断都不符合，则表示当前进程正在等待其任何子进程，即pid=-1的情况
		switch ((*p)->state) {
			//子进程p处于停止状态
			case TASK_STOPPED:
				if (!(options & WUNTRACED))
					continue;
				put_fs_long(0x7f,stat_addr);
				return (*p)->pid;
			//子进程p处于僵死状态
			case TASK_ZOMBIE:
				//首先把它在用户态和内核态运行的时间分别累计到当前进程(父进程)中
				current->cutime += (*p)->utime;
				current->cstime += (*p)->stime;
				//然后取出子进程的pid和退出码
				flag = (*p)->pid;
				code = (*p)->exit_code;
				//释放该子进程
				release(*p);
				//置状态信息为退出码值
				put_fs_long(code,stat_addr);
				//返回子进程的pid
				return flag;
			//其他情况
			default:
				flag=1;
				continue;
		}
	}
	//说明有符合等待要求的子进程并没有处于退出或僵死状态
	if (flag) {
		if (options & WNOHANG)
			return 0;
		//置当前进程为可中断等待状态
		current->state=TASK_INTERRUPTIBLE;	
		//进程调度
		schedule();
		if (!(current->signal &= ~(1<<(SIGCHLD-1))))
			goto repeat;
		else
			return -EINTR;
	}
	return -ECHILD;
}


