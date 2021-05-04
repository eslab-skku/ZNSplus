/* 
 * In-order Threaded Logging
 *
 */

#include <linux/fs.h>
#include <linux/f2fs_fs.h>
#include <linux/bio.h>
#include <linux/blkdev.h>

#include "f2fs.h"
#include "segment.h"

#define D2H READ
#define H2D WRITE

#define WAIT   1
#define NOWAIT 0

struct bio_dev_cmd_private {
    struct f2fs_sb_info *sbi;
    int prealloc_buf_idx;
    int prealloc_buf_len;
    void *org_buf_ptr;
    int org_buf_len;
    void *wait;
	void* buf_addr;
};

static void end_dev_cmd_io(struct bio *bio)
{
    struct bio_dev_cmd_private *priv;
    struct f2fs_sb_info *sbi;

    priv = (struct bio_dev_cmd_private *)bio->bi_private;
    sbi = priv->sbi;

    if (priv->wait) {
        complete(priv->wait);
    }
	kfree(priv->buf_addr);
    kfree(priv);
    bio_put(bio);
}

static int send_general_dev_cmd(struct f2fs_sb_info *sbi, int cmd_addr, 
				void *buf_ptr, int buf_len, int dir, int opt)
{
	struct page *page;
 	struct bio *bio;
 	int ret = 0;
 	struct bio_dev_cmd_private *priv;

retry_to_alloc:
 	priv = kmalloc(sizeof(struct bio_dev_cmd_private), GFP_NOFS);
 	if (!priv) {
 		cond_resched();
 		goto retry_to_alloc;
 	}

	priv->sbi = sbi;
 	priv->buf_addr = buf_ptr;

 bio_to_alloc:
 	/* No failure on bio allocation */
 	bio = bio_alloc(GFP_KERNEL, 1);
 	if(!bio) {
 		goto bio_to_alloc;	
 	}

	bio->bi_bdev = sbi->sb->s_bdev->bd_contains;

 	bio->bi_private = (void *)priv;
 	bio->bi_iter.bi_sector = cmd_addr*8;
 	bio->bi_end_io = end_dev_cmd_io;
 
 	//    if (IOTL_CMD_BUF_SIZE < PAGE_SIZE) {
 	page = virt_to_page(buf_ptr);
 	//        ret += bio_add_page(bio, page, prealloc_buf_len, offset);
 	ret += bio_add_page(bio, page, PAGE_SIZE, 0);
 	/*
 	   if(cmd_addr == ADDR_CLOSE_SECTION)
 	   trace_printk("%d %d %d] %d %d %d %d\n", ((u32*)buf_ptr)[0], ((u32*)buf_ptr)[1], ((u32*)buf_ptr)[2],
 	   bio->bi_iter.bi_sector,  bio->bi_iter.bi_size, 
 	   bio->bi_iter.bi_idx,  bio->bi_iter.bi_bvec_done);
 	 */
 
 	// bypass flag
 	bio_set_op_attrs(bio, REQ_OP_WRITE, F2FS_REQ_BYPASS_FLAGS);
 
 	// disable plugging
 	if (current->plug) {
 		blk_finish_plug(current->plug);
 	}

 	if (opt == WAIT) {
 		DECLARE_COMPLETION_ONSTACK(w);
 		priv->wait = (void *)&w;
 		bio->bi_opf |= REQ_FUA;
 		submit_bio(bio);
 		wait_for_completion(&w);
 	} else {
 		priv->wait = NULL;
 		submit_bio(bio);
 	}

	return 0;
}
void send_profile_cmd(struct f2fs_sb_info *sbi)
{
	void* cmd_buffer = NULL;

	printk("PROFILE\n");
	while(cmd_buffer == NULL)
 		cmd_buffer = kmalloc(PAGE_SIZE, GFP_NOFS);
 
         send_general_dev_cmd(sbi, ADDR_PROF, cmd_buffer, 0, H2D, WAIT);
}
#ifdef F2FS_INTERNAL_MERGE
int iotl_send_internal_merge(struct f2fs_sb_info *sbi)
{
//	printk("IZC - %u-%u \n", (sbi->gc_info_buf[512] + 32768) / 8192, sbi->gc_info_buf[512] % 8192);

	 	/* send internal command */
 	send_general_dev_cmd(sbi, ADDR_INTERNAL_MERGE, sbi->gc_info_buf, PAGE_SIZE, H2D, NOWAIT);
 alloc_err:
 	sbi->gc_info_buf = kmalloc(PAGE_SIZE, GFP_NOFS);
 	if(sbi->gc_info_buf == NULL)
 		goto alloc_err;
 
 	memset(sbi->gc_info_buf, 0, PAGE_SIZE);
 	
 	sbi->gc_info_buf_idx = 0;

	return 1;
}
#endif
#define F2FS_UINT_SIZE	(32)
#define F2FS_UINT_PER_BITMAP (512 / 32) //512: pages per seg

