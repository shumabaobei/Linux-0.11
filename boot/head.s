/*
 *  linux/boot/head.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  head.s contains the 32-bit startup code.
 *
 * NOTE!!! Startup happens at absolute address 0x00000000, which is also where
 * the page directory will exist. The startup code will be overwritten by
 * the page directory.
 */
.text
.globl _idt,_gdt,_pg_dir,_tmp_floppy_area
_pg_dir:			;标号_pg_dir标示内核分页机制完成后的内核起始位置，也就是物理内存的起始位置0x000000
startup_32:
	;设置各个段寄存器指向内核数据段
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	mov %ax,%fs
	mov %ax,%gs
	;表示_stack_start->ss:esp，设置系统堆栈(指向user_stack数据结构最末位置)
	lss _stack_start,%esp		
	call setup_idt
	call setup_gdt
	;重新加载各个寄存器
	movl $0x10,%eax		# reload all the segment registers
	mov %ax,%ds		# after changing gdt. CS was already
	mov %ax,%es		# reloaded in 'setup_gdt'
	mov %ax,%fs
	mov %ax,%gs
	lss _stack_start,%esp
	;测试A20地址线是否已经开启
	xorl %eax,%eax
1:	incl %eax		# check that A20 really IS enabled
	movl %eax,0x000000	# loop forever if it isn't
	cmpl %eax,0x100000
	je 1b				;'1b'表示向后跳转到标号1去
						;若是'5f'则表示向前跳转到标号5去
/*
 * NOTE! 486 should set bit 16, to check for write-protect in supervisor
 * mode. Then it would be unnecessary with the "verify_area()"-calls.
 * 486 users probably want to set the NE (#5) bit also, so as to use
 * int 16 for math errors.
 */
	;检查数学协处理器芯片是否存在
	movl %cr0,%eax		# check math chip
	andl $0x80000011,%eax	# Save PG,PE,ET
/* "orl $0x10020,%eax" here for 486 might be good */
	orl $2,%eax		# set MP
	movl %eax,%cr0
	call check_x87
	jmp after_page_tables

/*
 * We depend on ET to be correct. This checks for 287/387.
 */
check_x87:
	fninit
	fstsw %ax
	cmpb $0,%al
	je 1f			/* no coprocessor: have to set bits */
	movl %cr0,%eax
	xorl $6,%eax		/* reset MP, set EM */
	movl %eax,%cr0
	ret
.align 2
1:	.byte 0xDB,0xE4		/* fsetpm for 287, ignored by 387 */
	ret

/*
 *  setup_idt
 *
 *  sets up a idt with 256 entries pointing to
 *  ignore_int, interrupt gates. It then loads
 *  idt. Everything that wants to install itself
 *  in the idt-table may do so themselves. Interrupts
 *  are enabled elsewhere, when we can be relatively
 *  sure everything is ok. This routine will be over-
 *  written by the page tables.
 */
setup_idt:
	lea ignore_int,%edx
	movl $0x00080000,%eax
	movw %dx,%ax		/* selector = 0x0008 = cs */
	movw $0x8E00,%dx	/* interrupt gate - dpl=0, present */

	lea _idt,%edi
	mov $256,%ecx
rp_sidt:
	movl %eax,(%edi)
	movl %edx,4(%edi)
	addl $8,%edi
	dec %ecx
	jne rp_sidt
	lidt idt_descr
	ret

/*
 *  setup_gdt
 *
 *  This routines sets up a new gdt and loads it.
 *  Only two entries are currently built, the same
 *  ones that were built in init.s. The routine
 *  is VERY complicated at two whole lines, so this
 *  rather long comment is certainly needed :-).
 *  This routine will beoverwritten by the page tables.
 */
setup_gdt:
	lgdt gdt_descr
	ret

/*
 * I put the kernel page tables right after the page directory,
 * using 4 of them to span 16 Mb of physical memory. People with
 * more than 16MB will have to expand this.
 */
.org 0x1000
pg0:

.org 0x2000
pg1:

.org 0x3000
pg2:

.org 0x4000
pg3:

.org 0x5000
/*
 * tmp_floppy_area is used by the floppy-driver when DMA cannot
 * reach to a buffer-block. It needs to be aligned, so that it isn't
 * on a 64kB border.
 */
;软盘缓冲区1KB
_tmp_floppy_area:
	.fill 1024,1,0

;下面这几个入栈操作用于为跳转到init/main.c中的main()函数作准备工作
after_page_tables:
	;前面3个入栈0分别表示envp、argv指针和argc的值，但main()没有用到
	pushl $0		# These are the parameters to main :-)
	pushl $0
	pushl $0
	;此入栈操作是模拟调用main.c程序时首先将返回地址入栈的操作，所以如果main.c程序真的退出时
	;就会返回到这里的标号L6处继续执行下去
	pushl $L6		# return address for main, if it decides to.
	;将main.c的地址压入堆栈，这样执行'ret'返回指令时就会将main.c程序的地址弹出堆栈
	;并去执行main.c程序
	pushl $_main
	;创建分页机制
	jmp setup_paging
L6:
	jmp L6			# main should never return here, but
				# just in case, we know what happens.

