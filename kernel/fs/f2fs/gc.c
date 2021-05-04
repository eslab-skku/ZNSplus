/*
 * fs/f2fs/gc.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/backing-dev.h>
#include <linux/init.h>
#include <linux/f2fs_fs.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/freezer.h>

#include "f2fs.h"
#include "node.h"
#include "segment.h"
#include "gc.h"
#include <trace/events/f2fs.h>

int TOTAL_IM_COUNT = 0;
int IM_counter = 0;
static int gc_thread_func(void *data)
{
	struct f2fs_sb_info *sbi = data;
	struct f2fs_gc_kthread *gc_th = sbi->gc_thread;
	wait_queue_head_t *wq = &sbi->gc_thread->gc_wait_queue_head;
	long wait_ms;

	wait_ms = gc_th->min_sleep_time;

	do {
		if (try_to_freeze())
			continue;
		else
			wait_event_interruptible_timeout(*wq,
					kthread_should_stop(),
					msecs_to_jiffies(wait_ms));
		if (kthread_should_stop())
			break;

		if (sbi->sb->s_writers.frozen >= SB_FREEZE_WRITE) {
			increase_sleep_time(gc_th, &wait_ms);
			continue;
		}

#ifdef CONFIG_F2FS_FAULT_INJECTION
		if (time_to_inject(sbi, FAULT_CHECKPOINT))
			f2fs_stop_checkpoint(sbi, false);
#endif

		/*
		 * [GC triggering condition]
		 * 0. GC is not conducted currently.
		 * 1. There are enough dirty segments.
		 * 2. IO subsystem is idle by checking the # of writeback pages.
		 * 3. IO subsystem is idle by checking the # of requests in
		 *    bdev's request list.
		 *
		 * Note) We have to avoid triggering GCs frequently.
		 * Because it is possible that some segments can be
		 * invalidated soon after by user update or deletion.
		 * So, I'd like to wait some time to collect dirty segments.
		 */
		if (!mutex_trylock(&sbi->gc_mutex))
			continue;
		if (!is_idle(sbi)) {
			increase_sleep_time(gc_th, &wait_ms);
			mutex_unlock(&sbi->gc_mutex);
			continue;
		}

		if (has_enough_invalid_blocks(sbi))
			decrease_sleep_time(gc_th, &wait_ms);
		else
			increase_sleep_time(gc_th, &wait_ms);

		stat_inc_bggc_count(sbi);

		/* if return value is not zero, no victim was selected */
		if (f2fs_gc(sbi, test_opt(sbi, FORCE_FG_GC), true))
			wait_ms = gc_th->no_gc_sleep_time;

		trace_f2fs_background_gc(sbi->sb, wait_ms,
				prefree_segments(sbi), free_segments(sbi));

		/* balancing f2fs's metadata periodically */
		f2fs_balance_fs_bg(sbi);

	} while (!kthread_should_stop());
	return 0;
}

int start_gc_thread(struct f2fs_sb_info *sbi)
{
	struct f2fs_gc_kthread *gc_th;
	dev_t dev = sbi->sb->s_bdev->bd_dev;
	int err = 0;

	gc_th = f2fs_kmalloc(sbi, sizeof(struct f2fs_gc_kthread), GFP_KERNEL);
	if (!gc_th) {
		err = -ENOMEM;
		goto out;
	}

	gc_th->min_sleep_time = DEF_GC_THREAD_MIN_SLEEP_TIME;
	gc_th->max_sleep_time = DEF_GC_THREAD_MAX_SLEEP_TIME;
	gc_th->no_gc_sleep_time = DEF_GC_THREAD_NOGC_SLEEP_TIME;

	gc_th->gc_idle = 0;

	sbi->gc_thread = gc_th;
	init_waitqueue_head(&sbi->gc_thread->gc_wait_queue_head);
	sbi->gc_thread->f2fs_gc_task = kthread_run(gc_thread_func, sbi,
			"f2fs_gc-%u:%u", MAJOR(dev), MINOR(dev));
	if (IS_ERR(gc_th->f2fs_gc_task)) {
		err = PTR_ERR(gc_th->f2fs_gc_task);
		kfree(gc_th);
		sbi->gc_thread = NULL;
	}
out:
	return err;
}

void stop_gc_thread(struct f2fs_sb_info *sbi)
{
	struct f2fs_gc_kthread *gc_th = sbi->gc_thread;
	if (!gc_th)
		return;
	kthread_stop(gc_th->f2fs_gc_task);
	kfree(gc_th);
	sbi->gc_thread = NULL;
}

static int select_gc_type(struct f2fs_gc_kthread *gc_th, int gc_type)
{
	int gc_mode = (gc_type == BG_GC) ? GC_CB : GC_GREEDY;

	if (gc_th && gc_th->gc_idle) {
		if (gc_th->gc_idle == 1)
			gc_mode = GC_CB;
		else if (gc_th->gc_idle == 2)
			gc_mode = GC_GREEDY;
	}
	return gc_mode;
}

static void select_policy(struct f2fs_sb_info *sbi, int gc_type,
		int type, struct victim_sel_policy *p)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);

	if (p->alloc_mode == SSR) {
		p->gc_mode = GC_GREEDY;
		p->dirty_segmap = dirty_i->dirty_segmap[DIRTY];
		p->max_search = dirty_i->nr_dirty[DIRTY];
		p->ofs_unit = sbi->segs_per_sec;
	} else {
		p->gc_mode = select_gc_type(sbi->gc_thread, gc_type);
		p->dirty_segmap = dirty_i->dirty_segmap[DIRTY];
		p->max_search = dirty_i->nr_dirty[DIRTY];
		p->ofs_unit = sbi->segs_per_sec;

	}

	/* we need to check every dirty segments in the FG_GC case */
	//if (gc_type != FG_GC && p->max_search > sbi->max_victim_search)
	//	p->max_search = sbi->max_victim_search;

	p->offset = sbi->last_victim[p->gc_mode];
}

static unsigned int check_bg_victims(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	unsigned int secno;

	/*
	 * If the gc_type is FG_GC, we can select victim segments
	 * selected by background GC before.
	 * Those segments guarantee they have small valid blocks.
	 */
	for_each_set_bit(secno, dirty_i->victim_secmap, MAIN_SECS(sbi)) {
		if (sec_usage_check(sbi, secno))
			continue;
		clear_bit(secno, dirty_i->victim_secmap);
		return secno * sbi->segs_per_sec;
	}
	return NULL_SEGNO;
}

