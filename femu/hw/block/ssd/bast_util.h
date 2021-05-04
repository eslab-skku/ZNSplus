#ifndef _BAST_UTIL_H_
#define _BAST_UTIL_H_

#include "vssim_config_manager.h"

void actblk_print(struct ssdstate *ssd, int64_t bank);
struct act_blk_info *actblk_find(struct ssdstate *ssd, int64_t bank, int64_t lbn);
struct act_blk_info *assign_new_write_vpn(struct ssdstate *ssd, int64_t bank, int64_t lpn, int64_t *tt);
struct act_blk_info *actblk_find_free_slot(struct ssdstate *ssd, int64_t bank);
struct act_blk_info *actblk_find_victim(struct ssdstate *ssd, int64_t bank);
int64_t actblk_close(struct ssdstate *ssd, int64_t bank, struct act_blk_info *pact);
int64_t actblk_open(struct ssdstate *ssd, int64_t bank, int64_t lbn, struct act_blk_info *pact);
void actblk_init(struct ssdstate *ssd, int64_t bank, struct act_blk_info *pact);
void actblk_get_free_slot(struct ssdstate *ssd, int64_t bank, int64_t idx);
void actblk_set_free_slot(struct ssdstate *ssd, int64_t bank, int64_t idx);
int64_t actblk_check_write_order(struct ssdstate *ssd, struct act_blk_info *pact);
int64_t actblk_padding_copy(struct ssdstate *ssd, int64_t bank, int64_t bank_lpn, struct act_blk_info *pact);
int64_t actblk_close_all(struct ssdstate *ssd);

int64_t get_vpn(struct ssdstate *ssd, int64_t bank, int64_t lpn);
int64_t set_vpn(struct ssdstate *ssd, int64_t bank, int64_t lpn, int64_t off_in_page, struct act_blk_info *pact, int64_t type, int64_t old_vpn);

void dec_vc(struct ssdstate *ssd, int64_t bank, int64_t vbn, int64_t dec);
void inc_vc(struct ssdstate *ssd, int64_t bank, int64_t vbn, int64_t inc);
void set_vc(struct ssdstate *ssd, int64_t bank, int64_t vbn, int64_t vc);
int64_t get_vc(struct ssdstate *ssd, int64_t bank, int64_t vbn);

int64_t get_bmap(struct ssdstate *ssd, int64_t bank, int64_t lbn);
void set_bmap(struct ssdstate *ssd, int64_t bank, int64_t lbn, int64_t vbn);

int64_t get_assoc_type(struct ssdstate *ssd, int64_t bank, int64_t lbn);
void set_assoc_type(struct ssdstate *ssd, int64_t bank, int64_t lbn, int64_t assoc_type);
void add_garbage(struct ssdstate *ssd, int64_t bank, int64_t vbn);

void summ_copy(struct summary_page *summ0, struct summary_page *summ1);
void summ_init(struct ssdstate *ssd, struct summary_page *psumm);
int64_t summ_open(struct ssdstate *ssd, int64_t bank, struct summary_page *psumm);
void summ_close(struct ssdstate *ssd, int64_t bank, struct summary_page *psumm);

int64_t get_free_blk(struct ssdstate *ssd, int64_t bank, int64_t *tt);

int64_t get_num_bank(struct ssdstate *ssd, int64_t lpn);
int64_t get_num_stripe(struct ssdstate *ssd, int64_t lpn);

int64_t block_merge_copy(struct ssdstate *ssd, int64_t bank, struct act_blk_info *pact, int64_t dest_vbn, int64_t st_off, int64_t en_off);

int64_t attach_page_table(struct ssdstate *ssd, int64_t bank, int64_t lbn);
int64_t *find_free_pgt_seg(struct ssdstate *ssd, int64_t bank, int64_t *pgt_idx);

//GC
int64_t collect_garbage_blks(struct ssdstate *ssd, int64_t bank);
int64_t get_victim_block(struct ssdstate *ssd, int64_t bank, struct summary_page *pvic);
int64_t check_victim_info(struct ssdstate *ssd, int64_t bank, int64_t vbn, struct summary_page *pvic);
int64_t compaction_copy(struct ssdstate *ssd, int64_t bank, struct summary_page *vt_summ, int64_t *pnext_sc_off, int64_t cp_cnt, struct summary_page *dest_summ, int64_t *cur_need_to_emulate_tt);
int64_t check_page_validity(struct ssdstate *ssd, int64_t bank, int64_t vpn, int64_t lpn);

//ADDED
void padding_all_actblk(struct ssdstate *ssd, uint64_t now);
void check_chip_utilization(struct ssdstate *ssd, uint64_t now);
#endif
