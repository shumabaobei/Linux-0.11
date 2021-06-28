/*
 *  linux/mm/memory.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * demand-loading started 01.12.91 - seems it is high on the list of
 * things wanted, and it should be easy to implement. - Linus
 */

/*
 * Ok, demand-loading was easy, shared pages a little bit tricker. Shared
 * pages started 02.12.91, seems to work. - Linus.
 *
 * Tested sharing by executing about 30 /bin/sh: under the old kernel it
 * would have taken more than the 6M I have free, but it worked well as
 * far as I could see.
 *
 * Also corrected some "invalidate()"s - I wasn't doing enough of them.
 */

#include <signal.h>

#include <asm/system.h>

#include <linux/sched.h>
#include <linux/head.h>
#include <linux/kernel.h>

volatile void do_exit(long code);

//显示"内存已用完"出错信息，并退出
//函数名前的关键字volatile用于告诉编译器gcc该函数不会返回，这样可让gcc产生更好的代码
//更重要的是使用这个关键字可以避免产生某些(未初始化变量的)假警告信息
static inline volatile void oom(void)
{
	printk("out of memory\n\r");
	//退出代码，出错码含义"资源暂时不可用"
	do_exit(SIGSEGV);
}

//刷新页变换高速缓冲宏函数
//为了提高地址变换的效率，CPU将最近使用的页表数据存放在芯片中高速缓冲中。
//在修改页表信息之后，就需要刷新该缓冲区
//这里使用重新加载页目录基址寄存器cr3的方法来进行刷新。下面eax=0是页目录的基址
#define invalidate() \
__asm__("movl %%eax,%%cr3"::"a" (0))
//嵌入式汇编语法：a代表eax寄存器
//为了让gcc编译产生的汇编语言程序中寄存器名称前有一个百分号"%"，
//在嵌入汇编语句寄存器名称前就必须写上两个百分号"%%"

/* these are not to be changed without changing head.s etc */
#define LOW_MEM 0x100000					//机器物理内存低端(1M)
#define PAGING_MEMORY (15*1024*1024)		//分页内存(主内存区)15M
#define PAGING_PAGES (PAGING_MEMORY>>12)	//分页后的物理内存页面数(3840)
#define MAP_NR(addr) (((addr)-LOW_MEM)>>12)	//指定内存地址映射的页面号 addr为物理地址
#define USED 100							//页面被占用标志

//用于判断给定的线性地址是否位于当前进程的代码段中
#define CODE_SPACE(addr) ((((addr)+4095)&~4095) < \
current->start_code + current->end_code)

static long HIGH_MEMORY = 0;

//从from处复制1页内存到to处(4KB)
#define copy_page(from,to) \
__asm__("cld ; rep ; movsl"::"S" (from),"D" (to),"c" (1024):"cx","di","si")
//cld指令功能：将标志寄存器flag的方向标志位DF清零，在字串操作中使变址寄存器SI或DI的地址指针自动增加，字串处理由前往后
//rep指令功能：按计数寄存器cx中指定的次数重复执行字符串指令
//movsl指令功能：一次传送双字(4个字节)

//物理内存映射字节图(1字节代表1页内存)，每个页面对应的字节用于标志页面当前被引用次数
//PAGING_PAGES=3840
static unsigned char mem_map [ PAGING_PAGES ] = {0,};

