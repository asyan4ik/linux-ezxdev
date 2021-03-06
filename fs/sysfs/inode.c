/*
 * sysfs.c - The filesystem to export kernel objects.
 *
 * Copyright (c) 2001, 2002 Patrick Mochel
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * This is a simple, ramfs-based filesystem, designed to export kernel 
 * objects, their attributes, and the linkgaes between each other.
 *
 * Please see Documentation/filesystems/sysfs.txt for more information.
 */

#undef DEBUG 

#include <linux/list.h>
#include <linux/init.h>
#include <linux/pagemap.h>
#include <linux/stat.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#if 0 /* linux-pm */
#include <linux/namei.h>
#endif
#include <linux/module.h>
#include <linux/slab.h>
#if 0
#include <linux/backing-dev.h>
#endif
#if 1 /* linux-pm */
#include <linux/device.h>
#endif /* linux-pm */
#include <linux/kobject.h>
#include <linux/mount.h>
#include <asm/uaccess.h>

/* Random magic number */
#define SYSFS_MAGIC 0x62656572

static struct super_operations sysfs_ops;
static struct file_operations sysfs_file_operations;
static struct address_space_operations sysfs_aops;

static struct vfsmount *sysfs_mount;

#if 0 /* linux-pm */
static struct backing_dev_info sysfs_backing_dev_info = {
	.ra_pages	= 0,	/* No readahead */
	.memory_backed	= 1,	/* Does not contribute to dirty memory */
};
#endif /* linux-pm */

#if 1 /* linux-pm */

#define PageUptodate(page) Page_Uptodate(page)

/* From include/linux/list.h */

/**
 * list_for_each_entry  -       iterate over list of given type
 * @pos:        the type * to use as a loop counter.
 * @head:       the head for your list.
 * @member:     the name of the list_struct within the struct.
 */
#define list_for_each_entry(pos, head, member)                          \
        for (pos = list_entry((head)->next, typeof(*pos), member),      \
                     prefetch(pos->member.next);                        \
             &pos->member != (head);                                    \
             pos = list_entry(pos->member.next, typeof(*pos), member),  \
                     prefetch(pos->member.next))

/*
 * The following from fs/libfs.c .
 */

static struct dentry *simple_lookup(struct inode *dir, struct dentry *dentry)
{
	d_add(dentry, NULL);
	return NULL;
}

static int simple_statfs(struct super_block *sb, struct statfs *buf)
{
	buf->f_type = sb->s_magic;
	buf->f_bsize = PAGE_CACHE_SIZE;
	buf->f_namelen = NAME_MAX;
	return 0;
}

int simple_readpage(struct file *file, struct page *page)
{
        void *kaddr;

        if (PageUptodate(page))
                goto out;

        kaddr = kmap_atomic(page, KM_USER0);
        memset(kaddr, 0, PAGE_CACHE_SIZE);
        kunmap_atomic(kaddr, KM_USER0);
        flush_dcache_page(page);
        SetPageUptodate(page);
out:
        unlock_page(page);
        return 0;
}

int simple_prepare_write(struct file *file, struct page *page,
                        unsigned from, unsigned to)
{
        if (!PageUptodate(page)) {
                if (to - from != PAGE_CACHE_SIZE) {
                        void *kaddr = kmap_atomic(page, KM_USER0);
                        memset(kaddr, 0, from);
                        memset(kaddr + to, 0, PAGE_CACHE_SIZE - to);
                        flush_dcache_page(page);
                        kunmap_atomic(kaddr, KM_USER0);
                }
                SetPageUptodate(page);
        }
        return 0;
}

int simple_commit_write(struct file *file, struct page *page,
                        unsigned offset, unsigned to)
{
        struct inode *inode = page->mapping->host;
        loff_t pos = ((loff_t)page->index << PAGE_CACHE_SHIFT) + to;

        if (pos > inode->i_size)
                inode->i_size = pos;
        set_page_dirty(page);
        return 0;
}

static inline int simple_positive(struct dentry *dentry)
{
        return dentry->d_inode && !d_unhashed(dentry);
}

int simple_empty(struct dentry *dentry)
{
        struct dentry *child;
        int ret = 0;

        spin_lock(&dcache_lock);
        list_for_each_entry(child, &dentry->d_subdirs, d_child)
                if (simple_positive(child))
                        goto out;
        ret = 1;
out:
        spin_unlock(&dcache_lock);
        return ret;
}

int simple_unlink(struct inode *dir, struct dentry *dentry)
{
        struct inode *inode = dentry->d_inode;

        inode->i_nlink--;
        dput(dentry);
        return 0;
}

