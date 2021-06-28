/*
 *  linux/kernel/system_call.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  system_call.s  contains the system-call low-level handling routines.
 * This also contains the timer-interrupt handler, as some of the code is
 * the same. The hd- and flopppy-interrupts are also here.
 *
 * NOTE: This code handles signal-recognition, which happens every time
 * after a timer-interrupt and after each system call. Ordinary interrupts
 * don't handle signal-recognition, as that would clutter them up totally
 * unnecessarily.
 *
 * Stack layout in 'ret_from_system_call':
 *
 *	 0(%esp) - %eax
 *	 4(%esp) - %ebx
 *	 8(%esp) - %ecx
 *	 C(%esp) - %edx
 *	10(%esp) - %fs
 *	14(%esp) - %es
 *	18(%esp) - %ds
 *	1C(%esp) - %eip
 *	20(%esp) - %cs
 *	24(%esp) - %eflags
 *	28(%esp) - %oldesp
 *	2C(%esp) - %oldss
 */

SIG_CHLD	= 17

EAX		= 0x00
EBX		= 0x04
ECX		= 0x08
EDX		= 0x0C
FS		= 0x10
ES		= 0x14
DS		= 0x18
EIP		= 0x1C
CS		= 0x20
EFLAGS		= 0x24
OLDESP		= 0x28
OLDSS		= 0x2C

state	= 0		# these are offsets into the task-struct.
counter	= 4
priority = 8
signal	= 12
sigaction = 16		# MUST be 16 (=len of sigaction)
blocked = (33*16)

# offsets within sigaction
sa_handler = 0
sa_mask = 4
sa_flags = 8
sa_restorer = 12

nr_system_calls = 72

/*
 * Ok, I get parallel printer interrupts while using the floppy for some
 * strange reason. Urgel. Now I just ignore them.
 */
.globl _system_call,_sys_fork,_timer_interrupt,_sys_execve
.globl _hd_interrupt,_floppy_interrupt,_parallel_interrupt
.globl _device_not_available, _coprocessor_error

# 错误的系统调用号
.align 2			# 内存4字节对齐
bad_sys_call:
	movl $-1,%eax		#eax置1，退出中断
	iret
.align 2
reschedule:
	pushl $ret_from_sys_call
	jmp _schedule

# int 0x80------Linux系统调用入口点
.align 2
_system_call:
	cmpl $nr_system_calls-1,%eax	# 调用号如果超出范围就在eax中置-1并退出
	ja bad_sys_call			# JA（jump above）大于则转移到目标指令执行
	push %ds		# 保存原段寄存器值
	push %es
	push %fs
	# 一个系统调用最多可带有3个参数，也可以不带参数
	# 下面入栈的ebx,ecx和edx中放着系统调用相应C语言函数的调用参数
	# ebx---第一个参数 ecx---第二个参数 edx---第三个参数
	pushl %edx
	pushl %ecx		# push %ebx,%ecx,%edx as parameters
	pushl %ebx		# to the system call
	# ds,es指向内核数据段
	movl $0x10,%edx		# set up ds,es to kernel space
	mov %dx,%ds
	mov %dx,%es
	# fs指向局部数据段
	movl $0x17,%edx		# fs points to local data space
	mov %dx,%fs
	# 调用地址=[_sys_call_table+eax*4]
	call _sys_call_table(,%eax,4)
	# 系统调用返回值入栈
	pushl %eax
	# 查看当前任务的运行状态
	# 如果不在就绪状态(state不等于0)就去执行调度程序
	# 如果该任务在就绪状态，但其时间片已用完(counter=0),则也去执行调度程序
	movl _current,%eax
	cmpl $0,state(%eax)		# state 
	jne reschedule
	cmpl $0,counter(%eax)		# counter
	je reschedule
# 以下这段代码对信号进行识别处理
ret_from_sys_call:
	# 首先判别当前任务是否是初始任务task0,如果是则不必对其进行信号方面的处理，直接跳到下面的3
	movl _current,%eax		# task[0] cannot have signals
	cmpl _task,%eax
	je 3f
	cmpw $0x0f,CS(%esp)		# was old code segment supervisor ?
	jne 3f
	cmpw $0x17,OLDSS(%esp)		# was stack segment = 0x17 ?
	jne 3f
	movl signal(%eax),%ebx
	movl blocked(%eax),%ecx
	notl %ecx
	andl %ebx,%ecx
	bsfl %ecx,%ecx
	je 3f
	btrl %ecx,%ebx
	movl %ebx,signal(%eax)
	incl %ecx
	pushl %ecx
	call _do_signal
	popl %eax
