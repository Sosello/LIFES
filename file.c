// SPDX-License-Identifier: GPL-2.0
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2018 Redha Gouicem <redha.gouicem@lip6.fr>
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>

#include "ouichefs.h"
#include "bitmap.h"
#include "command.h"

static char list[256] = {0};
static int wasted = 0;

/*
 * Map the buffer_head passed in argument with the iblock-th block of the file
 * represented by inode. If the requested block is not allocated and create is
 * true, allocate a new block on disk and map it.
 */
static int ouichefs_file_get_block(struct inode *inode, sector_t iblock,
				   struct buffer_head *bh_result, int create)
{
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct ouichefs_file_index_block *index;
	struct buffer_head *bh_index;
	int ret = 0, bno;

	/* If block number exceeds filesize, fail */
	if (iblock >= OUICHEFS_BLOCK_SIZE >> 2)
		return -EFBIG;

	/* Read index block from disk */
	bh_index = sb_bread(sb, ci->index_block);
	if (!bh_index)
		return -EIO;
	index = (struct ouichefs_file_index_block *)bh_index->b_data;

	/*
	 * Check if iblock is already allocated. If not and create is true,
	 * allocate it. Else, get the physical block number.
	 */
	if (index->blocks[iblock] == 0) {
		if (!create) {
			ret = 0;
			goto brelse_index;
		}
		bno = get_free_block(sbi);
		if (!bno) {
			ret = -ENOSPC;
			goto brelse_index;
		}
		index->blocks[iblock] = bno;
	} else {
		bno = index->blocks[iblock];
	}

	/* Map the physical block to the given buffer_head */
	map_bh(bh_result, sb, bno);

brelse_index:
	brelse(bh_index);

	return ret;
}

/*
 * Called by the page cache to read a page from the physical disk and map it in
 * memory.
 */
static void ouichefs_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, ouichefs_file_get_block);
}

/*
 * Called by the page cache to write a dirty page to the physical disk (when
 * sync is called or when memory is needed).
 */
static int ouichefs_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, ouichefs_file_get_block, wbc);
}

/*
 * Called by the VFS when a write() syscall occurs on file before writing the
 * data in the page cache. This functions checks if the write will be able to
 * complete and allocates the necessary blocks through block_write_begin().
 */
static int ouichefs_write_begin(struct file *file,
				struct address_space *mapping, loff_t pos,
				unsigned int len, struct page **pagep,
				void **fsdata)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(file->f_inode->i_sb);
	int err;
	uint32_t nr_allocs = 0;

	/* Check if the write can be completed (enough space?) */
	if (pos + len > OUICHEFS_MAX_FILESIZE)
		return -ENOSPC;
	nr_allocs = max(pos + len, file->f_inode->i_size) / OUICHEFS_BLOCK_SIZE;
	if (nr_allocs > file->f_inode->i_blocks - 1)
		nr_allocs -= file->f_inode->i_blocks - 1;
	else
		nr_allocs = 0;
	if (nr_allocs > sbi->nr_free_blocks)
		return -ENOSPC;

	/* prepare the write */
	err = block_write_begin(mapping, pos, len, pagep,
				ouichefs_file_get_block);
	/* if this failed, reclaim newly allocated blocks */
	if (err < 0) {
		pr_err("%s:%d: newly allocated blocks reclaim not implemented yet\n",
		       __func__, __LINE__);
	}
	return err;
}

/*
 * Called by the VFS after writing data from a write() syscall to the page
 * cache. This functions updates inode metadata and truncates the file if
 * necessary.
 */
static int ouichefs_write_end(struct file *file, struct address_space *mapping,
			      loff_t pos, unsigned int len, unsigned int copied,
			      struct page *page, void *fsdata)
{
	int ret;
	struct inode *inode = file->f_inode;
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct super_block *sb = inode->i_sb;

	/* Complete the write() */
	ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
	if (ret < len) {
		pr_err("%s:%d: wrote less than asked... what do I do? nothing for now...\n",
		       __func__, __LINE__);
	} else {
		uint32_t nr_blocks_old = inode->i_blocks;

		/* Update inode metadata */
		inode->i_blocks = inode->i_size / OUICHEFS_BLOCK_SIZE + 2;
		inode->i_mtime = inode->i_ctime = current_time(inode);
		mark_inode_dirty(inode);

		/* If file is smaller than before, free unused blocks */
		if (nr_blocks_old > inode->i_blocks) {
			int i;
			struct buffer_head *bh_index;
			struct ouichefs_file_index_block *index;

			/* Free unused blocks from page cache */
			truncate_pagecache(inode, inode->i_size);

			/* Read index block to remove unused blocks */
			bh_index = sb_bread(sb, ci->index_block);
			if (!bh_index) {
				pr_err("failed truncating '%s'. we just lost %llu blocks\n",
				       file->f_path.dentry->d_name.name,
				       nr_blocks_old - inode->i_blocks);
				goto end;
			}
			index = (struct ouichefs_file_index_block *)
					bh_index->b_data;

			for (i = inode->i_blocks - 1; i < nr_blocks_old - 1;
			     i++) {
				put_block(OUICHEFS_SB(sb), index->blocks[i]);
				index->blocks[i] = 0;
			}
			mark_buffer_dirty(bh_index);
			brelse(bh_index);
		}
	}
end:
	return ret;
}

