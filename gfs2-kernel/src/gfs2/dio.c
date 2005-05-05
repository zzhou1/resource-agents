/******************************************************************************
*******************************************************************************
**
**  Copyright (C) Sistina Software, Inc.  1997-2003  All rights reserved.
**  Copyright (C) 2004 Red Hat, Inc.  All rights reserved.
**
**  This copyrighted material is made available to anyone wishing to use,
**  modify, copy, or redistribute it subject to the terms and conditions
**  of the GNU General Public License v.2.
**
*******************************************************************************
******************************************************************************/

#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/spinlock.h>
#include <asm/semaphore.h>
#include <linux/completion.h>
#include <linux/buffer_head.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/writeback.h>

#include "gfs2.h"
#include "dio.h"
#include "glock.h"
#include "glops.h"
#include "inode.h"
#include "log.h"
#include "lops.h"
#include "rgrp.h"
#include "trans.h"

#define buffer_busy(bh) \
((bh)->b_state & ((1ul << BH_Dirty) | (1ul << BH_Lock) | (1ul << BH_Pinned)))
#define buffer_in_io(bh) \
((bh)->b_state & ((1ul << BH_Dirty) | (1ul << BH_Lock)))

/**
 * aspace_get_block - 
 * @inode:
 * @lblock:
 * @bh_result:
 * @create:
 *
 * Returns: errno
 */

static int
aspace_get_block(struct inode *inode, sector_t lblock,
		 struct buffer_head *bh_result, int create)
{
	ENTER(G2FN_ASPACE_GET_BLOCK)
	gfs2_assert_warn(get_v2sdp(inode->i_sb), FALSE);
	RETURN(G2FN_ASPACE_GET_BLOCK, -ENOSYS);
}

/**
 * gfs2_aspace_writepage - write an aspace page
 * @page: the page
 * @wbc:
 *
 * Returns: errno
 */

static int 
gfs2_aspace_writepage(struct page *page, struct writeback_control *wbc)
{
	ENTER(G2FN_ASPACE_WRITEPAGE)
	RETURN(G2FN_ASPACE_WRITEPAGE,
	       block_write_full_page(page, aspace_get_block, wbc));
}

/**
 * stuck_releasepage - We're stuck in gfs2_releasepage().  Print stuff out.
 * @bh: the buffer we're stuck on
 *
 */

static void
stuck_releasepage(struct buffer_head *bh)
{
	ENTER(G2FN_STUCK_RELEASEPAGE)
	struct gfs2_sbd *sdp = get_v2sdp(bh->b_page->mapping->host->i_sb);
	struct gfs2_bufdata *bd = get_v2bd(bh);

	printk("GFS2: fsid=%s: stuck in gfs2_releasepage()...\n", sdp->sd_fsname);
	printk("GFS2: fsid=%s: blkno = %"PRIu64", bh->b_count = %d\n",
	       sdp->sd_fsname,
	       (uint64_t)bh->b_blocknr,
	       atomic_read(&bh->b_count));
	printk("GFS2: fsid=%s: pinned = %u\n",
	       sdp->sd_fsname, buffer_pinned(bh));
	printk("GFS2: fsid=%s: get_v2bd(bh) = %s\n",
	       sdp->sd_fsname,
	       (bd) ? "!NULL" : "NULL");

	if (bd) {
		struct gfs2_glock *gl = bd->bd_gl;

		printk("GFS2: fsid=%s: gl = (%u, %"PRIu64")\n",
		       sdp->sd_fsname,
		       gl->gl_name.ln_type,
		       gl->gl_name.ln_number);
		printk("GFS2: fsid=%s: bd_list_tr = %s, bd_le.le_list = %s\n",
		       sdp->sd_fsname,
		       (list_empty(&bd->bd_list_tr)) ? "no" : "yes",
		       (list_empty(&bd->bd_le.le_list)) ? "no" : "yes");

		if (gl->gl_ops == &gfs2_inode_glops) {
			struct gfs2_inode *ip = get_gl2ip(gl);

			if (ip) {
				unsigned int x;

				printk("GFS2: fsid=%s: ip = %"PRIu64"/%"PRIu64"\n",
				       sdp->sd_fsname,
				       ip->i_num.no_formal_ino,
				       ip->i_num.no_addr);
				printk("GFS2: fsid=%s: ip->i_count = %d, ip->i_vnode = %s\n",
				     sdp->sd_fsname,
				     atomic_read(&ip->i_count),
				     (ip->i_vnode) ? "!NULL" : "NULL");
				for (x = 0; x < GFS2_MAX_META_HEIGHT; x++)
					printk("GFS2: fsid=%s: ip->i_cache[%u] = %s\n",
					       sdp->sd_fsname, x,
					       (ip->i_cache[x]) ? "!NULL" : "NULL");
			}
		}
	}

	RET(G2FN_STUCK_RELEASEPAGE);
}