//get_free_page()函数用于在主内存区中申请一页空闲内存页，并返回物理内存页的起始地址
//首先扫描内存页面字节图数组mem_map[]，寻找值是0的字节项(对应空闲页面)
//若无则返回0结束，表示物理内存已使用完。若找到值为0的字节，则将其置1，并换算出对应空闲页面的起始地址
//然后对该内存页面作清零操作，最后返回该空闲页面的物理内存起始地址
unsigned long get_free_page(void)
{
register unsigned long __res asm("ax");

//std指令功能：将标志寄存器flag的方向标志位DF置1，在字串操作中使变址寄存器SI或DI的地址指针自动减少，字串处理由后往前
//scasb来判断al数据是否在edi中，配合repne来使用(当不为零时继续遍历)
__asm__("std ; repne ; scasb\n\t"	//置方向位，al(0)与对应每个页面的(di)内容比较
	"jne 1f\n\t"						//如果没有等于0的字节则跳转结束(返回0)
	"movb $1,1(%%edi)\n\t"			//1--->[1+edi],将对应页面的内存映像比特位置1
	"sall $12,%%ecx\n\t"			//exc算数左移12位，页的相对地址
	"addl %2,%%ecx\n\t"				//LOW_MEM+ecx，页的物理地址
	"movl %%ecx,%%edx\n\t"			//寄存器ecx置计数值1024
	"movl $1024,%%ecx\n\t"			//将4092+edx的位置--->edi(该页面的末端)
	"leal 4092(%%edx),%%edi\n\t"	//将eax(0)赋值给edi所指向的地址，即将该页面清零
	"rep ; stosl\n\t"
	"movl %%edx,%%eax\n"		//将页面起始地址--->eax(返回值)
	"1:"
	:"=a" (__res)
	:"0" (0),"i" (LOW_MEM),"c" (PAGING_PAGES),		//"0"表示使用与上面同个位置的输出相同的寄存器x
	"D" (mem_map+PAGING_PAGES-1)
	:"di","cx","dx");
return __res;								//返回空闲物理页面地址(若无空闲页面则返回0)
}

//free_page()用于释放指定地址处的一页物理内存
void free_page(unsigned long addr)
{
	//如果指定内存地址小于1M则返回(1M以内为内核专用)
	if (addr < LOW_MEM) return;
	//如果指定物理内存大于或等于实际内存最高端地址，则显示出错信息
	if (addr >= HIGH_MEMORY)
		panic("trying to free nonexistent page");
	//指定内存换算出页面号
	addr -= LOW_MEM;
	addr >>= 12;
	//判断页面号对应的mem_map[]字节项是否为0
	//若不为0则减一返回
	if (mem_map[addr]--) return;
	//否则对该字节清零，并显示出错信息"试图释放一空闲页面"
	mem_map[addr]=0;
	panic("trying to free free page");
}

/*
 * This function frees a continuos block of page tables, as needed
 * by 'exit()'. As does copy_page_tables(), this handles only 4Mb blocks.
 */
//free_page_tables()用于释放指定线性地址和长度(页表个数)对应的物理内存页
int free_page_tables(unsigned long from,unsigned long size)
{
	unsigned long *pg_table;
	unsigned long * dir, nr;

	//检测参数from给出的线性基地址 是否在4M的边界处
	if (from & 0x3fffff)
		panic("free_page_tables called with wrong alignment");
	//如果from=0则出错，说明试图释放内核和缓冲区所占空间
	if (!from)
		panic("Trying to free up swapper memory space");
	//参数size给出的长度所占的页目录项数
	size = (size + 0x3fffff) >> 22;
	//计算给出的线性基地址对应的起始目录项地址
	//页目录项号=from>>22，因为每项占4字节，由于页目录表从物理地址0开始存放，
	//因此实际目录项指针=目录项号<<2即from>>20，"与"上0xffc确保目录项指针范围有效
	//相当于dir = (unsigned long *) ((from>>22)<<2)
	dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
	//size是释放的页表个数，即页目录表项，而dir是起始目录项指针。
	//现在循环操作页目录项，依次释放每个页表的页表项
	for ( ; size-->0 ; dir++) {
		//如果当前页目录项无效(p位=0)
		//表示该目录项没有使用(对应的页表不存在)，则继续处理下一个页目录项
		if (!(1 & *dir))
			continue;
		//从目录项中取出页表地址pg_table
		pg_table = (unsigned long *) (0xfffff000 & *dir);
		//对该页表中的1024个表项进行处理
		for (nr=0 ; nr<1024 ; nr++) {
			//入股页表项有效(P位=1)，则释放对应的物理内存页面
			if (1 & *pg_table)
				free_page(0xfffff000 & *pg_table);
			//把页表项清零，继续处理下一页表项
			*pg_table = 0;
			pg_table++;
		}
		//当一个页表所有表项都处理完毕就释放该页表自身占据的内存页面
		free_page(0xfffff000 & *dir);
		*dir = 0;
	}
	//刷新页变换高速缓冲
	invalidate();
	return 0;
}