static unsigned int get_cb_cost(struct f2fs_sb_info *sbi, unsigned int segno)
{
	struct sit_info *sit_i = SIT_I(sbi);
	unsigned int secno = GET_SECNO(sbi, segno);
	unsigned int start = secno * sbi->segs_per_sec;
	unsigned long long mtime = 0;
	unsigned int vblocks;
	unsigned char age = 0;
	unsigned char u;
	unsigned int i;

	for (i = 0; i < sbi->segs_per_sec; i++)
		mtime += get_seg_entry(sbi, start + i)->mtime;
	vblocks = get_valid_blocks(sbi, segno, sbi->segs_per_sec);

	mtime = div_u64(mtime, sbi->segs_per_sec);
	vblocks = div_u64(vblocks, sbi->segs_per_sec);

	u = (vblocks * 100) >> sbi->log_blocks_per_seg;

	/* Handle if the system time has changed by the user */
	if (mtime < sit_i->min_mtime)
		sit_i->min_mtime = mtime;
	if (mtime > sit_i->max_mtime)
		sit_i->max_mtime = mtime;
	if (sit_i->max_mtime != sit_i->min_mtime)
		age = 100 - div64_u64(100 * (mtime - sit_i->min_mtime),
				sit_i->max_mtime - sit_i->min_mtime);

	return UINT_MAX - ((100 * (100 - u) * age) / (100 + u));
}
unsigned char get_age(struct f2fs_sb_info *sbi, unsigned int segno)
{
	struct sit_info *sit_i = SIT_I(sbi);
	unsigned int secno = GET_SECNO(sbi, segno);
	unsigned int start = secno * sbi->segs_per_sec;
	unsigned long long mtime = 0;
	unsigned int i;

	for (i = 0; i < sbi->segs_per_sec; i++)
		mtime += get_seg_entry(sbi, start + i)->mtime;
	mtime = div_u64(mtime, sbi->segs_per_sec);

	/* Handle if the system time has changed by the user */
	if (mtime < sit_i->min_mtime)
		sit_i->min_mtime = mtime;
	if (mtime > sit_i->max_mtime)
		sit_i->max_mtime = mtime;
	if (sit_i->max_mtime != sit_i->min_mtime)
		return 100 - div64_u64(100 * (mtime - sit_i->min_mtime),
				sit_i->max_mtime - sit_i->min_mtime);
	else
		return 0;
}
static inline unsigned int get_gc_cost(struct f2fs_sb_info *sbi,
		unsigned int segno, struct victim_sel_policy *p)
{
	if (p->alloc_mode == SSR)
		return get_ckpt_valid_blocks(sbi, segno, sbi->segs_per_sec);
	/* alloc_mode == LFS */
	if (p->gc_mode == GC_GREEDY)
		return get_valid_blocks(sbi, segno, sbi->segs_per_sec);
	else
		return get_cb_cost(sbi, segno);
}
static inline unsigned int find_gc_cost(struct f2fs_sb_info *sbi,
		unsigned int segno, struct victim_sel_policy *p)
{
	if (p->alloc_mode == SSR) {
		int i, ofsuno = segno / p->ofs_unit, cost = 0;

		for (i = ofsuno * p->ofs_unit; i < (ofsuno + 1) * p->ofs_unit; i++) {
			cost += get_seg_entry(sbi, i)->ckpt_valid_blocks;
		}
		return cost;
	}

	/* alloc_mode == LFS */
	return get_valid_blocks(sbi, segno, sbi->segs_per_sec);
}

static unsigned int count_bits(const unsigned long *addr,
		unsigned int offset, unsigned int len)
{
	unsigned int end = offset + len, sum = 0;

	while (offset < end) {
		if (test_bit(offset++, addr))
			++sum;
	}
	return sum;
}
int find_victim(struct f2fs_sb_info *sbi, 
		unsigned int *result, int gc_type, int type, char alloc_mode, bool checkpoint, unsigned char *age_input)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	struct victim_sel_policy p;
	unsigned int secno, last_victim;
	unsigned int last_segment = MAIN_SEGS(sbi);
	unsigned int nsearched = 0;

	mutex_lock(&dirty_i->seglist_lock);

	p.alloc_mode = alloc_mode;

	if(alloc_mode == SSR)
	{
		p.gc_mode = GC_GREEDY;
	} else
	{
		p.gc_mode= select_gc_type(sbi->gc_thread, gc_type);
	}
	p.dirty_segmap = dirty_i->dirty_segmap[type];
	p.max_search = dirty_i->nr_dirty[type];
	p.ofs_unit = sbi->segs_per_sec;
	if(alloc_mode != SSR)
	{
		p.dirty_segmap = dirty_i->dirty_segmap[DIRTY];
		p.max_search = dirty_i->nr_dirty[DIRTY];
		p.ofs_unit = sbi->segs_per_sec;
	}

	p.min_segno = NULL_SEGNO;
	p.min_cost = UINT_MAX;
	p.offset = 0;

	if (p.max_search == 0)
		goto out;

	last_victim = sbi->last_victim[p.gc_mode];
	while (1) {
		unsigned long cost;
		unsigned int segno;
		segno = find_next_bit(p.dirty_segmap, last_segment, p.offset);
		if (segno >= last_segment) {
			if (sbi->last_victim[p.gc_mode]) {
				last_segment = sbi->last_victim[p.gc_mode];
				p.offset = 0;
				continue;
			}
			break;
		}

		p.offset = segno + p.ofs_unit;
		if (p.ofs_unit > 1) {
			p.offset -= segno % p.ofs_unit;
			nsearched += count_bits(p.dirty_segmap,
					p.offset - p.ofs_unit,
					p.ofs_unit);
		} else {
			nsearched++;
		}

		secno = GET_SECNO(sbi, segno);

		if (sec_usage_check(sbi, secno))
			goto next;
		if (gc_type == BG_GC && test_bit(secno, dirty_i->victim_secmap))
			goto next;

		cost = get_gc_cost(sbi, segno, &p);

		if (p.min_cost > cost) {
			p.min_segno = segno;
			p.min_cost = cost;
		}
next:
		if (nsearched >= p.max_search) {
			break;
		}
	}
	if (p.min_segno != NULL_SEGNO) {
		*result = (p.min_segno / p.ofs_unit) * p.ofs_unit;

	}
out:
//	printk("victim-ssr %d %d %d %d\n", alloc_mode, type, p.min_segno, p.min_cost);
	mutex_unlock(&dirty_i->seglist_lock);

	return (p.min_segno == NULL_SEGNO) ? 0 : 1;
}
/*
 * This function is called from two paths.
 * One is garbage collection and the other is SSR segment selection.
 * When it is called during GC, it just gets a victim segment
 * and it does not remove it from dirty seglist.
 * When it is called from SSR segment selection, it finds a segment
 * which has minimum valid blocks and removes it from dirty seglist.
 */
static int get_victim_by_default(struct f2fs_sb_info *sbi,
		unsigned int *result, int gc_type, int type, char alloc_mode)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	struct victim_sel_policy p;
	unsigned int secno, last_victim;
	unsigned int last_segment = MAIN_SEGS(sbi);
	unsigned int nsearched = 0;

	mutex_lock(&dirty_i->seglist_lock);

	p.alloc_mode = alloc_mode;
	select_policy(sbi, gc_type, type, &p);

	p.min_segno = NULL_SEGNO;
	p.min_cost = UINT_MAX;  //use section SSR
	if (p.max_search == 0)
		goto out;

	last_victim = sbi->last_victim[p.gc_mode];
	if (p.alloc_mode == LFS && gc_type == FG_GC) {
		p.min_segno = check_bg_victims(sbi);
		if (p.min_segno != NULL_SEGNO)
			goto got_it;
	}
	while (1) {
		unsigned long cost;
		unsigned int segno;
		segno = find_next_bit(p.dirty_segmap, last_segment, p.offset);
		if (segno >= last_segment) {
			if (sbi->last_victim[p.gc_mode]) {
				last_segment = sbi->last_victim[p.gc_mode];
				sbi->last_victim[p.gc_mode] = 0;
				p.offset = 0;
				continue;
			}
			break;
		}

		p.offset = segno + p.ofs_unit;
		if (p.ofs_unit > 1) {
			p.offset -= segno % p.ofs_unit;
			nsearched += count_bits(p.dirty_segmap,
					p.offset - p.ofs_unit,
					p.ofs_unit);
		} else {
			nsearched++;
		}

		secno = GET_SECNO(sbi, segno);
		if (sec_usage_check(sbi, secno))
                        goto next;

		if (gc_type == BG_GC && test_bit(secno, dirty_i->victim_secmap))
			goto next;

		cost = get_gc_cost(sbi, segno, &p);
		if (p.min_cost > cost) {
			if(alloc_mode != SSR ||
					find_next_bit(DIRTY_I(sbi)->dirty_segmap[type], last_segment, segno) == segno)
			{
				p.min_segno = segno;
				p.min_cost = cost;
			}
		}
next:
		if (nsearched >= p.max_search) {
			if (!sbi->last_victim[p.gc_mode] && segno <= last_victim)
				sbi->last_victim[p.gc_mode] = last_victim + 1;
			else
				sbi->last_victim[p.gc_mode] = segno + 1;
			break;
		}
	}

	if (p.min_segno != NULL_SEGNO) {
got_it:
		if (p.alloc_mode == LFS) {
			secno = GET_SECNO(sbi, p.min_segno);
			if (gc_type == FG_GC)
				sbi->cur_victim_sec = secno;
			else
				set_bit(secno, dirty_i->victim_secmap);
		}

		*result = (p.min_segno / p.ofs_unit) * p.ofs_unit;
		trace_f2fs_get_victim(sbi->sb, type, gc_type, &p,
				sbi->cur_victim_sec,
				prefree_segments(sbi), free_segments(sbi));
	}