void send_close_cmd(struct f2fs_sb_info *sbi, int type, block_t new_blkaddr)
{
	u32 next_secno;
	struct f2fs_bio_info *mbio;
	enum page_type btype;
	static unsigned long last_accessed_section[6] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
        static unsigned long last_accessed_offset[6] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
	unsigned long current_offset;
	unsigned long section_close_by_offset_err = 0;
	struct sit_info *sit_i = SIT_I(sbi);
	struct seg_entry *sentry;
	u32 iter;

	int entries = SIT_VBLOCK_MAP_SIZE / sizeof(u64);
        u64 *ckpt_map;
        u64 *cur_map;
	int i;
	static unsigned int close_cmd_seq = 0;

	next_secno = GET_SEGNO(sbi, new_blkaddr) / sbi->segs_per_sec;
	current_offset = (GET_SEGNO(sbi, new_blkaddr) % sbi->segs_per_sec) * (sbi->blocks_per_seg) + (new_blkaddr % sbi->blocks_per_seg);
	if(type <= CURSEG_COLD_DATA)
		btype = DATA;
	else
		btype = NODE;

	if (current->plug) {
		blk_finish_plug(current->plug);
	}
	if(last_accessed_section[type] == next_secno && last_accessed_offset[type] > current_offset) {
		section_close_by_offset_err = 1;
	}
	last_accessed_section[type] = next_secno;
	last_accessed_offset[type] = current_offset;

	if (sbi->last_sec_in_bio[type] != next_secno || section_close_by_offset_err) {
		unsigned int *buf;
		struct curseg_info *curseg = CURSEG_I(sbi, type);
 alloc_buf:
 		buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
 		if(!buf)
 			goto alloc_buf;
		memset(buf, 0, PAGE_SIZE);

		/* submit merged bio */
		mbio = &sbi->write_io[btype];
		buf[0] = close_cmd_seq++;
		buf[1] = type;
		buf[2] = sbi->last_sec_in_bio[type];
		buf[3] = next_secno;
//		printk("close command : %u - %u - %u  alloc_type: %u\n", type, buf[2] + 5, buf[3] + 5, curseg->alloc_type);

		mutex_lock(&sit_i->sentry_lock);
		for(iter = 0; iter < sbi->segs_per_sec; iter++)
		{
			u32 start_offset = 4 + iter * F2FS_UINT_PER_BITMAP;
			sentry = get_seg_entry(sbi, next_secno * sbi->segs_per_sec + iter);

    			ckpt_map = (u64 *)(sbi->ckpt_valid_map_tmp[type] + (iter % sbi->segs_per_sec) * SIT_VBLOCK_MAP_SIZE);
        		cur_map = (u64 *)(sbi->cur_valid_map_tmp[type] + (iter % sbi->segs_per_sec) * SIT_VBLOCK_MAP_SIZE);

			for (i = 0; i < entries; i++)
			{
		                sbi->target_map[i] = ckpt_map[i] | cur_map[i];
				if(curseg->alloc_type == SSR)
				{
//					printk("bitmap: %u-%u-%u ckpt: %lx  cur: %lx\n", next_secno, iter, i, ckpt_map[i], cur_map[i]);
				}
			}

			memcpy(&buf[start_offset], sbi->target_map, SIT_VBLOCK_MAP_SIZE);
		}
		mutex_unlock(&sit_i->sentry_lock);
		send_general_dev_cmd(sbi, ADDR_CLOSE_SECTION, buf, sizeof(buf), H2D, WAIT);
		sbi->last_sec_in_bio[type] = next_secno;
	}
}
int iotl_stream_manager(struct f2fs_io_info *fio)
{
	struct f2fs_sb_info *sbi = fio->sbi;
	enum page_type btype = PAGE_TYPE_OF_BIO(fio->type);
	if (!iotl_enabled(sbi))
		return 0;

	if (is_read_io(fio->op))
		return 0;

	if (btype == META)
		return 0;

	BUG_ON(fio->curseg_type >= NR_CURSEG_TYPE);
	send_close_cmd(sbi, fio->curseg_type, fio->new_blkaddr);

	return 1;
}
void iotl_destroy(struct f2fs_sb_info *sbi)
{
	memset(&sbi->iotl_info, 0x0, sizeof(sbi->iotl_info));
}

