/*
 *  linux/fs/inode.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h>
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/system.h>

struct m_inode inode_table[NR_INODE]={{0,},};		//内存中i节点表(NR_INODE=32)

static void read_inode(struct m_inode * inode);
static void write_inode(struct m_inode * inode);

//等待指定i节点可用
static inline void wait_on_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	sti();
}

static inline void lock_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	inode->i_lock=1;
	sti();
}

static inline void unlock_inode(struct m_inode * inode)
{
	inode->i_lock=0;
	wake_up(&inode->i_wait);
}

void invalidate_inodes(int dev)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);
		if (inode->i_dev == dev) {
			if (inode->i_count)
				printk("inode in use on removed disk\n\r");
			inode->i_dev = inode->i_dirt = 0;
		}
	}
}

void sync_inodes(void)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);
		if (inode->i_dirt && !inode->i_pipe)
			write_inode(inode);
	}
}

//文件数据块映射到盘块的处理函数
//参数：inode-文件的i节点指针 block-文件中的数据块号 create-创建块标志
//该函数把指定的文件数据块block对应到设备上逻辑块并返回逻辑块号
//如果创建标志置位，则在设备上对应逻辑块不存在时就申请新磁盘块，返回文件数据块block对应在设备上的逻辑块号(盘块号)
static int _bmap(struct m_inode * inode,int block,int create)
{
	struct buffer_head * bh;
	int i;

	//首先判断参数文件数据块号block的有效性
	//如果块号小于0则停机
	if (block<0)
		panic("_bmap: block<0");
	//如果块号大于直接块数+间接块数+二次间接块数，超出文件系统表示范围则停机
	if (block >= 7+512+512*512)
		panic("_bmap: block>big");
	//如果块号小于7则使用直接块表示
	if (block<7) {
		//如果创建标志置位，并且i节点中对应该块的逻辑块字段为0,
		//则向相应设备申请一块磁盘块，并将盘上逻辑块号填入逻辑块字段中
		if (create && !inode->i_zone[block])
			if (inode->i_zone[block]=new_block(inode->i_dev)) {
				inode->i_ctime=CURRENT_TIME;
				inode->i_dirt=1;
			}
		//返回逻辑块号
		return inode->i_zone[block];
	}
	//如果该块号>=7且小于7+512，则说明使用的是一次间接块
	block -= 7;
	if (block<512) {
		if (create && !inode->i_zone[7])
			if (inode->i_zone[7]=new_block(inode->i_dev)) {
				inode->i_dirt=1;
				inode->i_ctime=CURRENT_TIME;
			}
		if (!inode->i_zone[7])
			return 0;
		//现在读取设备上该i节点的一次间接块，并取该间接块上的第block项中的逻辑块号(盘块号)i
		if (!(bh = bread(inode->i_dev,inode->i_zone[7])))
			return 0;
		i = ((unsigned short *) (bh->b_data))[block];
		if (create && !i)
			if (i=new_block(inode->i_dev)) {
				((unsigned short *) (bh->b_data))[block]=i;
				bh->b_dirt=1;
			}
		brelse(bh);
		//返回磁盘上新申请或原有的对应block的逻辑块号
		return i;
	}
	//若程序运行到此，则表明数据块属于二次间接块
	block -= 512;
	if (create && !inode->i_zone[8])
		if (inode->i_zone[8]=new_block(inode->i_dev)) {
			inode->i_dirt=1;
			inode->i_ctime=CURRENT_TIME;
		}
	if (!inode->i_zone[8])
		return 0;
	if (!(bh=bread(inode->i_dev,inode->i_zone[8])))
		return 0;
	i = ((unsigned short *)bh->b_data)[block>>9];
	if (create && !i)
		if (i=new_block(inode->i_dev)) {
			((unsigned short *) (bh->b_data))[block>>9]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	if (!i)
		return 0;
	if (!(bh=bread(inode->i_dev,i)))
		return 0;
	i = ((unsigned short *)bh->b_data)[block&511];
	if (create && !i)
		if (i=new_block(inode->i_dev)) {
			((unsigned short *) (bh->b_data))[block&511]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	return i;
}

//取文件数据块block在设备上对应的逻辑块号
//参数：inode-文件的内存i节点指针 block-文件中的数据块号
//若操作成功则返回对应的逻辑块号，否则返回0
int bmap(struct m_inode * inode,int block)
{
	return _bmap(inode,block,0);
}

//取文件数据块block在设备上对应的逻辑块号
int create_block(struct m_inode * inode, int block)
{
	return _bmap(inode,block,1);
}

//放回一个i节点(回写入设备)
//若是管道i节点，则唤醒等待的进程并递减引用计数
//若是块设备i节点则刷新设备
//若i节点的链接计数为0，则释放该i节点占用的所有磁盘逻辑块，并释放该i节点	
void iput(struct m_inode * inode)
{
	//首先判断参数给出的i节点的有效性，并等待inode节点解锁(如果已上锁的话)
	if (!inode)
		return;
	wait_on_inode(inode);
	//如果i节点的引用计数是0，表示该i节点已经是空闲的，则显示错误信息并停机
	if (!inode->i_count)
		panic("iput: trying to free free inode");
	//如果是管道i节点
	if (inode->i_pipe) {
		wake_up(&inode->i_wait);
		if (--inode->i_count)
			return;
		free_page(inode->i_size);
		inode->i_count=0;
		inode->i_dirt=0;
		inode->i_pipe=0;
		return;
	}
	//如果i节点对应的设备号=0，则将此节点的引用计数递减1，返回
	//例如用于管道操作的i节点，其i节点的设备号为0
	if (!inode->i_dev) {
		inode->i_count--;
		return;
	}
	//如果是块设备文件的i节点，此时逻辑块字段0(i_zone[0])中是设备号，则刷新该设备
	//并等待i节点解锁
	if (S_ISBLK(inode->i_mode)) {
		sync_dev(inode->i_zone[0]);
		wait_on_inode(inode);
	}
repeat:
	//如果i节点的引用计数大于1，则计数递减1后返回(因为该i节点还有人在用，不能释放)
	if (inode->i_count>1) {
		inode->i_count--;
		return;
	}
	//如果该i节点的链接数为0，则说明该文件被删除
	//于是释放该i节点的所有逻辑块，并释放该i节点
	if (!inode->i_nlinks) {
		truncate(inode);
		//用于实际释放i节点
		//即复位i节点对应的i节点位图比特位，清空i节点结构内容
		free_inode(inode);
		return;
	}
	//如果该i节点已作过修改，则回写更新该i节点，并等待该i节点解锁
	if (inode->i_dirt) {
		write_inode(inode);	/* we can sleep - so do again */
		wait_on_inode(inode);		//因为睡眠了，所以需要重新判断
		goto repeat;
	}
	//最后把i节点引用计数递减1，返回
	//此时该i节点的i_count=0表示已释放
	inode->i_count--;
	return;
}