out:
	mutex_unlock(&dirty_i->seglist_lock);


	return (p.min_segno == NULL_SEGNO) ? 0 : 1;
}
static const struct victim_selection default_v_ops = {
	.get_victim = get_victim_by_default,
};

static struct inode *find_gc_inode(struct gc_inode_list *gc_list, nid_t ino)
{
	struct inode_entry *ie;

	ie = radix_tree_lookup(&gc_list->iroot, ino);
	if (ie)
		return ie->inode;
	return NULL;
}

static void add_gc_inode(struct gc_inode_list *gc_list, struct inode *inode)
{
	struct inode_entry *new_ie;

	if (inode == find_gc_inode(gc_list, inode->i_ino)) {
		iput(inode);
		return;
	}
	new_ie = f2fs_kmem_cache_alloc(inode_entry_slab, GFP_NOFS);
	new_ie->inode = inode;

	f2fs_radix_tree_insert(&gc_list->iroot, inode->i_ino, new_ie);
	list_add_tail(&new_ie->list, &gc_list->ilist);
}

static void put_gc_inode(struct gc_inode_list *gc_list)
{
	struct inode_entry *ie, *next_ie;
	list_for_each_entry_safe(ie, next_ie, &gc_list->ilist, list) {
		radix_tree_delete(&gc_list->iroot, ie->inode->i_ino);
		iput(ie->inode);
		list_del(&ie->list);
		kmem_cache_free(inode_entry_slab, ie);
	}
}

static int check_valid_map(struct f2fs_sb_info *sbi,
		unsigned int segno, int offset)
{
	struct sit_info *sit_i = SIT_I(sbi);
	struct seg_entry *sentry;
	int ret;

	mutex_lock(&sit_i->sentry_lock);
	sentry = get_seg_entry(sbi, segno);
	ret = f2fs_test_bit(offset, sentry->cur_valid_map);
	mutex_unlock(&sit_i->sentry_lock);
	return ret;
}
#ifdef F2FS_INTERNAL_MERGE
struct PD_aware_struct {
#ifdef F2FS_CPBK_AWARE_GC
#if (NUM_FTL_BANKS < 8)
	unsigned long page_addr[NUM_FTL_BANKS][64];
#else
	unsigned long page_addr[NUM_FTL_BANKS][16];
#endif
	unsigned long count[NUM_FTL_BANKS];
#else
	unsigned long page_addr[128];
	unsigned long count;
#endif
	unsigned long single_page_count;
	unsigned long single_page_off[512];
};
#endif

/*
 * This function compares node address got in summary with that in NAT.
 * On validity, copy that node with cold status, otherwise (invalid node)
 * ignore that.
 */
static void gc_node_segment(struct f2fs_sb_info *sbi,
		struct f2fs_summary *sum, unsigned int segno, int gc_type)
{
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;
	unsigned long tick;
#ifdef F2FS_ADD_STAT
	int type = get_seg_entry(sbi,segno)->type;
#endif

	tick = rdtsc();
	start_addr = START_BLOCK(sbi, segno);
//	printk("GC(node) - %d(%d) type: %u  vblock: %u\n", segno, segno / 16, gc_type, get_valid_blocks(sbi, segno, 1));
next_step:
	entry = sum;

	for (off = 0; off < sbi->blocks_per_seg; off++, entry++) {
		nid_t nid = le32_to_cpu(entry->nid);
		struct page *node_page;
		struct node_info ni;

		/* stop BG_GC if there is not enough free sections. */
		if (gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0))
			return;

		if (check_valid_map(sbi, segno, off) == 0)
			continue;

		if (phase == 0) {
			ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
					META_NAT, true);
			continue;
		}

		if (phase == 1) {
			ra_node_page(sbi, nid);
			continue;
		}

		/* phase == 2 */
		node_page = get_node_page(sbi, nid);
		if (IS_ERR(node_page))
			continue;

		/* block may become invalid during get_node_page */
		if (check_valid_map(sbi, segno, off) == 0) {
			f2fs_put_page(node_page, 1);
			continue;
		}

		get_node_info(sbi, nid, &ni);
		if (ni.blk_addr != start_addr + off) {
			f2fs_put_page(node_page, 1);
			continue;
		}

		move_node_page(node_page, gc_type);
		stat_inc_node_blk_count(sbi, 1, gc_type);
		sbi->gc_count++;
#ifdef F2FS_ADD_STAT
		stat_inc_copy_count(sbi,type);
#endif
	}

	tick = rdtsc();

	if (++phase < 3)
		goto next_step;
}

/*
 * Calculate start block index indicating the given node offset.
 * Be careful, caller should give this node offset only indicating direct node
 * blocks. If any node offsets, which point the other types of node blocks such
 * as indirect or double indirect node blocks, are given, it must be a caller's
 * bug.
 */
block_t start_bidx_of_node(unsigned int node_ofs, struct inode *inode)
{
	unsigned int indirect_blks = 2 * NIDS_PER_BLOCK + 4;
	unsigned int bidx;

	if (node_ofs == 0)
		return 0;

	if (node_ofs <= 2) {
		bidx = node_ofs - 1;
	} else if (node_ofs <= indirect_blks) {
		int dec = (node_ofs - 4) / (NIDS_PER_BLOCK + 1);
		bidx = node_ofs - 2 - dec;
	} else {
		int dec = (node_ofs - indirect_blks - 3) / (NIDS_PER_BLOCK + 1);
		bidx = node_ofs - 5 - dec;
	}
	return bidx * ADDRS_PER_BLOCK + ADDRS_PER_INODE(inode);
}

static bool is_alive(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct node_info *dni, block_t blkaddr, unsigned int *nofs)
{
	struct page *node_page;
	nid_t nid;
	unsigned int ofs_in_node;
	block_t source_blkaddr;

	nid = le32_to_cpu(sum->nid);
	ofs_in_node = le16_to_cpu(sum->ofs_in_node);

	node_page = get_node_page(sbi, nid);
	if (IS_ERR(node_page))
		return false;

	get_node_info(sbi, nid, dni);

	if (sum->version != dni->version) {
		f2fs_put_page(node_page, 1);
		return false;
	}

	*nofs = ofs_of_node(node_page);
	source_blkaddr = datablock_addr(node_page, ofs_in_node);
	f2fs_put_page(node_page, 1);

	if (source_blkaddr != blkaddr)
		return false;
	return true;
}

