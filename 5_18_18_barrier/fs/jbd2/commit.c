// SPDX-License-Identifier: GPL-2.0+
/*
 * linux/fs/jbd2/commit.c
 *
 * Written by Stephen C. Tweedie <sct@redhat.com>, 1998
 *
 * Copyright 1998 Red Hat corp --- All Rights Reserved
 *
 * Journal commit routines for the generic filesystem journaling code;
 * part of the ext2fs journaling system.
 */

#include <linux/time.h>
#include <linux/fs.h>
#include <linux/jbd2.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/jiffies.h>
#include <linux/crc32.h>
#include <linux/writeback.h>
#include <linux/backing-dev.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/bitops.h>
#include <trace/events/jbd2.h>

/*
 * IO end handler for temporary buffer_heads handling writes to the journal.
 */
static void journal_end_buffer_io_sync(struct buffer_head *bh, int uptodate)
{
	struct buffer_head *orig_bh = bh->b_private;

	BUFFER_TRACE(bh, "");
	if (uptodate)
		set_buffer_uptodate(bh);
	else
		clear_buffer_uptodate(bh);
	if (orig_bh) {
		clear_bit_unlock(BH_Shadow, &orig_bh->b_state);
		smp_mb__after_atomic();
		wake_up_bit(&orig_bh->b_state, BH_Shadow);
	}
	unlock_buffer(bh);
}

/*
 * When an ext4 file is truncated, it is possible that some pages are not
 * successfully freed, because they are attached to a committing transaction.
 * After the transaction commits, these pages are left on the LRU, with no
 * ->mapping, and with attached buffers.  These pages are trivially reclaimable
 * by the VM, but their apparent absence upsets the VM accounting, and it makes
 * the numbers in /proc/meminfo look odd.
 *
 * So here, we have a buffer which has just come off the forget list.  Look to
 * see if we can strip all buffers from the backing page.
 *
 * Called under lock_journal(), and possibly under journal_datalist_lock.  The
 * caller provided us with a ref against the buffer, and we drop that here.
 */
static void release_buffer_page(struct buffer_head *bh)
{
	struct page *page;

	if (buffer_dirty(bh))
		goto nope;
	if (atomic_read(&bh->b_count) != 1)
		goto nope;
	page = bh->b_page;
	if (!page)
		goto nope;
	if (page->mapping)
		goto nope;

	/* OK, it's a truncated page */
	if (!trylock_page(page))
		goto nope;

	get_page(page);
	__brelse(bh);
	try_to_free_buffers(page);
	unlock_page(page);
	put_page(page);
	return;

nope:
	__brelse(bh);
}

static void jbd2_commit_block_csum_set(journal_t *j, struct buffer_head *bh)
{
	struct commit_header *h;
	__u32 csum;

	if (!jbd2_journal_has_csum_v2or3(j))
		return;

	h = (struct commit_header *)(bh->b_data);
	h->h_chksum_type = 0;
	h->h_chksum_size = 0;
	h->h_chksum[0] = 0;
	csum = jbd2_chksum(j, j->j_csum_seed, bh->b_data, j->j_blocksize);
	h->h_chksum[0] = cpu_to_be32(csum);
}

/*
 * Done it all: now submit the commit record.  We should have
 * cleaned up our previous buffers by now, so if we are in abort
 * mode we can now just skip the rest of the journal write
 * entirely.
 *
 * Returns 1 if the journal needs to be aborted or 0 on success
 */
static int journal_submit_commit_record(journal_t *journal,
					transaction_t *commit_transaction,
					struct buffer_head **cbh,
					__u32 crc32_sum)
{
	struct commit_header *tmp;
	struct buffer_head *bh;
	int ret;
	struct timespec64 now;

	*cbh = NULL;

	if (is_journal_aborted(journal))
		return 0;

	bh = jbd2_journal_get_descriptor_buffer(commit_transaction,
						JBD2_COMMIT_BLOCK);
	if (!bh)
		return 1;

	tmp = (struct commit_header *)bh->b_data;
	ktime_get_coarse_real_ts64(&now);
	tmp->h_commit_sec = cpu_to_be64(now.tv_sec);
	tmp->h_commit_nsec = cpu_to_be32(now.tv_nsec);

	if (jbd2_has_feature_checksum(journal)) {
		tmp->h_chksum_type 	= JBD2_CRC32_CHKSUM;
		tmp->h_chksum_size 	= JBD2_CRC32_CHKSUM_SIZE;
		tmp->h_chksum[0] 	= cpu_to_be32(crc32_sum);
	}
	jbd2_commit_block_csum_set(journal, bh);

	BUFFER_TRACE(bh, "submit commit block");
	lock_buffer(bh);
	clear_buffer_dirty(bh);
	set_buffer_uptodate(bh);
	bh->b_end_io = journal_end_buffer_io_sync;
				
	if (journal->j_flags & JBD2_BARRIER &&
	    !jbd2_has_feature_async_commit(journal))
		ret = submit_bh(REQ_OP_WRITE,
			REQ_SYNC | REQ_ORDERED | REQ_BARRIER, bh);
	else
		ret = submit_bh(REQ_OP_WRITE, REQ_SYNC, bh);

	*cbh = bh;
	return ret;
}

/*
 * This function along with journal_submit_commit_record
 * allows to write the commit record asynchronously.
 */
static int journal_wait_on_commit_record(journal_t *journal,
					 struct buffer_head *bh)
{
	int ret = 0;

	clear_buffer_dirty(bh);
	wait_on_buffer(bh);

	if (unlikely(!buffer_uptodate(bh)))
		ret = -EIO;
	put_bh(bh);            /* One for getblk() */

	return ret;
}

/* UFS : Wrapping Function of dispatch checking of Journal Commit Record */
static int journal_wait_on_dispatch_of_commit_record(journal_t *journal,
					 struct buffer_head *bh)
{
	wait_on_buffer_dispatch(bh);

	return 0;
}

/*
 * write the filemap data using writepage() address_space_operations.
 * We don't do block allocation here even for delalloc. We don't
 * use writepages() because with delayed allocation we may be doing
 * block allocation in writepages().
 */
int jbd2_journal_submit_inode_data_buffers(struct jbd2_inode *jinode)
{
	struct address_space *mapping = jinode->i_vfs_inode->i_mapping;
	struct writeback_control wbc = {
		.sync_mode =  WB_SYNC_ALL,
		.nr_to_write = mapping->nrpages * 2,
		.range_start = jinode->i_dirty_start,
		.range_end = jinode->i_dirty_end,
	};

	/*
	 * submit the inode data buffers. We use writepage
	 * instead of writepages. Because writepages can do
	 * block allocation with delalloc. We need to write
	 * only allocated blocks here.
	 */
	return generic_writepages(mapping, &wbc);
}

/* Send all the data buffers related to an inode */
int jbd2_submit_inode_data(struct jbd2_inode *jinode)
{

	if (!jinode || !(jinode->i_flags & JI_WRITE_DATA))
		return 0;

	trace_jbd2_submit_inode_data(jinode->i_vfs_inode);
	return jbd2_journal_submit_inode_data_buffers(jinode);

}
EXPORT_SYMBOL(jbd2_submit_inode_data);

int jbd2_wait_inode_data(journal_t *journal, struct jbd2_inode *jinode)
{
	if (!jinode || !(jinode->i_flags & JI_WAIT_DATA) ||
		!jinode->i_vfs_inode || !jinode->i_vfs_inode->i_mapping)
		return 0;
	return filemap_fdatawait_range_keep_errors(
		jinode->i_vfs_inode->i_mapping, jinode->i_dirty_start,
		jinode->i_dirty_end);
}
EXPORT_SYMBOL(jbd2_wait_inode_data);

/*
 * Submit all the data buffers of inode associated with the transaction to
 * disk.
 *
 * We are in a committing transaction. Therefore no new inode can be added to
 * our inode list. We use JI_COMMIT_RUNNING flag to protect inode we currently
 * operate on from being released while we write out pages.
 */
static int journal_submit_data_buffers(journal_t *journal,
		transaction_t *commit_transaction)
{
	struct jbd2_inode *jinode;
	int err, ret = 0;

	spin_lock(&journal->j_list_lock);
	list_for_each_entry(jinode, &commit_transaction->t_inode_list, i_list) {
		if (!(jinode->i_flags & JI_WRITE_DATA))
			continue;
		jinode->i_flags |= JI_COMMIT_RUNNING;
		spin_unlock(&journal->j_list_lock);
		/* submit the inode data buffers. */
		trace_jbd2_submit_inode_data(jinode->i_vfs_inode);
		if (journal->j_submit_inode_data_buffers) {
			err = journal->j_submit_inode_data_buffers(jinode);
			if (!ret)
				ret = err;
		}
		spin_lock(&journal->j_list_lock);
		J_ASSERT(jinode->i_transaction == commit_transaction);
		jinode->i_flags &= ~JI_COMMIT_RUNNING;
		smp_mb();
		wake_up_bit(&jinode->i_flags, __JI_COMMIT_RUNNING);
	}
	spin_unlock(&journal->j_list_lock);
	return ret;
}

int jbd2_journal_finish_inode_data_buffers(struct jbd2_inode *jinode)
{
	struct address_space *mapping = jinode->i_vfs_inode->i_mapping;

	return filemap_fdatawait_range_keep_errors(mapping,
						   jinode->i_dirty_start,
						   jinode->i_dirty_end);
}

/*
 * Wait for data submitted for writeout, refile inodes to proper
 * transaction if needed.
 *
 */
static int journal_finish_inode_data_buffers(journal_t *journal,
		transaction_t *commit_transaction)
{
	struct jbd2_inode *jinode, *next_i;
	int err, ret = 0;

	/* For locking, see the comment in journal_submit_data_buffers() */
	spin_lock(&journal->j_list_lock);
	list_for_each_entry(jinode, &commit_transaction->t_inode_list, i_list) {
		if (!(jinode->i_flags & JI_WAIT_DATA))
			continue;
		jinode->i_flags |= JI_COMMIT_RUNNING;
		spin_unlock(&journal->j_list_lock);
		/* wait for the inode data buffers writeout. */
		if (journal->j_finish_inode_data_buffers) {
			err = journal->j_finish_inode_data_buffers(jinode);
			if (!ret)
				ret = err;
		}
		spin_lock(&journal->j_list_lock);
		jinode->i_flags &= ~JI_COMMIT_RUNNING;
		smp_mb();
		wake_up_bit(&jinode->i_flags, __JI_COMMIT_RUNNING);
	}

	/* Now refile inode to proper lists */
	list_for_each_entry_safe(jinode, next_i,
				 &commit_transaction->t_inode_list, i_list) {
		list_del(&jinode->i_list);
		if (jinode->i_next_transaction) {
			jinode->i_transaction = jinode->i_next_transaction;
			jinode->i_next_transaction = NULL;
			list_add(&jinode->i_list,
				&jinode->i_transaction->t_inode_list);
		} else {
			jinode->i_transaction = NULL;
			jinode->i_dirty_start = 0;
			jinode->i_dirty_end = 0;
		}
	}
	spin_unlock(&journal->j_list_lock);

	return ret;
}