/*
 *  Well, here is one of the most complicated functions in mm. It
 * copies a range of linerar addresses by copying only the pages.
 * Let's hope this is bug-free, 'cause this one I don't want to debug :-)
 *
 * Note! We don't copy just any chunks of memory - addresses have to
 * be divisible by 4Mb (one page-directory entry), as this makes the
 * function easier. It's used only by fork anyway.
 *
 * NOTE 2!! When from==0 we are copying kernel space for the first
 * fork(). Then we DONT want to copy a full page-directory entry, as
 * that would lead to some serious memory waste - we just copy the
 * first 160 pages - 640kB. Even that is more than we need, but it
 * doesn't take any more memory - we don't copy-on-write in the low
 * 1 Mb-range, so the pages can be shared with the kernel. Thus the
 * special case for nr=xxxx.
 */
//copy_page_tables()用于复制指定线性地址和长度(页表个数)内存对应的页目录项和页表项，
//从而被复制的页目录和页表对应的原物理内存区被共享使用
int copy_page_tables(unsigned long from,unsigned long to,long size)
{
	unsigned long * from_page_table;
	unsigned long * to_page_table;
	unsigned long this_page;
	unsigned long * from_dir, * to_dir;
	unsigned long nr;

	//首先检测参数给出的源地址from和目的地址to的有效性(需要在4MB内存边界地址上)
	if ((from&0x3fffff) || (to&0x3fffff))
		panic("copy_page_tables called with wrong alignment");
	
	//计算源地址from和目的地址to的线性基地址对应的起始目录项地址
	from_dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
	to_dir = (unsigned long *) ((to>>20) & 0xffc);
	//参数size给出的长度所占的页目录项数
	size = ((unsigned) (size+0x3fffff)) >> 22;
	//
	for( ; size-->0 ; from_dir++,to_dir++) {
		//如果目的目录项指定的页表已经存在(P=1)，则出错死机
		if (1 & *to_dir)
			panic("copy_page_tables: already exist");
		//表示该目录项没有使用(对应的页表不存在)，则继续处理下一个页目录项
		if (!(1 & *from_dir))
			continue;
		//取出源目录项中页表地址from_page_table
		from_page_table = (unsigned long *) (0xfffff000 & *from_dir);
		//在主存中申请一页空闲内存页
		if (!(to_page_table = (unsigned long *) get_free_page()))
			return -1;	/* Out of memory, see freeing */
		//设置目的目录项信息，把最后3位置1，表示页表映射的内存页面是用户级，并且只读、存在
		*to_dir = ((unsigned long) to_page_table) | 7;
		//如果from=0(说明是在为第一次fork()调用复制内核空间)
		//如果是内核空间，则仅需要复制头160页对应的页表项(nr=160)，对应开始的640KB物理内存
		//否则需要复制一个页表中的所有1024个页表项(nr=1024)，可映射4M物理内存
		nr = (from==0)?0xA0:1024;
		//循环复制指定的nr个内存页面表项
		for ( ; nr-- > 0 ; from_page_table++,to_page_table++) {
			//取出源页表项内容
			this_page = *from_page_table;
			//如果源页面没有使用，则不用复制该表项，继续处理下一项
			if (!(1 & this_page))
				continue;
			//将页表项的位1置0，即让页表项对应的内存页面只读
			this_page &= ~2;
			//将该页表项复制到目的页表中
			*to_page_table = this_page;
			//如果该页表项所指物理页面的地址在1MB以上，则需要设置内存页面映射数组mem_map[]
			//计算页面号，并以它为索引在页面映射数组相应项中增加引用次数
			if (this_page > LOW_MEM) {
				*from_page_table = this_page;
				this_page -= LOW_MEM;
				this_page >>= 12;
				mem_map[this_page]++;
			}
		}
	}
	//刷新页变换高速缓冲
	invalidate();
	return 0;
}