/**
 * gfs2_aspace_releasepage - free the metadata associated with a page 
 * @page: the page that's being released
 * @gfp_mask: passed from Linux VFS, ignored by us
 *
 * Call try_to_free_buffers() if the buffers in this page can be
 * released.
 *
 * Returns: 0
 */

static int
gfs2_aspace_releasepage(struct page *page, int gfp_mask)
{
	ENTER(G2FN_ASPACE_RELEASEPAGE)
	struct inode *aspace = page->mapping->host;
	struct gfs2_sbd *sdp = get_v2sdp(aspace->i_sb);
	struct buffer_head *bh, *head;
	struct gfs2_bufdata *bd;
	unsigned long t;

	if (!page_has_buffers(page))
		goto out;

	head = bh = page_buffers(page);
	do {
		t = jiffies;

		while (atomic_read(&bh->b_count)) {
			if (atomic_read(&aspace->i_writecount)) {
				if (time_after_eq(jiffies,
						  t +
						  gfs2_tune_get(sdp, gt_stall_secs) * HZ)) {
					stuck_releasepage(bh);
					t = jiffies;
				}

				yield();
				continue;
			}

			RETURN(G2FN_ASPACE_RELEASEPAGE, 0);
		}

		gfs2_assert_warn(sdp, !buffer_pinned(bh));

		bd = get_v2bd(bh);
		if (bd) {
			gfs2_assert_warn(sdp, bd->bd_bh == bh);
		        gfs2_assert_warn(sdp, list_empty(&bd->bd_list_tr));
		        gfs2_assert_warn(sdp, list_empty(&bd->bd_le.le_list));
			gfs2_assert_warn(sdp, !bd->bd_ail);
			gfs2_memory_rm(bd);
			kmem_cache_free(gfs2_bufdata_cachep, bd);
			atomic_dec(&sdp->sd_bufdata_count);
			set_v2bd(bh, NULL);
		}

		bh = bh->b_this_page;
	}
	while (bh != head);

 out:
	RETURN(G2FN_ASPACE_RELEASEPAGE, try_to_free_buffers(page));
}

static struct address_space_operations aspace_aops = {
	.writepage = gfs2_aspace_writepage,
	.releasepage = gfs2_aspace_releasepage,
};

/**
 * gfs2_aspace_get - Create and initialize a struct inode structure
 * @sdp: the filesystem the aspace is in
 *
 * Right now a struct inode is just a struct inode.  Maybe Linux
 * will supply a more lightweight address space construct (that works)
 * in the future.
 *
 * Make sure pages/buffers in this aspace aren't in high memory.
 *
 * Returns: the aspace
 */

struct inode *
gfs2_aspace_get(struct gfs2_sbd *sdp)
{
	ENTER(G2FN_ASPACE_GET)
	struct inode *aspace;

	aspace = new_inode(sdp->sd_vfs);
	if (aspace) {
		mapping_set_gfp_mask(aspace->i_mapping, GFP_KERNEL);
		aspace->i_mapping->a_ops = &aspace_aops;
		aspace->i_size = ~0ULL;
		get_v2ip(aspace) = NULL;
		insert_inode_hash(aspace);
	}

	RETURN(G2FN_ASPACE_GET, aspace);
}

/**
 * gfs2_aspace_put - get rid of an aspace
 * @aspace:
 *
 */

void
gfs2_aspace_put(struct inode *aspace)
{
	ENTER(G2FN_ASPACE_PUT)
	remove_inode_hash(aspace);
	iput(aspace);
	RET(G2FN_ASPACE_PUT);
}

/**
 * gfs2_ail1_start_one - Start I/O on a part of the AIL
 * @sdp: the filesystem
 * @tr: the part of the AIL
 *
 */

void
gfs2_ail1_start_one(struct gfs2_sbd *sdp, struct gfs2_ail *ai)
{
	ENTER(G2FN_AIL1_START_ONE)
	struct list_head *head, *tmp, *prev;
	struct gfs2_bufdata *bd;
	struct buffer_head *bh;
	int retry;

	do {
		retry = FALSE;

		for (head = &ai->ai_ail1_list, tmp = head->prev, prev = tmp->prev;
		     tmp != head;
		     tmp = prev, prev = tmp->prev) {
			bd = list_entry(tmp, struct gfs2_bufdata, bd_ail_st_list);
			bh = bd->bd_bh;

			gfs2_assert(sdp, bd->bd_ail == ai,);

			if (!buffer_busy(bh)) {
				if (!buffer_uptodate(bh))
					gfs2_io_error_bh(sdp, bh);
				list_move(&bd->bd_ail_st_list, &ai->ai_ail2_list);
				continue;
			}

			if (!buffer_dirty(bh))
				continue;

			list_move(&bd->bd_ail_st_list, head);

			gfs2_log_unlock(sdp);
			wait_on_buffer(bh);
			ll_rw_block(WRITE, 1, &bh);
			gfs2_log_lock(sdp);

			retry = TRUE;
			break;
		}
	} while (retry);

	RET(G2FN_AIL1_START_ONE);
}