static __u32 jbd2_checksum_data(__u32 crc32_sum, struct buffer_head *bh)
{
	struct page *page = bh->b_page;
	char *addr;
	__u32 checksum;

	addr = kmap_atomic(page);
	checksum = crc32_be(crc32_sum,
		(void *)(addr + offset_in_page(bh->b_data)), bh->b_size);
	kunmap_atomic(addr);

	return checksum;
}

static void write_tag_block(journal_t *j, journal_block_tag_t *tag,
				   unsigned long long block)
{
	tag->t_blocknr = cpu_to_be32(block & (u32)~0);
	if (jbd2_has_feature_64bit(j))
		tag->t_blocknr_high = cpu_to_be32((block >> 31) >> 1);
}

static void jbd2_block_tag_csum_set(journal_t *j, journal_block_tag_t *tag,
				    struct buffer_head *bh, __u32 sequence)
{
	journal_block_tag3_t *tag3 = (journal_block_tag3_t *)tag;
	struct page *page = bh->b_page;
	__u8 *addr;
	__u32 csum32;
	__be32 seq;

	if (!jbd2_journal_has_csum_v2or3(j))
		return;

	seq = cpu_to_be32(sequence);
	addr = kmap_atomic(page);
	csum32 = jbd2_chksum(j, j->j_csum_seed, (__u8 *)&seq, sizeof(seq));
	csum32 = jbd2_chksum(j, csum32, addr + offset_in_page(bh->b_data),
			     bh->b_size);
	kunmap_atomic(addr);

	if (jbd2_has_feature_csum3(j))
		tag3->t_checksum = cpu_to_be32(csum32);
	else
		tag->t_checksum = cpu_to_be16(csum32);
}
/*
 * jbd2_journal_commit_transaction
 *
 * The primary function for committing a transaction to the log.  This
 * function is called by the journal thread to begin a complete commit.
 */