//从i节点表(inode_table)中获取一个空闲i节点项
//寻找引用计数count为0的i节点，并将其写盘后清零，返回其指针。引用计数被置1
struct m_inode * get_empty_inode(void)
{
	struct m_inode * inode;
	static struct m_inode * last_inode = inode_table;		//指向i节点表第1项
	int i;

	do {
	//在初始化last_inode指针指向i节点表头一项后循环扫描整个i节点表
		inode = NULL;
		for (i = NR_INODE; i ; i--) {
			//如果last_inode已经指向i节点表的最后一项之后，则让其重新指向i节点表开始处，
			//以继续循环寻找空闲i节点项
			if (++last_inode >= inode_table + NR_INODE)
				last_inode = inode_table;
			//如果last_inode所指向的i节点的计数值为0，则说明可能找到空闲i节点项，让inode指向该节点
			//如果该i节点的已修改标志和锁定标志均为0，则我们可以使用该i节点，于是退出for循环
			if (!last_inode->i_count) {
				inode = last_inode;
				if (!inode->i_dirt && !inode->i_lock)
					break;
			}
		}
		//如果没有找到空闲i节点，则将i节点表打印出来供调试使用，并停机
		if (!inode) {
			for (i=0 ; i<NR_INODE ; i++)
				printk("%04x: %6d\t",inode_table[i].i_dev,
					inode_table[i].i_num);
			panic("No free inodes in mem");
		}
		//等待i节点解锁(如果又被上锁的话)
		wait_on_inode(inode);
		//如果该i节点已修改标志被置位的话，则将该i节点刷新(同步)
		//因为刷新时可能会睡眠，因此需要再次循环等待该i节点解锁
		while (inode->i_dirt) {
			write_inode(inode);
			wait_on_inode(inode);
		}
	//如果i节点又被其他占用的话(i节点的计数值不为0)，则重新寻找空闲i节点
	} while (inode->i_count);
	//已找到符合要求的空闲i节点
	//将该i节点项内容清零，并置引用计数为1，返回该i节点指针
	memset(inode,0,sizeof(*inode));
	inode->i_count = 1;
	return inode;
}