/*
 * This function puts a page in memory at the wanted address.
 * It returns the physical address of the page gotten, 0 if
 * out of memory (either when trying to access page-table or
 * page.)
 */
//把物理内存页面映射到线性地址空间指定处
//或者说是把线性地址空间中指定地址address处的页面映射到主内存区页面page上
//参数page是分配的主内存区中某一页面的指针，address是线性地址
unsigned long put_page(unsigned long page,unsigned long address)
{
	unsigned long tmp, *page_table;

/* NOTE !!! This uses the fact that _pg_dir=0 */

	//首先判断参数给定物理内存页面page的有效性
	if (page < LOW_MEM || page >= HIGH_MEMORY)
		printk("Trying to put page %p at %p\n",page,address);
	if (mem_map[(page-LOW_MEM)>>12] != 1)
		printk("mem_map disagrees with %p at %p\n",page,address);
	//然后根据参数指定的线性地址address计算其在页目录表中对应的目录项指针，并从中取得二级页表地址
	page_table = (unsigned long *) ((address>>20) & 0xffc);
	//如果该目录项有效(P=1)，即指定的页表在内存中，
	//则从中取得指定页表地址放到page_table变量中
	if ((*page_table)&1)
		page_table = (unsigned long *) (0xfffff000 & *page_table);
	//否则就申请一空闲页面给页表使用，并在对应目录项中置相应标志
	//然后将该页表地址放到page_table变量中
	else {
		if (!(tmp=get_free_page()))
			return 0;
		*page_table = tmp|7;
		page_table = (unsigned long *) tmp;
	}
	//最后在找到的页表page_table中设置相关页表项内容，即把物理页面page的地址填入表项同时置位3个标志
	//该页表项在页表中的索引值等于线性地址位21-位12组成的10比特的值。每个页面共可有1024项(0-0x3ff)
	page_table[(address>>12) & 0x3ff] = page | 7;
/* no need for invalidate */
	return page;
}

void un_wp_page(unsigned long * table_entry)
{
	unsigned long old_page,new_page;

	old_page = 0xfffff000 & *table_entry;
	if (old_page >= LOW_MEM && mem_map[MAP_NR(old_page)]==1) {
		*table_entry |= 2;
		invalidate();
		return;
	}
	if (!(new_page=get_free_page()))
		oom();
	if (old_page >= LOW_MEM)
		mem_map[MAP_NR(old_page)]--;
	*table_entry = new_page | 7;
	invalidate();
	copy_page(old_page,new_page);
}	

/*
 * This routine handles present pages, when users try to write
 * to a shared page. It is done by copying the page to a new address
 * and decrementing the shared-page counter for the old page.
 *
 * If it's in code space we exit with a segment error.
 */
void do_wp_page(unsigned long error_code,unsigned long address)
{
#if 0
/* we cannot do this yet: the estdio library writes to code space */
/* stupid, stupid. I really want the libc.a from GNU */
	if (CODE_SPACE(address))
		do_exit(SIGSEGV);
#endif
	un_wp_page((unsigned long *)
		(((address>>10) & 0xffc) + (0xfffff000 &
		*((unsigned long *) ((address>>20) &0xffc)))));

}