/**
 * gfs2_ail1_empty_one - Check whether or not a trans in the AIL has been synced
 * @sdp: the filesystem
 * @ai: the AIL entry
 *
 */

int
gfs2_ail1_empty_one(struct gfs2_sbd *sdp, struct gfs2_ail *ai, int flags)
{
	ENTER(G2FN_AIL1_EMPTY_ONE)
	struct list_head *head, *tmp, *prev;
	struct gfs2_bufdata *bd;
	struct buffer_head *bh;

	for (head = &ai->ai_ail1_list, tmp = head->prev, prev = tmp->prev;
	     tmp != head;
	     tmp = prev, prev = tmp->prev) {
		bd = list_entry(tmp, struct gfs2_bufdata, bd_ail_st_list);
		bh = bd->bd_bh;

		gfs2_assert(sdp, bd->bd_ail == ai,);

		if (buffer_busy(bh)) {
			if (flags & DIO_ALL)
				continue;
			else
				break;
		}

		if (!buffer_uptodate(bh))
			gfs2_io_error_bh(sdp, bh);

		list_move(&bd->bd_ail_st_list, &ai->ai_ail2_list);
	}

	RETURN(G2FN_AIL1_EMPTY_ONE, list_empty(head));
}

/**
 * gfs2_ail2_empty_one - Check whether or not a trans in the AIL has been synced
 * @sdp: the filesystem
 * @ai: the AIL entry
 *
 */

void
gfs2_ail2_empty_one(struct gfs2_sbd *sdp, struct gfs2_ail *ai)
{
	ENTER(G2FN_AIL2_EMPTY_ONE)
       	struct list_head *head = &ai->ai_ail2_list;
	struct gfs2_bufdata *bd;

	while (!list_empty(head)) {
		bd = list_entry(head->prev, struct gfs2_bufdata, bd_ail_st_list);
		gfs2_assert(sdp, bd->bd_ail == ai,);
		bd->bd_ail = NULL;
		list_del(&bd->bd_ail_st_list);
		list_del(&bd->bd_ail_gl_list);
		atomic_dec(&bd->bd_gl->gl_ail_count);
		brelse(bd->bd_bh);
	}

	RET(G2FN_AIL2_EMPTY_ONE);
}

/**
 * ail_empty_gl - remove all buffers for a given lock from the AIL
 * @gl: the glock
 *
 * None of the buffers should be dirty, locked, or pinned.
 */

void
gfs2_ail_empty_gl(struct gfs2_glock *gl)
{
	ENTER(G2FN_AIL_EMPTY_GL)
	struct gfs2_sbd *sdp = gl->gl_sbd;
	unsigned int blocks;
	struct list_head *head = &gl->gl_ail_list;
	struct gfs2_bufdata *bd;
	struct buffer_head *bh;
	uint64_t blkno;
	int error;

	blocks = atomic_read(&gl->gl_ail_count);
	if (!blocks)
		RET(G2FN_AIL_EMPTY_GL);

	error = gfs2_trans_begin(sdp, 0, blocks);
	if (gfs2_assert_withdraw(sdp, !error))
		RET(G2FN_AIL_EMPTY_GL);

	gfs2_log_lock(sdp);
	while (!list_empty(head)) {
		bd = list_entry(head->next, struct gfs2_bufdata, bd_ail_gl_list);
		bh = bd->bd_bh;
		blkno = bh->b_blocknr;
		gfs2_assert_withdraw(sdp, !buffer_busy(bh));

		bd->bd_ail = NULL;
		list_del(&bd->bd_ail_st_list);
		list_del(&bd->bd_ail_gl_list);
		atomic_dec(&gl->gl_ail_count);
		brelse(bh);
		gfs2_log_unlock(sdp);

		gfs2_trans_add_revoke(sdp, blkno);

		gfs2_log_lock(sdp);
	}
	gfs2_assert_withdraw(sdp, !atomic_read(&gl->gl_ail_count));
	gfs2_log_unlock(sdp);

	gfs2_trans_end(sdp);
	gfs2_log_flush(sdp);

	RET(G2FN_AIL_EMPTY_GL);
}

/**
 * gfs2_inval_buf - Invalidate all buffers associated with a glock
 * @gl: the glock
 *
 */