int iotl_init(struct f2fs_sb_info *sbi)
{
	struct iotl_info *iotl_info;
	struct block_device *bdev;
	struct hd_struct *part;
	u8  info_buf[512];
	int i, j;
	bdev = sbi->sb->s_bdev;
	part = bdev->bd_part;

	sbi->target_map = kzalloc(SIT_VBLOCK_MAP_SIZE, GFP_KERNEL);

	for (i = 0; i < NR_CURSEG_TYPE; i++) {
		sbi->last_sec_in_bio[i] = NULL_SEGNO;
		sbi->cur_valid_map_tmp[i] = kzalloc(SIT_VBLOCK_MAP_SIZE * sbi->segs_per_sec, GFP_KERNEL);
		sbi->ckpt_valid_map_tmp[i] = kzalloc(SIT_VBLOCK_MAP_SIZE * sbi->segs_per_sec, GFP_KERNEL);

		for(j=0; j<sbi->segs_per_sec; j++)
		{
			memcpy(sbi->cur_valid_map_tmp[i] + (j * SIT_VBLOCK_MAP_SIZE), get_seg_entry(sbi, CURSEG_I(sbi, i)->segno + j)->cur_valid_map, SIT_VBLOCK_MAP_SIZE);
			memcpy(sbi->ckpt_valid_map_tmp[i] + (j * SIT_VBLOCK_MAP_SIZE), get_seg_entry(sbi, CURSEG_I(sbi, i)->segno + j)->ckpt_valid_map, SIT_VBLOCK_MAP_SIZE);
		}

		send_close_cmd(sbi, i, NEXT_FREE_BLKADDR(sbi, CURSEG_I(sbi, i)));
	}

#ifdef F2FS_INTERNAL_MERGE
	sbi->gc_info_buf = NULL;
#endif

	iotl_info = (struct iotl_info *)info_buf;

	iotl_info->fs_start_sector = part->start_sect;
	iotl_info->fs_sectors_per_block = 1;
	iotl_info->fs_blocks_per_seg = sbi->blocks_per_seg;
	iotl_info->fs_segs_per_sec = sbi->segs_per_sec;
	iotl_info->fs_seg0_blkaddr = SM_I(sbi)->seg0_blkaddr;
	iotl_info->fs_start_segno = FREE_I(sbi)->start_segno;
	iotl_info->fs_num_active_section = NR_CURSEG_TYPE;
	iotl_info->fs_num_meta_sects = 
		le32_to_cpu(F2FS_RAW_SUPER(sbi)->main_blkaddr) << sbi->log_sectors_per_block;



	memcpy(&sbi->iotl_info, iotl_info, sizeof(sbi->iotl_info));

	iotl_msg("complete\n");

	return 0;
}