int simple_rmdir(struct inode *dir, struct dentry *dentry)
{
        if (!simple_empty(dentry))
                return -ENOTEMPTY;

        dentry->d_inode->i_nlink--;
        simple_unlink(dir, dentry);
        dir->i_nlink--;
        return 0;
}

static struct file_operations simple_dir_operations = {
	read:		generic_read_dir,
	readdir:	dcache_readdir,
	/* Need the following for 2.4.20 (not 2.4.18). */
        open:           dcache_dir_open,
        release:        dcache_dir_close,
        llseek:         dcache_dir_lseek,
        fsync:          dcache_dir_fsync,
};


struct inode_operations simple_dir_inode_operations = {
        .lookup         = simple_lookup,
};


#endif /* linux-pm */

static struct inode *sysfs_get_inode(struct super_block *sb, int mode, int dev)
{
	struct inode *inode = new_inode(sb);

	if (inode) {
		inode->i_mode = mode;
		inode->i_uid = current->fsuid;
		inode->i_gid = current->fsgid;
		inode->i_blksize = PAGE_CACHE_SIZE;
		inode->i_blocks = 0;
		inode->i_rdev = NODEV;
		inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
		inode->i_mapping->a_ops = &sysfs_aops;
#if 0 /* linux-pm */
		inode->i_mapping->backing_dev_info = &sysfs_backing_dev_info;
#endif /* linux-pm */
		switch (mode & S_IFMT) {
		default:
			init_special_inode(inode, mode, dev);
			break;
		case S_IFREG:
			inode->i_fop = &sysfs_file_operations;
			break;
		case S_IFDIR:
			inode->i_op = &simple_dir_inode_operations;
			inode->i_fop = &simple_dir_operations;

			/* directory inodes start off with i_nlink == 2 (for "." entry) */
			inode->i_nlink++;
			break;
		case S_IFLNK:
			inode->i_op = &page_symlink_inode_operations;
			break;
		}
	}
	return inode;
}

static int sysfs_mknod(struct inode *dir, struct dentry *dentry, int mode, dev_t dev)
{
	struct inode *inode;
	int error = 0;

	if (!dentry->d_inode) {
		inode = sysfs_get_inode(dir->i_sb, mode, dev);
		if (inode)
			d_instantiate(dentry, inode);
		else
			error = -ENOSPC;
	} else
		error = -EEXIST;
	return error;
}

static int sysfs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	int res;
	mode = (mode & (S_IRWXUGO|S_ISVTX)) | S_IFDIR;
 	res = sysfs_mknod(dir, dentry, mode, 0);
 	if (!res)
 		dir->i_nlink++;
	return res;
}

static int sysfs_create(struct inode *dir, struct dentry *dentry, int mode)
{
	int res;
	mode = (mode & S_IALLUGO) | S_IFREG;
 	res = sysfs_mknod(dir, dentry, mode, 0);
	return res;
}

static int sysfs_symlink(struct inode * dir, struct dentry *dentry, const char * symname)
{
	struct inode *inode;
	int error = -ENOSPC;

	if (dentry->d_inode)
		return -EEXIST;

	inode = sysfs_get_inode(dir->i_sb, S_IFLNK|S_IRWXUGO, 0);
	if (inode) {
		int l = strlen(symname)+1;
#if 1 /* linux-pm */
		error = block_symlink(inode, symname, l);
#else
		error = page_symlink(inode, symname, l);
#endif
		if (!error)
			d_instantiate(dentry, inode);
		else
			iput(inode);
	}
	return error;
}

/**
 *	sysfs_read_file - read an attribute. 
 *	@file:	file pointer.
 *	@buf:	buffer to fill.
 *	@count:	number of bytes to read.
 *	@ppos:	starting offset in file.
 *
 *	Userspace wants to read an attribute file. The attribute descriptor
 *	is in the file's ->d_fsdata. The target object is in the directory's
 *	->d_fsdata. 
 *
 *	We allocate a %PAGE_SIZE buffer, and pass it to the object's ->show()
 *	method (along with the object). We loop doing this until @count is 
 *	satisfied, or ->show() returns %0. 
 */