static void move_encrypted_block(struct inode *inode, block_t bidx,
		unsigned int segno, int off)
{
	struct f2fs_io_info fio = {
		.sbi = F2FS_I_SB(inode),
		.type = DATA,
		.op = REQ_OP_READ,
		.op_flags = 0,
		.encrypted_page = NULL,
	};
	struct dnode_of_data dn;
	struct f2fs_summary sum;
	struct node_info ni;
	struct page *page;
	block_t newaddr;
	int err;

	/* do not read out */
	page = f2fs_grab_cache_page(inode->i_mapping, bidx, false);
	if (!page)
		return;

	if (!check_valid_map(F2FS_I_SB(inode), segno, off))
		goto out;

	set_new_dnode(&dn, inode, NULL, NULL, 0);
	err = get_dnode_of_data(&dn, bidx, LOOKUP_NODE);
	if (err)
		goto out;

	if (unlikely(dn.data_blkaddr == NULL_ADDR)) {
		ClearPageUptodate(page);
		goto put_out;
	}

	/*
	 * don't cache encrypted data into meta inode until previous dirty
	 * data were writebacked to avoid racing between GC and flush.
	 */
	f2fs_wait_on_page_writeback(page, DATA, true);

	get_node_info(fio.sbi, dn.nid, &ni);
	set_summary(&sum, dn.nid, dn.ofs_in_node, ni.version);

	/* read page */
	fio.page = page;
	fio.new_blkaddr = fio.old_blkaddr = dn.data_blkaddr;

	allocate_data_block(fio.sbi, NULL, fio.old_blkaddr, &newaddr,
			&sum, CURSEG_COLD_DATA, 1);
	fio.curseg_type = CURSEG_COLD_DATA;
	fio.encrypted_page = pagecache_get_page(META_MAPPING(fio.sbi), newaddr,
			FGP_LOCK | FGP_CREAT, GFP_NOFS);
	if (!fio.encrypted_page) {
		err = -ENOMEM;
		goto recover_block;
	}

	err = f2fs_submit_page_bio(&fio);
	if (err)
		goto put_page_out;

	/* write page */
	lock_page(fio.encrypted_page);

	if (unlikely(fio.encrypted_page->mapping != META_MAPPING(fio.sbi))) {
		err = -EIO;
		goto put_page_out;
	}
	if (unlikely(!PageUptodate(fio.encrypted_page))) {
		err = -EIO;
		goto put_page_out;
	}

	set_page_dirty(fio.encrypted_page);
	f2fs_wait_on_page_writeback(fio.encrypted_page, DATA, true);
	if (clear_page_dirty_for_io(fio.encrypted_page))
		dec_page_count(fio.sbi, F2FS_DIRTY_META);

	set_page_writeback(fio.encrypted_page);

	/* allocate block address */
	f2fs_wait_on_page_writeback(dn.node_page, NODE, true);

	fio.op = REQ_OP_WRITE;
	fio.op_flags = REQ_SYNC;
	fio.new_blkaddr = newaddr;
	f2fs_submit_page_mbio(&fio);

	f2fs_update_data_blkaddr(&dn, newaddr);
	set_inode_flag(inode, FI_APPEND_WRITE);
	if (page->index == 0)
		set_inode_flag(inode, FI_FIRST_BLOCK_WRITTEN);
put_page_out:
	f2fs_put_page(fio.encrypted_page, 1);
recover_block:
	if (err)
		__f2fs_replace_block(fio.sbi, &sum, newaddr, fio.old_blkaddr,
				true, true);
put_out:
	f2fs_put_dnode(&dn);
out:
	f2fs_put_page(page, 1);
}

static void move_data_page(struct inode *inode, block_t bidx, int gc_type,
		unsigned int segno, int off)
{
	struct page *page;
	page = get_lock_data_page(inode, bidx, true);
	if (IS_ERR(page))
		return;
	if (!check_valid_map(F2FS_I_SB(inode), segno, off))
		goto out;

	if (gc_type == BG_GC) {
		if (PageWriteback(page))
			goto out;
		set_page_dirty(page);
		set_cold_data(page);
	} else {
		struct f2fs_io_info fio = {
			.sbi = F2FS_I_SB(inode),
			.type = DATA,
			.op = REQ_OP_WRITE,
			.op_flags = REQ_SYNC,
			.page = page,
			.encrypted_page = NULL,
		};
		bool is_dirty = PageDirty(page);
		int err;

retry:
		set_page_dirty(page);
		f2fs_wait_on_page_writeback(page, DATA, true);
		if (clear_page_dirty_for_io(page)) {
			inode_dec_dirty_pages(inode);
			remove_dirty_inode(inode);
		}

		set_cold_data(page);
		err = do_write_data_page(&fio);
		if (err == -ENOMEM && is_dirty) {
			congestion_wait(BLK_RW_ASYNC, HZ/50);
			goto retry;
		}
	}
out:
	f2fs_put_page(page, 1);
}

#ifdef F2FS_INTERNAL_MERGE
static int pack_gc_info(struct inode *inode, block_t start_bidx, unsigned int segno, int off, int type)
{
	struct f2fs_sb_info *sbi = F2FS_SB(inode->i_sb);
	struct f2fs_summary sum;
	struct node_info ni;
	struct dnode_of_data dn;
	int err = 0;
	struct f2fs_io_info fio = {
		.sbi = F2FS_I_SB(inode),
		.type = DATA,
		.op = REQ_OP_WRITE,
		.op_flags = REQ_SYNC,
		.page = NULL,
		.encrypted_page = NULL,
	};

	if (!check_valid_map(F2FS_I_SB(inode), segno, off))
	{
		return 0;
	}

	set_new_dnode(&dn, inode, NULL, NULL, 0);
	err = get_dnode_of_data(&dn, start_bidx, LOOKUP_NODE);
	BUG_ON(err);

	fio.old_blkaddr = dn.data_blkaddr;

	/* This page is already truncated */
	if (fio.old_blkaddr == NULL_ADDR) {
		f2fs_put_dnode(&dn);
		return 0;
	}

	get_node_info(sbi, dn.nid, &ni);
	set_summary(&sum, dn.nid, dn.ofs_in_node, ni.version);

	/* block reservation .. new block address is assigned in new_blk_addr */
	reserve_write_block(&sum, &fio, type);

	if(sbi->gc_info_buf == NULL) {
		sbi->gc_info_buf = kmalloc(PAGE_SIZE, GFP_NOFS);
gc_buf_alloc:
		if(sbi->gc_info_buf == NULL)
			goto gc_buf_alloc;
		memset(sbi->gc_info_buf, 0, PAGE_SIZE);
		sbi->gc_info_buf_idx = 0;
	}

	sbi->gc_info_buf[sbi->gc_info_buf_idx] = fio.old_blkaddr;

	sbi->gc_info_buf[sbi->gc_info_buf_idx + sbi->blocks_per_seg] = fio.new_blkaddr;

	sbi->gc_info_buf_idx++;
	sbi->gc_info_buf_type = type;


	f2fs_update_data_blkaddr(&dn, fio.new_blkaddr);
	set_inode_flag(inode, FI_APPEND_WRITE);
	if (start_bidx == 0)
		set_inode_flag(inode, FI_FIRST_BLOCK_WRITTEN);
	f2fs_put_dnode(&dn);

	stat_inc_IM_data_block(sbi, 1);
	return 1;
}
#endif /* ifdef F2FS_INTERNAL_MERGE */