void jbd2_journal_commit_transaction(journal_t *journal)
{
	struct transaction_stats_s stats;
	transaction_t *commit_transaction;
	struct journal_head *jh;
	struct buffer_head *descriptor;
	struct buffer_head **wbuf = journal->j_wbuf;
	int bufs;
	int flags;
	int err;
	unsigned long long blocknr;
	ktime_t start_time;
	u64 commit_time;
	char *tagp = NULL;
	journal_block_tag_t *tag = NULL;
	int space_left = 0;
	int first_tag = 0;
	int tag_flag;
	int i;
	int tag_bytes = journal_tag_bytes(journal);
	struct buffer_head *cbh = NULL; /* For transactional checksums */
	__u32 crc32_sum = ~0;
	struct blk_plug plug;
	/* Tail of the journal */
	unsigned long first_block;
	tid_t first_tid;
	int csum_size = 0;
	LIST_HEAD(io_bufs);
	LIST_HEAD(log_bufs);
	int update_tail;

	if (jbd2_journal_has_csum_v2or3(journal))
		csum_size = sizeof(struct jbd2_journal_block_tail);

	/*
	 * First job: lock down the current transaction and wait for
	 * all outstanding updates to complete.
	 */

	/* Do we need to erase the effects of a prior jbd2_journal_flush? */
	if (journal->j_flags & JBD2_FLUSHED) {
		jbd_debug(3, "super block updated\n");
		mutex_lock_io(&journal->j_checkpoint_mutex);
		/*
		 * We hold j_checkpoint_mutex so tail cannot change under us.
		 * We don't need any special data guarantees for writing sb
		 * since journal is empty and it is ok for write to be
		 * flushed only with transaction commit.
		 */
		jbd2_journal_update_sb_log_tail(journal,
						journal->j_tail_sequence,
						journal->j_tail,
						REQ_SYNC);
		mutex_unlock(&journal->j_checkpoint_mutex);
	} else {
		jbd_debug(3, "superblock not updated\n");
	}

	J_ASSERT(journal->j_running_transaction != NULL);
	J_ASSERT(journal->j_committing_transaction == NULL);

	write_lock(&journal->j_state_lock);
	journal->j_flags |= JBD2_FULL_COMMIT_ONGOING;
	while (journal->j_flags & JBD2_FAST_COMMIT_ONGOING) {
		DEFINE_WAIT(wait);

		prepare_to_wait(&journal->j_fc_wait, &wait,
				TASK_UNINTERRUPTIBLE);
		write_unlock(&journal->j_state_lock);
		schedule();
		write_lock(&journal->j_state_lock);
		finish_wait(&journal->j_fc_wait, &wait);
		/*
		 * TODO: by blocking fast commits here, we are increasing
		 * fsync() latency slightly. Strictly speaking, we don't need
		 * to block fast commits until the transaction enters T_FLUSH
		 * state. So an optimization is possible where we block new fast
		 * commits here and wait for existing ones to complete
		 * just before we enter T_FLUSH. That way, the existing fast
		 * commits and this full commit can proceed parallely.
		 */
	}
	write_unlock(&journal->j_state_lock);

	commit_transaction = journal->j_running_transaction;

	trace_jbd2_start_commit(journal, commit_transaction);
	jbd_debug(1, "JBD2: starting commit of transaction %d\n",
			commit_transaction->t_tid);

	write_lock(&journal->j_state_lock);
	journal->j_fc_off = 0;
	J_ASSERT(commit_transaction->t_state == T_RUNNING);
	commit_transaction->t_state = T_LOCKED;

	trace_jbd2_commit_locking(journal, commit_transaction);
	stats.run.rs_wait = commit_transaction->t_max_wait;
	stats.run.rs_request_delay = 0;
	stats.run.rs_locked = jiffies;
	if (commit_transaction->t_requested)
		stats.run.rs_request_delay =
			jbd2_time_diff(commit_transaction->t_requested,
				       stats.run.rs_locked);
	stats.run.rs_running = jbd2_time_diff(commit_transaction->t_start,
					      stats.run.rs_locked);

	// waits for any t_updates to finish
	jbd2_journal_wait_updates(journal);

	commit_transaction->t_state = T_SWITCH;

	J_ASSERT (atomic_read(&commit_transaction->t_outstanding_credits) <=
			journal->j_max_transaction_buffers);

	/*
	 * First thing we are allowed to do is to discard any remaining
	 * BJ_Reserved buffers.  Note, it is _not_ permissible to assume
	 * that there are no such buffers: if a large filesystem
	 * operation like a truncate needs to split itself over multiple
	 * transactions, then it may try to do a jbd2_journal_restart() while
	 * there are still BJ_Reserved buffers outstanding.  These must
	 * be released cleanly from the current transaction.
	 *
	 * In this case, the filesystem must still reserve write access
	 * again before modifying the buffer in the new transaction, but
	 * we do not require it to remember exactly which old buffers it
	 * has reserved.  This is consistent with the existing behaviour
	 * that multiple jbd2_journal_get_write_access() calls to the same
	 * buffer are perfectly permissible.
	 * We use journal->j_state_lock here to serialize processing of
	 * t_reserved_list with eviction of buffers from journal_unmap_buffer().
	 */
	while (commit_transaction->t_reserved_list) {
		jh = commit_transaction->t_reserved_list;
		JBUFFER_TRACE(jh, "reserved, unused: refile");
		/*
		 * A jbd2_journal_get_undo_access()+jbd2_journal_release_buffer() may
		 * leave undo-committed data.
		 */
		if (jh->b_committed_data) {
			struct buffer_head *bh = jh2bh(jh);

			spin_lock(&jh->b_state_lock);
			jbd2_free(jh->b_committed_data, bh->b_size);
			jh->b_committed_data = NULL;
			spin_unlock(&jh->b_state_lock);
		}
		jbd2_journal_refile_buffer(journal, jh);
	}

	write_unlock(&journal->j_state_lock);
	/*
	 * Now try to drop any written-back buffers from the journal's
	 * checkpoint lists.  We do this *before* commit because it potentially
	 * frees some memory
	 */
	spin_lock(&journal->j_list_lock);
	__jbd2_journal_clean_checkpoint_list(journal, false);
	spin_unlock(&journal->j_list_lock);

	jbd_debug(3, "JBD2: commit phase 1\n");

	/*
	 * Clear revoked flag to reflect there is no revoked buffers
	 * in the next transaction which is going to be started.
	 */
	jbd2_clear_buffer_revoked_flags(journal);

	/*
	 * Switch to a new revoke table.
	 */
	jbd2_journal_switch_revoke_table(journal);

	write_lock(&journal->j_state_lock);
	/*
	 * Reserved credits cannot be claimed anymore, free them
	 */
	atomic_sub(atomic_read(&journal->j_reserved_credits),
		   &commit_transaction->t_outstanding_credits);

	trace_jbd2_commit_flushing(journal, commit_transaction);
	stats.run.rs_flushing = jiffies;
	stats.run.rs_locked = jbd2_time_diff(stats.run.rs_locked,
					     stats.run.rs_flushing);

	commit_transaction->t_state = T_FLUSH;
	journal->j_committing_transaction = commit_transaction;
	journal->j_running_transaction = NULL;
	start_time = ktime_get();
	commit_transaction->t_log_start = journal->j_head;
	wake_up(&journal->j_wait_transaction_locked);
	write_unlock(&journal->j_state_lock);

	jbd_debug(3, "JBD2: commit phase 2a\n");

	/*
	 * Now start flushing things to disk, in the order they appear
	 * on the transaction lists.  Data blocks go first.
	 */
	err = journal_submit_data_buffers(journal, commit_transaction);
	if (err)
		jbd2_journal_abort(journal, err);

	blk_start_plug(&plug);
	jbd2_journal_write_revoke_records(commit_transaction, &log_bufs);

	jbd_debug(3, "JBD2: commit phase 2b\n");

	/*
	 * Way to go: we have now written out all of the data for a
	 * transaction!  Now comes the tricky part: we need to write out
	 * metadata.  Loop over the transaction's entire buffer list:
	 */
	write_lock(&journal->j_state_lock);
	commit_transaction->t_state = T_COMMIT;
	write_unlock(&journal->j_state_lock);

	trace_jbd2_commit_logging(journal, commit_transaction);
	stats.run.rs_logging = jiffies;
	stats.run.rs_flushing = jbd2_time_diff(stats.run.rs_flushing,
					       stats.run.rs_logging);
	stats.run.rs_blocks = commit_transaction->t_nr_buffers;
	stats.run.rs_blocks_logged = 0;

	J_ASSERT(commit_transaction->t_nr_buffers <=
		 atomic_read(&commit_transaction->t_outstanding_credits));

	err = 0;
	bufs = 0;
	descriptor = NULL;
	while (commit_transaction->t_buffers) {

		/* Find the next buffer to be journaled... */

		jh = commit_transaction->t_buffers;

		/* If we're in abort mode, we just un-journal the buffer and
		   release it. */

		if (is_journal_aborted(journal)) {
			clear_buffer_jbddirty(jh2bh(jh));
			JBUFFER_TRACE(jh, "journal is aborting: refile");
			jbd2_buffer_abort_trigger(jh,
						  jh->b_frozen_data ?
						  jh->b_frozen_triggers :
						  jh->b_triggers);
			jbd2_journal_refile_buffer(journal, jh);
			/* If that was the last one, we need to clean up
			 * any descriptor buffers which may have been
			 * already allocated, even if we are now
			 * aborting. */
			if (!commit_transaction->t_buffers)
				goto start_journal_io;
			continue;
		}

		/* Make sure we have a descriptor block in which to
		   record the metadata buffer. */

		if (!descriptor) {
			J_ASSERT (bufs == 0);

			jbd_debug(4, "JBD2: get descriptor\n");

			descriptor = jbd2_journal_get_descriptor_buffer(
							commit_transaction,
							JBD2_DESCRIPTOR_BLOCK);
			if (!descriptor) {
				jbd2_journal_abort(journal, -EIO);
				continue;
			}

			jbd_debug(4, "JBD2: got buffer %llu (%p)\n",
				(unsigned long long)descriptor->b_blocknr,
				descriptor->b_data);
			tagp = &descriptor->b_data[sizeof(journal_header_t)];
			space_left = descriptor->b_size -
						sizeof(journal_header_t);
			first_tag = 1;
			set_buffer_jwrite(descriptor);
			set_buffer_dirty(descriptor);
			wbuf[bufs++] = descriptor;

			/* Record it so that we can wait for IO
                           completion later */
			BUFFER_TRACE(descriptor, "ph3: file as descriptor");
			jbd2_file_log_bh(&log_bufs, descriptor);
		}

		/* Where is the buffer to be written? */

		err = jbd2_journal_next_log_block(journal, &blocknr);
		/* If the block mapping failed, just abandon the buffer
		   and repeat this loop: we'll fall into the
		   refile-on-abort condition above. */
		if (err) {
			jbd2_journal_abort(journal, err);
			continue;
		}

		/*
		 * start_this_handle() uses t_outstanding_credits to determine
		 * the free space in the log.
		 */
		atomic_dec(&commit_transaction->t_outstanding_credits);

		/* Bump b_count to prevent truncate from stumbling over
                   the shadowed buffer!  @@@ This can go if we ever get
                   rid of the shadow pairing of buffers. */
		atomic_inc(&jh2bh(jh)->b_count);

		/*
		 * Make a temporary IO buffer with which to write it out
		 * (this will requeue the metadata buffer to BJ_Shadow).
		 */
		set_bit(BH_JWrite, &jh2bh(jh)->b_state);
		JBUFFER_TRACE(jh, "ph3: write metadata");
		flags = jbd2_journal_write_metadata_buffer(commit_transaction,
						jh, &wbuf[bufs], blocknr);
		if (flags < 0) {
			jbd2_journal_abort(journal, flags);
			continue;
		}
		jbd2_file_log_bh(&io_bufs, wbuf[bufs]);

		/* Record the new block's tag in the current descriptor
                   buffer */

		tag_flag = 0;
		if (flags & 1)
			tag_flag |= JBD2_FLAG_ESCAPE;
		if (!first_tag)
			tag_flag |= JBD2_FLAG_SAME_UUID;

		tag = (journal_block_tag_t *) tagp;
		write_tag_block(journal, tag, jh2bh(jh)->b_blocknr);
		tag->t_flags = cpu_to_be16(tag_flag);
		jbd2_block_tag_csum_set(journal, tag, wbuf[bufs],
					commit_transaction->t_tid);
		tagp += tag_bytes;
		space_left -= tag_bytes;
		bufs++;

		if (first_tag) {
			memcpy (tagp, journal->j_uuid, 16);
			tagp += 16;
			space_left -= 16;
			first_tag = 0;
		}

		/* If there's no more to do, or if the descriptor is full,
		   let the IO rip! */

		if (bufs == journal->j_wbufsize ||
		    commit_transaction->t_buffers == NULL ||
		    space_left < tag_bytes + 16 + csum_size) {

			jbd_debug(4, "JBD2: Submit %d IOs\n", bufs);

			/* Write an end-of-descriptor marker before
                           submitting the IOs.  "tag" still points to
                           the last tag we set up. */

			tag->t_flags |= cpu_to_be16(JBD2_FLAG_LAST_TAG);
start_journal_io:
			if (descriptor)
				jbd2_descriptor_block_csum_set(journal,
							descriptor);

			for (i = 0; i < bufs; i++) {
				struct buffer_head *bh = wbuf[i];
				/*
				 * Compute checksum.
				 */
				if (jbd2_has_feature_checksum(journal)) {
					crc32_sum =
					    jbd2_checksum_data(crc32_sum, bh);
				}

				lock_buffer(bh);
				clear_buffer_dirty(bh);
				set_buffer_uptodate(bh);
				bh->b_end_io = journal_end_buffer_io_sync;
				/* UFS */
				bh->b_tx = bh->b_tx == NULL ? commit_transaction : bh->b_tx;
				if (commit_transaction->t_buffers != NULL) {
					submit_bh(REQ_OP_WRITE, REQ_SYNC | REQ_ORDERED, bh);
				} else if (i == bufs - 1 && commit_transaction->t_buffers == NULL) {
					submit_bh(REQ_OP_WRITE, 
						REQ_SYNC | REQ_ORDERED | REQ_BARRIER, bh);
				} else {
					submit_bh(REQ_OP_WRITE, REQ_SYNC | REQ_ORDERED, bh);
				}
				// submit_bh(REQ_OP_WRITE, REQ_SYNC | REQ_ORDERED, bh);
			}
			cond_resched();

			/* Force a new descriptor to be generated next
                           time round the loop. */
			descriptor = NULL;
			bufs = 0;
		}
	}

	err = journal_finish_inode_data_buffers(journal, commit_transaction);
	if (err) {
		printk(KERN_WARNING
			"JBD2: Detected IO errors while flushing file data "
		       "on %s\n", journal->j_devname);
		if (journal->j_flags & JBD2_ABORT_ON_SYNCDATA_ERR)
			jbd2_journal_abort(journal, err);
		err = 0;
	}

	/*
	 * Get current oldest transaction in the log before we issue flush
	 * to the filesystem device. After the flush we can be sure that
	 * blocks of all older transactions are checkpointed to persistent
	 * storage and we will be safe to update journal start in the
	 * superblock with the numbers we get here.
	 */
	update_tail =
		jbd2_journal_get_log_tail(journal, &first_tid, &first_block);

	write_lock(&journal->j_state_lock);
	if (update_tail) {
		long freed = first_block - journal->j_tail;

		if (first_block < journal->j_tail)
			freed += journal->j_last - journal->j_first;
		/* Update tail only if we free significant amount of space */
		if (freed < jbd2_journal_get_max_txn_bufs(journal))
			update_tail = 0;
	}
	J_ASSERT(commit_transaction->t_state == T_COMMIT);
	commit_transaction->t_state = T_COMMIT_DFLUSH;
	write_unlock(&journal->j_state_lock);

	/*
	 * If the journal is not located on the file system device,
	 * then we must flush the file system device before we issue
	 * the commit record
	 */
	if (commit_transaction->t_need_data_flush &&
	    (journal->j_fs_dev != journal->j_dev) &&
	    (journal->j_flags & JBD2_BARRIER))
		blkdev_issue_flush(journal->j_fs_dev);

	/* Done it all: now write the commit record asynchronously. */
	if (jbd2_has_feature_async_commit(journal)) {
		err = journal_submit_commit_record(journal, commit_transaction,
						 &cbh, crc32_sum);
		if (err)
			jbd2_journal_abort(journal, err);
	}

	blk_finish_plug(&plug);

	/* Lo and behold: we have just managed to send a transaction to
           the log.  Before we can commit it, wait for the IO so far to
           complete.  Control buffers being written are on the
           transaction's t_log_list queue, and metadata buffers are on
           the io_bufs list.

	   Wait for the buffers in reverse order.  That way we are
	   less likely to be woken up until all IOs have completed, and
	   so we incur less scheduling load.
	*/

	jbd_debug(3, "JBD2: commit phase 3\n");

	while (!list_empty(&io_bufs)) {
		struct buffer_head *bh = list_entry(io_bufs.prev,
						    struct buffer_head,
						    b_assoc_buffers);

		wait_on_buffer(bh);
		cond_resched();

		if (unlikely(!buffer_uptodate(bh)))
			err = -EIO;
		jbd2_unfile_log_bh(bh);
		stats.run.rs_blocks_logged++;

		/*
		 * The list contains temporary buffer heads created by
		 * jbd2_journal_write_metadata_buffer().
		 */
		BUFFER_TRACE(bh, "dumping temporary bh");
		__brelse(bh);
		J_ASSERT_BH(bh, atomic_read(&bh->b_count) == 0);
		free_buffer_head(bh);

		/* We also have to refile the corresponding shadowed buffer */
		jh = commit_transaction->t_shadow_list->b_tprev;
		bh = jh2bh(jh);
		clear_buffer_jwrite(bh);
		J_ASSERT_BH(bh, buffer_jbddirty(bh));
		J_ASSERT_BH(bh, !buffer_shadow(bh));

		/* The metadata is now released for reuse, but we need
                   to remember it against this transaction so that when
                   we finally commit, we can do any checkpointing
                   required. */
		JBUFFER_TRACE(jh, "file as BJ_Forget");
		jbd2_journal_file_buffer(jh, commit_transaction, BJ_Forget);
		JBUFFER_TRACE(jh, "brelse shadowed buffer");
		__brelse(bh);
	}

	J_ASSERT (commit_transaction->t_shadow_list == NULL);

	jbd_debug(3, "JBD2: commit phase 4\n");

	/* Here we wait for the revoke record and descriptor record buffers */
	while (!list_empty(&log_bufs)) {
		struct buffer_head *bh;

		bh = list_entry(log_bufs.prev, struct buffer_head, b_assoc_buffers);
		wait_on_buffer(bh);
		cond_resched();

		if (unlikely(!buffer_uptodate(bh)))
			err = -EIO;

		BUFFER_TRACE(bh, "ph5: control buffer writeout done: unfile");
		clear_buffer_jwrite(bh);
		jbd2_unfile_log_bh(bh);
		stats.run.rs_blocks_logged++;
		__brelse(bh);		/* One for getblk */
		/* AKPM: bforget here */
	}

	if (err)
		jbd2_journal_abort(journal, err);

	jbd_debug(3, "JBD2: commit phase 5\n");
	write_lock(&journal->j_state_lock);
	J_ASSERT(commit_transaction->t_state == T_COMMIT_DFLUSH);
	commit_transaction->t_state = T_COMMIT_JFLUSH;
	write_unlock(&journal->j_state_lock);

	if (!jbd2_has_feature_async_commit(journal)) {
		err = journal_submit_commit_record(journal, commit_transaction,
						&cbh, crc32_sum);
		if (err)
			jbd2_journal_abort(journal, err);
	}
	if (cbh)
		err = journal_wait_on_commit_record(journal, cbh);
	stats.run.rs_blocks_logged++;

	/* UFS */
	if (journal->j_flags & JBD2_BARRIER)
		blkdev_issue_flush(journal->j_dev);

	if (jbd2_has_feature_async_commit(journal) &&
	    journal->j_flags & JBD2_BARRIER) {
		blkdev_issue_flush(journal->j_dev);
	}

	if (err)
		jbd2_journal_abort(journal, err);

	WARN_ON_ONCE(
		atomic_read(&commit_transaction->t_outstanding_credits) < 0);

	/*
	 * Now disk caches for filesystem device are flushed so we are safe to
	 * erase checkpointed transactions from the log by updating journal
	 * superblock.
	 */
	if (update_tail)
		jbd2_update_log_tail(journal, first_tid, first_block);

	/* End of a transaction!  Finally, we can do checkpoint
           processing: any buffers committed as a result of this
           transaction can be removed from any checkpoint list it was on
           before. */

	jbd_debug(3, "JBD2: commit phase 6\n");

	J_ASSERT(list_empty(&commit_transaction->t_inode_list));
	J_ASSERT(commit_transaction->t_buffers == NULL);
	J_ASSERT(commit_transaction->t_checkpoint_list == NULL);
	J_ASSERT(commit_transaction->t_shadow_list == NULL);

restart_loop:
	/*
	 * As there are other places (journal_unmap_buffer()) adding buffers
	 * to this list we have to be careful and hold the j_list_lock.
	 */
	spin_lock(&journal->j_list_lock);
	while (commit_transaction->t_forget) {
		transaction_t *cp_transaction;
		struct buffer_head *bh;
		int try_to_free = 0;
		bool drop_ref;

		jh = commit_transaction->t_forget;
		spin_unlock(&journal->j_list_lock);
		bh = jh2bh(jh);
		/*
		 * Get a reference so that bh cannot be freed before we are
		 * done with it.
		 */
		get_bh(bh);
		spin_lock(&jh->b_state_lock);
		J_ASSERT_JH(jh,	jh->b_transaction == commit_transaction);

		/*
		 * If there is undo-protected committed data against
		 * this buffer, then we can remove it now.  If it is a
		 * buffer needing such protection, the old frozen_data
		 * field now points to a committed version of the
		 * buffer, so rotate that field to the new committed
		 * data.
		 *
		 * Otherwise, we can just throw away the frozen data now.
		 *
		 * We also know that the frozen data has already fired
		 * its triggers if they exist, so we can clear that too.
		 */
		if (jh->b_committed_data) {
			jbd2_free(jh->b_committed_data, bh->b_size);
			jh->b_committed_data = NULL;
			if (jh->b_frozen_data) {
				jh->b_committed_data = jh->b_frozen_data;
				jh->b_frozen_data = NULL;
				jh->b_frozen_triggers = NULL;
			}
		} else if (jh->b_frozen_data) {
			jbd2_free(jh->b_frozen_data, bh->b_size);
			jh->b_frozen_data = NULL;
			jh->b_frozen_triggers = NULL;
		}

		spin_lock(&journal->j_list_lock);
		cp_transaction = jh->b_cp_transaction;
		if (cp_transaction) {
			JBUFFER_TRACE(jh, "remove from old cp transaction");
			cp_transaction->t_chp_stats.cs_dropped++;
			__jbd2_journal_remove_checkpoint(jh);
		}

		/* Only re-checkpoint the buffer_head if it is marked
		 * dirty.  If the buffer was added to the BJ_Forget list
		 * by jbd2_journal_forget, it may no longer be dirty and
		 * there's no point in keeping a checkpoint record for
		 * it. */

		/*
		 * A buffer which has been freed while still being journaled
		 * by a previous transaction, refile the buffer to BJ_Forget of
		 * the running transaction. If the just committed transaction
		 * contains "add to orphan" operation, we can completely
		 * invalidate the buffer now. We are rather through in that
		 * since the buffer may be still accessible when blocksize <
		 * pagesize and it is attached to the last partial page.
		 */
		if (buffer_freed(bh) && !jh->b_next_transaction) {
			struct address_space *mapping;

			clear_buffer_freed(bh);
			clear_buffer_jbddirty(bh);

			/*
			 * Block device buffers need to stay mapped all the
			 * time, so it is enough to clear buffer_jbddirty and
			 * buffer_freed bits. For the file mapping buffers (i.e.
			 * journalled data) we need to unmap buffer and clear
			 * more bits. We also need to be careful about the check
			 * because the data page mapping can get cleared under
			 * our hands. Note that if mapping == NULL, we don't
			 * need to make buffer unmapped because the page is
			 * already detached from the mapping and buffers cannot
			 * get reused.
			 */
			mapping = READ_ONCE(bh->b_page->mapping);
			if (mapping && !sb_is_blkdev_sb(mapping->host->i_sb)) {
				clear_buffer_mapped(bh);
				clear_buffer_new(bh);
				clear_buffer_req(bh);
				bh->b_bdev = NULL;
			}
		}

		if (buffer_jbddirty(bh)) {
			JBUFFER_TRACE(jh, "add to new checkpointing trans");
			__jbd2_journal_insert_checkpoint(jh, commit_transaction);
			if (is_journal_aborted(journal))
				clear_buffer_jbddirty(bh);
		} else {
			J_ASSERT_BH(bh, !buffer_dirty(bh));
			/*
			 * The buffer on BJ_Forget list and not jbddirty means
			 * it has been freed by this transaction and hence it
			 * could not have been reallocated until this
			 * transaction has committed. *BUT* it could be
			 * reallocated once we have written all the data to
			 * disk and before we process the buffer on BJ_Forget
			 * list.
			 */
			if (!jh->b_next_transaction)
				try_to_free = 1;
		}
		JBUFFER_TRACE(jh, "refile or unfile buffer");
		drop_ref = __jbd2_journal_refile_buffer(jh);
		spin_unlock(&jh->b_state_lock);
		if (drop_ref)
			jbd2_journal_put_journal_head(jh);
		if (try_to_free)
			release_buffer_page(bh);	/* Drops bh reference */
		else
			__brelse(bh);
		cond_resched_lock(&journal->j_list_lock);
	}
	spin_unlock(&journal->j_list_lock);
	/*
	 * This is a bit sleazy.  We use j_list_lock to protect transition
	 * of a transaction into T_FINISHED state and calling
	 * __jbd2_journal_drop_transaction(). Otherwise we could race with
	 * other checkpointing code processing the transaction...
	 */
	write_lock(&journal->j_state_lock);
	spin_lock(&journal->j_list_lock);
	/*
	 * Now recheck if some buffers did not get attached to the transaction
	 * while the lock was dropped...
	 */
	if (commit_transaction->t_forget) {
		spin_unlock(&journal->j_list_lock);
		write_unlock(&journal->j_state_lock);
		goto restart_loop;
	}

	/* Add the transaction to the checkpoint list
	 * __journal_remove_checkpoint() can not destroy transaction
	 * under us because it is not marked as T_FINISHED yet */
	if (journal->j_checkpoint_transactions == NULL) {
		journal->j_checkpoint_transactions = commit_transaction;
		commit_transaction->t_cpnext = commit_transaction;
		commit_transaction->t_cpprev = commit_transaction;
	} else {
		commit_transaction->t_cpnext =
			journal->j_checkpoint_transactions;
		commit_transaction->t_cpprev =
			commit_transaction->t_cpnext->t_cpprev;
		commit_transaction->t_cpnext->t_cpprev =
			commit_transaction;
		commit_transaction->t_cpprev->t_cpnext =
				commit_transaction;
	}
	spin_unlock(&journal->j_list_lock);

	/* Done with this transaction! */

	jbd_debug(3, "JBD2: commit phase 7\n");

	J_ASSERT(commit_transaction->t_state == T_COMMIT_JFLUSH);

	commit_transaction->t_start = jiffies;
	stats.run.rs_logging = jbd2_time_diff(stats.run.rs_logging,
					      commit_transaction->t_start);

	/*
	 * File the transaction statistics
	 */
	stats.ts_tid = commit_transaction->t_tid;
	stats.run.rs_handle_count =
		atomic_read(&commit_transaction->t_handle_count);
	trace_jbd2_run_stats(journal->j_fs_dev->bd_dev,
			     commit_transaction->t_tid, &stats.run);
	stats.ts_requested = (commit_transaction->t_requested) ? 1 : 0;

	commit_transaction->t_state = T_COMMIT_CALLBACK;
	J_ASSERT(commit_transaction == journal->j_committing_transaction);
	journal->j_commit_sequence = commit_transaction->t_tid;
	journal->j_committing_transaction = NULL;
	commit_time = ktime_to_ns(ktime_sub(ktime_get(), start_time));

	/*
	 * weight the commit time higher than the average time so we don't
	 * react too strongly to vast changes in the commit time
	 */
	if (likely(journal->j_average_commit_time))
		journal->j_average_commit_time = (commit_time +
				journal->j_average_commit_time*3) / 4;
	else
		journal->j_average_commit_time = commit_time;

	write_unlock(&journal->j_state_lock);

	if (journal->j_commit_callback)
		journal->j_commit_callback(journal, commit_transaction);
	if (journal->j_fc_cleanup_callback)
		journal->j_fc_cleanup_callback(journal, 1, commit_transaction->t_tid);

	trace_jbd2_end_commit(journal, commit_transaction);
	jbd_debug(1, "JBD2: commit %d complete, head %d\n",
		  journal->j_commit_sequence, journal->j_tail_sequence);

	write_lock(&journal->j_state_lock);
	journal->j_flags &= ~JBD2_FULL_COMMIT_ONGOING;
	journal->j_flags &= ~JBD2_FAST_COMMIT_ONGOING;
	spin_lock(&journal->j_list_lock);
	commit_transaction->t_state = T_FINISHED;
	/* Check if the transaction can be dropped now that we are finished */
	if (commit_transaction->t_checkpoint_list == NULL &&
	    commit_transaction->t_checkpoint_io_list == NULL) {
		__jbd2_journal_drop_transaction(journal, commit_transaction);
		jbd2_journal_free_transaction(commit_transaction);
	}
	spin_unlock(&journal->j_list_lock);
	write_unlock(&journal->j_state_lock);
	wake_up(&journal->j_wait_done_commit);
	wake_up(&journal->j_fc_wait);

	/*
	 * Calculate overall stats
	 */
	spin_lock(&journal->j_history_lock);
	journal->j_stats.ts_tid++;
	journal->j_stats.ts_requested += stats.ts_requested;
	journal->j_stats.run.rs_wait += stats.run.rs_wait;
	journal->j_stats.run.rs_request_delay += stats.run.rs_request_delay;
	journal->j_stats.run.rs_running += stats.run.rs_running;
	journal->j_stats.run.rs_locked += stats.run.rs_locked;
	journal->j_stats.run.rs_flushing += stats.run.rs_flushing;
	journal->j_stats.run.rs_logging += stats.run.rs_logging;
	journal->j_stats.run.rs_handle_count += stats.run.rs_handle_count;
	journal->j_stats.run.rs_blocks += stats.run.rs_blocks;
	journal->j_stats.run.rs_blocks_logged += stats.run.rs_blocks_logged;
	spin_unlock(&journal->j_history_lock);
}

