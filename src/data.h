#ifndef _SCOUTFS_FILERW_H_
#define _SCOUTFS_FILERW_H_

extern const struct address_space_operations scoutfs_file_aops;
extern const struct file_operations scoutfs_file_fops;

int scoutfs_data_truncate_items(struct super_block *sb, struct inode *inode,
				u64 ino, u64 iblock, u64 last, bool offline,
				struct scoutfs_lock *lock);
int scoutfs_data_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
			u64 start, u64 len);
long scoutfs_fallocate(struct file *file, int mode, loff_t offset, loff_t len);

int scoutfs_data_setup(struct super_block *sb);
void scoutfs_data_destroy(struct super_block *sb);

#endif
