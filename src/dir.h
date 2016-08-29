#ifndef _SCOUTFS_DIR_H_
#define _SCOUTFS_DIR_H_

#include "format.h"

extern const struct file_operations scoutfs_dir_fops;
extern const struct inode_operations scoutfs_dir_iops;
extern const struct inode_operations scoutfs_symlink_iops;

int scoutfs_dir_init(void);
void scoutfs_dir_exit(void);

struct scoutfs_path_component {
	struct list_head head;
	unsigned int len;
	char name[SCOUTFS_NAME_LEN];
};

int scoutfs_dir_next_path(struct super_block *sb, u64 ino, u64 *ctr,
			  struct list_head *list);
void scoutfs_dir_free_path(struct list_head *list);

#endif