const struct address_space_operations ouichefs_aops = {
	.readahead = ouichefs_readahead,
	.writepage = ouichefs_writepage,
	.write_begin = ouichefs_write_begin,
	.write_end = ouichefs_write_end
};

static int ouichefs_open(struct inode *inode, struct file *file) {
	bool wronly = (file->f_flags & O_WRONLY) != 0;
	bool rdwr = (file->f_flags & O_RDWR) != 0;
	bool trunc = (file->f_flags & O_TRUNC) != 0;

	if ((wronly || rdwr) && trunc && (inode->i_size != 0)) {
		struct super_block *sb = inode->i_sb;
		struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
		struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
		struct ouichefs_file_index_block *index;
		struct buffer_head *bh_index;
		sector_t iblock;

		/* Read index block from disk */
		bh_index = sb_bread(sb, ci->index_block);
		if (!bh_index)
			return -EIO;
		index = (struct ouichefs_file_index_block *)bh_index->b_data;

		for (iblock = 0; index->blocks[iblock] != 0; iblock++) {
			put_block(sbi, index->blocks[iblock]);
			index->blocks[iblock] = 0;
		}
		inode->i_size = 0;
		inode->i_blocks = 0;

		brelse(bh_index);
	}
	
	return 0;
}

static ssize_t ouichefs_read(struct file *file, char __user *data, size_t len, loff_t *pos)
{
	if (*pos - wasted >= file->f_inode->i_size){
		wasted = 0;
		return 0;
	}

	unsigned long to_be_copied = 0;

	struct super_block *sb = file->f_inode->i_sb;
	sector_t iblock = *pos / OUICHEFS_BLOCK_SIZE;
	struct ouichefs_file_index_block *index;
	struct buffer_head *bh_index;
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(file->f_inode);

	/* If block number exceeds filesize, fail */
	if (iblock >= OUICHEFS_BLOCK_SIZE >> 2)
		return -EFBIG;

	/* Read index block from disk */
	bh_index = sb_bread(sb, ci->index_block);
	if (!bh_index)
		return -EIO;
	index = (struct ouichefs_file_index_block *)bh_index->b_data;

	/* Get the block number for the current iblock */
	int bno = index->blocks[iblock];

	if (bno == 0) {
		brelse(bh_index);
		return -EIO;
	}

	struct buffer_head *bh = sb_bread(sb, bno);
	if (!bh) {
		brelse(bh_index);
		return -EIO;
	}
	struct ouichefs_block_info *bi = (struct ouichefs_block_info *)bh->b_data;

	// get data from the buffer from the current position
	size_t remain = strlen(bi->data) < OUICHEFS_BLOCK_SIZE ? strlen(bi->data) : OUICHEFS_BLOCK_SIZE;
	if (remain < OUICHEFS_BLOCK_SIZE)
		wasted += OUICHEFS_BLOCK_SIZE - remain;
	to_be_copied = len < remain ? len : remain;
	if(copy_to_user(data, bi->data + *pos % OUICHEFS_BLOCK_SIZE, to_be_copied)){
		brelse(bh);
		brelse(bh_index);
		return -EFAULT;
	}
	brelse(bh);
	brelse(bh_index);
	if (to_be_copied == remain) {
		*pos += OUICHEFS_BLOCK_SIZE - *pos % OUICHEFS_BLOCK_SIZE;
	} else {
		*pos += to_be_copied;
	}
	file->f_pos += to_be_copied;
	pr_info("pos: %lld\n", *pos);

	return to_be_copied;
}

/*
 * Write function for the ouichefs filesystem. This function allows to write data without
 * the use of page cache.
 */

int need_new_block(struct ouichefs_file_index_block *index, sector_t iblock, 
					size_t len, size_t remain_lenth) {
	if (index->blocks[iblock] == 0 ) {
		pr_info("block is empty\n");
		return 1;
	}
	if(remain_lenth < len) {
		pr_info("need new\n");
		return 1;
	}
	
	return 0;
}

