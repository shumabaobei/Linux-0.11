/*
 *  linux/mm/page.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * page.s contains the low-level page-exception code.
 * the real work is done in mm.c
 */

.globl _page_fault
/*
页异常中断处理程序

主要分两种情况处理：
	(1) 由于缺页引起的页异常中断，通过调用do_no_page(error_code, address)来处理
	(2) 由页写保护引起的页异常，此时调用页写保护处理函数do_wp_page(error_code, address)进行处理
*/
;其中的出错码(error_code)是由CPU自动产生并压入堆栈，
;出现异常时访问的线性地址是从控制器寄存器CR2中取得的，CR2是专门用来存放页出错时的线性地址
_page_fault: 
	xchgl %eax,(%esp)		;取出错误码到eax
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%edx			;置内核数据段选择符
	mov %dx,%ds
	mov %dx,%es
	mov %dx,%fs
	movl %cr2,%edx			;取引起页面异常的线性地址
	pushl %edx
	pushl %eax
	testl $1,%eax			;测试页存在标志P(位0)，如果不是缺页引起的异常则跳转
	jne 1f
	call _do_no_page	;调用缺页处理函数
	jmp 2f
1:	call _do_wp_page	;调用写保护处理函数
2:	addl $8,%esp
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret
