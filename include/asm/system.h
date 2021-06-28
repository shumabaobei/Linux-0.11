//移动到用户模式运行
//该函数利用iret指令实现从内核模式移动到初始０号进程执行
//模仿中断硬件压栈，顺序是ss,esp,eflags,cs,eip
#define move_to_user_mode() \
__asm__ ("movl %%esp,%%eax\n\t" \		//保存堆栈指针esp到eax寄存器
	"pushl $0x17\n\t" \					//ss入栈，0x17即二进制10111(3特权级、LDT、数据段)
	"pushl %%eax\n\t" \					//ESP入栈
	"pushfl\n\t" \						//EFLAGS入栈
	"pushl $0x0f\n\t" \					//CS入栈，0x0f即1111(3特权级、LDT、代码段)
	"pushl $1f\n\t" \					//EIP入栈(即下面标号1的偏移地址)
	"iret\n" \							//执行中断返回指令，则会跳转到下面标号1处
	"1:\tmovl $0x17,%%eax\n\t" \
	"movw %%ax,%%ds\n\t" \
	"movw %%ax,%%es\n\t" \
	"movw %%ax,%%fs\n\t" \
	"movw %%ax,%%gs" \
	:::"ax")

#define sti() __asm__ ("sti"::)
#define cli() __asm__ ("cli"::)
#define nop() __asm__ ("nop"::)

#define iret() __asm__ ("iret"::)

//设置门描述符，填充内容
#define _set_gate(gate_addr,type,dpl,addr) \
__asm__ ("movw %%dx,%%ax\n\t" \			//将edx的低位赋值给eax的低字
	"movw %0,%%dx\n\t" \				//%0对应第二个冒号后的第1行的"i"
	"movl %%eax,%1\n\t" \				//%1对应第二个冒号后的第2行的"o"
	"movl %%edx,%2" \					//%2对应第二个冒号后的第3行的"o"
	: \
	: "i" ((short) (0x8000+(dpl<<13)+(type<<8))), \	//立即数
	"o" (*((char *) (gate_addr))), \				//中断描述符前4个字节地址
	"o" (*(4+(char *) (gate_addr))), \				//中断描述符后4个字节地址
	"d" ((char *) (addr)),"a" (0x00080000))			//"d"对应edx(32位偏移地址)，"a"对应eax

//设置中断门函数
//参数n-中断号，addr-中断程序偏移地址
//&idt[n]是中断描述符表中中断号n对应项的偏移量，中断描述符类型是14，特权级是0
#define set_intr_gate(n,addr) \
	_set_gate(&idt[n],14,0,addr)

//设置陷阱门函数
//参数n-中断号，addr-中断程序偏移地址
//&idt[n]是中断描述符表中中断号n对应项的偏移量，中断描述符类型是15，特权级是0
#define set_trap_gate(n,addr) \
	_set_gate(&idt[n],15,0,addr)

//设置系统陷阱门函数
//参数n-中断号，addr-中断程序偏移地址
//&idt[n]是中断描述符表中中断号n对应项的偏移量，中断描述符类型是15，特权级是3
#define set_system_gate(n,addr) \
	_set_gate(&idt[n],15,3,addr)

#define _set_seg_desc(gate_addr,type,dpl,base,limit) {\
	*(gate_addr) = ((base) & 0xff000000) | \
		(((base) & 0x00ff0000)>>16) | \
		((limit) & 0xf0000) | \
		((dpl)<<13) | \
		(0x00408000) | \
		((type)<<8); \
	*((gate_addr)+1) = (((base) & 0x0000ffff)<<16) | \
		((limit) & 0x0ffff); }

//在全局表中设置任务状态段/局部表描述符
//状态段和局部表段的长度均被设置成104字节
#define _set_tssldt_desc(n,addr,type) \
__asm__ ("movw $104,%1\n\t" \		//将104，即1101000存入描述符的1、2字节
	"movw %%ax,%2\n\t" \			//将tss或ldt基地址的低16位存入描述符的第3、4字节
	"rorl $16,%%eax\n\t" \			//循环右移16位，即高、低字节互换
	"movb %%al,%3\n\t" \			//将互换完的第1字节，即地址的第3字节存入第5字节
	"movb $" type ",%4\n\t" \		//将0x89或0x82存入第6字节
	"movb $0x00,%5\n\t" \			//将0x00存入第7字节
	"movb %%ah,%6\n\t" \			//将互换完的第2字节，即地址的第4字节存入第8字节
	"rorl $16,%%eax" \				//复原eax
	::"a" (addr), "m" (*(n)), "m" (*(n+2)), "m" (*(n+4)), \
	 "m" (*(n+5)), "m" (*(n+6)), "m" (*(n+7)) \
	)

//在全局表中设置任务状态段描述符
//n是描述符的指针；addr是描述符中段基地址；0x89是任务状态段描述符类型
#define set_tss_desc(n,addr) _set_tssldt_desc(((char *) (n)),addr,"0x89")
//在全局表中设置局部表描述符
//n是描述符的指针；addr是描述符中段基地址；0x82是局部表段描述符类型
#define set_ldt_desc(n,addr) _set_tssldt_desc(((char *) (n)),addr,"0x82")