void
gfs2_inval_buf(struct gfs2_glock *gl)
{
	ENTER(G2FN_INVAL_BUF)
       	struct gfs2_sbd *sdp = gl->gl_sbd;
	struct inode *aspace = gl->gl_aspace;
	struct address_space *mapping = gl->gl_aspace->i_mapping;

	gfs2_assert_withdraw(sdp, !atomic_read(&gl->gl_ail_count));

	atomic_inc(&aspace->i_writecount);
	truncate_inode_pages(mapping, 0);
	atomic_dec(&aspace->i_writecount);

	gfs2_assert_withdraw(sdp, !mapping->nrpages);

	RET(G2FN_INVAL_BUF);
}

/**
 * gfs2_sync_buf - Sync all buffers associated with a glock
 * @gl: The glock
 * @flags: DIO_START | DIO_WAIT
 *
 */

void
gfs2_sync_buf(struct gfs2_glock *gl, int flags)
{
	ENTER(G2FN_SYNC_BUF)
	struct address_space *mapping = gl->gl_aspace->i_mapping;
	int error = 0;

	if (flags & DIO_START)
		error = filemap_fdatawrite(mapping);
	if (!error && (flags & DIO_WAIT))
		error = filemap_fdatawait(mapping);

	if (error)
		gfs2_io_error(gl->gl_sbd);

	RET(G2FN_SYNC_BUF);
}

/**
 * getbuf - Get a buffer with a given address space
 * @sdp: the filesystem
 * @aspace: the address space
 * @blkno: the block number (filesystem scope)
 * @create: TRUE if the buffer should be created
 *
 * Returns: the buffer
 */

static struct buffer_head *
getbuf(struct gfs2_sbd *sdp, struct inode *aspace, uint64_t blkno, int create)
{
	ENTER(G2FN_GETBUF)
	struct page *page;
	struct buffer_head *bh;
	unsigned int shift;
	unsigned long index;
	unsigned int bufnum;

	shift = PAGE_CACHE_SHIFT - sdp->sd_sb.sb_bsize_shift;
	index = blkno >> shift;             /* convert block to page */
	bufnum = blkno - (index << shift);  /* block buf index within page */

	if (create) {
		RETRY_MALLOC(page = grab_cache_page(aspace->i_mapping, index), page);
	} else {
		page = find_lock_page(aspace->i_mapping, index);
		if (!page)
			RETURN(G2FN_GETBUF, NULL);
	}

	if (!page_has_buffers(page))
		create_empty_buffers(page, sdp->sd_sb.sb_bsize, 0);

	/* Locate header for our buffer within our page */
	for (bh = page_buffers(page); bufnum--; bh = bh->b_this_page)
		/* Do nothing */;
	get_bh(bh);

	if (!buffer_mapped(bh))
		map_bh(bh, sdp->sd_vfs, blkno);
	else if (gfs2_assert_warn(sdp, bh->b_bdev == sdp->sd_vfs->s_bdev &&
				 bh->b_blocknr == blkno))
		map_bh(bh, sdp->sd_vfs, blkno);

	unlock_page(page);
	page_cache_release(page);

	RETURN(G2FN_GETBUF, bh);
}

/**
 * gfs2_dgetblk - Get a block
 * @gl: The glock associated with this block
 * @blkno: The block number
 *
 * Returns: The buffer
 */

struct buffer_head *
gfs2_dgetblk(struct gfs2_glock *gl, uint64_t blkno)
{
	ENTER(G2FN_DGETBLK)
	RETURN(G2FN_DGETBLK, getbuf(gl->gl_sbd, gl->gl_aspace, blkno, CREATE));
}

/**
 * gfs2_dread - Read a block from disk
 * @gl: The glock covering the block
 * @blkno: The block number
 * @flags: flags to gfs2_dreread()
 * @bhp: the place where the buffer is returned (NULL on failure)
 *
 * Returns: errno
 */

int
gfs2_dread(struct gfs2_glock *gl, uint64_t blkno,
	  int flags, struct buffer_head **bhp)
{
	ENTER(G2FN_DREAD)
	int error;

	*bhp = gfs2_dgetblk(gl, blkno);
	error = gfs2_dreread(gl->gl_sbd, *bhp, flags);
	if (error)
		brelse(*bhp);

	RETURN(G2FN_DREAD, error);
}

/**
 * gfs2_prep_new_buffer - Mark a new buffer we just gfs2_dgetblk()ed uptodate
 * @bh: the buffer
 *
 */

void
gfs2_prep_new_buffer(struct buffer_head *bh)
{
	ENTER(G2FN_PREP_NEW_BUFFER)
	struct gfs2_meta_header *mh = (struct gfs2_meta_header *)bh->b_data;

	lock_buffer(bh);
	clear_buffer_dirty(bh);
	set_buffer_uptodate(bh);
	unlock_buffer(bh);

	mh->mh_magic = cpu_to_gfs2_32(GFS2_MAGIC);
	mh->mh_blkno = cpu_to_gfs2_64(bh->b_blocknr);
	
	RET(G2FN_PREP_NEW_BUFFER);
}

