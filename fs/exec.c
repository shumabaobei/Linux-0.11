	/*
 *  linux/fs/exec.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * #!-checking implemented by tytso.
 */

/*
 * Demand-loading implemented 01.12.91 - no need to read anything but
 * the header into memory. The inode of the executable is put into
 * "current->executable", and page faults do the actual loading. Clean.
 *
 * Once more I can proudly say that linux stood up to being changed: it
 * was less than 2 hours work to get demand-loading completely implemented.
 */

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <a.out.h>

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/segment.h>

extern int sys_exit(int exit_code);
extern int sys_close(int fd);

/*
 * MAX_ARG_PAGES defines the number of pages allocated for arguments
 * and envelope for the new program. 32 should suffice, this gives
 * a maximum env+arg of 128kB !
 */
#define MAX_ARG_PAGES 32

/*
 * create_tables() parses the env- and arg-strings in new user
 * memory and creates the pointer tables from them, and puts their
 * addresses on the "stack", returning the new stack pointer value.
 */
//在新任务栈中创建参数和环境变量指针表
//参数：p-数据段中参数和环境信息偏移指针 argc-参数个数 envc-环境变量个数
//返回：栈指针值
static unsigned long * create_tables(char * p,int argc,int envc)
{
	unsigned long *argv,*envp;
	unsigned long * sp;

	//栈指针是以4字节为边界进行寻址的，因此这里需让sp为4的整数倍值，此时sp位于参数环境表的末端
	sp = (unsigned long *) (0xfffffffc & (unsigned long) p);
	//然后先把sp向下(低地址方向)移动，在栈中空出环境变量指针占用的空间，
	//并让环境变量指针envp指向该处，多空出的一个位置用于在最后存放一个NULL值
	sp -= envc+1;
	envp = sp;
	//再把sp向下移动，空出命令行参数指针占用的空间，并让argv指针指向该处，痛痒多空出的一个位置存放NULL值
	sp -= argc+1;
	argv = sp;
	//此时sp指向参数指针块的起始处，我们将环境参数块指针envp和命令行参数块指针以及命令行参数个数分别压入栈中
	put_fs_long((unsigned long)envp,--sp);
	put_fs_long((unsigned long)argv,--sp);
	put_fs_long((unsigned long)argc,--sp);
	//再将命令行各参数指针和环境变量各指针分别放入前面空出来IDE相应地方，最后放置一个NULL指针
	while (argc-->0) {
		put_fs_long((unsigned long) p,argv++);
		while (get_fs_byte(p++)) /* nothing */ ;		//p指向下一个参数串
	}
	put_fs_long(0,argv);
	while (envc-->0) {
		put_fs_long((unsigned long) p,envp++);
		while (get_fs_byte(p++)) /* nothing */ ;		//p指向下一个参数串
	}
	put_fs_long(0,envp);
	//返回构造的当前新栈指针
	return sp;
}

/*
 * count() counts the number of arguments/envelopes
 */
static int count(char ** argv)
{
	int i=0;
	char ** tmp;

	if (tmp = argv)
		while (get_fs_long((unsigned long *) (tmp++)))
			i++;

	return i;
}

/*
 * 'copy_string()' copies argument/envelope strings from user
 * memory to free pages in kernel mem. These are in a format ready
 * to be put directly into the top of new user memory.
 *
 * Modified by TYT, 11/24/91 to add the from_kmem argument, which specifies
 * whether the string and the string array are from user or kernel segments:
 * 
 * from_kmem     argv *        argv **
 *    0          user space    user space
 *    1          kernel space  user space
 *    2          kernel space  kernel space
 * 
 * We do this by playing games with the fs segment register.  Since it
 * it is expensive to load a segment register, we try to avoid calling
 * set_fs() unless we absolutely have to.
 */
