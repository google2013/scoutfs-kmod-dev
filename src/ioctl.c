/*
 * Copyright (C) 2016 Versity Software, Inc.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/compiler.h>
#include <linux/uio.h>
#include <linux/slab.h>

#include "format.h"
#include "btree.h"
#include "key.h"
#include "dir.h"
#include "name.h"
#include "ioctl.h"
#include "super.h"

/*
 * Find all the inodes that have had keys of a given type modified since
 * a given sequence number.  The user's arg struct specifies the inode
 * range to search within and the sequence value to return results from.
 * Different ioctls call this for different key types.
 *
 * When this is used for file data items the user is trying to find
 * inodes whose data has changed since a given time in the past.
 *
 * XXX We'll need to improve the walk and search to notice when file
 * data items have been truncated away.
 *
 * Inodes and their sequence numbers are copied out to userspace in
 * inode order, not sequence order.
 */
static long scoutfs_ioc_inodes_since(struct file *file, unsigned long arg,
				     u8 type)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct scoutfs_btree_root *meta = SCOUTFS_STABLE_META(sb);
	struct scoutfs_ioctl_inodes_since __user *uargs = (void __user *)arg;
	struct scoutfs_ioctl_inodes_since args;
	struct scoutfs_ioctl_ino_seq __user *uiseq;
	struct scoutfs_ioctl_ino_seq iseq;
	struct scoutfs_key key;
	struct scoutfs_key last;
	u64 seq;
	long bytes;
	int ret;

	if (copy_from_user(&args, uargs, sizeof(args)))
		return -EFAULT;

	uiseq = (void __user *)(unsigned long)args.buf_ptr;
	if (args.buf_len < sizeof(iseq) || args.buf_len > INT_MAX)
		return -EINVAL;

	scoutfs_set_key(&key, args.first_ino, type, 0);
	scoutfs_set_key(&last, args.last_ino, type, 0);

	bytes = 0;
	for (;;) {
		ret = scoutfs_btree_since(sb, meta, &key, &last, args.seq,
					  &key, &seq, NULL);
		if (ret < 0) {
			if (ret == -ENOENT)
				ret = 0;
			break;
		}

		iseq.ino = scoutfs_key_inode(&key);
		iseq.seq = seq;

		if (copy_to_user(uiseq, &iseq, sizeof(iseq))) {
			ret = -EFAULT;
			break;
		}

		uiseq++;
		bytes += sizeof(iseq);
		if (bytes + sizeof(iseq) > args.buf_len) {
			ret = 0;
			break;
		}

		key.inode = cpu_to_le64(iseq.ino + 1);
	}

	if (bytes)
		ret = bytes;

	return ret;
}

static int copy_to_ptr(char __user **to, const void *from,
		       unsigned long n, int space)
{
	if (n > space)
		return -EOVERFLOW;

	if (copy_to_user(*to, from, n))
		return -EFAULT;

	*to += n;
	return space - n;
}

/*
 * Fill the caller's buffer with all the paths from the on-disk root
 * directory to the target inode.  It will provide as many full paths as
 * there are final links to the target inode.
 *
 * The null terminated paths are stored consecutively in the buffer.  A
 * final zero length null terminated string follows the last path.
 *
 * This only walks back through full hard links.  None of the returned
 * paths will reflect symlinks to components in the path.
 *
 * This doesn't ensure that the caller has permissions to traverse the
 * returned paths to the inode.  It requires CAP_DAC_READ_SEARCH which
 * bypasses permissions checking.
 *
 * If the provided buffer isn't large enough EOVERFLOW will be returned.
 * The buffer can be approximately sized by multiplying the inode's
 * nlink by PATH_MAX.
 *
 * This call is not serialized with any modification (create, rename,
 * unlink) of the path components.  It will return all the paths that
 * were stable both before and after the call.  It may or may not return
 * paths which are created or unlinked during the call.
 *
 * This will return failure if it fails to read any path.  An empty
 * buffer is returned if the target inode doesn't exist or is
 * disconnected from the root.
 *
 * XXX
 *  - we may want to support partial failure
 *  - can dir renaming trick us into returning garbage paths?  seems likely.
 */