#ifdef F2FS_INTERNAL_MERGE
static void gc_data_segment(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct gc_inode_list *gc_list, unsigned int segno, int gc_type)
{
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr;
	int off, iter;
	int phase = 0;
#ifdef F2FS_ADD_STAT
	int type = get_seg_entry(sbi,segno)->type;
#endif
	int dst_type;
	int dst_head;
	struct seg_entry *sentry_temp;
	struct f2fs_bio_info *mbio = &sbi->write_io[DATA];
	struct PD_aware_struct cpbk;
	unsigned long super_page_offset = 0;
	unsigned long cur_page_valid_count = 0;
	unsigned long cur_page_in_DRAM = 0;
	unsigned long cur_page_clean = 1;
	struct page *data_page;
	struct inode *inode;
	struct node_info dni; /* dnode info for the data */
	unsigned int ofs_in_node, nofs;
	block_t start_bidx;
	unsigned long cur_offset_single = 0;
	int IZC_start = 0, IZC_count = 0;
	int partial_valid_start = 0;
	struct curseg_info *curseg;
	bool IZC_on;
#ifdef F2FS_CPBK_AWARE_GC
	unsigned long cur_offset[NUM_FTL_BANKS];
#else
	unsigned long cur_offset;
#endif
	unsigned long tick;
	tick = rdtsc();
#ifdef F2FS_CPBK_AWARE_GC
	for(iter = 0; iter < NUM_FTL_BANKS; iter++) {
		cpbk.count[iter] = 0;
		cur_offset[iter] = 0;
	}
#else
	cpbk.count = 0;
	cur_offset = 0;
#endif
	cpbk.single_page_count = 0;
	sentry_temp = get_seg_entry(sbi, segno);
	if(CURSEG_HOT_DATA == sentry_temp->type) {
		dst_type = CURSEG_HOT_DATA;
	}
	else {
		dst_type = CURSEG_COLD_DATA;
	}
	
	dst_head = (get_next_address(sbi, dst_type) % (PAGES_PER_SUPERPAGE * NUM_FTL_BANKS));
	curseg = CURSEG_I(sbi, dst_type);
	IZC_on = curseg->alloc_type == LFS;

	start_addr = START_BLOCK(sbi, segno);
next_step:
	entry = sum;

	for (off = 0; off < sbi->blocks_per_seg; off++, entry++) {
		nid_t nid = le32_to_cpu(entry->nid);

		/* stop BG_GC if there is not enough free sections. */
		if (gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0))
			return;

		if (check_valid_map(sbi, segno, off) == 0)
			continue;

		if (phase == 0) {
			ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
					META_NAT, true);
			continue;
		}

		if (phase == 1) {
			ra_node_page(sbi, nid);
			continue;
		}

		/* Get an inode by ino with checking validity */
		if (!is_alive(sbi, entry, &dni, start_addr + off, &nofs))
			continue;

		if (phase == 2) {
			ra_node_page(sbi, dni.ino);
			continue;
		}
		ofs_in_node = le16_to_cpu(entry->ofs_in_node);
		if(phase == 3) {
			//			trace_printk("Read Start- %u\n", off);
			inode = f2fs_iget(sb, dni.ino);
			if (IS_ERR(inode) || is_bad_inode(inode))
				continue;

			/* if encrypted inode, let's go phase 3 */
			if (f2fs_encrypted_inode(inode) &&
					S_ISREG(inode->i_mode)) {
				add_gc_inode(gc_list, inode);
				continue;
			}

			start_bidx = start_bidx_of_node(nofs, inode);
			//			if(0) {
			if(gc_type == FG_GC && IZC_on == 1) {
				if(off / PAGES_PER_SUPERPAGE != super_page_offset) { 
					if(cur_page_valid_count) {
						if(dst_head % PAGES_PER_SUPERPAGE != 0)
						{
						}
						else
						{
							if(cur_page_in_DRAM != cur_page_valid_count)
							{
								if(cur_page_valid_count == PAGES_PER_SUPERPAGE)
								{
									unsigned long src_way, dst_way;
									src_way = ((off - 1) / PAGES_PER_SUPERPAGE) % NUM_FTL_BANKS;
									dst_way = (dst_head / PAGES_PER_SUPERPAGE) % NUM_FTL_BANKS;
									if(src_way == dst_way)
										stat_inc_orig_copyback(sbi, cur_page_valid_count);
								}
							}
						}

						stat_inc_IM_cached_pages(sbi, cur_page_in_DRAM);
						if(cur_page_in_DRAM == cur_page_valid_count)
							stat_inc_IM_all_cached_pages(sbi, cur_page_in_DRAM);
						dst_head = ((dst_head + cur_page_valid_count) % (PAGES_PER_SUPERPAGE * NUM_FTL_BANKS));
#ifdef ALL_PAGE_CACHED_CHECK
						if(cur_page_valid_count == PAGES_PER_SUPERPAGE && cur_page_clean == 1 && cur_page_in_DRAM != PAGES_PER_SUPERPAGE) {
#else
						if(cur_page_valid_count == PAGES_PER_SUPERPAGE && cur_page_clean == 1) {
#endif
							unsigned long bank, start_offset;
							unsigned long page_offset;
							unsigned long last_off = cpbk.single_page_off[cpbk.single_page_count - 1];

							page_offset = (last_off) % PAGES_PER_SUPERPAGE;
							start_offset = (last_off) / PAGES_PER_SUPERPAGE;
							bank = start_offset % NUM_FTL_BANKS;
							start_offset = start_offset * PAGES_PER_SUPERPAGE;
#ifdef F2FS_CPBK_AWARE_GC
							cpbk.page_addr[bank][cpbk.count[bank]] = start_offset;
							cpbk.count[bank]++;
#else
							cpbk.page_addr[cpbk.count] = start_offset;
							cpbk.count++;
#endif
							cpbk.single_page_count -= cur_page_valid_count;
							IZC_count++;

						}
#ifdef ALL_PAGE_CACHED_CHECK
						else if(cur_page_clean == 0 || cur_page_in_DRAM == cur_page_valid_count) {
#else
						else if(cur_page_clean == 0) {
#endif
							//Host responsible
							for(iter = 0; iter < cur_page_valid_count; iter++)
							{
								u32 off_single = cpbk.single_page_off[cpbk.single_page_count - iter - 1];
								struct f2fs_summary *entry_single;
								struct node_info dni_single;
								unsigned int ofs_in_node_single;
								struct inode *inode_single;
								block_t start_bidx_single;

								entry_single = sum + off_single;
								ofs_in_node_single = le16_to_cpu(entry_single->ofs_in_node);

								is_alive(sbi, entry_single, &dni_single, start_addr + off_single, &nofs);
								inode_single = f2fs_iget(sb, dni_single.ino);
								start_bidx_single = start_bidx_of_node(nofs, inode_single);
								data_page = get_read_data_page(inode_single, start_bidx_single + ofs_in_node_single, REQ_RAHEAD, true);
								f2fs_put_page(data_page, 0);
								iput(inode_single);
							}
							cpbk.single_page_count -= cur_page_valid_count;
						}
					}
					cur_page_valid_count = 0;
					cur_page_in_DRAM = 0;
					cur_page_clean = 1;
				}
				data_page = find_get_page(inode->i_mapping, start_bidx + ofs_in_node);
				if(data_page) {
					if(PageDirty(data_page)) {
						cur_page_clean = 0;
					}
					cur_page_in_DRAM++;
				}
				cur_page_valid_count++;
				cpbk.single_page_off[cpbk.single_page_count] = off;
				cpbk.single_page_count++;
				super_page_offset = off / PAGES_PER_SUPERPAGE;
			} else {

				data_page = get_read_data_page(inode,
						start_bidx + ofs_in_node, REQ_RAHEAD,
						true);
				if (IS_ERR(data_page)) {
					iput(inode);
					continue;
				}
			}
			f2fs_put_page(data_page, 0);
			add_gc_inode(gc_list, inode);
			continue;
		}
		if(phase == 4)
		{
			inode = find_gc_inode(gc_list, dni.ino);
			if (inode) {
				struct f2fs_inode_info *fi = F2FS_I(inode);
				bool locked = false;
				if (S_ISREG(inode->i_mode)) {
					if (!down_write_trylock(&fi->dio_rwsem[READ]))
						continue;
					if (!down_write_trylock(
								&fi->dio_rwsem[WRITE])) {
						up_write(&fi->dio_rwsem[READ]);
						continue;
					}
					locked = true;
				}

				start_bidx = start_bidx_of_node(nofs, inode)
					+ ofs_in_node;
				sbi->n_int_merge_fail++;
				/* normal gc */
				if (f2fs_encrypted_inode(inode) && S_ISREG(inode->i_mode))
					move_encrypted_block(inode, start_bidx, segno, off);
				else {
					move_data_page(inode, start_bidx, FG_GC, segno, off);
				}
				if (locked) {
					up_write(&fi->dio_rwsem[WRITE]);
					up_write(&fi->dio_rwsem[READ]);
				}
				stat_inc_data_blk_count(sbi, 1, gc_type);	
				sbi->gc_count++;
				stat_inc_orig_rw(sbi, 1);
			}
		}
	}
	if(phase == 3 && IZC_on == 1) 
	{
		if(cur_page_valid_count) {
			if(dst_head % PAGES_PER_SUPERPAGE != 0)
			{
			}
			else
			{
				if(cur_page_in_DRAM != cur_page_valid_count)
				{
					if(cur_page_valid_count == PAGES_PER_SUPERPAGE)
					{
						unsigned long src_way, dst_way;
						src_way = ((off - 1) / PAGES_PER_SUPERPAGE) % NUM_FTL_BANKS;
						dst_way = (dst_head / PAGES_PER_SUPERPAGE) % NUM_FTL_BANKS;
						if(src_way == dst_way)
							stat_inc_orig_copyback(sbi, cur_page_valid_count);
					}
				}
			}

			stat_inc_IM_cached_pages(sbi, cur_page_in_DRAM);
			if(cur_page_in_DRAM == cur_page_valid_count)
				stat_inc_IM_all_cached_pages(sbi, cur_page_in_DRAM);


#ifdef ALL_PAGE_CACHED_CHECK
			if(cur_page_valid_count == PAGES_PER_SUPERPAGE && cur_page_clean == 1 && cur_page_in_DRAM != PAGES_PER_SUPERPAGE) {
#else
			if(cur_page_valid_count == PAGES_PER_SUPERPAGE && cur_page_clean == 1) {
#endif
				unsigned long bank, start_offset;
				unsigned long last_off = cpbk.single_page_off[cpbk.single_page_count - 1];

				start_offset = (last_off) / PAGES_PER_SUPERPAGE;
				bank = start_offset % NUM_FTL_BANKS;
				start_offset = start_offset * PAGES_PER_SUPERPAGE;

#ifdef F2FS_CPBK_AWARE_GC
				cpbk.page_addr[bank][cpbk.count[bank]] = start_offset;
				cpbk.count[bank]++;
#else
				cpbk.page_addr[cpbk.count] = start_offset;
				cpbk.count++;
#endif
				cpbk.single_page_count -= cur_page_valid_count;
				IZC_count++;
			}
#ifdef ALL_PAGE_CACHED_CHECK
			else if(cur_page_clean == 0 || cur_page_in_DRAM == cur_page_valid_count) {
#else
			else if(cur_page_clean == 0) {
#endif
				for(iter = 0; iter < cur_page_valid_count; iter++)
				{
					u32 off_single = cpbk.single_page_off[cpbk.single_page_count - iter - 1];
					struct f2fs_summary *entry_single;
					struct node_info dni_single;
					unsigned int ofs_in_node_single;
					struct inode *inode_single;
					block_t start_bidx_single;

					entry_single = sum + off_single;
					ofs_in_node_single = le16_to_cpu(entry_single->ofs_in_node);

					is_alive(sbi, entry_single, &dni_single, start_addr + off_single, &nofs);
					inode_single = f2fs_iget(sb, dni_single.ino);
					start_bidx_single = start_bidx_of_node(nofs, inode_single);
					data_page = get_read_data_page(inode_single, start_bidx_single + ofs_in_node_single, REQ_RAHEAD, true);
					f2fs_put_page(data_page, 0);
					iput(inode_single);
				}
				cpbk.single_page_count -= cur_page_valid_count;
			}
			cur_page_valid_count = 0;
		}

		while(1)
		{
			if(IZC_count == 0)
			{
				mutex_lock(&sbi->IM_mutex[dst_type]);
				break;
			}	

			mutex_lock(&sbi->IM_mutex[dst_type]);
			if((get_next_address(sbi, dst_type) % PAGES_PER_SUPERPAGE) == 0)
				break;
			mutex_unlock(&sbi->IM_mutex[dst_type]);

			if(cur_offset_single != cpbk.single_page_count)
			{
				u32 offset;
				offset = cpbk.single_page_off[cur_offset_single];
				cur_offset_single++;

				entry = sum + offset;
				if(check_valid_map(sbi, segno, offset) == 0) {
					continue;
				}
				if(!is_alive(sbi, entry, &dni, start_addr + offset, &nofs))
					continue;
				ofs_in_node = le16_to_cpu(entry->ofs_in_node);
				inode = find_gc_inode(gc_list, dni.ino);

				if(inode) {
					start_bidx = start_bidx_of_node(nofs, inode) + ofs_in_node;
					sbi->n_int_merge++;
					pack_gc_info(inode, start_bidx, segno, offset, dst_type);

					if(sbi->gc_info_buf[sbi->blocks_per_seg + sbi->gc_info_buf_idx - 1] % (sbi->segs_per_sec * sbi->blocks_per_seg) == sbi->segs_per_sec * sbi->blocks_per_seg - 1) {
						down_write(&mbio->io_rwsem);
						if(mbio->bio)
							__submit_merged_bio(mbio);
						up_write(&mbio->io_rwsem);

						iotl_send_internal_merge(sbi);
						stat_inc_IM_data_count(sbi, 1);

						send_close_cmd(sbi, dst_type, NEXT_FREE_BLKADDR(sbi, CURSEG_I(sbi, dst_type)));

						curseg = CURSEG_I(sbi, dst_type);
						if(curseg->alloc_type == SSR)
						{
							goto section_changed_to_ssr;
						}

					}
		
					stat_inc_data_blk_count(sbi, 1, gc_type);
					sbi->gc_count++;
					stat_inc_copy_count(sbi, type);
				}
			}
			else
			{
#ifdef F2FS_CPBK_AWARE_GC
				unsigned long max_bank;
				unsigned long max_count = 0;
				for(iter = 0; iter < NUM_FTL_BANKS; iter++)
				{
					if(max_count < cpbk.count[iter]) {
						max_count = cpbk.count[iter];
						max_bank = iter;
					}
				}

				if(max_count == 0)
				{
					mutex_lock(&sbi->IM_mutex[dst_type]);
					IZC_count = 0;
					break;
				}

				for(iter = 0; iter < PAGES_PER_SUPERPAGE; iter++)
				{
					cpbk.single_page_off[cpbk.single_page_count] = cpbk.page_addr[max_bank][cpbk.count[max_bank] - 1] + iter;
					cpbk.single_page_count++;
				}
				cpbk.count[max_bank]--;
#else
				if(cpbk.count == 0)
				{
					mutex_lock(&sbi->IM_mutex[dst_type]);
					IZC_count = 0;
					break;
				}
				for(iter = 0; iter < PAGES_PER_SUPERPAGE; iter++)
				{
					cpbk.single_page_off[cpbk.single_page_count++] = cpbk.page_addr[cpbk.count - 1] + iter;
				}
				cpbk.count--;
#endif
			}
		}
	}

	curseg = CURSEG_I(sbi, dst_type);
	if(IZC_on == 1 && curseg->alloc_type == SSR)
	{
		goto section_changed_to_ssr;
	}
	//internal merge phase 4
	while (phase == 3) {
		unsigned long count;
		unsigned long start_offset;
		unsigned long next_block_offset = get_next_address(sbi, dst_type);
		unsigned long sub_page_offset = next_block_offset % PAGES_PER_SUPERPAGE;
		unsigned long complete = 1;
		unsigned long bank = (next_block_offset / PAGES_PER_SUPERPAGE) % NUM_FTL_BANKS;
		unsigned long cnt;

#ifdef F2FS_CPBK_AWARE_GC
		for(off = 0; off < NUM_FTL_BANKS; off++) {
			if(cpbk.count[off] != cur_offset[off])
				complete = 0;
		}
#else
		if(cpbk.count != cur_offset)
			complete = 0;
#endif
		if(cpbk.single_page_count != cur_offset_single)
			complete = 0;

		if(complete)
			break;

		if(sub_page_offset != 0 || partial_valid_start == 1) {
single_page:
			start_offset = cpbk.single_page_off[cur_offset_single];
			count = 1;
			cur_offset_single++;
			stat_inc_IM_pages(sbi, 1);
		} else {
			if(IZC_start == 0)
			{
				IZC_start = 1;
			}
#ifdef F2FS_CPBK_AWARE_GC
			if(cpbk.count[bank] == cur_offset[bank]) {
				unsigned long max_bank = NUM_FTL_BANKS, max_count = 0;
				for(iter = 0; iter < NUM_FTL_BANKS; iter++) {
					unsigned long temp_bank = (bank + NUM_FTL_BANKS - iter) % NUM_FTL_BANKS;	
					if(cpbk.count[temp_bank] - cur_offset[temp_bank] > max_count)
					{
						max_bank = temp_bank;
						max_count = cpbk.count[temp_bank] - cur_offset[temp_bank];
					}
				}
				if(max_bank == NUM_FTL_BANKS){
					partial_valid_start = 1;
					goto single_page;
				} else {
					start_offset = cpbk.page_addr[max_bank][cur_offset[max_bank]];
					count = PAGES_PER_SUPERPAGE;
					cur_offset[max_bank]++;
					stat_inc_IM_pages(sbi, PAGES_PER_SUPERPAGE);
				}
			} else {
				start_offset = cpbk.page_addr[bank][cur_offset[bank]];
				count = PAGES_PER_SUPERPAGE;
				cur_offset[bank]++;
				stat_inc_IM_cpbk_pages(sbi, PAGES_PER_SUPERPAGE);	
			}
#else
			if(cpbk.count == cur_offset) {
				partial_valid_start = 1;
				goto single_page;
			} else {
				unsigned long bank_cur = (cpbk.page_addr[cur_offset] / PAGES_PER_SUPERPAGE) % NUM_FTL_BANKS;
				start_offset = cpbk.page_addr[cur_offset];
				count = PAGES_PER_SUPERPAGE;
				cur_offset++;
				if(bank_cur == bank)
				{
					stat_inc_IM_cpbk_pages(sbi, PAGES_PER_SUPERPAGE);
				}
				else
				{
					stat_inc_IM_pages(sbi, PAGES_PER_SUPERPAGE);
				}
			}

#endif
		}
		for(cnt = 0; cnt < count; cnt++) {
			off = start_offset + cnt;
			entry = sum + off;
			if (check_valid_map(sbi, segno, off) == 0) {
				BUG_ON(1);
				continue;
			}
			/* Get an inode by ino with checking validity */
			if (!is_alive(sbi, entry, &dni, start_addr + off, &nofs))
				continue;
			ofs_in_node = le16_to_cpu(entry->ofs_in_node);

			inode = find_gc_inode(gc_list, dni.ino);
			if (inode) {

				start_bidx = start_bidx_of_node(nofs, inode)
					+ ofs_in_node;
				sbi->n_int_merge++;
				pack_gc_info(inode, start_bidx, segno, off, dst_type);

				if(sbi->gc_info_buf[sbi->blocks_per_seg + sbi->gc_info_buf_idx - 1] % (sbi->segs_per_sec * sbi->blocks_per_seg) == sbi->segs_per_sec * sbi->blocks_per_seg - 1)
				{
					down_write(&mbio->io_rwsem);
					if(mbio->bio)
						__submit_merged_bio(mbio);
					up_write(&mbio->io_rwsem);
					iotl_send_internal_merge(sbi);
					stat_inc_IM_data_count(sbi, 1);
					send_close_cmd(sbi, dst_type, NEXT_FREE_BLKADDR(sbi, CURSEG_I(sbi, dst_type)));

					curseg = CURSEG_I(sbi, dst_type);
					if(curseg->alloc_type == SSR)
					{
						goto section_changed_to_ssr;
					}
				}

				stat_inc_data_blk_count(sbi, 1, gc_type);
				sbi->gc_count++;

#ifdef F2FS_ADD_STAT
				stat_inc_copy_count(sbi,type);
#endif
			}
		}
	}
	if(0)
	{
section_changed_to_ssr:
#ifdef F2FS_CPBK_AWARE_GC
		for(off = 0; off < NUM_FTL_BANKS; off++) {
			cpbk.count[off] = cur_offset[off];
		}
#else
		cpbk.count = cur_offset;
#endif
	}
	if (sbi->gc_info_buf_idx > 0) {
		static u32 IM_counter = 0;
		IM_counter++;
		down_write(&mbio->io_rwsem);
		if(mbio->bio)
			__submit_merged_bio(mbio);
		up_write(&mbio->io_rwsem);

		iotl_send_internal_merge(sbi);
		stat_inc_IM_data_count(sbi, 1);
		IZC_start = 0;
	}
	else if (IZC_start == 1)
	{
		IZC_start = 0;
	}
	if(phase == 3 && IZC_on == 1)
	{
		mutex_unlock(&sbi->IM_mutex[dst_type]);
	}

	sbi->stat_info->gc_data_phase_time[phase] += rdtsc() - tick;
	tick = rdtsc();

	if(++phase < 5)
		goto next_step;
}
#else
static void gc_data_segment(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct gc_inode_list *gc_list, unsigned int segno, int gc_type)
{
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;
	unsigned long tick;
	tick = rdtsc();
	start_addr = START_BLOCK(sbi, segno);

next_step:
	entry = sum;

	for (off = 0; off < sbi->blocks_per_seg; off++, entry++) {
		struct page *data_page;
		struct inode *inode;
		struct node_info dni; /* dnode info for the data */
		unsigned int ofs_in_node, nofs;
		block_t start_bidx;
		nid_t nid = le32_to_cpu(entry->nid);

		/* stop BG_GC if there is not enough free sections. */
		if (gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0))
			return;

		if (check_valid_map(sbi, segno, off) == 0)
			continue;

		if (phase == 0) {
			ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
					META_NAT, true);
			continue;
		}

		if (phase == 1) {
			ra_node_page(sbi, nid);
			continue;
		}

		/* Get an inode by ino with checking validity */
		if (!is_alive(sbi, entry, &dni, start_addr + off, &nofs))
			continue;
		if (phase == 2) {
			ra_node_page(sbi, dni.ino);
			continue;
		}
		ofs_in_node = le16_to_cpu(entry->ofs_in_node);

		if (phase == 3) {
			inode = f2fs_iget(sb, dni.ino);
			if (IS_ERR(inode) || is_bad_inode(inode))
				continue;
			/* if encrypted inode, let's go phase 3 */
			if (f2fs_encrypted_inode(inode) &&
					S_ISREG(inode->i_mode)) {
				add_gc_inode(gc_list, inode);
				continue;
			}

			start_bidx = start_bidx_of_node(nofs, inode);
			data_page = get_read_data_page(inode,
					start_bidx + ofs_in_node, REQ_RAHEAD,
					true);
			if (IS_ERR(data_page)) {
				iput(inode);
				continue;
			}

			f2fs_put_page(data_page, 0);
			add_gc_inode(gc_list, inode);
			continue;
		}

		/* phase 4 */
		inode = find_gc_inode(gc_list, dni.ino);
		if (inode) {
			struct f2fs_inode_info *fi = F2FS_I(inode);
			bool locked = false;

			if (S_ISREG(inode->i_mode)) {
				if (!down_write_trylock(&fi->dio_rwsem[READ]))
					continue;
				if (!down_write_trylock(
							&fi->dio_rwsem[WRITE])) {
					up_write(&fi->dio_rwsem[READ]);
					continue;
				}
				locked = true;
			}

			start_bidx = start_bidx_of_node(nofs, inode)
				+ ofs_in_node;
			if (f2fs_encrypted_inode(inode) && S_ISREG(inode->i_mode))
				move_encrypted_block(inode, start_bidx, segno, off);
			else
			{
				move_data_page(inode, start_bidx, gc_type, segno, off);
			}

			if (locked) {
				up_write(&fi->dio_rwsem[WRITE]);
				up_write(&fi->dio_rwsem[READ]);
			}

			stat_inc_data_blk_count(sbi, 1, gc_type);
			sbi->gc_count++;
			stat_inc_orig_rw(sbi, 1);
		}
	}
	sbi->stat_info->gc_data_phase_time[phase] += rdtsc() - tick;
	tick = rdtsc();

	if (++phase < 5)
	{
		goto next_step;
	}
}
#endif
static int __get_victim(struct f2fs_sb_info *sbi, unsigned int *victim,
		int gc_type)
{
	struct sit_info *sit_i = SIT_I(sbi);
	int ret;

	mutex_lock(&sit_i->sentry_lock);
	ret = DIRTY_I(sbi)->v_ops->get_victim(sbi, victim, gc_type,
			NO_CHECK_TYPE, LFS);
	mutex_unlock(&sit_i->sentry_lock);
	return ret;
}