void write_verify(unsigned long address)
{
	unsigned long page;

	if (!( (page = *((unsigned long *) ((address>>20) & 0xffc)) )&1))
		return;
	page &= 0xfffff000;
	page += ((address>>10) & 0xffc);
	if ((3 & *(unsigned long *) page) == 1)  /* non-writeable, present */
		un_wp_page((unsigned long *) page);
	return;
}

void get_empty_page(unsigned long address)
{
	unsigned long tmp;

	if (!(tmp=get_free_page()) || !put_page(tmp,address)) {
		free_page(tmp);		/* 0 is ok - ignored */
		oom();
	}
}

/*
 * try_to_share() checks the page at address "address" in the task "p",
 * to see if it exists, and if it is clean. If so, share it with the current
 * task.
 *
 * NOTE! This assumes we have checked that p != current, and that they
 * share the same executable.
 */
static int try_to_share(unsigned long address, struct task_struct * p)
{
	unsigned long from;
	unsigned long to;
	unsigned long from_page;
	unsigned long to_page;
	unsigned long phys_addr;

	from_page = to_page = ((address>>20) & 0xffc);
	from_page += ((p->start_code>>20) & 0xffc);
	to_page += ((current->start_code>>20) & 0xffc);
/* is there a page-directory at from? */
	from = *(unsigned long *) from_page;
	if (!(from & 1))
		return 0;
	from &= 0xfffff000;
	from_page = from + ((address>>10) & 0xffc);
	phys_addr = *(unsigned long *) from_page;
/* is the page clean and present? */
	if ((phys_addr & 0x41) != 0x01)
		return 0;
	phys_addr &= 0xfffff000;
	if (phys_addr >= HIGH_MEMORY || phys_addr < LOW_MEM)
		return 0;
	to = *(unsigned long *) to_page;
	if (!(to & 1))
		if (to = get_free_page())
			*(unsigned long *) to_page = to | 7;
		else
			oom();
	to &= 0xfffff000;
	to_page = to + ((address>>10) & 0xffc);
	if (1 & *(unsigned long *) to_page)
		panic("try_to_share: to_page already exists");
/* share them: write-protect */
	*(unsigned long *) from_page &= ~2;
	*(unsigned long *) to_page = *(unsigned long *) from_page;
	invalidate();
	phys_addr -= LOW_MEM;
	phys_addr >>= 12;
	mem_map[phys_addr]++;
	return 1;
}

/*
 * share_page() tries to find a process that could share a page with
 * the current one. Address is the address of the wanted page relative
 * to the current data space.
 *
 * We first check if it is at all feasible by checking executable->i_count.
 * It should be >1 if there are other tasks sharing this inode.
 */
//共享页面处理
static int share_page(unsigned long address)
{
	struct task_struct ** p;

	if (!current->executable)
		return 0;
	if (current->executable->i_count < 2)
		return 0;
	for (p = &LAST_TASK ; p > &FIRST_TASK ; --p) {
		if (!*p)
			continue;
		if (current == *p)
			continue;
		if ((*p)->executable != current->executable)
			continue;
		if (try_to_share(address,*p))
			return 1;
	}
	return 0;
}