/**
 * gfs2_dreread - Reread a block from disk
 * @sdp: the filesystem
 * @bh: The block to read
 * @flags: Flags that control the read
 *
 * Returns: errno
 */

int
gfs2_dreread(struct gfs2_sbd *sdp, struct buffer_head *bh, int flags)
{
	ENTER(G2FN_DREREAD)

	if (unlikely(test_bit(SDF_SHUTDOWN, &sdp->sd_flags)))
		RETURN(G2FN_DREREAD, -EIO);

	if (flags & DIO_FORCE)
		clear_buffer_uptodate(bh);

	if ((flags & DIO_START) && !buffer_uptodate(bh))
		ll_rw_block(READ, 1, &bh);

	if (flags & DIO_WAIT) {
		wait_on_buffer(bh);

		if (!buffer_uptodate(bh)) {
			if (get_transaction)
				gfs2_io_error_bh(sdp, bh);
			RETURN(G2FN_DREREAD, -EIO);
		}
		if (unlikely(test_bit(SDF_SHUTDOWN, &sdp->sd_flags)))
			RETURN(G2FN_DREREAD, -EIO);
	}

	RETURN(G2FN_DREREAD, 0);
}

/**
 * gfs2_dwrite - Write a buffer to disk (and/or wait for write to complete)
 * @sdp: the filesystem
 * @bh: The buffer to write
 * @flags:  DIO_XXX The type of write/wait operation to do
 *
 * Returns: errno
 */

int
gfs2_dwrite(struct gfs2_sbd *sdp, struct buffer_head *bh, int flags)
{
	ENTER(G2FN_DWRITE)

	if (gfs2_assert_warn(sdp, !test_bit(SDF_ROFS, &sdp->sd_flags)))
		RETURN(G2FN_DWRITE, -EIO);
	if (unlikely(test_bit(SDF_SHUTDOWN, &sdp->sd_flags)))
		RETURN(G2FN_DWRITE, -EIO);

	if (flags & DIO_CLEAN) {
		lock_buffer(bh);
		clear_buffer_dirty(bh);
		unlock_buffer(bh);
	}

	if (flags & DIO_DIRTY) {
		if (gfs2_assert_warn(sdp, buffer_uptodate(bh)))
			RETURN(G2FN_DWRITE, -EIO);
		mark_buffer_dirty(bh);
	}

	if ((flags & DIO_START) && buffer_dirty(bh)) {
		wait_on_buffer(bh);
		ll_rw_block(WRITE, 1, &bh);
	}

	if (flags & DIO_WAIT) {
		wait_on_buffer(bh);

		if (!buffer_uptodate(bh) || buffer_dirty(bh)) {
			gfs2_io_error_bh(sdp, bh);
			RETURN(G2FN_DWRITE, -EIO);
		}
		if (unlikely(test_bit(SDF_SHUTDOWN, &sdp->sd_flags)))
			RETURN(G2FN_DWRITE, -EIO);
	}

	RETURN(G2FN_DWRITE, 0);
}

/**
 * gfs2_attach_bufdata - attach a struct gfs2_bufdata structure to a buffer
 * @gl: the glock the buffer belongs to
 * @bh: The buffer to be attached to
 *
 */

void
gfs2_attach_bufdata(struct gfs2_glock *gl, struct buffer_head *bh)
{
	ENTER(G2FN_ATTACH_BUFDATA)
	struct gfs2_bufdata *bd;

	lock_page(bh->b_page);

	/* If there's one attached already, we're done */
	if (get_v2bd(bh)) {
		unlock_page(bh->b_page);
		RET(G2FN_ATTACH_BUFDATA);
	}

	RETRY_MALLOC(bd = kmem_cache_alloc(gfs2_bufdata_cachep, GFP_KERNEL), bd);
	gfs2_memory_add(bd);
	atomic_inc(&gl->gl_sbd->sd_bufdata_count);

	memset(bd, 0, sizeof(struct gfs2_bufdata));

	bd->bd_bh = bh;
	bd->bd_gl = gl;

	INIT_LIST_HEAD(&bd->bd_list_tr);
	INIT_LE(&bd->bd_le, &gfs2_buf_lops);

	set_v2bd(bh, bd);

	unlock_page(bh->b_page);

	RET(G2FN_ATTACH_BUFDATA);
}

