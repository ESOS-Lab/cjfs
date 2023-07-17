// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/ext4/fsync.c
 *
 *  Copyright (C) 1993  Stephen Tweedie (sct@redhat.com)
 *  from
 *  Copyright (C) 1992  Remy Card (card@masi.ibp.fr)
 *                      Laboratoire MASI - Institut Blaise Pascal
 *                      Universite Pierre et Marie Curie (Paris VI)
 *  from
 *  linux/fs/minix/truncate.c   Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext4fs fsync primitive
 *
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 *
 *  Removed unnecessary code duplication for little endian machines
 *  and excessive __inline__s.
 *        Andi Kleen, 1997
 *
 * Major simplications and cleanup - we only need to do the metadata, because
 * we can depend on generic_block_fdatasync() to sync the data blocks.
 */

#include <linux/time.h>
#include <linux/jbd2.h> /* CJFS - debug */
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>

#include "ext4.h"
#include "ext4_jbd2.h"

#include <trace/events/ext4.h>

/*
 * If we're not journaling and this is a just-created file, we have to
 * sync our parent directory (if it was freshly created) since
 * otherwise it will only be written by writeback, leaving a huge
 * window during which a crash may lose the file.  This may apply for
 * the parent directory's parent as well, and so on recursively, if
 * they are also freshly created.
 */
static int ext4_sync_parent(struct inode *inode)
{
	struct dentry *dentry, *next;
	int ret = 0;

	if (!ext4_test_inode_state(inode, EXT4_STATE_NEWENTRY))
		return 0;
	dentry = d_find_any_alias(inode);
	if (!dentry)
		return 0;
	while (ext4_test_inode_state(inode, EXT4_STATE_NEWENTRY)) {
		ext4_clear_inode_state(inode, EXT4_STATE_NEWENTRY);

		next = dget_parent(dentry);
		dput(dentry);
		dentry = next;
		inode = dentry->d_inode;

		/*
		 * The directory inode may have gone through rmdir by now. But
		 * the inode itself and its blocks are still allocated (we hold
		 * a reference to the inode via its dentry), so it didn't go
		 * through ext4_evict_inode()) and so we are safe to flush
		 * metadata blocks and the inode.
		 */
		ret = sync_mapping_buffers(inode->i_mapping);
		if (ret)
			break;
		ret = sync_inode_metadata(inode, 1);
		if (ret)
			break;
	}
	dput(dentry);
	return ret;
}

static int ext4_fsync_nojournal(struct inode *inode, bool datasync,
				bool *needs_barrier)
{
	int ret, err;

	ret = sync_mapping_buffers(inode->i_mapping);
	if (!(inode->i_state & I_DIRTY_ALL))
		return ret;
	if (datasync && !(inode->i_state & I_DIRTY_DATASYNC))
		return ret;

	err = sync_inode_metadata(inode, 1);
	if (!ret)
		ret = err;

	if (!ret)
		ret = ext4_sync_parent(inode);
	if (test_opt(inode->i_sb, BARRIER))
		*needs_barrier = true;

	return ret;
}

static int ext4_fsync_journal(struct inode *inode, bool datasync,
			     bool *needs_barrier)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	journal_t *journal = EXT4_SB(inode->i_sb)->s_journal;
	tid_t commit_tid = datasync ? ei->i_datasync_tid : ei->i_sync_tid;

	if (journal->j_flags & JBD2_BARRIER &&
	    !jbd2_trans_will_send_data_barrier(journal, commit_tid))
		*needs_barrier = true;

	return ext4_fc_commit(journal, commit_tid);
}

/*
 * akpm: A new design for ext4_sync_file().
 *
 * This is only called from sys_fsync(), sys_fdatasync() and sys_msync().
 * There cannot be a transaction open by this task.
 * Another task could have dirtied this inode.  Its data can be in any
 * state in the journalling system.
 *
 * What we do is just kick off a commit and wait on it.  This will snapshot the
 * inode to disk.
 */
#ifdef DEBUG_FSYNC_LATENCY
typedef struct {
	s64 fsync_intv[5];
} fsync_data;

