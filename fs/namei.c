/*
 *  linux/fs/namei.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * Some corrections by tytso.
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <const.h>
#include <sys/stat.h>

#define ACC_MODE(x) ("\004\002\006\377"[(x)&O_ACCMODE])

/*
 * comment out this line if you want names > NAME_LEN chars to be
 * truncated. Else they will be disallowed.
 */
/* #define NO_TRUNCATE */

#define MAY_EXEC 1
#define MAY_WRITE 2
#define MAY_READ 4

/*
 *	permission()
 *
 * is used to check for read/write/execute permissions on a file.
 * I don't know if we should look at just the euid or both euid and
 * uid, but that should be easily changed.
 */
static int permission(struct m_inode * inode,int mask)
{
	int mode = inode->i_mode;

/* special case: not even root can read/write a deleted file */
	if (inode->i_dev && !inode->i_nlinks)
		return 0;
	else if (current->euid==inode->i_uid)
		mode >>= 6;
	else if (current->egid==inode->i_gid)
		mode >>= 3;
	if (((mode & mask & 0007) == mask) || suser())
		return 1;
	return 0;
}

/*
 * ok, we cannot use strncmp, as the name is not in our data space.
 * Thus we'll have to use match. No big problem. Match also makes
 * some sanity tests.
 *
 * NOTE! unlike strncmp, match returns 1 for success, 0 for failure.
 */
//我们不能使用strncmp字符串比较函数，因为名称不在我们的数据空间(不在内核空间)
//指定长度字符串比较函数
static int match(int len,const char * name,struct dir_entry * de)
{
	register int same __asm__("ax");

	if (!de || !de->inode || len > NAME_LEN)
		return 0;
	if (len < NAME_LEN && de->name[len])
		return 0;
	//使用嵌入式汇编语句进行快速比较操作
	//它会在用户数据空间(fs段)执行字符串的比较操作
	__asm__("cld\n\t"			//清方向位
		"fs ; repe ; cmpsb\n\t"			//用户空间执行循环比较[esi+1]和[edi+1]
		"setz %%al"					//若结果一样(z=0)则设置al=1(same=eax)
		:"=a" (same)
		:"0" (0),"S" ((long) name),"D" ((long) de->name),"c" (len)
		:"cx","di","si");
	return same;
}

/*
 *	find_entry()
 *
 * finds an entry in the specified directory with the wanted name. It
 * returns the cache buffer in which the entry was found, and the entry
 * itself (as a parameter - res_dir). It does NOT read the inode of the
 * entry - you'll have to do that yourself if you want to.
 *
 * This also takes care of the few special cases due to '..'-traversal
 * over a pseudo-root and a mount point.
 */