//复制指定个数的参数与字符串到参数和环境空间中
//返回：参数和环境空间当前头部指针，若出错则返回0
static unsigned long copy_strings(int argc,char ** argv,unsigned long *page,
		unsigned long p, int from_kmem)
{
	char *tmp, *pag;
	int len, offset = 0;
	unsigned long old_fs, new_fs;

	if (!p)
		return 0;	/* bullet-proofing */
	//首先取当前寄存器ds(指向内核数据段)和fs段，分别保存到变量new_fs和old_fs中
	new_fs = get_ds();
	old_fs = get_fs();
	//如果字符串和字符串数组(指针)来自内核空间，则设置fs段寄存器指向内核数据段
	if (from_kmem==2)
		set_fs(new_fs);
	//然后循环处理各个参数，从最后一个参数逆向开始复制，复制到指定偏移地址处
	while (argc-- > 0) {
		if (from_kmem == 1)
			set_fs(new_fs);
		//首先取得复制的当前字符串指针tmp
		if (!(tmp = (char *)get_fs_long(((unsigned long *)argv)+argc)))
			panic("argc is wrong");
		if (from_kmem == 1)
			set_fs(old_fs);
		//然后从用户空间取该字符串，并计算该参数字符串长度len，此后tmp指向该字符串末端
		len=0;		/* remember zero-padding */
		do {
			len++;
		} while (get_fs_byte(tmp++));
		//如果该字符串长度超过此时参数和环境空间中还 剩余的空间长度(通常不可能发生这情况)
		if (p-len < 0) {	/* this shouldn't happen - 128kB */
			set_fs(old_fs);
			return 0;
		}
		//接着我们逆向逐个字符地把字符串复制到参数和环境空间末端处
		while (len) {
			--p; --tmp; --len;
			if (--offset < 0) {
				offset = p % PAGE_SIZE;
				if (from_kmem==2)
					set_fs(old_fs);
				//如果当前偏移值p所在的串空间页面指针数组项page[p/PAGE_SIZE]==0
				//表示此时p指针所处的空间内存页面还不存在，则需申请一空闲内存页
				//并将该页面指针填入指针数组，同时也使页面指针pag指向该新页面
				if (!(pag = (char *) page[p/PAGE_SIZE]) &&
				    !(pag = (char *) page[p/PAGE_SIZE] =
				      (unsigned long *) get_free_page())) 
					return 0;
				if (from_kmem==2)
					set_fs(new_fs);

			}
			//然后从fs段中复制字符串的1字节到参数和环境空间内存页面pag的offset处
			*(pag + offset) = get_fs_byte(tmp);
		}
	}
	if (from_kmem==2)
		set_fs(old_fs);
	return p;
}

//修改任务局部描述表内容
//修改局部描述符表LDT中描述符的段基址和段限长，并将参数和环境空间页面放置在数据段末端
//参数：text_size-执行文件头部中a_text字段给出的代码段长度值
//返回：数据段限长值(64MB)
static unsigned long change_ldt(unsigned long text_size,unsigned long * page)
{
	unsigned long code_limit,data_limit,code_base,data_base;
	int i;

	//首先根据执行文件头部代码长度字段a_text值，计算以页面长度为边界的代码段限长
	code_limit = text_size+PAGE_SIZE -1;
	code_limit &= 0xFFFFF000;
	//设置数据段长度为64MB
	data_limit = 0x4000000;
	//然后取当前进程局部描述符表代码段描述符中代码段基址，代码段基址与数据段基址相同
	//并使用这些新值重新设置局部表中代码段和数据段描述符中的基址和段限长
	code_base = get_base(current->ldt[1]);
	data_base = code_base;
	set_base(current->ldt[1],code_base);
	set_limit(current->ldt[1],code_limit);
	set_base(current->ldt[2],data_base);
	set_limit(current->ldt[2],data_limit);
/* make sure fs points to the NEW data segment */
	//fs段寄存器中放入局部表数据段描述符的选择符(0x17)
	__asm__("pushl $0x17\n\tpop %%fs"::);
	//然后将参数和环境空间已存放数据的页面(最多MAX_ARG_PAGES页，128KB)放到 数据段末端
	//方法是从进程空间末端逆向一页一页地放
	//函数put_page()用于把物理页面映射到进程逻辑空间中
	data_base += data_limit;
	for (i=MAX_ARG_PAGES-1 ; i>=0 ; i--) {
		data_base -= PAGE_SIZE;
		if (page[i])
			put_page(page[i],data_base);
	}
	//最后返回数据段限长
	return data_limit;
}

/*
 * 'do_execve()' executes a new program.
 */