static ssize_t
sysfs_read_file(struct file *file, char *buf, size_t count, loff_t *ppos)
{
	struct attribute * attr = file->f_dentry->d_fsdata;
	struct sysfs_ops * ops = file->private_data;
	struct kobject * kobj = file->f_dentry->d_parent->d_fsdata;
	unsigned char *page;
	ssize_t retval = 0;

	if (count > PAGE_SIZE)
		count = PAGE_SIZE;

	page = (unsigned char*)__get_free_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	while (count > 0) {
		ssize_t len;

		len = ops->show(kobj,attr,page,count,*ppos);

		if (len <= 0) {
			if (len < 0)
				retval = len;
			break;
		} else if (len > count)
			len = count;

		if (copy_to_user(buf,page,len)) {
			retval = -EFAULT;
			break;
		}

		*ppos += len;
		count -= len;
		buf += len;
		retval += len;
	}
	free_page((unsigned long)page);
	return retval;
}

/**
 *	sysfs_write_file - write an attribute.
 *	@file:	file pointer
 *	@buf:	data to write
 *	@count:	number of bytes
 *	@ppos:	starting offset
 *
 *	Identical to sysfs_read_file(), though going the opposite direction.
 *	We allocate a %PAGE_SIZE buffer and copy in the userspace buffer. We
 *	pass that to the object's ->store() method until we reach @count or 
 *	->store() returns %0 or less.
 */

static ssize_t
sysfs_write_file(struct file *file, const char *buf, size_t count, loff_t *ppos)
{
	struct attribute * attr = file->f_dentry->d_fsdata;
	struct sysfs_ops * ops = file->private_data;
	struct kobject * kobj = file->f_dentry->d_parent->d_fsdata;
	ssize_t retval = 0;
	char * page;


	page = (char *)__get_free_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	if (count >= PAGE_SIZE)
		count = PAGE_SIZE - 1;
	if (copy_from_user(page,buf,count))
		goto done;
	*(page + count) = '\0';

	while (count > 0) {
		ssize_t len;

		len = ops->store(kobj,attr,page + retval,count,*ppos);

		if (len <= 0) {
			if (len < 0)
				retval = len;
			break;
		}
		retval += len;
		count -= len;
		*ppos += len;
		buf += len;
	}
 done:
	free_page((unsigned long)page);
	return retval;
}

static int check_perm(struct inode * inode, struct file * file)
{
	struct kobject * kobj = kobject_get(file->f_dentry->d_parent->d_fsdata);
	struct attribute * attr = file->f_dentry->d_fsdata;
	struct sysfs_ops * ops = NULL;
	int error = 0;

	if (!kobj || !attr)
		goto Einval;
	
	if (kobj->subsys)
		ops = kobj->subsys->sysfs_ops;

	/* No sysfs operations, either from having no subsystem,
	 * or the subsystem have no operations.
	 */
	if (!ops)
		goto Eaccess;

	/* File needs write support.
	 * The inode's perms must say it's ok, 
	 * and we must have a store method.
	 */
	if (file->f_mode & FMODE_WRITE) {

		if (!(inode->i_mode & S_IWUGO))
			goto Eperm;
		if (!ops->store)
			goto Eaccess;

	}

	/* File needs read support.
	 * The inode's perms must say it's ok, and we there
	 * must be a show method for it.
	 */
	if (file->f_mode & FMODE_READ) {
		if (!(inode->i_mode & S_IRUGO))
			goto Eperm;
		if (!ops->show)
			goto Eaccess;
	}

	/* No error? Great, store the ops in file->private_data
	 * for easy access in the read/write functions.
	 */
	file->private_data = ops;
	goto Done;

 Einval:
	error = -EINVAL;
	goto Done;
 Eaccess:
	error = -EACCES;
	goto Done;
 Eperm:
	error = -EPERM;
 Done:
	return error;
}

static int sysfs_open_file(struct inode * inode, struct file * filp)
{
	return check_perm(inode,filp);
}

static int sysfs_release(struct inode * inode, struct file * filp)
{
	struct kobject * kobj = filp->f_dentry->d_parent->d_fsdata;
	if (kobj) 
		kobject_put(kobj);
	return 0;
}

static struct file_operations sysfs_file_operations = {
	.read		= sysfs_read_file,
	.write		= sysfs_write_file,
	.llseek		= generic_file_llseek,
	.open		= sysfs_open_file,
	.release	= sysfs_release,
};

static struct address_space_operations sysfs_aops = {
	.readpage	= simple_readpage,
	.writepage	= fail_writepage,
	.prepare_write	= simple_prepare_write,
	.commit_write	= simple_commit_write
};

static struct super_operations sysfs_ops = {
	.statfs		= simple_statfs,
#if 0 /* linux-pm */
	.drop_inode	= generic_delete_inode,
#endif
};

