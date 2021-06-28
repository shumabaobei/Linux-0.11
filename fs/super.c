/*
 *  linux/fs/super.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * super.c contains code to handle the super-block tables.
 */
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include <errno.h>
#include <sys/stat.h>

int sync_dev(int dev);
void wait_for_keypress(void);

/* set_bit uses setb, as gas doesn't recognize setc */
#define set_bit(bitnr,addr) ({ \
register int __res __asm__("ax"); \
__asm__("bt %2,%3;setb %%al":"=a" (__res):"a" (0),"r" (bitnr),"m" (*(addr))); \
__res; })

struct super_block super_block[NR_SUPER];
/* this is initialized in init/main.c */
int ROOT_DEV = 0;

static void lock_super(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sb->s_lock = 1;
	sti();
}

static void free_super(struct super_block * sb)
{
	cli();
	sb->s_lock = 0;
	wake_up(&(sb->s_wait));
	sti();
}

static void wait_on_super(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sti();
}

//取指定设备的超级块
//在超级块表(数组)中搜索指定设备dev的超级块结构信息
struct super_block * get_super(int dev)
{
	struct super_block * s;

	if (!dev)
		return NULL;
	s = 0+super_block;
	while (s < NR_SUPER+super_block)
		//如果当前搜索项是指定设备的超级块，则先等待该超级块解锁(若已被其他进程上锁的话)
		if (s->s_dev == dev) {
			wait_on_super(s);
			if (s->s_dev == dev)
				return s;
			s = 0+super_block;
		} else
			s++;
	return NULL;
}

void put_super(int dev)
{
	struct super_block * sb;
	struct m_inode * inode;
	int i;

	if (dev == ROOT_DEV) {
		printk("root diskette changed: prepare for armageddon\n\r");
		return;
	}
	if (!(sb = get_super(dev)))
		return;
	if (sb->s_imount) {
		printk("Mounted disk changed - tssk, tssk\n\r");
		return;
	}
	lock_super(sb);
	sb->s_dev = 0;
	for(i=0;i<I_MAP_SLOTS;i++)
		brelse(sb->s_imap[i]);
	for(i=0;i<Z_MAP_SLOTS;i++)
		brelse(sb->s_zmap[i]);
	free_super(sb);
	return;
}

//读取指定设备的超级块
//如果指定设备dev上的文件系统超级块已经在超级块表中，则直接返回该超级块项的指针
//否则就从设备dev上读取超级块到缓冲块中，并复制到超级块中，并返回超级块指针
static struct super_block * read_super(int dev)
{
	struct super_block * s;
	struct buffer_head * bh;
	int i,block;