extern fsync_data fsync_array[4000000];
extern atomic_t fsync_index;
#endif
int ext4_sync_file(struct file *file, loff_t start, loff_t end, int datasync)
{
	int ret = 0, err;
	bool needs_barrier = false;
	struct inode *inode = file->f_mapping->host;
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct ext4_inode_info *ei = EXT4_I(inode);
	
	/* CJFS debug */
	journal_t *journal = EXT4_SB(inode->i_sb)->s_journal;
	
#ifdef DEBUG_FSYNC_LATENCY
	ktime_t start1, end1;
	fsync_data temp;
	int j = 0, seq = 0;

	for (j = 0; j < 5; j++) {
		temp.fsync_intv[j] = 0;
	}
	start1 = ktime_get();
#endif

	if (unlikely(ext4_forced_shutdown(sbi)))
		return -EIO;

	ASSERT(ext4_journal_current_handle() == NULL);

	trace_ext4_sync_file_enter(file, datasync);

	if (sb_rdonly(inode->i_sb)) {
		/* Make sure that we read updated s_mount_flags value */
		smp_rmb();
		if (ext4_test_mount_flag(inode->i_sb, EXT4_MF_FS_ABORTED))
			ret = -EROFS;
		goto out;
	}

	/* UFS */
	// trace_ext4_sync_file_data_start(inode, datasync);
	//printk(KERN_INFO "[SWDBG] (%s) sync_start! pid : %d, tid : %d\n"
	//	,__func__,task_pid(current),commit_tid); 
	
	if (datasync)
		ret = file_write_and_wait_range(file, start, end);
	else
		ret = filemap_write_and_dispatch_range(file->f_mapping, start, end);
	if (ret)
		goto out;
	
	// trace_ext4_sync_file_data_complete(inode, datasync);

	/*
	 * data=writeback,ordered:
	 *  The caller's filemap_fdatawrite()/wait will sync the data.
	 *  Metadata is in the journal, we wait for proper transaction to
	 *  commit here.
	 *
	 * data=journal:
	 *  filemap_fdatawrite won't do anything (the buffers are clean).
	 *  ext4_force_commit will write the file data into the journal and
	 *  will wait on that.
	 *  filemap_fdatawait() will encounter a ton of newly-dirtied pages
	 *  (they were dirtied by commit).  But that's OK - the blocks are
	 *  safe in-journal, which is all fsync() needs to ensure.
	 */
#ifdef DEBUG_FSYNC_LATENCY
	//read_lock(&journal->j_state_lock);
	//spin_lock(&journal->j_list_lock);
	temp.fsync_intv[1] = 0; // journal->j_commit_sequence;
	temp.fsync_intv[2] = 0; // journal->j_transfer_sequence;
	temp.fsync_intv[3] = 0; // journal->j_flush_sequence;
	//spin_unlock(&journal->j_list_lock);
	//read_unlock(&journal->j_state_lock);
	temp.fsync_intv[4] = 0; // datasync ? ei->i_datasync_tid : ei->i_sync_tid; 
#endif
	if (!sbi->s_journal)
		ret = ext4_fsync_nojournal(inode, datasync, &needs_barrier);
	else if (ext4_should_journal_data(inode))
		ret = ext4_force_commit(inode->i_sb);
	else /* Need to be modified after Dual Mode Journaling is ported */
		ret = ext4_fsync_journal(inode, datasync, &needs_barrier);

	if (needs_barrier) {
		err = blkdev_issue_flush(inode->i_sb->s_bdev);
		if (!ret)
			ret = err;
	}
	
	// commit_tid = datasync ? ei->i_datasync_tid : ei->i_sync_tid;
	//printk(KERN_INFO "[SWDBG] (%s) sync_end! pid : %d, tid : %d\n"
	//	,__func__,task_pid(current),commit_tid); 
out:
	err = file_check_and_advance_wb_err(file);
	if (ret == 0)
		ret = err;
	trace_ext4_sync_file_exit(inode, ret);
#ifdef DEBUG_FSYNC_LATENCY
	end1 = ktime_get();
	seq = atomic_add_return(1, &fsync_index);
	if (seq >= 4000000)
		return ret;
	temp.fsync_intv[0] += ktime_to_ns(ktime_sub(end1,start1));
	for (j = 0; j < 1; j++) {                                  
        	fsync_array[seq-1].fsync_intv[j] = temp.fsync_intv[j];
	}                                                      
        fsync_array[seq-1].fsync_intv[1] = temp.fsync_intv[1];
        fsync_array[seq-1].fsync_intv[2] = temp.fsync_intv[2];
        fsync_array[seq-1].fsync_intv[3] = temp.fsync_intv[3];
        fsync_array[seq-1].fsync_intv[4] = temp.fsync_intv[4];
#endif
	return ret;
}

/* UFS */
int ext4_fbarrier_file(struct file *file, loff_t start, loff_t end, int datasync)
{
	int ret = 0, err;
	bool needs_barrier = false;
	struct inode *inode = file->f_mapping->host;
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);

	if (unlikely(ext4_forced_shutdown(sbi)))
		return -EIO;

	ASSERT(ext4_journal_current_handle() == NULL);

	trace_ext4_sync_file_enter(file, datasync);

	if (sb_rdonly(inode->i_sb)) {
		/* Make sure that we read updated s_mount_flags value */
		smp_rmb();
		if (ext4_test_mount_flag(inode->i_sb, EXT4_MF_FS_ABORTED))
			ret = -EROFS;
		goto out;
	}

	if (datasync) {
		current->barrier_fail = 0;
		ret = filemap_ordered_write_range(file->f_mapping, start, end);
		if (current->barrier_fail)
			needs_barrier = true;
		filemap_fdatadispatch_range(file->f_mapping, start, end);
	}
	else
		ret = filemap_write_and_dispatch_range(file->f_mapping, start, end);
	if (ret)
		goto out;

	/*
	 * data=writeback,ordered:
	 *  The caller's filemap_fdatawrite()/wait will sync the data.
	 *  Metadata is in the journal, we wait for proper transaction to
	 *  commit here.
	 *
	 * data=journal:
	 *  filemap_fdatawrite won't do anything (the buffers are clean).
	 *  ext4_force_commit will write the file data into the journal and
	 *  will wait on that.
	 *  filemap_fdatawait() will encounter a ton of newly-dirtied pages
	 *  (they were dirtied by commit).  But that's OK - the blocks are
	 *  safe in-journal, which is all fsync() needs to ensure.
	 */
	if (!sbi->s_journal)
		ret = ext4_fsync_nojournal(inode, datasync, &needs_barrier);
	else if (ext4_should_journal_data(inode))
		ret = ext4_force_commit(inode->i_sb);
	else {/* Need to be modified after Dual Mode Journaling is ported */
		if (datasync && needs_barrier) 
			current->barrier_fail = 0;
		// ret = ext4_fsync_journal(inode, datasync, &needs_barrier);
	}
out:
	// err = file_check_and_advance_wb_err(file);
	if (ret == 0)
		ret = err;
	trace_ext4_sync_file_exit(inode, ret);
	return ret;
}