static int sysfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct inode *inode;
	struct dentry *root;

	sb->s_blocksize = PAGE_CACHE_SIZE;
	sb->s_blocksize_bits = PAGE_CACHE_SHIFT;
	sb->s_magic = SYSFS_MAGIC;
	sb->s_op = &sysfs_ops;
	inode = sysfs_get_inode(sb, S_IFDIR | 0755, 0);

	if (!inode) {
		pr_debug("%s: could not get inode!\n",__FUNCTION__);
		return -ENOMEM;
	}

	root = d_alloc_root(inode);
	if (!root) {
		pr_debug("%s: could not get root dentry!\n",__FUNCTION__);
		iput(inode);
		return -ENOMEM;
	}
	sb->s_root = root;
	return 0;
}

#if 1 /* linux-pm */
struct super_block *sysfs_read_super(struct super_block *s, void *data, 
				     int silent)
{
	return (sysfs_fill_super(s, data, silent) == 0) ? s : NULL;
}

#else /* linux-pm */
static struct super_block *sysfs_get_sb(struct file_system_type *fs_type,
	int flags, char *dev_name, void *data)
{
	return get_sb_single(fs_type, flags, data, sysfs_fill_super);
}
#endif /* linux-pm */

static struct file_system_type sysfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "sysfs",
#if 1 /* linux-pm */
	.fs_flags	= FS_SINGLE,
	.read_super	= sysfs_read_super,
#else /* linux-pm */
	.get_sb		= sysfs_get_sb,
	.kill_sb	= kill_litter_super,
#endif /* linux-pm */
};

#if 1 /* linux-pm */
int __init sysfs_init(void)
#else /* linux-pm */
static int __init sysfs_init(void)
#endif /* linux-pm */
{
	int err;

	err = register_filesystem(&sysfs_fs_type);
	if (!err) {
		sysfs_mount = kern_mount(&sysfs_fs_type);
		if (IS_ERR(sysfs_mount)) {
			printk(KERN_ERR "sysfs: could not mount!\n");
			err = PTR_ERR(sysfs_mount);
			sysfs_mount = NULL;
		}
	}
	return err;
}

#if 0 /* linux-pm */
core_initcall(sysfs_init);
#endif /* linux-pm */

static struct dentry * get_dentry(struct dentry * parent, const char * name)
{
	struct qstr qstr;

	qstr.name = name;
	qstr.len = strlen(name);
	qstr.hash = full_name_hash(name,qstr.len);
	return lookup_hash(&qstr,parent);
}


/**
 *	sysfs_create_dir - create a directory for an object.
 *	@parent:	parent parent object.
 *	@kobj:		object we're creating directory for. 
 */

int sysfs_create_dir(struct kobject * kobj)
{
	struct dentry * dentry = NULL;
	struct dentry * parent;
	int error = 0;

	if (!kobj)
		return -EINVAL;

	if (kobj->parent)
		parent = kobj->parent->dentry;
	else if (sysfs_mount && sysfs_mount->mnt_sb)
		parent = sysfs_mount->mnt_sb->s_root;
	else
		return -EFAULT;

	down(&parent->d_inode->i_sem);
	dentry = get_dentry(parent,kobj->name);

	if (!IS_ERR(dentry)) {
		dentry->d_fsdata = (void *)kobj;
		kobj->dentry = dentry;
		error = sysfs_mkdir(parent->d_inode,dentry,
				    (S_IFDIR| S_IRWXU | S_IRUGO | S_IXUGO));
	} else
		error = PTR_ERR(dentry);
	up(&parent->d_inode->i_sem);
	return error;
}


/**
 *	sysfs_create_file - create an attribute file for an object.
 *	@kobj:	object we're creating for. 
 *	@attr:	atrribute descriptor.
 */

int sysfs_create_file(struct kobject * kobj, struct attribute * attr)
{
	struct dentry * dentry;
	struct dentry * parent;
	int error = 0;

	if (!kobj || !attr)
		return -EINVAL;

	parent = kobj->dentry;

	down(&parent->d_inode->i_sem);
	dentry = get_dentry(parent,attr->name);
	if (!IS_ERR(dentry)) {
		dentry->d_fsdata = (void *)attr;
		error = sysfs_create(parent->d_inode,dentry,attr->mode);
	} else
		error = PTR_ERR(dentry);
	up(&parent->d_inode->i_sem);
	return error;
}


static int object_depth(struct kobject * kobj)
{
	struct kobject * p = kobj;
	int depth = 0;
	do { depth++; } while ((p = p->parent));
	return depth;
}

