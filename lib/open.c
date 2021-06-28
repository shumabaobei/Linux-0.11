/*
 *  linux/lib/open.c
 *
 *  (C) 1991  Linus Torvalds
 */

#define __LIBRARY__
#include <unistd.h>
#include <stdarg.h>

//打开文件函数
//打开并有可能创建一个文件
//参数：filename-文件名 flag-文件打开标志 ...
//返回：文件描述符，若出错则置出错码，并返回-1
int open(const char * filename, int flag, ...)
{
	register int res;
	va_list arg;

	//利用va_start()宏函数，取得flag后面参数的指针
	//然后调用系统中断0x80，功能open进行文件打开操作
	va_start(arg,flag);
	__asm__("int $0x80"
		:"=a" (res)
		:"0" (__NR_open),"b" (filename),"c" (flag),
		"d" (va_arg(arg,int)));		//%4-edx(后随参数文件属性mode)
	//系统中断调用返回值>=0表示是一个文件描述符，则直接返回
	if (res>=0)
		return res;
	errno = -res;
	return -1;
}