//在指定目录中寻找到一个与名字匹配的目录项
//返回一个含有找到目录项的高速缓冲块以及目录项本身(作为一个参数res_dir)
static struct buffer_head * find_entry(struct m_inode ** dir,
	const char * name, int namelen, struct dir_entry ** res_dir)
{
	int entries;
	int block,i;
	struct buffer_head * bh;
	struct dir_entry * de;
	struct super_block * sb;

//对函数参数有效性进行判断和验证
#ifdef NO_TRUNCATE
	if (namelen > NAME_LEN)
		return NULL;
#else
	if (namelen > NAME_LEN)
		namelen = NAME_LEN;
#endif
	//首先计算本目录中目录项项数entries
	entries = (*dir)->i_size / (sizeof (struct dir_entry));
	*res_dir = NULL;
	if (!namelen)
		return NULL;
/* check for '..', as we might have to do some "magic" for it */
	//接下来对目录项文件名是''..'的情况进行特殊处理
	if (namelen==2 && get_fs_byte(name)=='.' && get_fs_byte(name+1)=='.') {
/* '..' in a pseudo-root results in a faked '.' (just change namelen) */
		if ((*dir) == current->root)
			namelen=1;
		else if ((*dir)->i_num == ROOT_INO) {
/* '..' over a mount-point results in 'dir' being exchanged for the mounted
   directory-inode. NOTE! We set mounted, so that we can iput the new dir */
			sb=get_super((*dir)->i_dev);
			if (sb->s_imount) {
				iput(*dir);
				(*dir)=sb->s_imount;
				(*dir)->i_count++;
			}
		}
	}
	//查找指定文件名的目录项在什么地方
	//因此我们需要读取目录的数据，即取出目录i节点对应块设备数据区中的数据块(逻辑块)信息
	//这些逻辑块的块号保存在i节点结构的i_zone[9]数组中，我们先取其中第1个块号
	if (!(block = (*dir)->i_zone[0]))
		return NULL;
	//从节点所在设备读取指定的目录项数据块
	if (!(bh = bread((*dir)->i_dev,block)))
		return NULL;
	//此时我们就在这个读取的目录i节点数据块中搜索匹配指定文件名的目录项
	//首先让de指向缓冲块中的数据块部分，并在不超过目录中目录项数的条件下循环执行搜索
	i = 0;
	de = (struct dir_entry *) bh->b_data;
	while (i < entries) {
		//如果当前目录项数据已经搜索完，还没有找到匹配的目录项，则释放当前目录项数据块
		if ((char *)de >= BLOCK_SIZE+bh->b_data) {
			brelse(bh);
			bh = NULL;
			//再读入目录的下一个逻辑块
			if (!(block = bmap(*dir,i/DIR_ENTRIES_PER_BLOCK)) ||
			    !(bh = bread((*dir)->i_dev,block))) {
				i += DIR_ENTRIES_PER_BLOCK;
				continue;
			}
			de = (struct dir_entry *) bh->b_data;
		}
		//如果找到匹配的目录项的话，则返回该目录项结构指针de和该目录项i节点指针*dir以及目录项数据块指针bh
		if (match(namelen,name,de)) {
			*res_dir = de;
			return bh;
		}
		de++;
		i++;
	}
	//如果指定目录中的所有目录项都搜索完后还没有找到相应的目录项，则释放目录的数据块
	brelse(bh);
	return NULL;
}

/*
 *	add_entry()
 *
 * adds a file entry to the specified directory, using the same
 * semantics as find_entry(). It returns NULL if it failed.
 *
 * NOTE!! The inode part of 'de' is left at 0 - which means you
 * may not sleep between calling this and putting something into
 * the entry, as someone else might have used it while you slept.
 */
//根据指定的目录和文件名添加目录项
//参数：dir-指定目录的i节点；name-文件名；namelen-文件名长度
//返回：高速缓冲区指针；res_dir-返回的目录项结构指针
static struct buffer_head * add_entry(struct m_inode * dir,
	const char * name, int namelen, struct dir_entry ** res_dir)
{
	int block,i;
	struct buffer_head * bh;
	struct dir_entry * de;

	//对函数参数的有效性进行判断和验证
	*res_dir = NULL;
#ifdef NO_TRUNCATE
	if (namelen > NAME_LEN)
		return NULL;
#else
	if (namelen > NAME_LEN)
		namelen = NAME_LEN;
#endif
	//向指定目录中添加一个指定文件名的目录项
	//先读取目录的数据，即取出目录i节点对应块设备数据取中的数据块信息
	if (!namelen)
		return NULL;
	if (!(block = dir->i_zone[0]))
		return NULL;
	if (!(bh = bread(dir->i_dev,block)))
		return NULL;
	//此时我们就在这个目录i节点数据块中循环查找未使用的空目录项
	//首先让目录项结构指针de指向缓冲块中的数据块部分，即第一个目录项处
	//其中i是目录中的目录项索引号，在循环开始时初始化为0
	i = 0;
	de = (struct dir_entry *) bh->b_data;
	while (1) {
		//如果当前目录项数据块已经搜索完毕，但还没有找到需要的空目录项，
		//则释放当前目录项数据块，再读入目录的下一个逻辑块
		if ((char *)de >= BLOCK_SIZE+bh->b_data) {
			brelse(bh);
			bh = NULL;
			block = create_block(dir,i/DIR_ENTRIES_PER_BLOCK);
			if (!block)
				return NULL;
			if (!(bh = bread(dir->i_dev,block))) {
				i += DIR_ENTRIES_PER_BLOCK;
				continue;
			}
			de = (struct dir_entry *) bh->b_data;
		}
		if (i*sizeof(struct dir_entry) >= dir->i_size) {
			de->inode=0;
			dir->i_size = (i+1)*sizeof(struct dir_entry);
			dir->i_dirt = 1;
			dir->i_ctime = CURRENT_TIME;
		}
		//若当前搜索的目录项de的i节点为空，则表示找到一个还未使用的空闲目录项或者是添加的新目录项
		if (!de->inode) {
			//于是更新目录的修改时间为当前时间
			dir->i_mtime = CURRENT_TIME;
			//并从用户数据区复制文件名到该目录项的文件名字段
			for (i=0; i < NAME_LEN ; i++)
				de->name[i]=(i<namelen)?get_fs_byte(name+i):0;
			//置含有本目录项的相应高速缓冲块已修改标志
			bh->b_dirt = 1;
			//返回该目录项的指针以及该高速缓冲块的指针
			*res_dir = de;
			return bh;
		}
		//如果该目录项已经被使用，则继续检测下一个目录项
		de++;
		i++;
	}
	brelse(bh);
	return NULL;
}

