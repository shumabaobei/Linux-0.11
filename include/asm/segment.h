//在Linux系统中，当用户程序通过系统调用开始执行内核代码时，内核程序会首先在段寄存器ds和es中加载
//全局描述符表GDT中的内核数据段描述符(段值0x10)，即把ds和es用于访问内核数据段；而在fs中加载了局部
//描述符表LDT中的任务的数据段描述符(段值0x17)，即把fs用于访问用户数据段
//因此在执行内核代码时，若要存取用户程序(任务)中的数据就需要使用特殊的方式
//本文件中的get_fs_byte()和put_fs_byte()等函数就是专门用来访问用户程序中的数据

//读取fs段中指定地址处的字节
//返回：返回字节fs:[addr]处的字节
extern inline unsigned char get_fs_byte(const char * addr)
{
	//定义寄存器变量_v，以便高效访问和操作
	unsigned register char _v;

	__asm__ ("movb %%fs:%1,%0":"=r" (_v):"m" (*addr));
	return _v;
}

extern inline unsigned short get_fs_word(const unsigned short *addr)
{
	unsigned short _v;

	__asm__ ("movw %%fs:%1,%0":"=r" (_v):"m" (*addr));
	return _v;
}

extern inline unsigned long get_fs_long(const unsigned long *addr)
{
	unsigned long _v;

	__asm__ ("movl %%fs:%1,%0":"=r" (_v):"m" (*addr)); \
	return _v;
}

extern inline void put_fs_byte(char val,char *addr)
{
__asm__ ("movb %0,%%fs:%1"::"r" (val),"m" (*addr));
}

extern inline void put_fs_word(short val,short * addr)
{
__asm__ ("movw %0,%%fs:%1"::"r" (val),"m" (*addr));
}

extern inline void put_fs_long(unsigned long val,unsigned long * addr)
{
__asm__ ("movl %0,%%fs:%1"::"r" (val),"m" (*addr));
}

/*
 * Someone who knows GNU asm better than I should double check the followig.
 * It seems to work, but I don't know if I'm doing something subtly wrong.
 * --- TYT, 11/24/91
 * [ nothing wrong here, Linus ]
 */

extern inline unsigned long get_fs() 
{
	unsigned short _v;
	__asm__("mov %%fs,%%ax":"=a" (_v):);
	return _v;
}

extern inline unsigned long get_ds() 
{
	unsigned short _v;
	__asm__("mov %%ds,%%ax":"=a" (_v):);
	return _v;
}

extern inline void set_fs(unsigned long val)
{
	__asm__("mov %0,%%fs"::"a" ((unsigned short) val));
}

