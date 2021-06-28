#ifndef _HEAD_H
#define _HEAD_H

typedef struct desc_struct {
	unsigned long a,b;		//每个描述符占8个字节
} desc_table[256];			//每个描述符表有256项

extern unsigned long pg_dir[1024];
extern desc_table idt,gdt;		//中断描述符表,全局描述符表

#define GDT_NUL 0
#define GDT_CODE 1
#define GDT_DATA 2
#define GDT_TMP 3

#define LDT_NUL 0
#define LDT_CODE 1
#define LDT_DATA 2

#endif