/*
 *	get_dir()
 *
 * Getdir traverses the pathname until it hits the topmost directory.
 * It returns NULL on failure.
 */
//该函数根据给出的路径名进行搜索，直到达到最顶端的目录。如果失败返回NULL
//返回目录或文件的i节点指针
static struct m_inode * get_dir(const char * pathname)
{
	char c;
	const char * thisname;
	struct m_inode * inode;
	struct buffer_head * bh;
	int namelen,inr,idev;
	struct dir_entry * de;

	//搜索操作会从当前进程任务结构中设置的根i节点或当前工作目录i节点开始
	if (!current->root || !current->root->i_count)		//当前进程的根i节点不存在或引用计数为0，则死机
		panic("No root inode");
	if (!current->pwd || !current->pwd->i_count)		//当前进程的当前工作目录根i节点不存在或引用计数为0，则死机
		panic("No cwd inode");
	//如果用户指定的路径名的第1个字符是'/'，则说明路径名是绝对路径名
	if ((c=get_fs_byte(pathname))=='/') {
		inode = current->root;
		pathname++;
	//否则若第一个字符是其他字符，则表示给定的是相对路径名，应从进程的当前工作目录开始操作
	} else if (c)
		inode = current->pwd;
	else
		return NULL;	/* empty name is bad */
	//i节点引用计数增1
	inode->i_count++;
	//然后针对路径名中的各个目录名部分和文件名进行循环处理
	while (1) {
		thisname = pathname;
		//先对当前正在处理的目录名部分(或文件名)的i节点进行有效性判断
		if (!S_ISDIR(inode->i_mode) || !permission(inode,MAY_EXEC)) {
			iput(inode);
			return NULL;
		}
		//每当检索到字符串中的'/'字符或者c为'\0'，循环都会跳出
		for(namelen=0;(c=get_fs_byte(pathname++))&&(c!='/');namelen++)
			/* nothing */ ;
		if (!c)
			return inode;
		//在当前处理的目录中寻找指定名称的目录项
		if (!(bh = find_entry(&inode,thisname,namelen,&de))) {
			iput(inode);
			return NULL;
		}
		//在找到的目录项中取出其i节点号inr和设备号idev
		inr = de->inode;
		idev = inode->i_dev;
		//释放包含该目录项的高速缓冲块并放回该i节点
		brelse(bh);
		iput(inode);
		//然后取节点号的nr的i节点inode，并以该目录项为当前目录继续循环处理路径名中的下一目录名部分(或文件名)
		if (!(inode = iget(idev,inr)))
			return NULL;
	}
}