//获取管道节点
//首先扫描i节点表，寻找一个空闲i节点项，然后取得一页空闲内存供管道使用
//然后将得到的i节点的引用计数置为2(读者和写者)，初始化管道的头和尾，置i节点的管道类型表示
//返回i节点指针，如果失败则返回NULL
struct m_inode * get_pipe_inode(void)
{
	struct m_inode * inode;

	//首先从内存i节点表中取得一个空闲i节点
	if (!(inode = get_empty_inode()))
		return NULL;
	//然后为该i节点申请一页内存，并让节点的i_size字段指向该页面
	if (!(inode->i_size=get_free_page())) {
		inode->i_count = 0;
		return NULL;
	}
	//然后设置该i节点的引用计数为2，并复位管道头尾指针
	//i节点逻辑块号数组i_zone[]的i_zone[0]和i_zone[1]中分别用来存放管道头和管道尾指针
	inode->i_count = 2;	/* sum of readers/writers */
	PIPE_HEAD(*inode) = PIPE_TAIL(*inode) = 0;
	//最后设置i节点为管道i节点标志，并返回该i节点
	inode->i_pipe = 1;
	return inode;
}

//取得一个i节点
//参数：dev-设备号 nr-i节点号
//从设备上读取指定节点号的i节点到内存i节点表中，并返回该i节点指针
//首先从i节点表中搜寻，若找到指定节点号的i节点则立刻返回该i节点指针
//否则从设备dev上读取指定i节点的i节点信息放入i节点表中，并返回该i节点指针
struct m_inode * iget(int dev,int nr)
{
	struct m_inode * inode, * empty;

	//首先判断参数有效性
	if (!dev)
		panic("iget with dev==0");
	//预先从一个i节点表中取一个空闲i节点备用
	empty = get_empty_inode();
	//接着扫描i节点表，寻找参数指定及节点号nr的i节点
	inode = inode_table;
	while (inode < NR_INODE+inode_table) {
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode++;
			continue;
		}
		//如果找到指定设备号dev和节点号nr的i节点，则等待该节点解锁(如果上锁的话)
		wait_on_inode(inode);
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode = inode_table;
			continue;
		}
		//到这里表示你找到相应的i节点，于是将该i节点引用计数增1
		inode->i_count++;
		//如果该i节点是其他文件系统的安装点，则在超级块表中搜寻安装在此i节点的超级块
		if (inode->i_mount) {
			int i;

			for (i = 0 ; i<NR_SUPER ; i++)
				if (super_block[i].s_imount==inode)
					break;
			if (i >= NR_SUPER) {
				printk("Mounted inode hasn't got sb\n");
				if (empty)
					iput(empty);
				return inode;
			}
			iput(inode);
			dev = super_block[i].s_dev;
			nr = ROOT_INO;
			inode = inode_table;
			continue;
		}
		if (empty)
			iput(empty);
		return inode;
	}
	//如果我们在i节点表中没有找到指定的i节点，则利用前面申请的空闲i节点empty在i节点表中建立该i节点
	//并从相应设备上读取该i节点信息，返回该i节点指针
	if (!empty)
		return (NULL);
	inode=empty;
	inode->i_dev = dev;		//设置i节点的设备
	inode->i_num = nr;		//设置i节点号
	read_inode(inode);		//读取指定i节点信息
	return inode;
}

//读取指定i节点信息
//从设备中读取含有指定i节点信息的i节点盘块，然后复制到指定的i节点结构中
static void read_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	//首先锁定该i节点，并取该节点所在设备的超级块
	lock_inode(inode);
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to read inode without dev");
	//该i节点所在的设备逻辑块号=(启动块+超级块)+i节点位图所占的块数+逻辑块位图所占的块数+(i节点号-1)/每块含有i节点结构数
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;
	//从设备上读取该i节点所在的逻辑块，并复制指定i节点内容到inode指针所指位置处
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	*(struct d_inode *)inode =
		((struct d_inode *)bh->b_data)
			[(inode->i_num-1)%INODES_PER_BLOCK];
	//释放读入的缓冲块并解锁该i节点
	brelse(bh);
	unlock_inode(inode);
}

//将i节点信息写回缓冲区中
//该函数把参数指定的i节点写入缓冲区相应的缓冲块中,待缓冲区刷新时会写入盘中
static void write_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	if (!inode->i_dirt || !inode->i_dev) {
		unlock_inode(inode);
		return;
	}
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to write inode without device");
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	((struct d_inode *)bh->b_data)
		[(inode->i_num-1)%INODES_PER_BLOCK] =
			*(struct d_inode *)inode;
	bh->b_dirt=1;
	inode->i_dirt=0;
	brelse(bh);
	unlock_inode(inode);
}