static ssize_t ouichefs_write(struct file *file, const char __user *data, size_t len, loff_t *pos)
{
	// pr_info("write\n");

	struct inode *inode = file->f_inode;
	struct super_block *sb = inode->i_sb;	
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct ouichefs_file_index_block *index;
	struct buffer_head *bh_index;
	sector_t iblock = *pos / OUICHEFS_BLOCK_SIZE;

	bh_index = sb_bread(sb, ci->index_block);
	if (!bh_index) {
		return -EIO;
	}
	index = (struct ouichefs_file_index_block *)bh_index->b_data;

	int bno;
	
	struct buffer_head * bh_data = sb_bread(sb, index->blocks[iblock]);
	struct ouichefs_block_info *bi = (struct ouichefs_block_info *)bh_data->b_data;

	size_t length = strlen(bi->data) < OUICHEFS_BLOCK_SIZE ? strlen(bi->data) : OUICHEFS_BLOCK_SIZE;
	size_t remain_lenth = OUICHEFS_BLOCK_SIZE - length;
	if (need_new_block(index, iblock, len, remain_lenth)) {
		bno = get_free_block(OUICHEFS_SB(sb));
		if (!bno) {
			return -ENOSPC;
		}
		int prev_block = index->blocks[iblock];
		index->blocks[iblock] = bno;
		for (int i = iblock + 1; i < OUICHEFS_BLOCK_SIZE/sizeof(uint32_t); i++) {
			int should_break = 0;
			if (index->blocks[i] == 0) {
				should_break = 1;
			}
			int temp = index->blocks[i];
			
			index->blocks[i] = prev_block;
			prev_block = temp;
			if (should_break) {
				break;
			}
		}

	} else {
		bno = index->blocks[iblock];
	}
	struct buffer_head *bh = sb_bread(sb, bno);
	if (!bh) {
		brelse(bh_index);
		return -EIO;
	}
	char *buffer = bh->b_data;
	int remain = len < OUICHEFS_BLOCK_SIZE - *pos % OUICHEFS_BLOCK_SIZE  ? 
		len : OUICHEFS_BLOCK_SIZE - *pos % OUICHEFS_BLOCK_SIZE;
	if(copy_from_user(buffer + *pos % OUICHEFS_BLOCK_SIZE, data, remain)) {
		brelse(bh);
		brelse(bh_index);
		return -EFAULT;
	}
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);

	*pos += remain;
	data += remain;
	file->f_pos = *pos;
	len -= remain;
	brelse(bh);
	brelse(bh_index);
	
	inode->i_size += remain;
	inode->i_blocks = inode->i_size / OUICHEFS_BLOCK_SIZE + 2;
	inode->i_mtime = inode->i_ctime = current_time(inode);
	mark_inode_dirty(inode);
	

	return len;
}


static long ouichefs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	if (_IOC_TYPE(cmd) != 'N') {
        pr_info("Invalid type\n");
        return -ENOTTY;
    }
	struct super_block *sb = file->f_inode->i_sb;
    struct inode *inode = file->f_inode;
    struct ouichefs_inode_info *oi = OUICHEFS_INODE(inode);
    struct buffer_head *bh_index;
	int used_blocks = 0, partially_blocks = 0, wasted_bytes = 0;
	memset(list, 0, sizeof(list));
	bh_index = sb_bread(sb, oi->index_block);
	struct ouichefs_file_index_block *index = (struct ouichefs_file_index_block *)bh_index->b_data;
	if (index == NULL) {
		pr_err("Failed to read index block\n");
		return -EIO;
	}

    for (int i = 0; i < OUICHEFS_BLOCK_SIZE/sizeof(uint32_t); i++) {
		uint32_t block_number = index->blocks[i];
		if (block_number == 0) {
			break;
		}
		used_blocks++;
		struct ouichefs_block_info *bi;
		struct buffer_head *bh = sb_bread(sb, block_number);
		if (!bh) {
			pr_err("Failed to read block %d\n", block_number);
			return -EIO;
		}
		bi = (struct ouichefs_block_info *)bh->b_data;
		int length = strlen(bi->data) < OUICHEFS_BLOCK_SIZE ? strlen(bi->data) : OUICHEFS_BLOCK_SIZE;
		if (length < OUICHEFS_BLOCK_SIZE) {
			partially_blocks++;
			wasted_bytes += OUICHEFS_BLOCK_SIZE - length;
		}
		char string[32];
		snprintf(string, 32, "[%d, ", block_number);
		strlcat(list, string, sizeof(list));

    	snprintf(string, 32, "%d] ", min(OUICHEFS_BLOCK_SIZE, length));
		strlcat(list, string, sizeof(list));

		brelse(bh);
    }
	brelse(bh_index);
	char buf[100];
	switch (cmd) {
		case USED_BLOCKS:
			if(snprintf(buf, 100, "%d", used_blocks) < 0)
				return -EFAULT;
			if(copy_to_user((char *)arg, buf, 100))
				return -EFAULT;	
			return 0;
		case PARTIALLY_BLOCKS:
			if(snprintf(buf, 100, "%d", partially_blocks) < 0)
				return -EFAULT;
			if(copy_to_user((char *)arg, buf, 100))
				return -EFAULT;	
			return 0;
		case WASTED_BYTES:
			if(snprintf(buf, 100, "%d", wasted_bytes) < 0)
				return -EFAULT;
			if(copy_to_user((char *)arg, buf, 100))
				return -EFAULT;	
			return 0;
		case LIST_USED_BLOCKS:
			if(copy_to_user((char *)arg, list, 100))
				return -EFAULT;
			return 0;
		default:
			return -ENOTTY;
	}
	return 0;
}

const struct file_operations ouichefs_file_ops = {
	.owner = THIS_MODULE,
	.open = ouichefs_open,
	.read = ouichefs_read,
	.write = ouichefs_write,
	.unlocked_ioctl = ouichefs_ioctl,
	.llseek = generic_file_llseek,
	.read_iter = generic_file_read_iter,
	.write_iter = generic_file_write_iter,
};