/*
 *	dir_namei()
 *
 * dir_namei() returns the inode of the directory of the
 * specified name, and the name within that directory.
 */
//函数返回指定目录名的i节点指针以及在最顶层目录的名称
static struct m_inode * dir_namei(const char * pathname,
	int * namelen, const char ** name)
{
	char c;
	const char * basename;
	struct m_inode * dir;

	//首先取得指定路径名最顶层目录的i节点
	if (!(dir = get_dir(pathname)))
		return NULL;
	basename = pathname;
	//逐个遍历/dev/tty0字符串，每次循环都将一个字符复制给c，直到字符串结束
	while (c=get_fs_byte(pathname++))
		if (c=='/')
			basename=pathname;
	*namelen = pathname-basename-1;		//确定tty0名字的长度
	*name = basename;		//得到tty0中第一个't'字符的地址
	return dir;		
}

/*
 *	namei()
 *
 * is used by most simple commands to get the inode of a specified name.
 * Open, link etc use their own routines, but this is enough for things
 * like 'chmod' etc.
 */
//取指定路径名的i节点
//参数：pathname-路径名
//返回：对应的i节点
struct m_inode * namei(const char * pathname)
{
	const char * basename;
	int inr,dev,namelen;
	struct m_inode * dir;
	struct buffer_head * bh;
	struct dir_entry * de;

	//首先查找指定路径的最顶层目录的目录名并得到其i节点，若不存在则返回NULL退出
	if (!(dir = dir_namei(pathname,&namelen,&basename)))
		return NULL;
	//如果返回的最顶层名字的长度是0，则表示该路径名以一个目录名为最后一项
	//因此我们已经找到对应目录的i节点，可以直接返回该i节点退出
	if (!namelen)			/* special case: '/usr/' etc */
		return dir;
	//然后在返回的顶层目录中寻找指定文件名目录项的i节点
	bh = find_entry(&dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		return NULL;
	}
	//接着取该目录项的i节点号和设备号，并释放包含该目录项的高速缓冲块并放回目录i节点
	inr = de->inode;
	dev = dir->i_dev;
	brelse(bh);
	iput(dir);
	//然后取对应节点号的i节点
	dir=iget(dev,inr);
	//修改其被访问时间为当前时间并置已修改标志
	if (dir) {
		dir->i_atime=CURRENT_TIME;
		dir->i_dirt=1;
	}
	//最后返回该i节点
	return dir;
}

/*
 *	open_namei()
 *
 * namei for open - this is in fact almost the whole open-routine.
 */
//open()函数使用的namei函数-这其实几乎是完整的打开文件程序
//res_inode-返回对应文件路径名的i节点指针
int open_namei(const char * pathname, int flag, int mode,
	struct m_inode ** res_inode)
{
	const char * basename;
	int inr,dev,namelen;
	struct m_inode * dir, *inode;
	struct buffer_head * bh;
	struct dir_entry * de;