static long scoutfs_ioc_inode_paths(struct file *file, unsigned long arg)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct scoutfs_ioctl_inode_paths __user *uargs = (void __user *)arg;
	struct scoutfs_ioctl_inode_paths args;
	struct scoutfs_path_component *comp;
	struct scoutfs_path_component *tmp;
	static char slash = '/';
	static char null = '\0';
	char __user *ptr;
	LIST_HEAD(list);
	u64 ctr;
	int ret;
	int len;

	if (!capable(CAP_DAC_READ_SEARCH))
		return -EPERM;

	if (copy_from_user(&args, uargs, sizeof(args)))
		return -EFAULT;

	if (args.buf_len > INT_MAX)
		return -EINVAL;

	ptr = (void __user *)(unsigned long)args.buf_ptr;
	len = args.buf_len;

	ctr = 0;
	while ((ret = scoutfs_dir_next_path(sb, args.ino, &ctr, &list)) > 0) {
		ret = 0;

		/* copy the components out as a path */
		list_for_each_entry_safe(comp, tmp, &list, head) {
			len = copy_to_ptr(&ptr, comp->name, comp->len, len);
			if (len < 0)
				goto out;

			list_del_init(&comp->head);
			kfree(comp);

			if (!list_empty(&list)) {
				len = copy_to_ptr(&ptr, &slash, 1, len);
				if (len < 0)
					goto out;
			}
		}
		len = copy_to_ptr(&ptr, &null, 1, len);
		if (len < 0)
			goto out;
	}

	len = copy_to_ptr(&ptr, &null, 1, len);
out:
	scoutfs_dir_free_path(&list);

	if (ret == 0 && len < 0)
		ret = len;
	return ret;
}

/*
 * Find inodes that might contain a given xattr name or value.
 *
 * The inodes are filled in sorted order from the first to the last
 * inode.  The number of found inodes is returned.  If an error is hit
 * it can return the number of inodes found before the error.
 *
 * The search can be continued from the next inode after the last
 * returned.
 */
static long scoutfs_ioc_find_xattr(struct file *file, unsigned long arg,
				   bool find_name)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct scoutfs_btree_root *meta = SCOUTFS_STABLE_META(sb);
	struct scoutfs_ioctl_find_xattr args;
	struct scoutfs_key key;
	struct scoutfs_key last;
	char __user *ustr;
	u64 __user *uino;
	char *str;
	int copied = 0;
	int ret = 0;
	u64 ino;
	u8 type;
	u64 h;

	if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
		return -EFAULT;

	if (args.str_len > SCOUTFS_MAX_XATTR_LEN || args.ino_count > INT_MAX)
		return -EINVAL;

	if (args.first_ino > args.last_ino)
		return -EINVAL;

	if (args.ino_count == 0)
		return 0;

	ustr = (void __user *)(unsigned long)args.str_ptr;
	uino = (void __user *)(unsigned long)args.ino_ptr;

	str = kmalloc(args.str_len, GFP_KERNEL);
	if (!str)
		return -ENOMEM;

	if (copy_from_user(str, ustr, args.str_len)) {
		ret = -EFAULT;
		goto out;
	}

	h = scoutfs_name_hash(str, args.str_len);

	if (find_name) {
		h &= ~SCOUTFS_XATTR_NAME_HASH_MASK;
		type = SCOUTFS_XATTR_NAME_HASH_KEY;
	} else {
		type = SCOUTFS_XATTR_VAL_HASH_KEY;
	}

	scoutfs_set_key(&key, h, type, args.first_ino);
	scoutfs_set_key(&last, h, type, args.last_ino);

	while (copied < args.ino_count) {

		ret = scoutfs_btree_next(sb, meta, &key, &last, &key, NULL);
		if (ret < 0) {
			if (ret == -ENOENT)
				ret = 0;
			break;
		}

		ino = scoutfs_key_offset(&key);
		if (put_user(ino, uino)) {
			ret = -EFAULT;
			break;
		}

		uino++;
		copied++;
		scoutfs_inc_key(&key);
	}

out:
	kfree(str);
	return copied ?: ret;
}

long scoutfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case SCOUTFS_IOC_INODES_SINCE:
		return scoutfs_ioc_inodes_since(file, arg, SCOUTFS_INODE_KEY);
	case SCOUTFS_IOC_INODE_PATHS:
		return scoutfs_ioc_inode_paths(file, arg);
	case SCOUTFS_IOC_FIND_XATTR_NAME:
		return scoutfs_ioc_find_xattr(file, arg, true);
	case SCOUTFS_IOC_FIND_XATTR_VAL:
		return scoutfs_ioc_find_xattr(file, arg, false);
	case SCOUTFS_IOC_INODE_DATA_SINCE:
		return scoutfs_ioc_inodes_since(file, arg, SCOUTFS_BMAP_KEY);
	}

	return -ENOTTY;
}
