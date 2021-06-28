/*
 *  linux/fs/bitmap.c
 *
 *  (C) 1991  Linus Torvalds
 */

/* bitmap.c contains the code that handles the inode and block bitmaps */
#include <string.h>

#include <linux/sched.h>
#include <linux/kernel.h>

#define clear_block(addr) \
__asm__("cld\n\t" \
	"rep\n\t" \
	"stosl" \
	::"a" (0),"c" (BLOCK_SIZE/4),"D" ((long) (addr)):"cx","di")

//把指定地址开始的第nr个位偏移处的比特位置位
#define set_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__ __volatile__("btsl %2,%3\n\tsetb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

#define clear_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__ __volatile__("btrl %2,%3\n\tsetnb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

#define find_first_zero(addr) ({ \
int __res; \
__asm__("cld\n" \
	"1:\tlodsl\n\t" \
	"notl %%eax\n\t" \
	"bsfl %%eax,%%edx\n\t" \
	"je 2f\n\t" \
	"addl %%edx,%%ecx\n\t" \
	"jmp 3f\n" \
	"2:\taddl $32,%%ecx\n\t" \
	"cmpl $8192,%%ecx\n\t" \
	"jl 1b\n" \
	"3:" \
	:"=c" (__res):"c" (0),"S" (addr):"ax","dx","si"); \
__res;})

//释放设备dev上数据区中的逻辑块block
void free_block(int dev, int block)
{
	struct super_block * sb;
	struct buffer_head * bh;

	if (!(sb = get_super(dev)))
		panic("trying to free block on nonexistent device");
	if (block < sb->s_firstdatazone || block >= sb->s_nzones)
		panic("trying to free block not in datazone");
	bh = get_hash_table(dev,block);
	if (bh) {
		if (bh->b_count != 1) {
			printk("trying to free block (%04x:%d), count=%d\n",
				dev,block,bh->b_count);
			return;
		}
		bh->b_dirt=0;
		bh->b_uptodate=0;
		brelse(bh);
	}
	//接着复位block在逻辑块位图中的比特位(置0)
	block -= sb->s_firstdatazone - 1 ;
	if (clear_bit(block&8191,sb->s_zmap[block/8192]->b_data)) {
		printk("block (%04x:%d) ",dev,block+sb->s_firstdatazone-1);
		panic("free_block: bit already cleared");
	}
	sb->s_zmap[block/8192]->b_dirt = 1;
}

//向设备申请一个逻辑块号
//函数首先取得设备的超级块,并在超级块中的逻辑块位图中寻找第一个0值比特位(代表一个空闲逻辑块)
//然后置位对应逻辑块在逻辑块位图中的比特位,接着从设备上读取该逻辑块到高速缓冲区中
//最后将新逻辑块清零,并设置其已更新标志和已修改标志,并返回逻辑块号
//函数执行成功则返回逻辑块号,否则返回0
int new_block(int dev)
{
	struct buffer_head * bh;
	struct super_block * sb;
	int i,j;

	//首先获取设备dev的超级块
	if (!(sb = get_super(dev)))
		panic("trying to get new block from nonexistant device");
	//然后扫描文件系统的 8块逻辑块位图,寻找第1个0值位,以寻找空闲逻辑块
	j = 8192;
	for (i=0 ; i<8 ; i++)
		if (bh=sb->s_zmap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	if (i>=8 || !bh || j>=8192)
		return 0;
	//接着设置找到的新逻辑块j对应逻辑块位图中的比特位
	if (set_bit(j,bh->b_data))
		panic("new_block: bit already set");
	bh->b_dirt = 1;
	j += i*8192 + sb->s_firstdatazone-1;
	if (j >= sb->s_nzones)
		return 0;
	//然后在高速缓冲区中为该设备上指定的逻辑块号取得一个缓冲块,并返回缓冲块头指针
	if (!(bh=getblk(dev,j)))
		panic("new_block: cannot get block");
	if (bh->b_count != 1)
		panic("new block: count is != 1");
	//最后将新逻辑块清零
	clear_block(bh->b_data);
	bh->b_uptodate = 1;
	bh->b_dirt = 1;
	brelse(bh);
	return j;
}

//释放指定的i节点
void free_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;

	//首先判断指定i节点有效性
	if (!inode)
		return;
	if (!inode->i_dev) {
		memset(inode,0,sizeof(*inode));
		return;
	}
	//如果i节点还有其他程序引用,则不能释放
	if (inode->i_count>1) {
		printk("trying to free inode with count=%d\n",inode->i_count);
		panic("free_inode");
	}
	//如果文件链接数不为0,则表示还有其他文件目录项在使用该节点,因此也不能释放
	if (inode->i_nlinks)
		panic("trying to free inode with links");
	//利用超级快信息对其中的i节点位图进行操作
	if (!(sb = get_super(inode->i_dev)))
		panic("trying to free inode on nonexistent device");
	if (inode->i_num < 1 || inode->i_num > sb->s_ninodes)
		panic("trying to free inode 0 or nonexistant inode");
	if (!(bh=sb->s_imap[inode->i_num>>13]))
		panic("nonexistent imap in superblock");
	//复位i节点对应的节点位图中的比特位
	if (clear_bit(inode->i_num&8191,bh->b_data))
		printk("free_inode: bit already cleared.\n\r");
	bh->b_dirt = 1;
	//清空i节点结构所占的内存区
	memset(inode,0,sizeof(*inode));
}

//为设备dev建立一个新i节点，初始化并返回该新i节点的指针
//在内存i节点表中获取一个空闲i节点表项，并从i节点位图中找一个空闲i节点
struct m_inode * new_inode(int dev)
{
	struct m_inode * inode;
	struct super_block * sb;
	struct buffer_head * bh;
	int i,j;

	//首先从内存i节点表(inode_table)中获取一个空闲i节点项，并读取指定设备的超级块
	if (!(inode=get_empty_inode()))
		return NULL;
	if (!(sb = get_super(dev)))
		panic("new_inode with unknown device");
	//然后扫描超级块中8块i节点 位图，寻找第一个0位，
	//寻找空闲节点，获取放置该i节点的节点号
	j = 8192;
	for (i=0 ; i<8 ; i++)
		if (bh=sb->s_imap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	if (!bh || j >= 8192 || j+i*8192 > sb->s_ninodes) {
		iput(inode);
		return NULL;
	}
	//现在已经找到了还未使用的i节点号j
	//于是置位i节点j对应的i节点位图相应比特位
	if (set_bit(j,bh->b_data))
		panic("new_inode: bit already set");
	//置i节点位图所在缓冲块已修改标志
	bh->b_dirt = 1;
	//最后初始化该i节点结构
	inode->i_count=1;
	inode->i_nlinks=1;
	inode->i_dev=dev;
	inode->i_uid=current->euid;
	inode->i_gid=current->egid;
	inode->i_dirt=1;
	inode->i_num = j + i*8192;		//对应设备的i节点号
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	return inode;
}