static int do_garbage_collect(struct f2fs_sb_info *sbi,
		unsigned int start_segno,
		struct gc_inode_list *gc_list, int gc_type)
{
	struct page *sum_page;
	struct f2fs_summary_block *sum;
	struct blk_plug plug;
	unsigned int segno = start_segno;
	unsigned int end_segno = start_segno + sbi->segs_per_sec;
	int sec_freed = 0;
	unsigned char type = IS_DATASEG(get_seg_entry(sbi, segno)->type) ?
		SUM_TYPE_DATA : SUM_TYPE_NODE;

	/* readahead multi ssa blocks those have contiguous address */
	if (sbi->segs_per_sec > 1)
		ra_meta_pages(sbi, GET_SUM_BLOCK(sbi, segno),
				sbi->segs_per_sec, META_SSA, true);

	/* reference all summary page */
	while (segno < end_segno) {
		sum_page = get_sum_page(sbi, segno++);
		unlock_page(sum_page);
	}

	blk_start_plug(&plug);

	for (segno = start_segno; segno < end_segno; segno++) {

		/* find segment summary of victim */
		sum_page = find_get_page(META_MAPPING(sbi),
				GET_SUM_BLOCK(sbi, segno));
		f2fs_put_page(sum_page, 0);

		if (get_valid_blocks(sbi, segno, 1) == 0 ||
				!PageUptodate(sum_page) ||
				unlikely(f2fs_cp_error(sbi)))
			goto next;

		sum = page_address(sum_page);
		f2fs_bug_on(sbi, type != GET_SUM_TYPE((&sum->footer)));

		/*
		 * this is to avoid deadlock:
		 * - lock_page(sum_page)         - f2fs_replace_block
		 *  - check_valid_map()            - mutex_lock(sentry_lock)
		 *   - mutex_lock(sentry_lock)     - change_curseg()
		 *                                  - lock_page(sum_page)
		 */
		if (type == SUM_TYPE_NODE)
			gc_node_segment(sbi, sum->entries, segno, gc_type);
		else
			gc_data_segment(sbi, sum->entries, gc_list, segno,
					gc_type);

		stat_inc_seg_count(sbi, type, gc_type);
next:
		f2fs_put_page(sum_page, 0);
	}
	if (gc_type == FG_GC)
		f2fs_submit_merged_bio(sbi,
				(type == SUM_TYPE_NODE) ? NODE : DATA, WRITE);
	blk_finish_plug(&plug);