	//首先对函数参数进行合理的处理
	if ((flag & O_TRUNC) && !(flag & O_ACCMODE))
		flag |= O_WRONLY;
	mode &= 0777 & ~current->umask;
	mode |= I_REGULAR;
	//然后根据指定的路径名寻找到对应的i节点以及最顶端目录名及其长度
	if (!(dir = dir_namei(pathname,&namelen,&basename)))
		return -ENOENT;
	//此时如果最顶端目录名长度为0(例如'/usr/'这种路径名的情况)
	//那么若操作不是读写、创建和文件长度截0，则表示是在打开一个目录名文件操作，
	//于是直接返回该目录的i节点并返回0退出
	if (!namelen) {			/* special case: '/usr/' etc */
		if (!(flag & (O_ACCMODE|O_CREAT|O_TRUNC))) {
			*res_inode=dir;
			return 0;
		}
		//否则说明进程操作非法，于是放回i节点并返回出错码
		iput(dir);
		return -EISDIR;
	}
	//接着根据上面得到的最顶层目录名的i节点dir，在其中查找取得路径名字字符串中最后的
	//文件名对应的目录项结构de，并同时得到该目录所在的高速缓冲区指针
	bh = find_entry(&dir,basename,namelen,&de);
	//如果该高速缓冲指针为NULL，则表示没有找到对应文件名的目录项，因此只可能是创建文件操作
	if (!bh) {
		//如果不是创建文件，则放回该目录的i节点，返回出错号退出
		if (!(flag & O_CREAT)) {
			iput(dir);
			return -ENOENT;
		}
		//如果用户在该目录没有写的权力，则放回该目录的i节点，返回出错号退出
		if (!permission(dir,MAY_WRITE)) {
			iput(dir);
			return -EACCES;
		}
		//现在确定是创建文件并有写操作许可
		//在目录i节点对应设备上申请一个新的i节点给路径名上指定的文件使用
		inode = new_inode(dir->i_dev);
		if (!inode) {
			iput(dir);
			return -ENOSPC;
		}
		//对新的i节点进行初始设置
		inode->i_uid = current->euid;
		inode->i_mode = mode;
		inode->i_dirt = 1;
		//然后在指定目录dir中添加一个新目录项
		bh = add_entry(dir,basename,namelen,&de);
		//如果添加目录项操作失败
		if (!bh) {
			inode->i_nlinks--;
			iput(inode);
			iput(dir);
			return -ENOSPC;
		}
		//说明添加目录项操作成功，设置新目录项的初始值
		//置目录项i节点为新申请到的i节点的号码
		de->inode = inode->i_num;
		bh->b_dirt = 1;
		//释放该高速缓冲区并放回目录的i节点
		brelse(bh);
		iput(dir);
		//返回新目录的i节点指针
		*res_inode = inode;
		return 0;
	}
	inr = de->inode;		//得到i节点号
	dev = dir->i_dev;		//得到虚拟盘的设备号
	//释放该高速缓冲区并放回目录的i节点
	brelse(bh);
	iput(dir);
	if (flag & O_EXCL)
		return -EEXIST;
	//然后我们读取该目录项的i节点内容
	if (!(inode=iget(dev,inr)))
		return -EACCES;
	if ((S_ISDIR(inode->i_mode) && (flag & O_ACCMODE)) ||
	    !permission(inode,ACC_MODE(flag))) {
		iput(inode);
		return -EPERM;
	}
	//接着我们更新该i节点的访问时间字段值为当前时间
	inode->i_atime = CURRENT_TIME;
	if (flag & O_TRUNC)
		truncate(inode);
	//返回该目录项i节点的指针
	*res_inode = inode;
	return 0;
}

