/*
 *  linux/lib/close.c
 *
 *  (C) 1991  Linus Torvalds
 */

#define __LIBRARY__
#include <unistd.h>

//关闭文件函数
//下面该调用宏函数对应：int close(int fd)
_syscall1(int,close,int,fd)