	//判断参数的有效性，如果没有指明设备，则返回空指针
	if (!dev)
		return NULL;
	//检查该设备是否更换过盘片(即是不是软盘设备)
	//如果更换过盘，则高速缓冲区有关该设备的所有缓冲块均失效，
	//需进行失效处理，即释放原来加载的文件系统
	check_disk_change(dev);
	//如果指定设备dev上的文件系统超级块已经在超级块表中，则直接返回该超级块项的指针
	//否则在超级块中找出一个空项(即字段s_dev=0的项)
	if (s = get_super(dev))
		return s;
	for (s = 0+super_block ;; s++) {
		if (s >= NR_SUPER+super_block)
			return NULL;
		if (!s->s_dev)
			break;
	}
	//对该超级块结构中的内存字段进行部分初始化处理
	s->s_dev = dev;
	s->s_isup = NULL;
	s->s_imount = NULL;
	s->s_time = 0;
	s->s_rd_only = 0;
	s->s_dirt = 0;
	//锁定该超级块
	lock_super(s);
	//从设备上读取超级块信息到bh指向的缓冲块中
	//超级块位于块设备的第2个逻辑块(1号块)中，第1个引导盘块
	if (!(bh = bread(dev,1))) {
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
	//将设备上读到的超级块信息从缓冲块数据区复制到数据块数组相应结构中
	*((struct d_super_block *) s) =
		*((struct d_super_block *) bh->b_data);
	//释放存放读取信息的高速缓冲块
	brelse(bh);
	//朝看超级块的文件系统魔数字段是否正确
	if (s->s_magic != SUPER_MAGIC) {
		s->s_dev = 0;
		free_super(s);
		return NULL;
	}
	//下面开始读取设备上i节点位图和逻辑块位图数据
	//初始化内存超级块结构中位图空间
	for (i=0;i<I_MAP_SLOTS;i++)
		s->s_imap[i] = NULL;
	for (i=0;i<Z_MAP_SLOTS;i++)
		s->s_zmap[i] = NULL;
	block=2;
	//从设备上读取i节点位图和逻辑块位图信息，并存放在超级块对应字段中
	//i节点位图保存在设备上2号块开始的逻辑块中，共占用s_imap_blocks个块
	//逻辑块位图在i节点位图所在块的后续块中，共占用s_zmap_blocks个块
	for (i=0 ; i < s->s_imap_blocks ; i++)
		if (s->s_imap[i]=bread(dev,block))
			block++;
		else
			break;
	for (i=0 ; i < s->s_zmap_blocks ; i++)
		if (s->s_zmap[i]=bread(dev,block))
			block++;
		else
			break;
	//如果读出的位图块数不等于位图应该占用的逻辑块数，说明超级块初始化失败
	//释放前面申请并占用的资源
	if (block != 2+s->s_imap_blocks+s->s_zmap_blocks) {
		for(i=0;i<I_MAP_SLOTS;i++)
			brelse(s->s_imap[i]);
		for(i=0;i<Z_MAP_SLOTS;i++)
			brelse(s->s_zmap[i]);
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
	//由于对于申请空闲i节点的函数来讲，如果设备上所有的i节点已经全被使用，则查找函数会返回0值
	//因此0号节点是不能用的，所以这里将位图中第1块的最低比特位设置为1，以防止文件系统分配0号i节点
	//同样道理，也将逻辑块位图的最低位设置为1
	s->s_imap[0]->b_data[0] |= 1;
	s->s_zmap[0]->b_data[0] |= 1;
	//解锁该超级块
	free_super(s);
	return s;
}

int sys_umount(char * dev_name)
{
	struct m_inode * inode;
	struct super_block * sb;
	int dev;

	if (!(inode=namei(dev_name)))
		return -ENOENT;
	dev = inode->i_zone[0];
	if (!S_ISBLK(inode->i_mode)) {
		iput(inode);
		return -ENOTBLK;
	}
	iput(inode);
	if (dev==ROOT_DEV)
		return -EBUSY;
	if (!(sb=get_super(dev)) || !(sb->s_imount))
		return -ENOENT;
	if (!sb->s_imount->i_mount)
		printk("Mounted inode has i_mount=0\n");
	for (inode=inode_table+0 ; inode<inode_table+NR_INODE ; inode++)
		if (inode->i_dev==dev && inode->i_count)
				return -EBUSY;
	sb->s_imount->i_mount=0;
	iput(sb->s_imount);
	sb->s_imount = NULL;
	iput(sb->s_isup);
	sb->s_isup = NULL;
	put_super(dev);
	sync_dev(dev);
	return 0;
}

int sys_mount(char * dev_name, char * dir_name, int rw_flag)
{
	struct m_inode * dev_i, * dir_i;
	struct super_block * sb;
	int dev;

	if (!(dev_i=namei(dev_name)))
		return -ENOENT;
	dev = dev_i->i_zone[0];
	if (!S_ISBLK(dev_i->i_mode)) {
		iput(dev_i);
		return -EPERM;
	}
	iput(dev_i);
	if (!(dir_i=namei(dir_name)))
		return -ENOENT;
	if (dir_i->i_count != 1 || dir_i->i_num == ROOT_INO) {
		iput(dir_i);
		return -EBUSY;
	}
	if (!S_ISDIR(dir_i->i_mode)) {
		iput(dir_i);
		return -EPERM;
	}
	if (!(sb=read_super(dev))) {
		iput(dir_i);
		return -EBUSY;
	}
	if (sb->s_imount) {
		iput(dir_i);
		return -EBUSY;
	}
	if (dir_i->i_mount) {
		iput(dir_i);
		return -EPERM;
	}
	sb->s_imount=dir_i;
	dir_i->i_mount=1;
	dir_i->i_dirt=1;		/* NOTE! we don't iput(dir_i) */
	return 0;			/* we do that in umount */
}

//安装根文件系统
//函数首先初始化文件表数组file_table[]和超级块表(数组)
//然后读取根文件系统超级块，并取得文件系统根i节点
//最后统计并显示出根文件系统上的可用资源(空闲块数和空闲i节点数)
void mount_root(void)
{
	int i,free;
	struct super_block * p;
	struct m_inode * mi;

	//若磁盘i节点不是32字节，则出错停机
	if (32 != sizeof (struct d_inode))
		panic("bad i-node size");
	//初始化文件表数组(共64x项，即系统同时只能打开64个文件)
	for(i=0;i<NR_FILE;i++)
		file_table[i].f_count=0;		//将所有文件结构中的引用计数设置为0(表示空闲)
	//如果根文件系统所在设备是软盘的话，就提示"插入根文件系统盘"
	//2代表软盘，此时根设备是虚拟盘，是1
	if (MAJOR(ROOT_DEV) == 2) {
		printk("Insert root floppy and press ENTER");
		wait_for_keypress();
	}
	//初始化超级块表
	for(p = &super_block[0] ; p < &super_block[NR_SUPER] ; p++) {
		p->s_dev = 0;
		p->s_lock = 0;
		p->s_wait = NULL;
	}
	//开始安装根文件系统
	//从根设备上读取文件系统超级块，并取得文件系统的根i节点(1号节点)在内存i节点表中的指针
	if (!(p=read_super(ROOT_DEV)))
		panic("Unable to mount root");
	if (!(mi=iget(ROOT_DEV,ROOT_INO)))	//ROOT_INO=1
		panic("Unable to read root i-node");
	//现在对超级块和geni节点进行设置
	//把根i节点引用次数递增3次
	mi->i_count += 3 ;	/* NOTE! it is logically used 4 times, not 1 */
	//置该超级块的被安装文件系统i节点和被安装到i节点字段为该i节点
	p->s_isup = p->s_imount = mi;
	//设置当前进程的当前工作目录和根目录i节点，此时当前进程是1号进程(init进程)
	current->pwd = mi;			//当前进程掌控根文件系统的根i节点
	current->root = mi;			//父子进程创建机制将这个特性遗传给子进程
	//然后对根文件系统上的资源作统计工作，统计该设备上空闲块数和空闲i节点数
	//空闲块数
	free=0;
	i=p->s_nzones;
	while (-- i >= 0)
		if (!set_bit(i&8191,p->s_zmap[i>>13]->b_data))
			free++;
	printk("%d/%d free blocks\n\r",free,p->s_nzones);
	//空闲i节点数
	free=0;
	i=p->s_ninodes+1;
	while (-- i >= 0)
		if (!set_bit(i&8191,p->s_imap[i>>13]->b_data))
			free++;
	printk("%d/%d free inodes\n\r",free,p->s_ninodes);
}