static int object_path_length(struct kobject * kobj)
{
	struct kobject * p = kobj;
	int length = 1;
	do {
		length += strlen(p->name) + 1;
		p = p->parent;
	} while (p);
	return length;
}

static void fill_object_path(struct kobject * kobj, char * buffer, int length)
{
	struct kobject * p;

	--length;
	for (p = kobj; p; p = p->parent) {
		int cur = strlen(p->name);

		/* back up enough to print this bus id with '/' */
		length -= cur;
		strncpy(buffer + length,p->name,cur);
		*(buffer + --length) = '/';
	}
}

/**
 *	sysfs_create_link - create symlink between two objects.
 *	@kobj:	object whose directory we're creating the link in.
 *	@target:	object we're pointing to.
 *	@name:		name of the symlink.
 */
int sysfs_create_link(struct kobject * kobj, struct kobject * target, char * name)
{
	struct dentry * dentry = kobj->dentry;
	struct dentry * d;
	int error = 0;
	int size;
	int depth;
	char * path;
	char * s;

	depth = object_depth(kobj);
	size = object_path_length(target) + depth * 3 - 1;
	if (size > PATH_MAX)
		return -ENAMETOOLONG;
	pr_debug("%s: depth = %d, size = %d\n",__FUNCTION__,depth,size);

	path = kmalloc(size,GFP_KERNEL);
	if (!path)
		return -ENOMEM;
	memset(path,0,size);

	for (s = path; depth--; s += 3)
		strcpy(s,"../");

	fill_object_path(target,path,size);
	pr_debug("%s: path = '%s'\n",__FUNCTION__,path);

	down(&dentry->d_inode->i_sem);
	d = get_dentry(dentry,name);
	if (!IS_ERR(d))
		error = sysfs_symlink(dentry->d_inode,d,path);
	else
		error = PTR_ERR(d);
	up(&dentry->d_inode->i_sem);
	kfree(path);
	return error;
}

static void hash_and_remove(struct dentry * dir, const char * name)
{
	struct dentry * victim;

	down(&dir->d_inode->i_sem);
	victim = get_dentry(dir,name);
	if (!IS_ERR(victim)) {
		/* make sure dentry is really there */
		if (victim->d_inode && 
		    (victim->d_parent->d_inode == dir->d_inode)) {
			d_invalidate(victim);
			simple_unlink(dir->d_inode,victim);
		}
	}
	up(&dir->d_inode->i_sem);
}


/**
 *	sysfs_remove_file - remove an object attribute.
 *	@kobj:	object we're acting for.
 *	@attr:	attribute descriptor.
 *
 *	Hash the attribute name and kill the victim.
 */

void sysfs_remove_file(struct kobject * kobj, struct attribute * attr)
{
	hash_and_remove(kobj->dentry,attr->name);
}


/**
 *	sysfs_remove_link - remove symlink in object's directory.
 *	@kobj:	object we're acting for.
 *	@name:	name of the symlink to remove.
 */

void sysfs_remove_link(struct kobject * kobj, char * name)
{
	hash_and_remove(kobj->dentry,name);
}


/**
 *	sysfs_remove_dir - remove an object's directory.
 *	@kobj:	object. 
 *
 *	The only thing special about this is that we remove any files in 
 *	the directory before we remove the directory, and we've inlined
 *	what used to be sysfs_rmdir() below, instead of calling separately.
 */

void sysfs_remove_dir(struct kobject * kobj)
{
	struct list_head * node, * next;
	struct dentry * dentry = kobj->dentry;
	struct dentry * parent;

	if (!dentry)
		return;

	parent = dget(dentry->d_parent);
	down(&parent->d_inode->i_sem);
	down(&dentry->d_inode->i_sem);

	list_for_each_safe(node,next,&dentry->d_subdirs) {
		struct dentry * d = list_entry(node,struct dentry,d_child);
		/* make sure dentry is still there */
		if (d->d_inode) {
			d_invalidate(d);
			simple_unlink(dentry->d_inode,d);
		}
	}

	up(&dentry->d_inode->i_sem);
	d_invalidate(dentry);
	simple_rmdir(parent->d_inode,dentry);

	up(&parent->d_inode->i_sem);
	dput(parent);
}

EXPORT_SYMBOL(sysfs_create_file);
EXPORT_SYMBOL(sysfs_remove_file);
EXPORT_SYMBOL(sysfs_create_link);
EXPORT_SYMBOL(sysfs_remove_link);
EXPORT_SYMBOL(sysfs_create_dir);
EXPORT_SYMBOL(sysfs_remove_dir);