//执行缺页处理，页异常中断处理过程中调用的函数
//函数参数error_code和address是进程在访问页面时由CPU因缺页产生异常而自动生成的
//该函数首先尝试与已加载的相同文件进行页面共享，或者只是由于进程动态申请内存页面而只需映射一页物理内存页即可
//若共享操作不成功，那么只能从相应文件中读入所缺的数据页面到指定线性地址处
void do_no_page(unsigned long error_code,unsigned long address)
{
	int nr[4];
	unsigned long tmp;
	unsigned long page;
	int block,i;

	//首先取线性地址空间中指定地址address处页面地址
	//从而可算出指定线性地址在进程空间中相对于进程基址的偏移长度值tmp，即对应的逻辑地址
	address &= 0xfffff000;
	tmp = address - current->start_code;
	//若当前进程的executable节点指针为空，
	//或者指定地址超过(代码+数据)长度，表明进程在申请新的内存页面存放堆或栈中数据，
	//则申请一页物理内存，并映射到指定的线性地址处
	if (!current->executable || tmp >= current->end_data) {
		get_empty_page(address);
		return;
	}
	//否则说明所缺页面在进程执行映像文件范围内，于是就尝试共享页面操作，若成功则退出
	//若不成功就只能申请一页物理内存页面page，然后从设备上读取执行文件中的相应页面并映射到进程页面逻辑地址tmp处
	if (share_page(tmp))
		return;
	if (!(page = get_free_page()))			//申请一页物理内存
		oom();
/* remember that 1 block is used for header */
	//因为块设备上存放的执行文件映像第1块数据是程序头结构，因此在读取该文件时需要跳过第1块数据
	//所以需要首先计算缺页所在的数据块号。
	//因为每块数据长度BLOCK_SIZE=1KM，因此一页内存可存放4个数据块
	//进程逻辑地址tmp除以数据块大小再加1，即可得出缺少的页面在执行映像文件中的起始块号block
	block = 1 + tmp/BLOCK_SIZE;
	//根据这个块号和执行文件的i节点，我们就可以从映射位图中找到对应块设备中对应的设备逻辑块号(保存到nr[]数组中)
	for (i=0 ; i<4 ; block++,i++)
		nr[i] = bmap(current->executable,block);
	//调用bread_page()即可把这4个逻辑块读入到物理页面page中
	bread_page(page,current->executable->i_dev,nr);
	//在读设备逻辑块操作时，可能会出现这样一种情况
	//即在执行文件中的读取页面位置可能离文件尾不到1个页面的长度，因此可能读入一些无用的信息
	//下面把这部分超出执行文件end_data以后的部分清零处理
	i = tmp + 4096 - current->end_data;
	tmp = page + 4096;
	while (i-- > 0) {
		tmp--;
		*(char *)tmp = 0;
	}
	//最后把引起缺页异常的一页物理页面映射到指定线性地址address处
	//若操作成功就返回，否则就释放内存页，显示内存不够
	if (put_page(page,address))
		return;
	free_page(page);
	oom();
}

//物理内存管理初始化
//该函数对1MB以上内存区域以页面为单位进行管理前的初始化设置工作
//在没有设置虚拟盘RAMDISK的情况下start_mem通常为4MB，end_mem是16MB
//因此此时主内存区范围是4MB-16MB，共有3072个物理页面可供分配
//而范围0-1MB内存空间用于内核系统(其实内核只使用0-640KB，剩下部分被部分高速缓冲和设备内存占用)
void mem_init(long start_mem, long end_mem)
{
	int i;

	//设置内存高端为16MB
	HIGH_MEMORY = end_mem;
	//将1MB到16MB范围内所有内存页面对应的内存映射字节数据项置为已占用状态USED(100)
	//PAGING_PAGES被设置为(15MB/4KB=3840)
	for (i=0 ; i<PAGING_PAGES ; i++)
		mem_map[i] = USED;
	//计算主内存区起始位置处页面号
	i = MAP_NR(start_mem);
	//主内存区的总页面数
	end_mem -= start_mem;
	end_mem >>= 12;
	//主内存区对应页面字节值清零
	while (end_mem-->0)
		mem_map[i++]=0;
}

void calc_mem(void)
{
	int i,j,k,free=0;
	long * pg_tbl;

	for(i=0 ; i<PAGING_PAGES ; i++)
		if (!mem_map[i]) free++;
	printk("%d pages free (of %d)\n\r",free,PAGING_PAGES);
	for(i=2 ; i<1024 ; i++) {
		if (1&pg_dir[i]) {
			pg_tbl=(long *) (0xfffff000 & pg_dir[i]);
			for(j=k=0 ; j<1024 ; j++)
				if (pg_tbl[j]&1)
					k++;
			printk("Pg-dir[%d] uses %d pages\n",i,k);
		}
	}
}
