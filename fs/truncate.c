/*
 *  linux/fs/truncate.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <linux/sched.h>

#include <sys/stat.h>

//释放所有一次间接块
static void free_ind(int dev,int block)
{
	struct buffer_head * bh;
	unsigned short * p;
	int i;

	//首先判断参数的有效性
	if (!block)
		return;
	if (bh=bread(dev,block)) {
		p = (unsigned short *) bh->b_data;
		for (i=0;i<512;i++,p++)
			if (*p)
				free_block(dev,*p);
		brelse(bh);
	}
	free_block(dev,block);
}

static void free_dind(int dev,int block)
{
	struct buffer_head * bh;
	unsigned short * p;
	int i;

	if (!block)
		return;
	if (bh=bread(dev,block)) {
		p = (unsigned short *) bh->b_data;
		for (i=0;i<512;i++,p++)
			if (*p)
				free_ind(dev,*p);
		brelse(bh);
	}
	free_block(dev,block);
}

//截断文件数据函数
//将节点对应的文件长度截为0,并释放占用的设备空间
void truncate(struct m_inode * inode)
{
	int i;

	//首先判断指定i节点有效性
	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode)))
		return;
	//然后释放i节点7个直接逻辑块,并将这个逻辑块项全置0
	for (i=0;i<7;i++)
		if (inode->i_zone[i]) {
			free_block(inode->i_dev,inode->i_zone[i]);
			inode->i_zone[i]=0;
		}
	//将一级间接块自身占用的逻辑块以及它管理的逻辑块在逻辑位图上对应的位清零
	free_ind(inode->i_dev,inode->i_zone[7]);
	//将二级间接块自身占用的逻辑块以及它管理的逻辑块在逻辑位图上对应的位清零
	free_dind(inode->i_dev,inode->i_zone[8]);
	inode->i_zone[7] = inode->i_zone[8] = 0;		//逻辑块项7 8置零
	inode->i_size = 0;		//文件大小置零
	inode->i_dirt = 1;		//置节点已修改标志
	inode->i_mtime = inode->i_ctime = CURRENT_TIME;
}