/**
 * gfs2_dpin - Pin a metadata buffer in memory
 * @sdp: the filesystem the buffer belongs to
 * @bh: The buffer to be pinned
 *
 * "Pinning" means keeping buffer from being written to its in-place location.
 * A buffer should be pinned from the time it is added to a new transaction,
 *   until after it has been written to the log.
 * If an earlier change to this buffer is still pinned, waiting to be written
 *   to on-disk log, we need to keep a "frozen" copy of the old data while this
 *   transaction is modifying the real data.  We keep the frozen copy until
 *   this transaction's incore_commit(), i.e. until the transaction has
 *   finished modifying the real data, at which point we can use the real
 *   buffer for logging, even if the frozen copy didn't get written to the log.
 *
 */

void
gfs2_dpin(struct gfs2_sbd *sdp, struct buffer_head *bh)
{
	ENTER(G2FN_DPIN)
	struct gfs2_bufdata *bd = get_v2bd(bh);

	gfs2_assert_withdraw(sdp, !test_bit(SDF_ROFS, &sdp->sd_flags));

	if (test_set_buffer_pinned(bh))
		gfs2_assert_withdraw(sdp, FALSE);

	wait_on_buffer(bh);

	/* If this buffer is in the AIL and it has already been written
	   to in-place disk block, remove it from the AIL. */

	gfs2_log_lock(sdp);
	if (bd->bd_ail && !buffer_in_io(bh))
		list_move(&bd->bd_ail_st_list, &bd->bd_ail->ai_ail2_list);
	gfs2_log_unlock(sdp);

	clear_buffer_dirty(bh);
	wait_on_buffer(bh);

	if (!buffer_uptodate(bh))
		gfs2_io_error_bh(sdp, bh);

	get_bh(bh);

	RET(G2FN_DPIN);
}

/**
 * gfs2_dunpin - Unpin a buffer
 * @sdp: the filesystem the buffer belongs to
 * @bh: The buffer to unpin
 * @tr: The transaction in the AIL that contains this buffer
 *      If NULL, don't attach buffer to any AIL list
 *      (i.e. when dropping a pin reference when merging a new transaction
 *       with an already existing incore transaction)
 *
 * Called for (meta) buffers, after they've been logged to on-disk journal.
 * Make a (meta) buffer writeable to in-place location on-disk, if recursive
 *   pin count is 1 (i.e. no other, later transaction is modifying this buffer).
 * Add buffer to AIL lists of 1) the latest transaction that's modified and
 *   logged (on-disk) the buffer, and of 2) the glock that protects the buffer.
 * A single buffer might have been modified by more than one transaction
 *   since the buffer's previous write to disk (in-place location).  We keep
 *   the buffer on only one transaction's AIL list, i.e. that of the latest
 *   transaction that's completed logging this buffer (no need to write it to
 *   in-place block multiple times for multiple transactions, only once with
 *   the most up-to-date data).
 * A single buffer will be protected by one and only one glock.  If buffer is 
 *   already on a (previous) transaction's AIL, we know that we're already
 *   on buffer's glock's AIL.
 * 
 */

void
gfs2_dunpin(struct gfs2_sbd *sdp, struct buffer_head *bh,
	   struct gfs2_ail *ai)
{
	ENTER(G2FN_DUNPIN)
	struct gfs2_bufdata *bd = get_v2bd(bh);

	gfs2_assert_withdraw(sdp, buffer_uptodate(bh));

	if (!buffer_pinned(bh))
		gfs2_assert_withdraw(sdp, FALSE);

	mark_buffer_dirty(bh);
	clear_buffer_pinned(bh);

	gfs2_log_lock(sdp);
	if (bd->bd_ail) {
		list_del(&bd->bd_ail_st_list);
		brelse(bh);
	} else {
		struct gfs2_glock *gl = bd->bd_gl;
		list_add(&bd->bd_ail_gl_list, &gl->gl_ail_list);
		atomic_inc(&gl->gl_ail_count);
	}
	bd->bd_ail = ai;
	list_add(&bd->bd_ail_st_list, &ai->ai_ail1_list);
	gfs2_log_unlock(sdp);

	RET(G2FN_DUNPIN);
}

/**
 * gfs2_buf_wipe - make inode's buffers so they aren't dirty/AILed anymore
 * @ip: the inode who owns the buffers
 * @bstart: the first buffer in the run
 * @blen: the number of buffers in the run
 *
 * Called when de-allocating a contiguous run of meta blocks within an rgrp.
 */