int sys_mknod(const char * filename, int mode, int dev)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh;
	struct dir_entry * de;
	
	if (!suser())
		return -EPERM;
	if (!(dir = dir_namei(filename,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EPERM;
	}
	bh = find_entry(&dir,basename,namelen,&de);
	if (bh) {
		brelse(bh);
		iput(dir);
		return -EEXIST;
	}
	inode = new_inode(dir->i_dev);
	if (!inode) {
		iput(dir);
		return -ENOSPC;
	}
	inode->i_mode = mode;
	if (S_ISBLK(mode) || S_ISCHR(mode))
		inode->i_zone[0] = dev;
	inode->i_mtime = inode->i_atime = CURRENT_TIME;
	inode->i_dirt = 1;
	bh = add_entry(dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		inode->i_nlinks=0;
		iput(inode);
		return -ENOSPC;
	}
	de->inode = inode->i_num;
	bh->b_dirt = 1;
	iput(dir);
	iput(inode);
	brelse(bh);
	return 0;
}

int sys_mkdir(const char * pathname, int mode)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh, *dir_block;
	struct dir_entry * de;

	if (!suser())
		return -EPERM;
	if (!(dir = dir_namei(pathname,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EPERM;
	}
	bh = find_entry(&dir,basename,namelen,&de);
	if (bh) {
		brelse(bh);
		iput(dir);
		return -EEXIST;
	}
	inode = new_inode(dir->i_dev);
	if (!inode) {
		iput(dir);
		return -ENOSPC;
	}
	inode->i_size = 32;
	inode->i_dirt = 1;
	inode->i_mtime = inode->i_atime = CURRENT_TIME;
	if (!(inode->i_zone[0]=new_block(inode->i_dev))) {
		iput(dir);
		inode->i_nlinks--;
		iput(inode);
		return -ENOSPC;
	}
	inode->i_dirt = 1;
	if (!(dir_block=bread(inode->i_dev,inode->i_zone[0]))) {
		iput(dir);
		free_block(inode->i_dev,inode->i_zone[0]);
		inode->i_nlinks--;
		iput(inode);
		return -ERROR;
	}
	de = (struct dir_entry *) dir_block->b_data;
	de->inode=inode->i_num;
	strcpy(de->name,".");
	de++;
	de->inode = dir->i_num;
	strcpy(de->name,"..");
	inode->i_nlinks = 2;
	dir_block->b_dirt = 1;
	brelse(dir_block);
	inode->i_mode = I_DIRECTORY | (mode & 0777 & ~current->umask);
	inode->i_dirt = 1;
	bh = add_entry(dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		free_block(inode->i_dev,inode->i_zone[0]);
		inode->i_nlinks=0;
		iput(inode);
		return -ENOSPC;
	}
	de->inode = inode->i_num;
	bh->b_dirt = 1;
	dir->i_nlinks++;
	dir->i_dirt = 1;
	iput(dir);
	iput(inode);
	brelse(bh);
	return 0;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
static int empty_dir(struct m_inode * inode)
{
	int nr,block;
	int len;
	struct buffer_head * bh;
	struct dir_entry * de;

	len = inode->i_size / sizeof (struct dir_entry);
	if (len<2 || !inode->i_zone[0] ||
	    !(bh=bread(inode->i_dev,inode->i_zone[0]))) {
	    	printk("warning - bad directory on dev %04x\n",inode->i_dev);
		return 0;
	}
	de = (struct dir_entry *) bh->b_data;
	if (de[0].inode != inode->i_num || !de[1].inode || 
	    strcmp(".",de[0].name) || strcmp("..",de[1].name)) {
	    	printk("warning - bad directory on dev %04x\n",inode->i_dev);
		return 0;
	}
	nr = 2;
	de += 2;
	while (nr<len) {
		if ((void *) de >= (void *) (bh->b_data+BLOCK_SIZE)) {
			brelse(bh);
			block=bmap(inode,nr/DIR_ENTRIES_PER_BLOCK);
			if (!block) {
				nr += DIR_ENTRIES_PER_BLOCK;
				continue;
			}
			if (!(bh=bread(inode->i_dev,block)))
				return 0;
			de = (struct dir_entry *) bh->b_data;
		}
		if (de->inode) {
			brelse(bh);
			return 0;
		}
		de++;
		nr++;
	}
	brelse(bh);
	return 1;
}

int sys_rmdir(const char * name)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh;
	struct dir_entry * de;

	if (!suser())
		return -EPERM;
	if (!(dir = dir_namei(name,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EPERM;
	}
	bh = find_entry(&dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		return -ENOENT;
	}
	if (!(inode = iget(dir->i_dev, de->inode))) {
		iput(dir);
		brelse(bh);
		return -EPERM;
	}
	if ((dir->i_mode & S_ISVTX) && current->euid &&
	    inode->i_uid != current->euid) {
		iput(dir);
		iput(inode);
		brelse(bh);
		return -EPERM;
	}
	if (inode->i_dev != dir->i_dev || inode->i_count>1) {
		iput(dir);
		iput(inode);
		brelse(bh);
		return -EPERM;
	}
	if (inode == dir) {	/* we may not delete ".", but "../dir" is ok */
		iput(inode);
		iput(dir);
		brelse(bh);
		return -EPERM;
	}
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		iput(dir);
		brelse(bh);
		return -ENOTDIR;
	}
	if (!empty_dir(inode)) {
		iput(inode);
		iput(dir);
		brelse(bh);
		return -ENOTEMPTY;
	}
	if (inode->i_nlinks != 2)
		printk("empty directory has nlink!=2 (%d)",inode->i_nlinks);
	de->inode = 0;
	bh->b_dirt = 1;
	brelse(bh);
	inode->i_nlinks=0;
	inode->i_dirt=1;
	dir->i_nlinks--;
	dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	dir->i_dirt=1;
	iput(dir);
	iput(inode);
	return 0;
}

//删除文件名对应的目录项
//从文件系统删除一个名字,如果是文件的最后一个链接,并且美誉进程正打开该文件
//则该文件也将被删除,并释放所占用的设备空间
int sys_unlink(const char * name)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh;
	struct dir_entry * de;

	//首先检查参数的有效性并取路径名中顶层目录的i节点
	if (!(dir = dir_namei(name,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EPERM;
	}
	//然后根据指定目录的i节点和目录名利用函数find_entry()寻找对应目录项
	//再根据该目录项de的i节点号利用iget()函数得到对应的i节点node
	bh = find_entry(&dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		return -ENOENT;
	}
	if (!(inode = iget(dir->i_dev, de->inode))) {
		iput(dir);
		brelse(bh);
		return -ENOENT;
	}
	//检查当前进程是否有权限删除该目录
	if ((dir->i_mode & S_ISVTX) && !suser() &&
	    current->euid != inode->i_uid &&
	    current->euid != dir->i_uid) {
		iput(dir);
		iput(inode);
		brelse(bh);
		return -EPERM;
	}
	//如果该文件名是一个目录,则也不能删除
	if (S_ISDIR(inode->i_mode)) {
		iput(inode);
		iput(dir);
		brelse(bh);
		return -EPERM;
	}
	//如果该i节点的链接计数值已经为0,则显示警告信息,并修正其为1
	if (!inode->i_nlinks) {
		printk("Deleting nonexistent file (%04x:%d), %d\n",
			inode->i_dev,inode->i_num,inode->i_nlinks);
		inode->i_nlinks=1;
	}
	//现在我们可以删除文件名目录项了
	//将文件名目录项中的i节点号字段置0,表示释放该目录项 
	//并设置包含该目录项的缓冲块已修改标志,释放该高速缓冲块
	de->inode = 0;
	bh->b_dirt = 1;
	brelse(bh);
	inode->i_nlinks--;
	inode->i_dirt = 1;
	inode->i_ctime = CURRENT_TIME;
	iput(inode);
	iput(dir);
	return 0;
}

int sys_link(const char * oldname, const char * newname)
{
	struct dir_entry * de;
	struct m_inode * oldinode, * dir;
	struct buffer_head * bh;
	const char * basename;
	int namelen;

	oldinode=namei(oldname);
	if (!oldinode)
		return -ENOENT;
	if (S_ISDIR(oldinode->i_mode)) {
		iput(oldinode);
		return -EPERM;
	}
	dir = dir_namei(newname,&namelen,&basename);
	if (!dir) {
		iput(oldinode);
		return -EACCES;
	}
	if (!namelen) {
		iput(oldinode);
		iput(dir);
		return -EPERM;
	}
	if (dir->i_dev != oldinode->i_dev) {
		iput(dir);
		iput(oldinode);
		return -EXDEV;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		iput(oldinode);
		return -EACCES;
	}
	bh = find_entry(&dir,basename,namelen,&de);
	if (bh) {
		brelse(bh);
		iput(dir);
		iput(oldinode);
		return -EEXIST;
	}
	bh = add_entry(dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		iput(oldinode);
		return -ENOSPC;
	}
	de->inode = oldinode->i_num;
	bh->b_dirt = 1;
	brelse(bh);
	iput(dir);
	oldinode->i_nlinks++;
	oldinode->i_ctime = CURRENT_TIME;
	oldinode->i_dirt = 1;
	iput(oldinode);
	return 0;
}