/* This is the default interrupt "handler" :-) */
int_msg:
	.asciz "Unknown interrupt\n\r"
.align 2
ignore_int:
	pushl %eax
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	mov %ax,%fs
	pushl $int_msg
	call _printk
	popl %eax
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret


/*
 * Setup_paging
 *
 * This routine sets up paging by setting the page bit
 * in cr0. The page tables are set up, identity-mapping
 * the first 16MB. The pager assumes that no illegal
 * addresses are produced (ie >4Mb on a 4Mb machine).
 *
 * NOTE! Although all physical memory should be identity
 * mapped by this routine, only the kernel page functions
 * use the >1Mb addresses directly. All "normal" functions
 * use just the lower 1Mb, or the local data space, which
 * will be mapped to some other place - mm keeps track of
 * that.
 *
 * For those with more memory than 16 Mb - tough luck. I've
 * not got it, why should you :-) The source is here. Change
 * it. (Seriously - it shouldn't be too difficult. Mostly
 * change some constants etc. I left it at 16Mb, as my machine
 * even cannot be extended past that (ok, but it was cheap :-)
 * I've tried to show which constants to change by having
 * some kind of marker at them (search for "16Mb"), but I
 * won't guarantee that's all :-( )z
 */
.align 2					;按4字节方式对齐内存地址边界
;开启分页机制
;在内存物理地址0x0处开始存放1页页目录表和4页页表
;页目录表是系统所有进程公用，而这里的4页页表则属于内核专用，它们一一映射线性地址起始16MB的物理内存上
;对于新的进程，系统会在主内存区为其申请页面存放页表
setup_paging:
	;首先对5页内存清零
	movl $1024*5,%ecx		/* 5 pages - pg_dir+4 page tables */
	xorl %eax,%eax
	xorl %edi,%edi			/* pg_dir is at 0x000 */
	cld;rep;stosl	;stosl指令：将EAX中的值保存到ES:EDI指向的地址中
	;设置页目录表中的项
	;如"$pg0+7"表示：0x00001007是页目录表的第1项
	;则第1页表所在的地址=0x00001007&0xfffff000=0x1000
	;第1个页面的属性标志=0x0x00001007&0x00000fff=0x07，表示该页存在、用户可读写
	movl $pg0+7,_pg_dir		/* set present bit/user r/w */
	movl $pg1+7,_pg_dir+4		/*  --------- " " --------- */
	movl $pg2+7,_pg_dir+8		/*  --------- " " --------- */
	movl $pg3+7,_pg_dir+12		/*  --------- " " --------- */
	;填写4个页面中所有项的内容，共有4(页表)*1024(项)=4096项，能映射4096*4KB=16MB物理内存
	;每项的内容：当前项所映射的物理内存地址+该页的标志(这里均为7)
	;从最后一个页表的最后一项开始按倒退顺序填写
	;最后一页的最后一项的位置是$pg3+4092
	movl $pg3+4092,%edi
	;;最后一项对应的物理内存页面地址是0xfff000
	movl $0xfff007,%eax		/*  16Mb - 4096 + 7 (r/w user,p) */
	std			;方向位置位，edi值递减(4字节)
1:	stosl			/* fill pages backwards - more efficient :-) */
	subl $0x1000,%eax		;每填好一项，物理地址值减0x1000
	jge 1b					;如果小于0说明全填写好了
	;设置页目录表基址寄存器cr3指向页目录表(0x0000)
	xorl %eax,%eax		/* pg_dir is at 0x0000 */
	movl %eax,%cr3		/* cr3 - page directory start */
	;开启分页机制，置cr0寄存器PG位为1
	movl %cr0,%eax
	orl $0x80000000,%eax
	movl %eax,%cr0		/* set paging (PG) bit */
	;在改变分页处理标志后要求使用转移指令刷新预取指令队列，这里使用的是返回指令ret
	ret			/* this also flushes prefetch-queue */


.align 2
.word 0
;加载中断描述符表寄存器idtr指令lidt要求的6字节操作数
idt_descr:
	.word 256*8-1		# idt contains 256 entries
	.long _idt
.align 2
.word 0
;加载中断描述符表寄存器gdtr指令lgdt要求的6字节操作数
gdt_descr:
	.word 256*8-1		# so does gdt (not that that's any
	.long _gdt		# magic number, but it works for me :^)

	.align 3
_idt:	.fill 256,8,0		# idt is uninitialized 256项，每项8字节，填0

;全局表
;前4项分别是空项(不用)、代码段描述符、数据段描述符、系统段描述符(Linux没有派用处)
;后面预留252项空间用于放置所创建任务的局部描述符(LDT)和对应的任务状态段TSS描述符
_gdt:	.quad 0x0000000000000000	/* NULL descriptor */
	.quad 0x00c09a0000000fff	/* 16Mb */ ;0x08，内核代码段最大长度16MB
	.quad 0x00c0920000000fff	/* 16Mb */ ;0x10，内核数据段最大长度16MB
	.quad 0x0000000000000000	/* TEMPORARY - don't use */
	.fill 252,8,0			/* space for LDT's and TSS's etc */ ;预留空间