void
gfs2_buf_wipe(struct gfs2_inode *ip, uint64_t bstart, uint32_t blen)
{
	ENTER(G2FN_BUF_WIPE)
	struct gfs2_sbd *sdp = ip->i_sbd;
	struct inode *aspace = ip->i_gl->gl_aspace;
	struct buffer_head *bh;

	while (blen) {
		bh = getbuf(sdp, aspace, bstart, NO_CREATE);
		if (bh) {
			struct gfs2_bufdata *bd = get_v2bd(bh);

			if (test_clear_buffer_pinned(bh)) {
				gfs2_log_lock(sdp);
				list_del_init(&bd->bd_le.le_list);
				gfs2_assert_warn(sdp, sdp->sd_log_num_buf);
				sdp->sd_log_num_buf--;
				gfs2_log_unlock(sdp);
				get_transaction->tr_num_buf_rm++;
				brelse(bh);
			}
			if (bd) {
				gfs2_log_lock(sdp);
				if (bd->bd_ail) {
					uint64_t blkno = bh->b_blocknr;
					bd->bd_ail = NULL;
					list_del(&bd->bd_ail_st_list);
					list_del(&bd->bd_ail_gl_list);
					atomic_dec(&bd->bd_gl->gl_ail_count);
					brelse(bh);
					gfs2_log_unlock(sdp);
					gfs2_trans_add_revoke(sdp, blkno);
				} else
					gfs2_log_unlock(sdp);
			}

			lock_buffer(bh);
			clear_buffer_dirty(bh);
			clear_buffer_uptodate(bh);
			unlock_buffer(bh);

			brelse(bh);
		}

		bstart++;
		blen--;
	}

	RET(G2FN_BUF_WIPE);
}

/**
 * gfs2_sync_meta - sync all the buffers in a filesystem
 * @sdp: the filesystem
 *
 * Flush metadata blocks to on-disk journal, then
 * Flush metadata blocks (now in AIL) to on-disk in-place locations
 * Periodically keep checking until done (AIL empty)
 */

void
gfs2_sync_meta(struct gfs2_sbd *sdp)
{
	ENTER(G2FN_SYNC_META)

	gfs2_log_flush(sdp);
	for (;;) {
		gfs2_ail1_start(sdp, DIO_ALL);
		if (gfs2_ail1_empty(sdp, DIO_ALL))
			break;
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ / 10);
	}

	RET(G2FN_SYNC_META);
}

/**
 * gfs2_flush_meta_cache - get rid of any references on buffers for this inode
 * @ip: The GFS2 inode
 *
 * This releases buffers that are in the most-recently-used array of
 *   blocks used for indirect block addressing for this inode.
 * Don't confuse this with the meta-HEADER cache (mhc)!
 */

void
gfs2_flush_meta_cache(struct gfs2_inode *ip)
{
	ENTER(G2FN_FLUSH_META_CACHE)
	struct buffer_head **bh_slot;
	unsigned int x;

	spin_lock(&ip->i_lock);

	for (x = 0; x < GFS2_MAX_META_HEIGHT; x++) {
		bh_slot = &ip->i_cache[x];
		if (*bh_slot) {
			brelse(*bh_slot);
			*bh_slot = NULL;
		}
	}

	spin_unlock(&ip->i_lock);

	RET(G2FN_FLUSH_META_CACHE);
}

/**
 * gfs2_get_meta_buffer - Get a metadata buffer
 * @ip: The GFS2 inode
 * @height: The level of this buf in the metadata (indir addr) tree (if any)
 * @num: The block number (device relative) of the buffer
 * @new: Non-zero if we may create a new buffer
 * @bhp: the buffer is returned here
 *
 * Returns: errno
 */

int
gfs2_get_meta_buffer(struct gfs2_inode *ip, int height, uint64_t num, int new,
		    struct buffer_head **bhp)
{
	ENTER(G2FN_GET_META_BUFFER)
	struct buffer_head *bh, **bh_slot = &ip->i_cache[height];
	int error;

	/* Try to use the gfs2_inode's MRU metadata tree cache */
	spin_lock(&ip->i_lock);
	bh = *bh_slot;
	if (bh) {
		if (bh->b_blocknr == num)
			get_bh(bh);
		else
			bh = NULL;
	}
	spin_unlock(&ip->i_lock);

	if (bh) {
		if (new)
			gfs2_prep_new_buffer(bh);
		else {
			error = gfs2_dreread(ip->i_sbd, bh, DIO_START | DIO_WAIT);
			if (error) {
				brelse(bh);
				RETURN(G2FN_GET_META_BUFFER, error);
			}
		}
	} else {
		if (new) {
			bh = gfs2_dgetblk(ip->i_gl, num);
			gfs2_prep_new_buffer(bh);
		} else {
			error = gfs2_dread(ip->i_gl, num, DIO_START | DIO_WAIT, &bh);
			if (error)
				RETURN(G2FN_GET_META_BUFFER, error);
		}

		spin_lock(&ip->i_lock);
		if (*bh_slot != bh) {
			if (*bh_slot)
				brelse(*bh_slot);
			*bh_slot = bh;
			get_bh(bh);
		}
		spin_unlock(&ip->i_lock);
	}