#ifdef DEBUG_PROC_OP
typedef struct {
	s64 op_intv[5];
} op_data; 
extern op_data op_array[4000000];
extern atomic_t op_index;

typedef struct {         
        s64 cc_intv[1];  
} cc_data;               
                         
extern atomic_t cc_index;       
extern cc_data cc_array[4000000];
#endif


/*
 * jbd2_journal_barrier_commit_transaction
 *
 * This function is a variant of jbd2_journal_commit_transaction.
 * It just dispatches a journal desciprtor block, log blocks, and
 * a journal commit block without waiting any interrupt completions
 * and calling a storage flush. It does not reserve a checkpoint.
 * All things that excluded from here are processed by
 * jbd2_journal_barrier_flush_transaction.
 *
 * TODO:
 * - Fix code associated with a stats
 * 
 *
 * - 2022. 8. 23. Joontaek Oh.
 */
void jbd2_journal_barrier_commit_transaction(journal_t *journal)
{
	transaction_t *commit_transaction;
	struct journal_head *jh;
	struct buffer_head *descriptor;
	struct buffer_head **wbuf = journal->j_wbuf;
	int bufs;
	int flags;
	int err;
	unsigned long long blocknr;
	char *tagp = NULL;
	journal_block_tag_t *tag = NULL;
	int space_left = 0;
	int first_tag = 0;
	int tag_flag;
	int i;
	int tag_bytes = journal_tag_bytes(journal);
	struct blk_plug plug;
	/* Tail of the journal */
	int csum_size = 0;
	/* UFS */
	LIST_HEAD(t_io_bufs);
	LIST_HEAD(t_log_bufs);
	/* CJFS */
	int ver_tid;
	ktime_t start, dispatch_start, c_wait_start;
	int seq;

	if (jbd2_journal_has_csum_v2or3(journal))
		csum_size = sizeof(struct jbd2_journal_block_tail);

	// commit_transaction->t_commit_start_time = ktime_get();

	/*
	 * First job: lock down the current transaction and wait for
	 * all outstanding updates to complete.
	 */

	/* Do we need to erase the effects of a prior jbd2_journal_flush? */
	if (journal->j_flags & JBD2_FLUSHED) {
		jbd_debug(3, "super block updated\n");
		mutex_lock_io(&journal->j_checkpoint_mutex);
		/*
		 * We hold j_checkpoint_mutex so tail cannot change under us.
		 * We don't need any special data guarantees for writing sb
		 * since journal is empty and it is ok for write to be
		 * flushed only with transaction commit.
		 */
		jbd2_journal_update_sb_log_tail(journal,
						journal->j_tail_sequence,
						journal->j_tail,
						REQ_SYNC);
		mutex_unlock(&journal->j_checkpoint_mutex);
	} else {
		jbd_debug(3, "superblock not updated\n");
	}

	J_ASSERT(journal->j_running_transaction != NULL);
	J_ASSERT(journal->j_committing_transaction == NULL);

	write_lock(&journal->j_state_lock);
	journal->j_flags |= JBD2_FULL_COMMIT_ONGOING;
	while (journal->j_flags & JBD2_FAST_COMMIT_ONGOING) {
		DEFINE_WAIT(wait);

		prepare_to_wait(&journal->j_fc_wait, &wait,
				TASK_UNINTERRUPTIBLE);
		write_unlock(&journal->j_state_lock);
		schedule();
		write_lock(&journal->j_state_lock);
		finish_wait(&journal->j_fc_wait, &wait);
		/*
		 * TODO: by blocking fast commits here, we are increasing
		 * fsync() latency slightly. Strictly speaking, we don't need
		 * to block fast commits until the transaction enters T_FLUSH
		 * state. So an optimization is possible where we block new fast
		 * commits here and wait for existing ones to complete
		 * just before we enter T_FLUSH. That way, the existing fast
		 * commits and this full commit can proceed parallely.
		 */
	}
	write_unlock(&journal->j_state_lock);

	commit_transaction = journal->j_running_transaction;
	ver_tid = commit_transaction->t_tid % MAX_JH_VERSION;

	// commit_transaction->t_commit_start_time = ktime_get();
#ifdef DEBUG_PROC_OP
	c_wait_start = ktime_get();
#endif

#ifdef OP_COALESCING
op_coalescing:
	read_lock(&journal->j_state_lock);
	if (atomic_read(&commit_transaction->t_conflict_count)) {
		J_ASSERT(0 <= atomic_read(&commit_transaction->t_conflict_count));
		read_unlock(&journal->j_state_lock);
		wait_event(journal->j_wait_done_flush
			,!atomic_read(&commit_transaction->t_conflict_count));
		goto op_coalescing;
	}
	read_unlock(&journal->j_state_lock);
#endif

	start = ktime_get();

	trace_jbd2_start_commit(journal, commit_transaction);
	jbd_debug(1, "JBD2: starting commit of transaction %d\n",
			commit_transaction->t_tid);

	write_lock(&journal->j_state_lock);
	journal->j_fc_off = 0;
	J_ASSERT(commit_transaction->t_state == T_RUNNING);
	commit_transaction->t_state = T_LOCKED;

	trace_jbd2_commit_locking(journal, commit_transaction);
	commit_transaction->stats.run.rs_wait = commit_transaction->t_max_wait;
	commit_transaction->stats.run.rs_request_delay = 0;
	commit_transaction->stats.run.rs_locked = jiffies;
	if (commit_transaction->t_requested)
		commit_transaction->stats.run.rs_request_delay =
			jbd2_time_diff(commit_transaction->t_requested,
				       commit_transaction->stats.run.rs_locked);
	commit_transaction->stats.run.rs_running = jbd2_time_diff(commit_transaction->t_start,
					      commit_transaction->stats.run.rs_locked);

	// waits for any t_updates to finish
	jbd2_journal_wait_updates(journal);

	commit_transaction->t_state = T_SWITCH;

	J_ASSERT (atomic_read(&commit_transaction->t_outstanding_credits) <=
			journal->j_max_transaction_buffers);

	/*
	 * If there are pages which are conflicted with previous
	 * committing transactions, it waits for that the committing
	 * transactions are flushed.
	 * - 2022. 8. 27. Joontaek Oh.
	 */
#ifdef OP_COALESCING
	if (atomic_read(&commit_transaction->t_conflict_count)) {
		J_ASSERT(0 <= atomic_read(&commit_transaction->t_conflict_count));
		commit_transaction->t_state = T_RUNNING;
		write_unlock(&journal->j_state_lock);
		wake_up(&journal->j_wait_transaction_locked);
		wait_event(journal->j_wait_done_flush
			,!atomic_read(&commit_transaction->t_conflict_count));
		goto op_coalescing;
	}	
#else
	write_unlock(&journal->j_state_lock);
	while (atomic_read(&commit_transaction->t_conflict_count)) {
		wait_event(journal->j_wait_done_flush,
		         !atomic_read(&commit_transaction->t_conflict_count));
	}
	write_lock(&journal->j_state_lock);
#endif
#ifdef DEBUG_PROC_OP
	seq = atomic_add_return(1, &op_index);
	commit_transaction->seq = seq;
	op_array[seq - 1].op_intv[0] = 
		commit_transaction->t_tid;
	op_array[seq - 1].op_intv[1] = 
		atomic_read(&commit_transaction->t_pconflict_count);
	op_array[seq - 1].op_intv[2] = 
		ktime_to_ns(ktime_sub(ktime_get(), c_wait_start));
	// seq = atomic_add_return(1, &cc_index);
	// cc_array[seq - 1].cc_intv[0] = atomic_read(&commit_transaction->t_pconflict_count);
#endif

	/*
	 * First thing we are allowed to do is to discard any remaining
	 * BJ_Reserved buffers.  Note, it is _not_ permissible to assume
	 * that there are no such buffers: if a large filesystem
	 * operation like a truncate needs to split itself over multiple
	 * transactions, then it may try to do a jbd2_journal_restart() while
	 * there are still BJ_Reserved buffers outstanding.  These must
	 * be released cleanly from the current transaction.
	 *
	 * In this case, the filesystem must still reserve write access
	 * again before modifying the buffer in the new transaction, but
	 * we do not require it to remember exactly which old buffers it
	 * has reserved.  This is consistent with the existing behaviour
	 * that multiple jbd2_journal_get_write_access() calls to the same
	 * buffer are perfectly permissible.
	 * We use journal->j_state_lock here to serialize processing of
	 * t_reserved_list with eviction of buffers from journal_unmap_buffer().
	 */
	while (commit_transaction->t_reserved_list) {
		jh = commit_transaction->t_reserved_list;
		JBUFFER_TRACE(jh, "reserved, unused: refile");
		/*
		 * A jbd2_journal_get_undo_access()+jbd2_journal_release_buffer() may
		 * leave undo-committed data.
		 */
		if (jh->b_jh_entry[ver_tid]->b_committed_data) {
			struct buffer_head *bh = jh2bh(jh);

			spin_lock(&jh->b_state_lock);
			jbd2_free(jh->b_jh_entry[ver_tid]->b_committed_data, bh->b_size);
			jh->b_jh_entry[ver_tid]->b_committed_data = NULL;
			spin_unlock(&jh->b_state_lock);
		}
		jbd2_journal_refile_buffer_entry(journal, jh, commit_transaction);
	}

	write_unlock(&journal->j_state_lock);
	/*
	 * Now try to drop any written-back buffers from the journal's
	 * checkpoint lists.  We do this *before* commit because it potentially
	 * frees some memory
	 */
	spin_lock(&journal->j_list_lock);
	__jbd2_journal_clean_checkpoint_list(journal, false);
	spin_unlock(&journal->j_list_lock);

	jbd_debug(3, "JBD2: commit phase 1\n");

	/*
	 * Clear revoked flag to reflect there is no revoked buffers
	 * in the next transaction which is going to be started.
	 */
	jbd2_clear_buffer_revoked_flags(journal);

	/*
	 * Switch to a new revoke table.
	 */
	jbd2_journal_switch_revoke_table(journal);

	write_lock(&journal->j_state_lock);
	/*
	 * Reserved credits cannot be claimed anymore, free them
	 */
	atomic_sub(atomic_read(&journal->j_reserved_credits),
		   &commit_transaction->t_outstanding_credits);

	trace_jbd2_commit_flushing(journal, commit_transaction);
	commit_transaction->stats.run.rs_flushing = jiffies;
	commit_transaction->stats.run.rs_locked = jbd2_time_diff(commit_transaction->stats.
						run.rs_locked,
					     commit_transaction->stats.run.rs_flushing);

	commit_transaction->t_state = T_FLUSH;
	journal->j_committing_transaction = commit_transaction;
	journal->j_running_transaction = NULL;
	commit_transaction->t_commit_start_time = ktime_get();
	commit_transaction->t_log_start = journal->j_head;
	wake_up(&journal->j_wait_transaction_locked);
	write_unlock(&journal->j_state_lock);

	jbd_debug(3, "JBD2: commit phase 2a\n");

	/*
	 * Now start flushing things to disk, in the order they appear
	 * on the transaction lists.  Data blocks go first.
	 */
	err = journal_submit_data_buffers(journal, commit_transaction);
	if (err)
		jbd2_journal_abort(journal, err);

	blk_start_plug(&plug);
	jbd2_journal_write_revoke_records(commit_transaction, &t_log_bufs);

	jbd_debug(3, "JBD2: commit phase 2b\n");

	/*
	 * Way to go: we have now written out all of the data for a
	 * transaction!  Now comes the tricky part: we need to write out
	 * metadata.  Loop over the transaction's entire buffer list:
	 */
	write_lock(&journal->j_state_lock);
	commit_transaction->t_state = T_COMMIT;
	write_unlock(&journal->j_state_lock);

	trace_jbd2_commit_logging(journal, commit_transaction);
	commit_transaction->stats.run.rs_logging = jiffies;
	commit_transaction->stats.run.rs_flushing = jbd2_time_diff(commit_transaction->stats.
						run.rs_flushing,
					       commit_transaction->stats.run.rs_logging);
	commit_transaction->stats.run.rs_blocks = commit_transaction->t_nr_buffers;
	commit_transaction->stats.run.rs_blocks_logged = 0;

	J_ASSERT(commit_transaction->t_nr_buffers <=
		 atomic_read(&commit_transaction->t_outstanding_credits));

	err = 0;
	bufs = 0;
	descriptor = NULL;
	dispatch_start = ktime_get();
	while (commit_transaction->t_buffers) {

		/* Find the next buffer to be journaled... */

		jh = commit_transaction->t_buffers;

		/* If we're in abort mode, we just un-journal the buffer and
		   release it. */

		if (is_journal_aborted(journal)) {
			clear_buffer_jbddirty(jh2bh(jh));
			JBUFFER_TRACE(jh, "journal is aborting: refile");
			jbd2_buffer_abort_trigger(jh,
						  jh->b_jh_entry[ver_tid]->b_frozen_data ?
						  jh->b_jh_entry[ver_tid]->b_frozen_triggers :
						  jh->b_jh_entry[ver_tid]->b_triggers);
			jbd2_journal_unfile_buffer_entry(journal, jh, ver_tid);
			/* If that was the last one, we need to clean up
			 * any descriptor buffers which may have been
			 * already allocated, even if we are now
			 * aborting. */
			if (!commit_transaction->t_buffers)
				goto start_journal_io;
			continue;
		}

		/* Make sure we have a descriptor block in which to
		   record the metadata buffer. */

		if (!descriptor) {
			J_ASSERT (bufs == 0);

			jbd_debug(4, "JBD2: get descriptor\n");

			descriptor = jbd2_journal_get_descriptor_buffer(
							commit_transaction,
							JBD2_DESCRIPTOR_BLOCK);
			if (!descriptor) {
				jbd2_journal_abort(journal, -EIO);
				continue;
			}

			jbd_debug(4, "JBD2: got buffer %llu (%p)\n",
				(unsigned long long)descriptor->b_blocknr,
				descriptor->b_data);
			tagp = &descriptor->b_data[sizeof(journal_header_t)];
			space_left = descriptor->b_size -
						sizeof(journal_header_t);
			first_tag = 1;
			set_buffer_jwrite(descriptor);
			set_buffer_dirty(descriptor);
			wbuf[bufs++] = descriptor;

			/* Record it so that we can wait for IO
                           completion later */
			BUFFER_TRACE(descriptor, "ph3: file as descriptor");
			jbd2_file_log_bh(&t_log_bufs, descriptor);
		}

		/* Where is the buffer to be written? */

		err = jbd2_journal_next_log_block(journal, &blocknr);
		/* If the block mapping failed, just abandon the buffer
		   and repeat this loop: we'll fall into the
		   refile-on-abort condition above. */
		if (err) {
			jbd2_journal_abort(journal, err);
			continue;
		}

		/*
		 * start_this_handle() uses t_outstanding_credits to determine
		 * the free space in the log.
		 */
		atomic_dec(&commit_transaction->t_outstanding_credits);

		/* Bump b_count to prevent truncate from stumbling over
                   the shadowed buffer!  @@@ This can go if we ever get
                   rid of the shadow pairing of buffers. */
		atomic_inc(&jh2bh(jh)->b_count);

		/*
		 * Make a temporary IO buffer with which to write it out
		 * (this will requeue the metadata buffer to BJ_Shadow).
		 */
		set_bit(BH_JWrite, &jh2bh(jh)->b_state);
		JBUFFER_TRACE(jh, "ph3: write metadata");
		// seq = atomic_add_return(1, &op_index);
		start = ktime_get();
		flags = jbd2_journal_write_metadata_buffer(commit_transaction,
						jh, &wbuf[bufs], blocknr);
		// op_array[seq - 1].op_intv[0] = ktime_to_ns(ktime_sub(ktime_get(),start));
		if (flags < 0) {
			jbd2_journal_abort(journal, flags);
			continue;
		}
		jbd2_file_log_bh(&t_io_bufs, wbuf[bufs]);

		/* Record the new block's tag in the current descriptor
                   buffer */

		tag_flag = 0;
		if (flags & 1)
			tag_flag |= JBD2_FLAG_ESCAPE;
		if (!first_tag)
			tag_flag |= JBD2_FLAG_SAME_UUID;

		tag = (journal_block_tag_t *) tagp;
		write_tag_block(journal, tag, jh2bh(jh)->b_blocknr);
		tag->t_flags = cpu_to_be16(tag_flag);
		jbd2_block_tag_csum_set(journal, tag, wbuf[bufs],
					commit_transaction->t_tid);
		tagp += tag_bytes;
		space_left -= tag_bytes;
		bufs++;

		if (first_tag) {
			memcpy (tagp, journal->j_uuid, 16);
			tagp += 16;
			space_left -= 16;
			first_tag = 0;
		}

		/* If there's no more to do, or if the descriptor is full,
		   let the IO rip! */

		if (bufs == journal->j_wbufsize ||
		    commit_transaction->t_buffers == NULL ||
		    space_left < tag_bytes + 16 + csum_size) {

			jbd_debug(4, "JBD2: Submit %d IOs\n", bufs);

			/* Write an end-of-descriptor marker before
                           submitting the IOs.  "tag" still points to
                           the last tag we set up. */

			tag->t_flags |= cpu_to_be16(JBD2_FLAG_LAST_TAG);
start_journal_io:
			if (descriptor)
				jbd2_descriptor_block_csum_set(journal,
							descriptor);

			for (i = 0; i < bufs; i++) {
				struct buffer_head *bh = wbuf[i];
				/*
				 * Compute checksum.
				 */
				if (jbd2_has_feature_checksum(journal)) {
					commit_transaction->t_crc32_sum =
					  jbd2_checksum_data(
					  commit_transaction->t_crc32_sum, bh);
				}

				lock_buffer(bh);
				clear_buffer_dirty(bh);
				set_buffer_uptodate(bh);
				bh->b_end_io = journal_end_buffer_io_sync;
				// submit_bh(REQ_OP_WRITE, REQ_SYNC, bh);
				/* UFS */
				bh->b_tx = bh->b_tx == NULL ? commit_transaction : bh->b_tx;
				if (commit_transaction->t_buffers != NULL) {
					submit_bh(REQ_OP_WRITE, REQ_SYNC | REQ_ORDERED, bh);
				} else if (i == bufs - 1 
						&& commit_transaction->t_buffers == NULL) {
					submit_bh(REQ_OP_WRITE, 
						REQ_SYNC | REQ_ORDERED | REQ_BARRIER, bh);
				} else {
					submit_bh(REQ_OP_WRITE, REQ_SYNC | REQ_ORDERED, bh);
				}
			}
			cond_resched();

			/* Force a new descriptor to be generated next
                           time round the loop. */
			descriptor = NULL;
			bufs = 0;
		}
	}

#ifdef DEBUG_PROC_OP
	// op_array[seq - 1].op_intv[1] = ktime_to_ns(ktime_sub(ktime_get(), dispatch_start));
#endif

	err = journal_finish_inode_data_buffers(journal, commit_transaction);
	if (err) {
		printk(KERN_WARNING
			"JBD2: Detected IO errors while flushing file data "
		       "on %s\n", journal->j_devname);
		if (journal->j_flags & JBD2_ABORT_ON_SYNCDATA_ERR)
			jbd2_journal_abort(journal, err);
		err = 0;
	}

	/*
	 * Get current oldest transaction in the log before we issue flush
	 * to the filesystem device. After the flush we can be sure that
	 * blocks of all older transactions are checkpointed to persistent
	 * storage and we will be safe to update journal start in the
	 * superblock with the numbers we get here.
	 */
	commit_transaction->t_update_tail =
		jbd2_journal_get_log_tail(journal,
		                          &commit_transaction->t_first_tid,
		                          &commit_transaction->t_first_block);

	write_lock(&journal->j_state_lock);
	if (commit_transaction->t_update_tail) {
		long freed = commit_transaction->t_first_block - journal->j_tail;

		if (commit_transaction->t_first_block < journal->j_tail)
			freed += journal->j_last - journal->j_first;
		/* Update tail only if we free significant amount of space */
		if (freed < jbd2_journal_get_max_txn_bufs(journal))
			commit_transaction->t_update_tail = 0;
	}
	J_ASSERT(commit_transaction->t_state == T_COMMIT);
	commit_transaction->t_state = T_COMMIT_DFLUSH;
	write_unlock(&journal->j_state_lock);

	/*
	 * If the journal is not located on the file system device,
	 * then we must flush the file system device before we issue
	 * the commit record
	 */
	if (commit_transaction->t_need_data_flush &&
	    (journal->j_fs_dev != journal->j_dev) &&
	    (journal->j_flags & JBD2_BARRIER))
		blkdev_issue_flush(journal->j_fs_dev);

	/* Done it all: now write the commit record asynchronously. */
	if (jbd2_has_feature_async_commit(journal)) {
		err = journal_submit_commit_record(journal, commit_transaction,
		                             &commit_transaction->t_cbh,
		                             commit_transaction->t_crc32_sum);
		if (err)
			jbd2_journal_abort(journal, err);
	}

	blk_finish_plug(&plug);

	/*
	 * Here, we wait for the completion of dispatch. Because we don't have
	 * an order-preserving block layer yet, I commented this code out now.
	 * If the order-preserving block layer is implemented, We can
	 * uncomment this code.
	 * - 2022. 8. 26. Joontaek Oh.
	 */

	while (!list_empty(&t_io_bufs)) {
		struct buffer_head *bh = list_entry(t_io_bufs.prev,
		       				    struct buffer_head,
		       			 	    b_assoc_buffers);
		wait_on_buffer_dispatch(bh);

		jbd2_unfile_log_bh(bh);
		jbd2_file_log_bh(&commit_transaction->t_io_bufs, bh);
	}

	while (!list_empty(&t_log_bufs)) {
		struct buffer_head *bh = list_entry(t_log_bufs.prev,
		                		    struct buffer_head, 
						    b_assoc_buffers);
		wait_on_buffer_dispatch(bh);
		jbd2_unfile_log_bh(bh);
		jbd2_file_log_bh(&commit_transaction->t_log_bufs, bh);
	}

	if (!jbd2_has_feature_async_commit(journal)) {
		err = journal_submit_commit_record(journal, commit_transaction,
		                              &commit_transaction->t_cbh,
		                              commit_transaction->t_crc32_sum);
		if (err)
			jbd2_journal_abort(journal, err);
	}

	/* UFS */
	if (commit_transaction->t_cbh)
		err = journal_wait_on_dispatch_of_commit_record(journal, 
				commit_transaction->t_cbh);

	/*
	 * Insert the committing transaction into the flush transaction list.
	 * The flush thead will get the committing transaction and flush it.
	 * - 2022. 8. 27. Joontaek Oh.
	 */
	spin_lock(&journal->j_list_lock);
	if (journal->j_flushing_transactions == NULL) {
		journal->j_flushing_transactions = commit_transaction;
		commit_transaction->t_flushnext = commit_transaction;
		commit_transaction->t_flushprev = commit_transaction;
	} else {
		commit_transaction->t_flushnext =
			journal->j_flushing_transactions;
		commit_transaction->t_flushprev =
			commit_transaction->t_flushnext->t_flushprev;
		commit_transaction->t_flushnext->t_flushprev =
			commit_transaction;
		commit_transaction->t_flushprev->t_flushnext =
				commit_transaction;
	}
	spin_unlock(&journal->j_list_lock);


	/* Done with this transaction! */
	write_lock(&journal->j_state_lock);
	jbd_debug(3, "JBD2: commit phase 7\n");

	commit_transaction->t_start = jiffies;
	commit_transaction->stats.run.rs_logging = jbd2_time_diff(commit_transaction->stats.
						run.rs_logging,
					      commit_transaction->t_start);

	/*
	 * File the transaction statistics
	 */
	commit_transaction->stats.ts_tid = commit_transaction->t_tid;
	commit_transaction->stats.run.rs_handle_count =
		atomic_read(&commit_transaction->t_handle_count);
	trace_jbd2_run_stats(journal->j_fs_dev->bd_dev,
			     commit_transaction->t_tid, &commit_transaction->stats.run);
	commit_transaction->stats.ts_requested = (commit_transaction->t_requested) ? 1 : 0;

	J_ASSERT(commit_transaction == journal->j_committing_transaction);
	journal->j_commit_sequence = commit_transaction->t_tid;
	journal->j_committing_transaction = NULL;
	// commit_time = ktime_to_ns(ktime_sub(ktime_get(), commit_transaction->start_time));

	/*
	 * weight the commit time higher than the average time so we don't
	 * react too strongly to vast changes in the commit time
	 */
	/* CJFS
	if (likely(journal->j_average_commit_time))
		journal->j_average_commit_time = (commit_time +
				journal->j_average_commit_time*3) / 4;
	else
		journal->j_average_commit_time = commit_time;
	*/

	write_unlock(&journal->j_state_lock);

	if (journal->j_commit_callback)
		journal->j_commit_callback(journal, commit_transaction);
	if (journal->j_fc_cleanup_callback)
		journal->j_fc_cleanup_callback(journal, 1, commit_transaction->t_tid);

	trace_jbd2_end_commit(journal, commit_transaction);
	jbd_debug(1, "JBD2: commit %d complete, head %d\n",
		  journal->j_commit_sequence, journal->j_tail_sequence);
}