//execve()系统中断调用函数，加载并执行子程序(其他程序)
//函数的参数是进入系统调用处理过程后直到调用本系统调用处理过程和调用本函数之前逐步压入栈中的值
//(1) 入栈的edx、ecx和ebx寄存器值，分别对应**envp、**argv和*filename
//(2) 调用sys_call_table中sys_execve()函数(指针)时压入栈的函数返回地址(tmp)
//(3) 调用本函数do_execve()前入栈的指向栈中调用系统中断的程序代码指针eip
//参数：
//eip：调用系统中断的程序代码指针
//tmp：系统中断在调用_sys_execve时的返回地址，无用
//filename：被执行程序文件名指针
// argv：命令行参数指针数组的指针
// envp：环境变量指针数组的指针
//返回：如果调用成功，则不返回，否则设置出错号，并返回-1
int do_execve(unsigned long * eip,long tmp,char * filename,
	char ** argv, char ** envp)
{
	struct m_inode * inode;						//内存中i节点指针
	struct buffer_head * bh;					//高速缓存块头指针
	struct exec ex;									//执行文件头部数据结构变量
	unsigned long page[MAX_ARG_PAGES];		//参数和环境串空间页面指针数组
	int i,argc,envc;
	int e_uid, e_gid;							//有效用户ID和有效组ID
	int retval;									//返回值
	int sh_bang = 0;							//控制是否需要执行脚本程序
	unsigned long p=PAGE_SIZE*MAX_ARG_PAGES-4;		//p指向参数和环境空间的最后部

	//内核准备了128KB(32个页面)空间来存放执行文件的命令行参数和环境字符串
	//上行把p初始设置成位于128KB空间的最后1个长字处
	//在初始参数和环境空间的操作过程中，p将用来指明在128KB空间中的当前位置
	
	//参数eip[1]是调用本次系统调用的原用户程序代码段寄存器CS值，应当是0x000f
	if ((0xffff & eip[1]) != 0x000f)		//通过检测特权级来判断是否是内核调用了do_execve()函数
		panic("execve called from supervisor mode");
	//将参数和环境变量的页面指针管理表清零
	for (i=0 ; i<MAX_ARG_PAGES ; i++)	/* clear page-table */
		page[i]=0;		
	//获取shell程序所在文件的i节点			
	if (!(inode=namei(filename)))		/* get executables inode */
		return -ENOENT;
	argc = count(argv);			//命令行参数个数
	envc = count(envp);			//环境变量参数个数
	
restart_interp:
	if (!S_ISREG(inode->i_mode)) {	/* must be regular file */
		retval = -EACCES;
		goto exec_error2;			//若不是常规文件则置出错码跳转
	}
	//下面检查当前进程是否有权限运行指定的执行文件，
	//即根据执行文件i节点中的属性看看本进程是否有权执行它g
	i = inode->i_mode;
	e_uid = (i & S_ISUID) ? inode->i_uid : current->euid;
	e_gid = (i & S_ISGID) ? inode->i_gid : current->egid;
	//根据进程的euid和egid和执行文件的访问属性进行比较，调整i节点属性中的权限值s
	if (current->euid == inode->i_uid)
		i >>= 6;
	else if (current->egid == inode->i_gid)
		i >>= 3;
	//如果用户没有权限执行该程序则退出
	if (!(i & 1) &&
	    !((inode->i_mode & 0111) && suser())) {
		retval = -ENOEXEC;
		goto exec_error2;
	}
	//程序执行到这里，说明当前进程有运行指定执行文件的权限
	//首先读取执行文件第1块数据到高速缓冲块中，并复制缓冲块数据到ex中
	if (!(bh = bread(inode->i_dev,inode->i_zone[0]))) {
		retval = -EACCES;
		goto exec_error2;
	}
	ex = *((struct exec *) bh->b_data);	/* read exec-header */
	//如果执行文件开始的两个字节是字符'#!'，则说明执行文件是一个脚本文本文件
	//如果想运行脚本文件，我们就需要执行脚本文件的解释程序(如shell程序)
	//通常脚本文件的第一行文本为"#!/bin/bash"，它指明了运行脚本文件所需要的解释程序
	//运行方法是从脚本文件第一行(带字符'#!')中取出其中的解释程序名及后面的参数(若有的话)，
	//然后将这些参数和脚本文件名放进执行文件(此时是解释程序)的命令行参数空间中
	//在这之前我们当然需要先把函数指定的原有命令行参数和环境字符串当到128KB空间中，
	//而这里建立起来的命令行参数则放到它们前面位置处(因为是逆向放置)，最后让内核执行脚本文件的解释程序
	//下面就是在设置好解释程序的脚本文件名等参数后，取出解释沉痼的i节点并跳转到restart_interp处去执行解释程序
	if ((bh->b_data[0] == '#') && (bh->b_data[1] == '!') && (!sh_bang)) {
		/*
		 * This section does the #! interpretation.
		 * Sorta complicated, but hopefully it will work.  -TYT
		 */

		char buf[1023], *cp, *interp, *i_name, *i_arg;
		unsigned long old_fs;

		strncpy(buf, bh->b_data+2, 1022);
		brelse(bh);
		iput(inode);
		buf[1022] = '\0';
		if (cp = strchr(buf, '\n')) {
			*cp = '\0';
			for (cp = buf; (*cp == ' ') || (*cp == '\t'); cp++);
		}
		if (!cp || *cp == '\0') {
			retval = -ENOEXEC; /* No interpreter name found */
			goto exec_error1;
		}
		interp = i_name = cp;
		i_arg = 0;
		for ( ; *cp && (*cp != ' ') && (*cp != '\t'); cp++) {
 			if (*cp == '/')
				i_name = cp+1;
		}
		if (*cp) {
			*cp++ = '\0';
			i_arg = cp;
		}
		/*
		 * OK, we've parsed out the interpreter name and
		 * (optional) argument.
		 */
		if (sh_bang++ == 0) {
			p = copy_strings(envc, envp, page, p, 0);
			p = copy_strings(--argc, argv+1, page, p, 0);
		}
		/*
		 * Splice in (1) the interpreter's name for argv[0]
		 *           (2) (optional) argument to interpreter
		 *           (3) filename of shell script
		 *
		 * This is done in reverse order, because of how the
		 * user environment and arguments are stored.
		 */
		p = copy_strings(1, &filename, page, p, 1);
		argc++;
		if (i_arg) {
			p = copy_strings(1, &i_arg, page, p, 2);
			argc++;
		}
		p = copy_strings(1, &i_name, page, p, 2);
		argc++;
		if (!p) {
			retval = -ENOMEM;
			goto exec_error1;
		}
		/*
		 * OK, now restart the process with the interpreter's inode.
		 */
		old_fs = get_fs();
		set_fs(get_ds());
		if (!(inode=namei(interp))) { /* get executables inode */
			set_fs(old_fs);
			retval = -ENOENT;
			goto exec_error1;
		}
		set_fs(old_fs);
		goto restart_interp;
	}
	//此时缓冲块中的执行文件头结构数据已经复制到了ex中
	//于是先释放该缓冲块，并开始对ex中的执行头信息进行判断处理
	brelse(bh);
	//对于下列情况将不执行程序：
	//执行文件不是需求页可执行文件(ZMAGIC)；代码和数据重定位部分长度不等于0；
	//(代码段+数据段+堆)长度超过50MB；执行文件长度小于(代码段+数据段+符号表长度+执行头部分)长度的总和
	if (N_MAGIC(ex) != ZMAGIC || ex.a_trsize || ex.a_drsize ||
		ex.a_text+ex.a_data+ex.a_bss>0x3000000 ||
		inode->i_size < ex.a_text+ex.a_data+ex.a_syms+N_TXTOFF(ex)) {
		retval = -ENOEXEC;
		goto exec_error2;
	}
	//如果执行文件中代码开始处没有位于1个页面(1024字节)边界处，则不能执行
	//因为需求页技术要求加载执行文件内容时以页面为单位，因此要求执行文件映像中代码和数据都从页面边界处开始
	if (N_TXTOFF(ex) != BLOCK_SIZE) {
		printk("%s: N_TXTOFF != BLOCK_SIZE. See a.out.h.", filename);
		retval = -ENOEXEC;
		goto exec_error2;
	}
	//如果sh_bang标志没有设置，则复制指定个数的命令行参数和环境字符串到参数和环境空间中
	if (!sh_bang) {
		p = copy_strings(envc,envp,page,p,0);
		p = copy_strings(argc,argv,page,p,0);
		if (!p) {
			retval = -ENOMEM;
			goto exec_error2;
		}
	}
/* OK, This is the point of no return */
	//为执行文件做初始化进程任务结构信息、建立页表等工作
	//由于执行文件直接使用当前进程的"躯壳"，即当前进程将被改造成执行文件的进程
	//因此我们需要首先释放当前进程占用的某些系统资源，包括关闭指定的已打开文件、占用的页表和内存页面等
	//然后根据执行文件头结构信息修改当前进程使用的局部描述符表LDT中描述符的内容，重新设置代码段和数据段描述符限长
	//再利用前面处理得到的e_uid和e_gid等信息来设置进程任务结构中相关的字段
	//最后把执行本次系统调用程序的返回地址eip[]指向执行文件中代码的起始位置处
	//这样当本系统调用退出返回后就会去运行新执行文件的代码了
	//注意，虽然此时新执行文件代码和数据还没有从文件中加载到内存中，但其参数和环境块已经在copy_strings()中使用
	//get_free_page()分配了物理内存来保存数据，并在change_ldt()函数中使用put_page()放到了进程逻辑空间的末端处
	//另外，在create_table()中也会由于在用户栈上存放参数和环境指针表而引起缺页异常，
	//从而内存管理程序也会就此为用户栈空间映射物理内存页

	//首先放回进程原执行程序的i节点，并且让进程executable字段指向新执行文件的i节点
	if (current->executable)
		iput(current->executable);
	current->executable = inode;
	//然后复位原进程的所有信号处理句柄
	for (i=0 ; i<32 ; i++)
		current->sigaction[i].sa_handler = NULL;
	//再根据设定的执行时关闭文件句柄(close_on_exec)位图标志，关闭指定的打开文件并复位该标志
	for (i=0 ; i<NR_OPEN ; i++)
		if ((current->close_on_exec>>i)&1)
			sys_close(i);
	current->close_on_exec = 0;
	//然后根据当前进程指定的基地址和限长，释放原来程序的代码段和数据段所对应的内存页表指定的物理内存页面及页表本身
	free_page_tables(get_base(current->ldt[1]),get_limit(0x0f));
	free_page_tables(get_base(current->ldt[2]),get_limit(0x17));
	//如果"上次任务使用了协处理器"指向的是当前进程，则将其置空，并复位使用了协处理器标志
	if (last_task_used_math == current)
		last_task_used_math = NULL;
	current->used_math = 0;
	//然后根据新执行文件头结构中的代码长度字段a_text的值修改局部表中描述符基址和段限长
	//并将128KB的参数和环境空间页面放置在数据段末端
	//执行下面语句之后，p更改成以数据段起始处为原点的偏移值，但仍指向参数和环境空间数据开始处，即已转换成栈指针值
	p += change_ldt(ex.a_text,page)-MAX_ARG_PAGES*PAGE_SIZE;
	//然后调用内部函数create_table()在栈空间中创建环境和参数变量指针表，
	//供程序的main()作为参数使用，并返回该栈指针
	p = (unsigned long) create_tables((char *)p,argc,envc);
	//接着修改进程个字段值为新执行文件的信息
	current->brk = ex.a_bss +
		(current->end_data = ex.a_data +
		(current->end_code = ex.a_text));
	current->start_stack = p & 0xfffff000;
	current->euid = e_uid;
	current->egid = e_gid;
	//如果执行文件代码加数据长度末端不在页面边界上，则把最后不到1页长度的内存空间初始化为0
	i = ex.a_text+ex.a_data;
	while (i&0xfff)
		put_fs_byte(0,(char *) (i++));
	//最后将原系统调用系统中断的程序在堆栈上的代码指针替换为指向新执行程序的入口点，并将栈指针替换为新执行文件的栈指针
	//此后返回指令将弹出这些栈数据并使得CPU取执行新执行文件，因此不会返回到原系统调用系统中断的程序中去
	eip[0] = ex.a_entry;		//设置进程开始执行的EIP /* eip, magic happens :-) */
	eip[3] = p;			//设置进程的栈顶指针EIP /* stack pointer */
	return 0;
exec_error2:
	iput(inode);			//放回i节点
exec_error1:
	for (i=0 ; i<MAX_ARG_PAGES ; i++)
		free_page(page[i]);			//释放存放参数和环境串的内存页面
	return(retval);						//返回出错码
}