	if (new) {
		if (gfs2_assert_warn(ip->i_sbd, height)) {
			brelse(bh);
			RETURN(G2FN_GET_META_BUFFER, -EIO);
		}
		gfs2_trans_add_bh(ip->i_gl, bh);
		gfs2_metatype_set(bh, GFS2_METATYPE_IN, GFS2_FORMAT_IN);
		gfs2_buffer_clear_tail(bh, sizeof(struct gfs2_meta_header));
	} else if (gfs2_metatype_check(ip->i_sbd, bh,
				      (height) ? GFS2_METATYPE_IN : GFS2_METATYPE_DI)) {
		brelse(bh);
		RETURN(G2FN_GET_META_BUFFER, -EIO);
	}

	*bhp = bh;

	RETURN(G2FN_GET_META_BUFFER, 0);
}

/**
 * gfs2_get_data_buffer - Get a data buffer
 * @ip: The GFS2 inode
 * @num: The block number (device relative) of the data block
 * @new: Non-zero if this is a new allocation
 * @bhp: the buffer is returned here
 *
 * Returns: errno
 */

int
gfs2_get_data_buffer(struct gfs2_inode *ip, uint64_t block, int new,
		    struct buffer_head **bhp)
{
	ENTER(G2FN_GET_DATA_BUFFER)
	struct buffer_head *bh;
	int error = 0;

	if (block == ip->i_num.no_addr) {
		if (gfs2_assert_warn(ip->i_sbd, !new))
			RETURN(G2FN_GET_DATA_BUFFER, -EIO);
		error = gfs2_dread(ip->i_gl, block, DIO_START | DIO_WAIT, &bh);
		if (error)
			RETURN(G2FN_GET_DATA_BUFFER, error);
		if (gfs2_metatype_check(ip->i_sbd, bh, GFS2_METATYPE_DI)) {
			brelse(bh);
			RETURN(G2FN_GET_DATA_BUFFER, -EIO);
		}
	} else if (gfs2_is_jdata(ip)) {
		if (new) {
			bh = gfs2_dgetblk(ip->i_gl, block);
			gfs2_prep_new_buffer(bh);
			gfs2_trans_add_bh(ip->i_gl, bh);
			gfs2_metatype_set(bh, GFS2_METATYPE_JD, GFS2_FORMAT_JD);
			gfs2_buffer_clear_tail(bh, sizeof(struct gfs2_meta_header));
		} else {
			error = gfs2_dread(ip->i_gl, block,
					  DIO_START | DIO_WAIT, &bh);
			if (error)
				RETURN(G2FN_GET_DATA_BUFFER, error);
			if (gfs2_metatype_check(ip->i_sbd, bh, GFS2_METATYPE_JD)) {
				brelse(bh);
				RETURN(G2FN_GET_DATA_BUFFER, -EIO);
			}
		}
	} else {
		if (new) {
			bh = gfs2_dgetblk(ip->i_gl, block);
			gfs2_prep_new_buffer(bh);
		} else {
			error = gfs2_dread(ip->i_gl, block,
					  DIO_START | DIO_WAIT, &bh);
			if (error)
				RETURN(G2FN_GET_DATA_BUFFER, error);
		}
	}

	*bhp = bh;

	RETURN(G2FN_GET_DATA_BUFFER, 0);
}

/**
 * gfs2_start_ra - start readahead on an extent of a file
 * @gl: the glock the blocks belong to
 * @dblock: the starting disk block
 * @extlen: the number of blocks in the extent
 *
 */

void
gfs2_start_ra(struct gfs2_glock *gl, uint64_t dblock, uint32_t extlen)
{
	ENTER(G2FN_START_RA)
	struct gfs2_sbd *sdp = gl->gl_sbd;
	struct inode *aspace = gl->gl_aspace;
	struct buffer_head *first_bh, *bh;
	uint32_t max_ra = gfs2_tune_get(sdp, gt_max_readahead) >> sdp->sd_sb.sb_bsize_shift;
	int error;

	if (!extlen || !max_ra)
		RET(G2FN_START_RA);
	if (extlen > max_ra)
		extlen = max_ra;

	first_bh = getbuf(sdp, aspace, dblock, CREATE);

	if (buffer_uptodate(first_bh))
		goto out;
	if (!buffer_locked(first_bh)) {
		error = gfs2_dreread(sdp, first_bh, DIO_START);
		if (error)
			goto out;
	}

	dblock++;
	extlen--;

	while (extlen) {
		bh = getbuf(sdp, aspace, dblock, CREATE);

		if (!buffer_uptodate(bh) && !buffer_locked(bh)) {
			error = gfs2_dreread(sdp, bh, DIO_START);
			brelse(bh);
			if (error)
				goto out;
		} else
			brelse(bh);

		dblock++;
		extlen--;

		if (buffer_uptodate(first_bh))
			break;
	}

 out:
	brelse(first_bh);

	RET(G2FN_START_RA);
}