	if (gc_type == FG_GC &&
			get_valid_blocks(sbi, start_segno, sbi->segs_per_sec) == 0)
		sec_freed = 1;

	stat_inc_call_count(sbi->stat_info);

	return sec_freed;
}
int f2fs_force_gc(struct f2fs_sb_info *sbi)
{
	unsigned int segno;
	int gc_type = FG_GC;
	int sec_freed = 0;
	int ret = -EINVAL;
	struct cp_control cpc;
	struct gc_inode_list gc_list = {
		.ilist = LIST_HEAD_INIT(gc_list.ilist),
		.iroot = RADIX_TREE_INIT(GFP_NOFS),
	};
	segno = NULL_SEGNO;

	DIRTY_I(sbi)->v_ops->get_victim(sbi, &segno, gc_type,
			CURSEG_WARM_DATA, LFS);

	if(segno == NULL_SEGNO)
		return 0;

#ifdef F2FS_INTERNAL_MERGE
	sbi->gc_info_buf_idx = 0;
	gc_type = FG_GC;
#endif
	if (do_garbage_collect(sbi, segno, &gc_list, gc_type) &&
			gc_type == FG_GC)
		sec_freed++;

	if (gc_type == FG_GC)
		sbi->cur_victim_sec = NULL_SEGNO;

	ret = write_checkpoint(sbi, &cpc);
	mutex_unlock(&sbi->gc_mutex);

	put_gc_inode(&gc_list);

	return ret;

}
int f2fs_gc(struct f2fs_sb_info *sbi, bool sync, bool background)
{
	unsigned int segno;
	int gc_type = sync ? FG_GC : BG_GC;
	int sec_freed = 0;
	int ret = -EINVAL;
	struct cp_control cpc;
	struct gc_inode_list gc_list = {
		.ilist = LIST_HEAD_INIT(gc_list.ilist),
		.iroot = RADIX_TREE_INIT(GFP_NOFS),
	};
	bool get_victim = false;
	unsigned char is_node_seg = SUM_TYPE_NODE;
	unsigned long t1, t2;
	t1 = rdtsc();
	cpc.reason = __get_cp_reason(sbi);
gc_more:
	segno = NULL_SEGNO;

	if (unlikely(!(sbi->sb->s_flags & MS_ACTIVE)))
		goto stop;
	if (unlikely(f2fs_cp_error(sbi))) {
		ret = -EIO;
		goto stop;
	}

	if (gc_type == BG_GC && has_not_enough_free_secs(sbi, sec_freed, 0)) {
		gc_type = FG_GC;
		get_victim = __get_victim(sbi, &segno, gc_type);

		/*
		 * If there is no victim and no prefree segment but still not
		 * enough free sections, we should flush dent/node blocks and do
		 * garbage collections.
		 */
		if (get_victim || prefree_segments(sbi)) {
			ret = write_checkpoint(sbi, &cpc);
			if (ret)
				goto stop;
			segno = NULL_SEGNO;
			sbi->cur_victim_sec = NULL_SEGNO;
		} else if (has_not_enough_free_secs(sbi, 0, 0)) {
			ret = write_checkpoint(sbi, &cpc);
			if (ret)
				goto stop;
		}
	} else if (gc_type == BG_GC && !background) {
		/* f2fs_balance_fs doesn't need to do BG_GC in critical path. */
		goto stop;
	}

	if (segno == NULL_SEGNO && !__get_victim(sbi, &segno, gc_type))
		goto stop;
	ret = 0;


	is_node_seg = IS_DATASEG(get_seg_entry(sbi, segno)->type) ?
                SUM_TYPE_DATA : SUM_TYPE_NODE;

	//trace_printk("GC cost: %d\n", get_valid_blocks(sbi, segno, sbi->segs_per_sec));
	if (do_garbage_collect(sbi, segno, &gc_list, gc_type) &&
			gc_type == FG_GC)
		sec_freed++;

	if (gc_type == FG_GC)
		sbi->cur_victim_sec = NULL_SEGNO;

	if (!sync) {
		unsigned int start_tick;
		if (has_not_enough_free_secs(sbi, sec_freed, 0))
		{
			start_tick = rdtsc();
			if(sec_freed > reserved_sections(sbi)/2)
				ret = write_checkpoint(sbi, &cpc);
			t2 = rdtsc();
			if(is_node_seg != SUM_TYPE_NODE)
				stat_inc_gc_data_time(sbi, t2-t1);
			t1 = rdtsc();
			if (ret)
				goto stop;
			
			goto gc_more;
		}
		start_tick = rdtsc();
		if (gc_type == FG_GC)
			ret = write_checkpoint(sbi, &cpc);
	}
	t2 = rdtsc();
	if(is_node_seg != SUM_TYPE_NODE)
             stat_inc_gc_data_time(sbi, t2-t1);
stop:
	mutex_unlock(&sbi->gc_mutex);

	put_gc_inode(&gc_list);
	if (sync)
		ret = sec_freed ? 0 : -EAGAIN;
	return ret;
}

void build_gc_manager(struct f2fs_sb_info *sbi)
{
	u64 main_count, resv_count, ovp_count, blocks_per_sec;

	DIRTY_I(sbi)->v_ops = &default_v_ops;

	/* threshold of # of valid blocks in a section for victims of FG_GC */
	main_count = SM_I(sbi)->main_segments << sbi->log_blocks_per_seg;
	resv_count = SM_I(sbi)->reserved_segments << sbi->log_blocks_per_seg;
	ovp_count = SM_I(sbi)->ovp_segments << sbi->log_blocks_per_seg;
	blocks_per_sec = sbi->blocks_per_seg * sbi->segs_per_sec;

}