/*
 * jbd2_journal_barrier_flush_transaction
 *
 * This function is another variant of
 * jbd2_journal_commit_transaction. It waits for completion
 * of transfer for the journal descriptor block, log blocks
 * and the journal commit block which are dispatched by
 * jbd2_journal_barrier_commit_transaction.
 * Finally, it calls a storage flush and reserves
 * a checkpoint.
 *
 * - 2022. 8. 23. Joontaek Oh.
 */
void jbd2_journal_barrier_flush_transaction(journal_t *journal)
{
	transaction_t *commit_transaction;
	struct journal_head *jh;
	int err = 0;
#ifdef COMPOUND_FLUSH
	int flush_tid;
	int commit_empty;
#endif
	/* CJFS */
	int ver_tid, seq;

	/*
	 * Bring the committing transaction which has been processed
	 * by a jbd2_journal_barrier_commit_transaction.
	 */
	spin_lock(&journal->j_list_lock);

	if (journal->j_flushing_transactions == NULL) {
		spin_unlock(&journal->j_list_lock);
		return;
	}

	commit_transaction = journal->j_flushing_transactions;
	ver_tid = commit_transaction->t_tid % MAX_JH_VERSION;
#ifdef COMPOUND_FLUSH
	flush_tid = commit_transaction->t_tid % COMPOUND_FLUSH;
	commit_empty = journal->j_flushing_transactions 
		== commit_transaction->t_flushnext ? 1 : 0;
#endif
	// printk(KERN_INFO "[SWDBG] (%s) tid : %d flush_tid : %d, commit_emtpy : %d\n"
	//		,__func__, commit_transaction->t_tid, flush_tid, commit_empty);
	spin_unlock(&journal->j_list_lock);
	
	/* Lo and behold: we have just managed to send a transaction to
           the log.  Before we can commit it, wait for the IO so far to
           complete.  Control buffers being written are on the
           transaction's t_log_list queue, and metadata buffers are on
           the io_bufs list.

	   Wait for the buffers in reverse order.  That way we are
	   less likely to be woken up until all IOs have completed, and
	   so we incur less scheduling load.
	*/

	jbd_debug(3, "JBD2: commit phase 3\n");

	while (!list_empty(&commit_transaction->t_io_bufs)) {
		struct buffer_head *bh =
		                 list_entry(commit_transaction->t_io_bufs.prev,
		                            struct buffer_head,
		                            b_assoc_buffers);

		wait_on_buffer(bh);
		cond_resched();

		if (unlikely(!buffer_uptodate(bh)))
			err = -EIO;
		jbd2_unfile_log_bh(bh);
		commit_transaction->stats.run.rs_blocks_logged++;

		/*
		 * The list contains temporary buffer heads created by
		 * jbd2_journal_write_metadata_buffer().
		 */
		BUFFER_TRACE(bh, "dumping temporary bh");
		__brelse(bh);
		J_ASSERT_BH(bh, atomic_read(&bh->b_count) == 0);
		free_buffer_head(bh);

		/* We also have to refile the corresponding shadowed buffer */
		jh = commit_transaction->t_shadow_list->b_jh_entry[ver_tid]->b_tprev;
		bh = jh2bh(jh);
		clear_buffer_jwrite(bh);
		// J_ASSERT_BH(bh, buffer_jbddirty(bh));
		// J_ASSERT_BH(bh, !buffer_shadow(bh));

		/* The metadata is now released for reuse, but we need
                   to remember it against this transaction so that when
                   we finally commit, we can do any checkpointing
                   required. */
		JBUFFER_TRACE(jh, "file as BJ_Forget");
		jbd2_journal_file_buffer(jh, commit_transaction, BJ_Forget);
		JBUFFER_TRACE(jh, "brelse shadowed buffer");
		__brelse(bh);
	}

	J_ASSERT (commit_transaction->t_shadow_list == NULL);

	jbd_debug(3, "JBD2: commit phase 4\n");

	/* Here we wait for the revoke record and descriptor record buffers */
	while (!list_empty(&commit_transaction->t_log_bufs)) {
		struct buffer_head *bh;

		bh = list_entry(commit_transaction->t_log_bufs.prev,
		                struct buffer_head, b_assoc_buffers);
		wait_on_buffer(bh);
		cond_resched();

		if (unlikely(!buffer_uptodate(bh)))
			err = -EIO;

		BUFFER_TRACE(bh, "ph5: control buffer writeout done: unfile");
		clear_buffer_jwrite(bh);
		jbd2_unfile_log_bh(bh);
		commit_transaction->stats.run.rs_blocks_logged++;
		__brelse(bh);		/* One for getblk */
		/* AKPM: bforget here */
	}

	if (err)
		jbd2_journal_abort(journal, err);

	jbd_debug(3, "JBD2: commit phase 5\n");
	write_lock(&journal->j_state_lock);
	J_ASSERT(commit_transaction->t_state == T_COMMIT_DFLUSH);
	commit_transaction->t_state = T_COMMIT_JFLUSH;
	write_unlock(&journal->j_state_lock);

	if (commit_transaction->t_cbh)
		err = journal_wait_on_commit_record(journal, commit_transaction->t_cbh);
	commit_transaction->stats.run.rs_blocks_logged++;
#ifdef COMPOUND_FLUSH
	//printk(KERN_INFO "[SWDBG] (%s) tid : %d, flush_tid : %d, commit_emtpy : %d\n"
	//	,__func__, commit_transaction->t_tid, flush_tid, commit_empty);
	if ((journal->j_flags & JBD2_BARRIER) 
		&& (flush_tid == 0 || commit_empty == 1)) {
		blkdev_issue_flush(journal->j_dev);
		// printk(KERN_INFO "[SWDBG] (%s) tid : %d, wait on flush ! flush_tid : %d, commit_emtpy : %d\n"
		//	,__func__, commit_transaction->t_tid, flush_tid, commit_empty);
	}
#ifdef DEBUG_PROC_OP
	seq = commit_transaction->seq;
	op_array[seq - 1].op_intv[3] = 
		commit_transaction->stats.run.rs_blocks;
	op_array[seq - 1].op_intv[4] = 
		((journal->j_flags & JBD2_BARRIER) && (flush_tid == 0 || commit_empty == 1)) ? 1 : 0;
#endif
#else
	if (journal->j_flags & JBD2_BARRIER) {
		blkdev_issue_flush(journal->j_dev);
	}
#endif
	if (err)
		jbd2_journal_abort(journal, err);

	WARN_ON_ONCE(
		atomic_read(&commit_transaction->t_outstanding_credits) < 0);

	/*
	 * Now disk caches for filesystem device are flushed so we are safe to
	 * erase checkpointed transactions from the log by updating journal
	 * superblock.
	 */
	if (commit_transaction->t_update_tail)
		jbd2_update_log_tail(journal,
		                     commit_transaction->t_first_tid,
		                     commit_transaction->t_first_block);

	/* End of a transaction!  Finally, we can do checkpoint
           processing: any buffers committed as a result of this
           transaction can be removed from any checkpoint list it was on
           before. */

	jbd_debug(3, "JBD2: commit phase 6\n");

	J_ASSERT(list_empty(&commit_transaction->t_inode_list));
	J_ASSERT(commit_transaction->t_buffers == NULL);
	J_ASSERT(commit_transaction->t_checkpoint_list == NULL);
	J_ASSERT(commit_transaction->t_shadow_list == NULL);

restart_loop:
	/*
	 * As there are other places (journal_unmap_buffer()) adding buffers
	 * to this list we have to be careful and hold the j_list_lock.
	 */
	spin_lock(&journal->j_list_lock);
	while (commit_transaction->t_forget) {
		transaction_t *cp_transaction;
		struct buffer_head *bh;
		int try_to_free = 0;
		bool drop_ref;

		jh = commit_transaction->t_forget;
		spin_unlock(&journal->j_list_lock);
		bh = jh2bh(jh);
		/*
		 * Get a reference so that bh cannot be freed before we are
		 * done with it.
		 */
		get_bh(bh);
		spin_lock(&jh->b_state_lock);
		J_ASSERT_JH(jh,	jh->b_jh_entry[ver_tid]->b_transaction == commit_transaction);

		/*
		 * If there is undo-protected committed data against
		 * this buffer, then we can remove it now.  If it is a
		 * buffer needing such protection, the old frozen_data
		 * field now points to a committed version of the
		 * buffer, so rotate that field to the new committed
		 * data.
		 *
		 * Otherwise, we can just throw away the frozen data now.
		 *
		 * We also know that the frozen data has already fired
		 * its triggers if they exist, so we can clear that too.
		 */
		if (jh->b_jh_entry[ver_tid]->b_committed_data) {
			jbd2_free(jh->b_jh_entry[ver_tid]->b_committed_data, bh->b_size);
			jh->b_jh_entry[ver_tid]->b_committed_data = NULL;
			if (jh->b_jh_entry[ver_tid]->b_frozen_data) {
				jh->b_jh_entry[ver_tid]->b_committed_data = jh->b_frozen_data;
				jh->b_jh_entry[ver_tid]->b_frozen_data = NULL;
				jh->b_jh_entry[ver_tid]->b_frozen_triggers = NULL;
			}
		} else if (jh->b_jh_entry[ver_tid]->b_frozen_data) {
			jbd2_free(jh->b_jh_entry[ver_tid]->b_frozen_data, bh->b_size);
			jh->b_jh_entry[ver_tid]->b_frozen_data = NULL;
			jh->b_jh_entry[ver_tid]->b_frozen_triggers = NULL;
		}

		spin_lock(&journal->j_list_lock);
		cp_transaction = jh->b_cp_transaction;
		if (cp_transaction) {
			JBUFFER_TRACE(jh, "remove from old cp transaction");
			cp_transaction->t_chp_stats.cs_dropped++;
			__jbd2_journal_remove_checkpoint(jh);
		}

		/* Only re-checkpoint the buffer_head if it is marked
		 * dirty.  If the buffer was added to the BJ_Forget list
		 * by jbd2_journal_forget, it may no longer be dirty and
		 * there's no point in keeping a checkpoint record for
		 * it. */

		/*
		 * A buffer which has been freed while still being journaled
		 * by a previous transaction, refile the buffer to BJ_Forget of
		 * the running transaction. If the just committed transaction
		 * contains "add to orphan" operation, we can completely
		 * invalidate the buffer now. We are rather through in that
		 * since the buffer may be still accessible when blocksize <
		 * pagesize and it is attached to the last partial page.
		 */
		/* CJFS */
		if (buffer_freed(bh) && jh->b_ver_count == 1) {
			struct address_space *mapping;

			clear_buffer_freed(bh);
			clear_buffer_jbddirty(bh);

			/*
			 * Block device buffers need to stay mapped all the
			 * time, so it is enough to clear buffer_jbddirty and
			 * buffer_freed bits. For the file mapping buffers (i.e.
			 * journalled data) we need to unmap buffer and clear
			 * more bits. We also need to be careful about the check
			 * because the data page mapping can get cleared under
			 * our hands. Note that if mapping == NULL, we don't
			 * need to make buffer unmapped because the page is
			 * already detached from the mapping and buffers cannot
			 * get reused.
			 */
			mapping = READ_ONCE(bh->b_page->mapping);
			if (mapping && !sb_is_blkdev_sb(mapping->host->i_sb)) {
				clear_buffer_mapped(bh);
				clear_buffer_new(bh);
				clear_buffer_req(bh);
				bh->b_bdev = NULL;
			}
		}

		if (buffer_jbddirty(bh)) {
			JBUFFER_TRACE(jh, "add to new checkpointing trans");
			__jbd2_journal_insert_checkpoint(jh, commit_transaction);
			if (is_journal_aborted(journal))
				clear_buffer_jbddirty(bh);
		} else {
			// J_ASSERT_BH(bh, !buffer_dirty(bh));
			/*
			 * The buffer on BJ_Forget list and not jbddirty means
			 * it has been freed by this transaction and hence it
			 * could not have been reallocated until this
			 * transaction has committed. *BUT* it could be
			 * reallocated once we have written all the data to
			 * disk and before we process the buffer on BJ_Forget
			 * list.
			 */
			if (jh->b_ver_count == 1)
				try_to_free = 1;
		}
		JBUFFER_TRACE(jh, "refile or unfile buffer");
		drop_ref = __jbd2_journal_refile_buffer_entry(jh, commit_transaction);
		spin_unlock(&jh->b_state_lock);
		if (drop_ref)
			jbd2_journal_put_journal_head(jh);
		if (try_to_free)
			release_buffer_page(bh);	/* Drops bh reference */
		else
			__brelse(bh);
		cond_resched_lock(&journal->j_list_lock);
	}
	spin_unlock(&journal->j_list_lock);
	/*
	 * This is a bit sleazy.  We use j_list_lock to protect transition
	 * of a transaction into T_FINISHED state and calling
	 * __jbd2_journal_drop_transaction(). Otherwise we could race with
	 * other checkpointing code processing the transaction...
	 */
	write_lock(&journal->j_state_lock);
	spin_lock(&journal->j_list_lock);
	/*
	 * Now recheck if some buffers did not get attached to the transaction
	 * while the lock was dropped...
	 */
	if (commit_transaction->t_forget) {
		spin_unlock(&journal->j_list_lock);
		write_unlock(&journal->j_state_lock);
		goto restart_loop;
	}
#ifdef COMPOUND_FLUSH	
	journal->j_transfer_sequence = commit_transaction->t_tid;
	if (flush_tid == 0 || commit_empty == 1) 
		journal->j_flush_sequence = commit_transaction->t_tid;
#else
	journal->j_flush_sequence = commit_transaction->t_tid;
#endif

	/* Add the transaction to the checkpoint list
	 * __journal_remove_checkpoint() can not destroy transaction
	 * under us because it is not marked as T_FINISHED yet */
	if (journal->j_checkpoint_transactions == NULL) {
		journal->j_checkpoint_transactions = commit_transaction;
		commit_transaction->t_cpnext = commit_transaction;
		commit_transaction->t_cpprev = commit_transaction;
	} else {
		commit_transaction->t_cpnext =
			journal->j_checkpoint_transactions;
		commit_transaction->t_cpprev =
			commit_transaction->t_cpnext->t_cpprev;
		commit_transaction->t_cpnext->t_cpprev =
			commit_transaction;
		commit_transaction->t_cpprev->t_cpnext =
				commit_transaction;
	}
	spin_unlock(&journal->j_list_lock);
	write_unlock(&journal->j_state_lock);

	write_lock(&journal->j_state_lock);
	journal->j_flags &= ~JBD2_FULL_COMMIT_ONGOING;
	journal->j_flags &= ~JBD2_FAST_COMMIT_ONGOING;
	spin_lock(&journal->j_list_lock);
	commit_transaction->t_state = T_FINISHED;
	/* Check if the transaction can be dropped now that we are finished */
	if (commit_transaction->t_checkpoint_list == NULL &&
	    commit_transaction->t_checkpoint_io_list == NULL) {
		__jbd2_journal_drop_transaction(journal, commit_transaction);
		jbd2_journal_free_transaction(commit_transaction);
	}
	spin_unlock(&journal->j_list_lock);

	commit_transaction->commit_time = ktime_to_ns(ktime_sub(ktime_get(), 
				commit_transaction->t_commit_start_time));
	
	if (likely(journal->j_average_commit_time))
		journal->j_average_commit_time = (commit_transaction->commit_time +
				journal->j_average_commit_time*3) / 4;
	else
		journal->j_average_commit_time = commit_transaction->commit_time;
	
	write_unlock(&journal->j_state_lock);
	wake_up(&journal->j_wait_done_commit);
	wake_up(&journal->j_fc_wait);
	/*
	 * Calculate overall stats
	 */
	spin_lock(&journal->j_history_lock);
	journal->j_stats.ts_tid++;
	journal->j_stats.ts_requested += commit_transaction->stats.ts_requested;
	journal->j_stats.run.rs_wait += commit_transaction->stats.run.rs_wait;
	journal->j_stats.run.rs_request_delay += commit_transaction->stats.run.rs_request_delay;
	journal->j_stats.run.rs_running += commit_transaction->stats.run.rs_running;
	journal->j_stats.run.rs_locked += commit_transaction->stats.run.rs_locked;
	journal->j_stats.run.rs_flushing += commit_transaction->stats.run.rs_flushing;
	journal->j_stats.run.rs_logging += commit_transaction->stats.run.rs_logging;
	journal->j_stats.run.rs_handle_count += commit_transaction->stats.run.rs_handle_count;
	journal->j_stats.run.rs_blocks += commit_transaction->stats.run.rs_blocks;
	journal->j_stats.run.rs_blocks_logged += commit_transaction->stats.run.rs_blocks_logged;
	spin_unlock(&journal->j_history_lock);

	/*
	 * Remove commit_transaction from j_flushing_transactions.
	 * 2022. 8. 27. Joontaek Oh.
	 */
	spin_lock(&journal->j_list_lock);
	commit_transaction->t_flushnext->t_flushprev = commit_transaction->t_flushprev;
	commit_transaction->t_flushprev->t_flushnext = commit_transaction->t_flushnext;

	if (journal->j_flushing_transactions == commit_transaction)
		journal->j_flushing_transactions = commit_transaction->t_flushnext;
	if (journal->j_flushing_transactions == commit_transaction)
		journal->j_flushing_transactions = NULL;
	spin_unlock(&journal->j_list_lock);
	
}