# 如果是进程0则直接跳到这个地方执行，将7个寄存器的值出栈给CPU
3:	popl %eax				# eax中含有之前入栈的系统调用返回值
	popl %ebx
	popl %ecx
	popl %edx
	pop %fs
	pop %es
	pop %ds
	iret

.align 2
_coprocessor_error:
	push %ds
	push %es
	push %fs
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	pushl $ret_from_sys_call
	jmp _math_error

.align 2
_device_not_available:
	push %ds
	push %es
	push %fs
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	pushl $ret_from_sys_call
	clts				# clear TS so that we can use math
	movl %cr0,%eax
	testl $0x4,%eax			# EM (math emulation bit)
	je _math_state_restore
	pushl %ebp
	pushl %esi
	pushl %edi
	call _math_emulate
	popl %edi
	popl %esi
	popl %ebp
	ret

; int 32------(int 0x20)时钟中断处理程序
; 中断频率被设置为100Hz
; 这段代码将jiffies增1，发送结束中断指令给8259控制器，然后用当前特权级作为参数调用C函数do_timer(long CPL)
; 当调用返回时转去检测并处理信号
.align 2
_timer_interrupt:
	push %ds		# save ds,es and put kernel data space
	push %es		# into them. %fs is used by _system_call
	push %fs
	pushl %edx		# we save %eax,%ecx,%edx as gcc doesn't
	pushl %ecx		# save those across function calls. %ebx
	pushl %ebx		# is saved as we use that in ret_sys_call
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	incl _jiffies
;由于初始化中断控制芯片时没有采用自动EOI，所以这里需要发指令结束该硬件中断
	movb $0x20,%al		# EOI to interrupt controller #1
	outb %al,$0x20
	movl CS(%esp),%eax
	andl $3,%eax		# %eax is CPL (0 or 3, 0=supervisor)
	pushl %eax
	call _do_timer		# 'do_timer(long CPL)' does everything from
	addl $4,%esp		# task switching to accounting ...
	jmp ret_from_sys_call

.align 2
;这是sys_execve()系统调用，取中断调用程序的代码指针作为参数调用C函数do_execve()
_sys_execve:					;EIP= 0x1C
	lea EIP(%esp),%eax		;eax指向堆栈中保存用户程序eip指针处(EIP+%esp)
	pushl %eax				;把EIP值"所在栈空间的地址值"
	call _do_execve
	addl $4,%esp
	ret

.align 2
_sys_fork:
	;汇编调用C语言返回自动恢复堆栈
	call _find_empty_process		;call指令：(1)将当前的IP压入栈中(2)转移
	testl %eax,%eax					;在eax中返回进程号PID，若返回负数则退出
	js 1f							;js(jump if sign)指令:条件转移指令，结果为负则转移
	push %gs
	pushl %esi
	pushl %edi
	pushl %ebp
	pushl %eax
	call _copy_process
	addl $20,%esp
1:	ret

# int 0x2E硬盘中断处理程序，响应硬件中断请求IRQ14
# 当请求的硬盘操作完成或出错就会发出此中断信号
# 首先向8259A中断控制 从芯片发送结束硬件中断指令EOI，然后取变量do_hd中的函数指针放入edx寄存器中，并置do_hd为NULL
# 接着判断edx指针是否为空
_hd_interrupt:
	pushl %eax
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%eax			# ds,es置为内核数据段
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax			# fs置为调用程序的局部数据段
	mov %ax,%fs
	movb $0x20,%al
	outb %al,$0xA0		# EOI to interrupt controller #1	# 送从8259A
	jmp 1f			# give port chance to breathe		jmp起延时作用
1:	jmp 1f
	# do_hd定义为一个函数指针，将被赋予read_intr()或write_intr()函数地址
	# 放到edx寄存器后就将do_hd指针变量置为NULL
1:	xorl %edx,%edx
	xchgl _do_hd,%edx
	testl %edx,%edx				# 测试函数指针是否为NULL
	jne 1f
	movl $_unexpected_hd_interrupt,%edx
1:	outb %al,$0x20				# 送主8259A中断控制器EOI指令(结束硬件中断)
	call *%edx		# "interesting" way of handling intr.	调用read_intr()或write_intr()函数
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret

_floppy_interrupt:
	pushl %eax
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	movb $0x20,%al
	outb %al,$0x20		# EOI to interrupt controller #1
	xorl %eax,%eax
	xchgl _do_floppy,%eax
	testl %eax,%eax
	jne 1f
	movl $_unexpected_floppy_interrupt,%eax
1:	call *%eax		# "interesting" way of handling intr.
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret

_parallel_interrupt:
	pushl %eax
	movb $0x20,%al
	outb %al,$0x20
	popl %eax
	iret
