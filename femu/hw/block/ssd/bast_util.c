#include "bast_util.h"

uint32_t IOUB_FirstInvalid(struct ssdstate *ssd, struct act_blk_info *pact) {
	struct ssdconf *sc = &(ssd->ssdparams);
	uint32_t off_in_page;

	for (int iter = 0; iter < sc->PAGE_NB * sc->NUM_BANKS; iter++)
	{
		if ((pact->page_validity[iter] & 0xf) == 0xf)
			continue;
		for (off_in_page = 0; off_in_page < sc->LPAGE_PER_PPAGE; off_in_page++)
		{
			if (!(pact->page_validity[iter] & (1 << off_in_page)))
				break;
		}
		return (iter * sc->LPAGE_PER_PPAGE) + off_in_page;
	}
	return (sc->LPAGE_PER_PPAGE * sc->PAGE_NB * sc->NUM_BANKS);

}

void check_chip_utilization(struct ssdstate *ssd, uint64_t now) {
	struct ssdconf *sc = &(ssd->ssdparams);
	struct act_blk_info *pact;
	int64_t *chip_next_avail_time = ssd->chip_next_avail_time;

	for(int bank = 0; bank < sc->NUM_BANKS; bank++) {
		if(now < chip_next_avail_time[bank])
			prof_IOUB->CH_total_active++;
	}
	prof_IOUB->CH_total_num++;
}

void padding_all_actblk(struct ssdstate *ssd, uint64_t now) {
	struct ssdconf *sc = &(ssd->ssdparams);
	struct act_blk_info *pact;
	int64_t *chip_next_avail_time = ssd->chip_next_avail_time;


	for(int bank = 0; bank < sc->NUM_BANKS; bank++) {
		if(now < chip_next_avail_time[bank])
			continue;

		for(int iter = 0; iter < sc->NUM_ACT_BLKS; iter++) {
			pact =  &ssd->v_misc_metas[bank].act_blks[iter];

			if(pact->cpoff != sc->PAGE_NB && pact->page_validity[pact->cpoff * sc->NUM_BANKS + bank] == 0xf) {
				prof_IOUB->SSR_pre_padding += sc->LPAGE_PER_PPAGE;
				block_merge_copy(ssd, bank, pact, pact->vbn, pact->cpoff, pact->cpoff);
				break;
			}

		}
	}


}

void actblk_print(struct ssdstate *ssd, int64_t bank)
{
	struct ssdconf *sc = &(ssd->ssdparams);

	printf("[actblk_print] bank %d ", bank);
	for (int i = 0; i < sc->NUM_ACT_BLKS; i++) 
	{
		printf("%d\t", ssd->v_misc_metas[bank].act_blks[i].lbn);
	}
	printf("\n");
}

struct act_blk_info *actblk_find(struct ssdstate *ssd, int64_t bank, int64_t lbn)
{
	struct ssdconf *sc = &(ssd->ssdparams);

	for (int i = 0; i < sc->NUM_ACT_BLKS; i++) {
		if (ssd->v_misc_metas[bank].act_blks[i].lbn == lbn) {
			return (struct act_blk_info *)&ssd->v_misc_metas[bank].act_blks[i];
		}
	}
	return NULL;
}

struct act_blk_info *assign_new_write_vpn(struct ssdstate *ssd, int64_t bank, int64_t lpn, int64_t *tt)
{
	struct ssdconf *sc = &(ssd->ssdparams);
	int64_t lbn;
	int64_t max_cpoff;    // max current page offset
	int64_t assoc_type;
	struct act_blk_info *pact;
	int64_t cur_need_to_emulate_tt = 0, max_need_to_emulate_tt = 0;
	int idx;
	int tmp_cpoff;

	lbn = lpn / sc->PAGE_NB;
	assoc_type = get_assoc_type(ssd, bank, lbn);

	if (assoc_type == ASSOC_PAGE) {
		pact = (struct act_blk_info *)&ssd->v_misc_metas[bank].summ_pages[0];
		max_cpoff = sc->PAGE_NB - 2;        // leave one page
	}
	else {
		pact = actblk_find(ssd, bank, lbn);    // find an active block mapped to the lbn
		max_cpoff = sc->PAGE_NB - 1;        // use all pages
	}
	if (pact == NULL) {
		// assign new active block
		pact = actblk_find_free_slot(ssd, bank);
		if (pact == NULL) {
			pact = actblk_find_victim(ssd, bank);
			if (pact == NULL) assert(0);
			cur_need_to_emulate_tt = actblk_close(ssd, bank, pact);
			if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
		}
		cur_need_to_emulate_tt = actblk_open(ssd, bank, lbn, pact);
		if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
	}
	else {
		if (pact->cpoff > max_cpoff) {
			// if the active block is full
			cur_need_to_emulate_tt = actblk_close(ssd, bank, pact);
			if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
			cur_need_to_emulate_tt = actblk_open(ssd, bank, lbn, pact);
			if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
		}
		else if (pact->cpoff == NULL_OFF) {
			cur_need_to_emulate_tt = actblk_open(ssd, bank, lbn, pact);
			if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
		}
	}
#ifdef IOUB_PADDING
	if (pact->assoc_type == ASSOC_BLOCK) {
		cur_need_to_emulate_tt = actblk_padding_copy(ssd, bank, lpn, pact);
		if (cur_need_to_emulate_tt == -1) {
			*tt = max_need_to_emulate_tt;
			return NULL;
		}
		if (cur_need_to_emulate_tt > max_need_to_emulate_tt)
			max_need_to_emulate_tt = cur_need_to_emulate_tt;
	}
#endif

	*tt = max_need_to_emulate_tt;

	return pact;
}

struct act_blk_info *actblk_find_free_slot(struct ssdstate *ssd, int64_t bank)
{
	struct ssdconf *sc = &(ssd->ssdparams);
	int64_t pos, idx;

	if (ssd->v_misc_metas[bank].act_free_pos > 0) {
		for (pos = 1, idx = 0; pos < (1 << sc->NUM_ACT_BLKS); pos <<= 1, idx++) {
			if (ssd->v_misc_metas[bank].act_free_pos & pos) {
				return (struct act_blk_info *)&ssd->v_misc_metas[bank].act_blks[idx];
			}
		}
	}
	return NULL;
}

struct act_blk_info *actblk_find_victim(struct ssdstate *ssd, int64_t bank)
{
	struct ssdconf *sc = &(ssd->ssdparams);
	int64_t idx, oldest_idx, oldest_age;

	oldest_age = ssd->v_misc_metas[bank].act_blks[0].age;
	oldest_idx = 0;

	// the most recent active block has the highest age
	for (idx = 1; idx < sc->NUM_ACT_BLKS; idx++) {
		if (ssd->v_misc_metas[bank].act_blks[idx].age < oldest_age) {
			oldest_idx = idx;
			oldest_age = ssd->v_misc_metas[bank].act_blks[idx].age;
		}

		if (ssd->v_misc_metas[bank].act_blks[idx].cpoff == sc->PAGE_NB) {
			return (struct act_blk_info *)&ssd->v_misc_metas[bank].act_blks[idx];
		}
	}

	if (oldest_idx == NULL_IDX) return NULL;
	else return (struct act_blk_info *)&ssd->v_misc_metas[bank].act_blks[oldest_idx];

}

int64_t actblk_close(struct ssdstate *ssd, int64_t bank, struct act_blk_info *pact)
{
	int64_t dest_vbn, dest_cpoff, lbn;
	int64_t old_vbn;
	int64_t cur_need_to_emulate_tt = 0;
	struct ssdconf *sc = &(ssd->ssdparams);

	if (pact->assoc_type == ASSOC_PAGE) {
		summ_close(ssd, bank, (struct summary_page *)pact);
		return;
	}

	lbn = pact->lbn;
	old_vbn = get_bmap(ssd, bank, lbn);

	if (!actblk_check_write_order(ssd, pact)) {
		assert(0);
	}
	else {
		// switch merge
		dest_vbn = pact->vbn;
		dest_cpoff = (pact->off_in_page > 0) ? (pact->cpoff + 1) : pact->cpoff;

		if (dest_cpoff < sc->PAGE_NB) {
			// partial merge
			prof_IOUB->SSR_fg_padding += (sc->PAGE_NB - dest_cpoff) * sc->LPAGE_PER_PPAGE;
			cur_need_to_emulate_tt = block_merge_copy(ssd, bank, pact, dest_vbn, dest_cpoff, sc->PAGE_NB - 1);
		}
		else {
			// empty merge
		}
	}

	if (old_vbn != NULL_VBN) {
		set_vc(ssd, bank, old_vbn, 0);
		add_garbage(ssd, bank, old_vbn);
	}

	dec_vc(ssd, bank, pact->vbn, sc->VC_ACTIVE_BLK);

	set_bmap(ssd, bank, lbn, dest_vbn);
	actblk_set_free_slot(ssd, bank, pact->slot_idx);

	pact->lbn = NULL_LBN;
	pact->vbn = NULL_VBN;
	pact->cpoff = NULL_OFF;

	return cur_need_to_emulate_tt;
}

int64_t actblk_open(struct ssdstate *ssd, int64_t bank, int64_t lbn, struct act_blk_info *pact)
{
	struct ssdconf *sc = &(ssd->ssdparams);
	int64_t data_vbn;
	int64_t cur_need_to_emulate_tt = 0;

	if (pact->assoc_type == ASSOC_PAGE) {
		printf("atcblk_open call somm_open\n");
		cur_need_to_emulate_tt = summ_open(ssd, bank, (struct summary_page *)pact);
		return cur_need_to_emulate_tt;
	}

	actblk_get_free_slot(ssd, bank, pact->slot_idx); // mark the slot_idx on the free slot bitmap as in use

	// initialize
	pact->age = inc_actblk_seq(ssd, bank);
	pact->vbn = get_free_blk(ssd, bank, &cur_need_to_emulate_tt);
	pact->cpoff = pact->off_in_page = 0;
	pact->lbn = lbn;

	inc_vc(ssd, bank, pact->vbn, sc->VC_ACTIVE_BLK);
	data_vbn = get_bmap(ssd, bank, lbn);
	if (data_vbn != NULL_VBN) inc_vc(ssd, bank, data_vbn, sc->VC_ACTIVE_BLK);

	memset(pact->log_table, NULL_64BIT, sizeof(int64_t) * sc->PAGE_NB);
	if(bank != 0)
	{
		int64_t offset;
		for(offset = 0; offset < sc->NUM_ACT_BLKS; offset++)
		{
			if(ssd->v_misc_metas[0].act_blks[offset].lbn == lbn)
				break;
		}
		if(offset == sc->NUM_ACT_BLKS)
			assert(0);
		pact->page_validity = ssd->v_misc_metas[0].act_blks[offset].page_validity;
	}

	ssd->v_bank_infos[bank].block_assoc_table[pact->vbn] = ASSOC_BLOCK;

	return cur_need_to_emulate_tt;
}

void actblk_init(struct ssdstate *ssd, int64_t bank, struct act_blk_info *pact) {
	struct ssdconf *sc = &(ssd->ssdparams);

	pact->age = NULL_64BIT;
	pact->lbn = NULL_LBN;
	pact->vbn = NULL_VBN;
	pact->cpoff = NULL_OFF;
	pact->off_in_page = 0;
	pact->assoc_type = ASSOC_BLOCK;
	memset(pact->log_table, NULL_64BIT, sizeof(int64_t) * sc->PAGE_NB);
	if(bank == 0)
		memset(pact->page_validity, 0, sizeof(int8_t) * sc->PAGE_NB*sc->NUM_BANKS);
}

void actblk_get_free_slot(struct ssdstate *ssd, int64_t bank, int64_t idx) {
	ssd->v_misc_metas[bank].act_free_pos &= ~(1 << idx);
}

void actblk_set_free_slot(struct ssdstate *ssd, int64_t bank, int64_t idx) {
	ssd->v_misc_metas[bank].act_free_pos |= (1 << idx);
}

int64_t actblk_check_write_order(struct ssdstate *ssd, struct act_blk_info *pact) {
	int64_t lpoff;

	for (lpoff = 0; lpoff < pact->cpoff; lpoff++) {
		if (pact->log_table[lpoff] != lpoff) {
			return 0;
		}
	}
	return 1;
}

int64_t actblk_padding_copy(struct ssdstate *ssd, int64_t bank, int64_t bank_lpn, struct act_blk_info *pact) {
	struct ssdconf *sc = &(ssd->ssdparams);
	int64_t lbn, lpoff, cur_need_to_emulate_tt = 0;

	assert(pact->assoc_type == ASSOC_BLOCK);

	if (pact->cpoff > (sc->PAGE_NB - 1)) {
		assert(0);
	}

	if (!actblk_check_write_order(ssd, pact)) {
		assert(0);
	}

	lpoff = bank_lpn % sc->PAGE_NB;

	if (pact->cpoff == lpoff) return 0;        // no need to padding

	lbn = pact->lbn;

	if (pact->cpoff > lpoff) {
		printf("NON INCREASING ORDER lpn %d bank %d bank_lpn %d lbn %d vbn %d lpoff %d cpoff %d\n", sc->NUM_BANKS * bank_lpn + bank, bank, bank_lpn, pact->lbn, pact->vbn, lpoff, pact->cpoff);
		return -1;
	}
	// copy pages from cpoff to (lpoff - 1)
	prof_IOUB->SSR_fg_padding +=(lpoff - pact->cpoff) * sc->LPAGE_PER_PPAGE;
	cur_need_to_emulate_tt = block_merge_copy(ssd, bank, pact, pact->vbn, pact->cpoff, lpoff - 1);

	for (; pact->cpoff < lpoff; pact->cpoff++) {
		pact->log_table[pact->cpoff] = pact->cpoff;
	}

	return cur_need_to_emulate_tt;
}

int64_t actblk_close_all(struct ssdstate *ssd) {
	struct ssdconf *sc = &(ssd->ssdparams);
	int64_t cnt = 0;

	for (int bank = 0; bank < sc->NUM_BANKS; bank++) {
		for (int idx = 0; idx < sc->NUM_ACT_BLKS; idx++) {
			if (ssd->v_misc_metas[bank].act_blks[idx].lbn != NULL_LBN) {
				actblk_close(ssd, bank, &ssd->v_misc_metas[bank].act_blks[idx]);
				cnt++;
			}
		}
	}

	return cnt;
}

int64_t get_vpn(struct ssdstate *ssd, int64_t bank, int64_t lpn) {
	struct ssdconf *sc = &(ssd->ssdparams);
	int64_t lbn, vbn, vpn, assoc_type, lpoff, mapoff;
	int64_t *page_mapping_table;
	struct act_blk_info *pact;

	lbn = lpn / sc->PAGE_NB;
	lpoff = lpn % sc->PAGE_NB;
	vbn = get_bmap(ssd, bank, lbn);
	assoc_type = get_assoc_type(ssd, bank, lbn);
	vpn = NULL_VPN;

	if (assoc_type == ASSOC_PAGE) {
		page_mapping_table = ssd->v_bank_infos[bank].page_mapping_table;
		vpn = page_mapping_table[lbn * sc->PAGE_NB + lpoff];
	}
	else {
		pact = actblk_find(ssd, bank, lbn);

		if ((pact != NULL) && (pact->log_table[lpoff] != NULL_OFF)) {
			// if the active block has a write for the lpoff
			assert(pact->assoc_type == ASSOC_BLOCK);
			mapoff = pact->log_table[lpoff];
			vpn = pact->vbn * sc->PAGE_NB + mapoff;
		}
		else if (vbn != NULL_VBN) {
			// if a write exists for the lpoff (not active block)
			mapoff = lpoff;
			vpn = vbn * sc->PAGE_NB + mapoff;
		}
		else {
			vpn = NULL_VPN;
		}
	}

	return vpn;
}

int64_t set_vpn(struct ssdstate *ssd, int64_t bank, int64_t lpn, int64_t off_in_page, struct act_blk_info *pact, int64_t type, int64_t old_vpn) {
	struct ssdconf *sc = &(ssd->ssdparams);
	int64_t lbn, lpoff, pgt_segnum, pgt_offset, new_vpn;
	int64_t *page_mapping_table;
	struct summary_page *psumm;
	int64_t vblock, page_num;
	int64_t cur_need_to_emulate_tt, max_need_to_emulate_tt = 0;

	assert(pact != NULL);

	lbn = lpn / sc->PAGE_NB;
	lpoff = lpn % sc->PAGE_NB;

	if (pact->assoc_type == ASSOC_PAGE) {
		assert(get_assoc_type(ssd, bank, lbn) != ASSOC_BLOCK);
		new_vpn = pact->vbn * sc->PAGE_NB + pact->cpoff;

		page_mapping_table = ssd->v_bank_infos[bank].page_mapping_table;

		pgt_offset = lbn * sc->PAGE_NB + lpoff;
		page_mapping_table[pgt_offset] = new_vpn;

		set_bmap(ssd, bank, lbn, pact->vbn);

		psumm = (struct summary_page *)pact;

		if (psumm->log_table[psumm->cpoff] != lpn) {
			if (psumm->log_table[psumm->cpoff] == NULL_64BIT)
				psumm->log_table[psumm->cpoff] = lpn;
			else
				psumm->log_table[++psumm->cpoff] = lpn;
			psumm->off_in_page = off_in_page + 1;
		}
		else {
			psumm->off_in_page = off_in_page + 1;
			if (psumm->off_in_page == sc->LPAGE_PER_PPAGE) {
				uint32_t lpn_4K = lpn * 16 + off_in_page;
				psumm->cpoff++;
				psumm->off_in_page = 0;

				switch (type) {
					case 0:
						cur_need_to_emulate_tt = SSD_PAGE_READ(ssd, bank, CALC_BLOCK(ssd, old_vpn), CALC_PAGE(ssd, old_vpn), NULL);
						if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
						cur_need_to_emulate_tt = SSD_PAGE_WRITE(ssd, bank, CALC_BLOCK(ssd, new_vpn), CALC_PAGE(ssd, old_vpn), NULL);

						if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
						break;
					case 1:
						cur_need_to_emulate_tt = SSD_PAGE_WRITE(ssd, bank, CALC_BLOCK(ssd, new_vpn), CALC_PAGE(ssd, old_vpn), NULL);
						if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
						break;
					case 2:
#ifdef IOUB_DO_COPYBACK
						cur_need_to_emulate_tt = SSD_PAGE_COPYBACK(ssd, bank, CALC_BLOCK(ssd, old_vpn), CALC_PAGE(ssd, old_vpn), CALC_BLOCK(ssd, new_vpn), CALC_PAGE(ssd, new_vpn));
						if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
#else
						cur_need_to_emulate_tt = SSD_PAGE_READ(ssd, bank, CALC_BLOCK(ssd, old_vpn), CALC_PAGE(ssd, old_vpn), NULL);
						if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
						cur_need_to_emulate_tt = SSD_PAGE_WRITE(ssd, bank, CALC_BLOCK(ssd, new_vpn), CALC_PAGE(ssd, old_vpn), NULL);
						if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
#endif
				}
			}
		}
	}
	else {
		int64_t validity_offset, page_offset;
		assert(get_assoc_type(ssd, bank, lbn) == ASSOC_BLOCK);
		assert(pact->lbn == lbn);

		pact->log_table[lpoff] = pact->cpoff;

		validity_offset = (lpn * sc->NUM_BANKS + bank) % (sc->NUM_BANKS * sc->PAGE_NB);
		page_offset = off_in_page;

		pact->page_validity[validity_offset] |= (1 << page_offset);

		new_vpn = pact->vbn * sc->PAGE_NB + lpoff;

		pact->off_in_page = off_in_page;
		pact->off_in_page++;
		if(pact->page_validity[validity_offset] == 0xf) {
			uint32_t lpn_4K = lpn * 16 + off_in_page;
			pact->cpoff++;
			pact->off_in_page = 0;

			switch (type) {
				case 0:
					cur_need_to_emulate_tt = SSD_PAGE_READ(ssd, bank, CALC_BLOCK(ssd, old_vpn), CALC_PAGE(ssd, old_vpn), NULL);
					if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
					cur_need_to_emulate_tt = SSD_PAGE_WRITE(ssd, bank, CALC_BLOCK(ssd, new_vpn), CALC_PAGE(ssd, old_vpn), NULL);
					if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
					break;
				case 1:
					cur_need_to_emulate_tt = SSD_PAGE_WRITE(ssd, bank, CALC_BLOCK(ssd, new_vpn), CALC_PAGE(ssd, old_vpn), NULL);
					if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
					break;
				case 2:
#ifdef IOUB_DO_COPYBACK
					cur_need_to_emulate_tt = SSD_PAGE_COPYBACK(ssd, bank, CALC_BLOCK(ssd, old_vpn), CALC_PAGE(ssd, old_vpn), CALC_BLOCK(ssd, new_vpn), CALC_PAGE(ssd, new_vpn));
					if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
#else
					cur_need_to_emulate_tt = SSD_PAGE_READ(ssd, bank, CALC_BLOCK(ssd, old_vpn), CALC_PAGE(ssd, old_vpn), NULL);
					if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
					cur_need_to_emulate_tt = SSD_PAGE_WRITE(ssd, bank, CALC_BLOCK(ssd, new_vpn), CALC_PAGE(ssd, old_vpn), NULL);
					if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
#endif
			}

#ifndef BACK_PD_OFF
			if(pact->cpoff != sc->PAGE_NB) {
				int8_t cur_validity;
PRE_PD_RETRY:
				cur_validity = pact->page_validity[pact->cpoff * sc->NUM_BANKS + bank];
				if(type != 2 && cur_validity == 0xf) {
					uint32_t src_vbn, dst_vbn;
					src_vbn = CALC_BLOCK(ssd, old_vpn);
					dst_vbn = pact->vbn;


					pact->log_table[pact->cpoff] = pact->cpoff;
					new_vpn = pact->vbn * sc->PAGE_NB + lpoff;

					pact->cpoff++;
					pact->off_in_page = 0;
#ifdef IOUB_DO_COPYBACK
					cur_need_to_emulate_tt = SSD_PAGE_COPYBACK(ssd, bank, src_vbn, pact->cpoff, dst_vbn, pact->cpoff);
#else
					cur_need_to_emulate_tt = SSD_PAGE_READ(ssd, bank, CALC_BLOCK(ssd, old_vpn), pact->cpoff, NULL);
					cur_need_to_emulate_tt = SSD_PAGE_WRITE(ssd, bank, CALC_BLOCK(ssd, new_vpn), pact->cpoff, NULL);
#endif

					dec_vc(ssd, bank, src_vbn, 1);
					inc_vc(ssd, bank, dst_vbn, 1);
					prof_IOUB->SSR_padding_count += sc->LPAGE_PER_PPAGE;
					prof_IOUB->SSR_padding_cpbk += sc->LPAGE_PER_PPAGE;
					prof_IOUB->SSR_back_padding +=sc->LPAGE_PER_PPAGE;
#ifdef PRE_PD_OFF
					goto PRE_PD_RETRY;
#endif
				}
				else if(cur_validity != 0 && cur_validity != 0xf) {
					uint32_t valid_count = 0, iter;
					for(iter = 1; iter < sc->LPAGE_PER_PPAGE; iter++)
					{
						if(cur_validity & (1 << iter))
							valid_count++;
					}
					prof_IOUB->SSR_padding_lpn += valid_count;
					prof_IOUB->SSR_padding_count += valid_count;
					prof_IOUB->SSR_back_padding += valid_count;
					cur_need_to_emulate_tt = SSD_PAGE_READ(ssd, bank, CALC_BLOCK(ssd, old_vpn), pact->cpoff, NULL);

				}
				else {
				}
			}
#endif
		}
		ssd->v_bank_infos[bank].inverse_mapping_table[new_vpn] = lpn;
	}

	return max_need_to_emulate_tt;
}

void dec_vc(struct ssdstate *ssd, int64_t bank, int64_t vbn, int64_t dec) {
	struct ssdconf *sc = &(ssd->ssdparams);
	int64_t vc;
	struct bank_info *pbank_info;

	assert(vbn < sc->BLOCK_NB);
	assert(vbn != NULL_VBN);

	pbank_info = &(ssd->v_bank_infos[bank]);
	vc = pbank_info->vcount_table[vbn];

	set_vc(ssd, bank, vbn, vc - dec);
}

void inc_vc(struct ssdstate *ssd, int64_t bank, int64_t vbn, int64_t inc) {
	struct ssdconf *sc = &(ssd->ssdparams);
	int64_t vc;
	struct bank_info *pbank_info;

	assert(vbn < sc->BLOCK_NB);
	assert(vbn != NULL_VBN);

	pbank_info = &(ssd->v_bank_infos[bank]);
	vc = pbank_info->vcount_table[vbn];

	set_vc(ssd, bank, vbn, vc + inc);
}

void set_vc(struct ssdstate *ssd, int64_t bank, int64_t vbn, int64_t vc) {
	struct ssdconf *sc = &(ssd->ssdparams);
	struct bank_info *pbank_info;

	assert(vbn < sc->BLOCK_NB);
	assert(vbn != NULL_VBN);

	pbank_info = &(ssd->v_bank_infos[bank]);
	pbank_info->vcount_table[vbn] = vc;
}

int64_t get_vc(struct ssdstate *ssd, int64_t bank, int64_t vbn) {
	struct ssdconf *sc = &(ssd->ssdparams);
	struct bank_info *pbank_info;
	int64_t vc;

	assert(vbn < sc->BLOCK_NB);
	assert(vbn != NULL_VBN);

	pbank_info = &(ssd->v_bank_infos[bank]);
	vc = pbank_info->vcount_table[vbn];

	return vc;
}

int64_t get_bmap(struct ssdstate *ssd, int64_t bank, int64_t lbn) {
	int64_t vbn = ssd->v_bank_infos[bank].block_mapping_table[lbn];
	return vbn;
}

void set_bmap(struct ssdstate *ssd, int64_t bank, int64_t lbn, int64_t vbn) {
	ssd->v_bank_infos[bank].block_mapping_table[lbn] = vbn;
}

int64_t get_assoc_type(struct ssdstate *ssd, int64_t bank, int64_t lbn) {
	return ssd->v_bank_infos[bank].assoc_type_table[lbn];
}

void set_assoc_type(struct ssdstate *ssd, int64_t bank, int64_t lbn, int64_t assoc_type) {
	ssd->v_bank_infos[bank].assoc_type_table[lbn] = assoc_type;
}

void assoc_type_print(struct ssdstate *ssd, int64_t bank) {
	struct ssdconf *sc = &(ssd->ssdparams);

	printf("[assoc_type_print] ");
	for (int lbn = 0; lbn < sc->BLOCK_NB; lbn++) {
		if (ssd->v_bank_infos[bank].assoc_type_table[lbn] == ASSOC_PAGE) {
			printf("%d\t", lbn);
		}
	}
	printf("\n");
}

void summ_copy(struct summary_page *summ0, struct summary_page *summ1) {
	summ0->age = summ1->age;
	summ0->lbn = summ1->lbn;
	summ0->vbn = summ1->vbn;
	summ0->cpoff = summ1->cpoff;
	summ0->assoc_type = summ1->assoc_type;
	summ0->slot_idx = summ1->slot_idx;

	for (int i = 0; i < summ1->cpoff; i++) {
		summ0->log_table[i] = summ1->log_table[i];
	}
}

void summ_init(struct ssdstate *ssd, struct summary_page *psumm) {
	struct ssdconf *sc = &(ssd->ssdparams);
	psumm->age = NULL_64BIT;
	psumm->lbn = NULL_LBN;
	psumm->vbn = NULL_VBN;
	psumm->cpoff = NULL_OFF;
	psumm->assoc_type = ASSOC_PAGE;
	psumm->slot_idx = 0;
	psumm->off_in_page = 0;
	memset(psumm->log_table, NULL_64BIT, sizeof(int64_t) * sc->PAGE_NB);
	memset(psumm->page_validity, 0, sizeof(int8_t) * sc->PAGE_NB);
}

int64_t summ_open(struct ssdstate *ssd, int64_t bank, struct summary_page *psumm) {
	struct ssdconf *sc = &(ssd->ssdparams);
	int64_t cur_need_to_emulate_tt = 0;

	psumm->age = inc_actblk_seq(ssd, bank);

	psumm->vbn = get_free_blk(ssd, bank, &cur_need_to_emulate_tt);
	psumm->cpoff = psumm->off_in_page = 0;
	psumm->lbn = ASSOC_PAGE;

	set_vc(ssd, bank, psumm->vbn, sc->VC_ACTIVE_BLK);
	memset(psumm->log_table, NULL_64BIT, sizeof(int64_t) * sc->PAGE_NB);
	memset(psumm->page_validity, 0, sizeof(int8_t) * sc->PAGE_NB);

	ssd->v_bank_infos[bank].block_assoc_table[psumm->vbn] = ASSOC_PAGE;

	return cur_need_to_emulate_tt;
}

void summ_close(struct ssdstate *ssd, int64_t bank, struct summary_page *psumm) {
	struct ssdconf *sc = &(ssd->ssdparams);
	int i, vpn;
	assert(psumm->cpoff <= sc->PAGE_NB - 1);

	for (i = 0; i < sc->PAGE_NB; i++) {
		vpn = psumm->vbn * sc->PAGE_NB + i;
		ssd->v_bank_infos[bank].inverse_mapping_table[vpn] = psumm->log_table[i];    //store P2L mapping table
	}

	dec_vc(ssd, bank, psumm->vbn, sc->VC_ACTIVE_BLK);

	psumm->lbn = NULL_LBN;
	psumm->vbn = NULL_VBN;
	psumm->cpoff = NULL_OFF;
}

void add_garbage(struct ssdstate *ssd, int64_t bank, int64_t vbn) {
	int64_t *gbitmap;
	int64_t wpos, mask;
	int64_t val;
	struct misc_metadata *pmisc_meta;

	pmisc_meta = &ssd->v_misc_metas[bank];
	gbitmap = (int64_t *)ssd->v_bank_infos[bank].garbage_bitmap;

	wpos = vbn / sizeof(int64_t);
	mask = 1 << (vbn % sizeof(int64_t));

	val = gbitmap[wpos];

	if (val & mask) assert(0);

	gbitmap[wpos] = (val | mask);

	pmisc_meta->num_garbage_blks++;
}

int64_t get_free_blk(struct ssdstate *ssd, int64_t bank, int64_t *tt) {
	struct ssdconf *sc = &(ssd->ssdparams);
	int64_t free_vbn, free_idx;
	int64_t *gbitmap;
	int64_t bit_pos, word_idx, start_idx, end_idx, word_val;
	struct misc_metadata *pmisc_meta;
	int64_t vbn;

	pmisc_meta = &ssd->v_misc_metas[bank];

	if (pmisc_meta->num_free_blk < 3 && pmisc_meta->num_garbage_blks > 0) {

retry:
		gbitmap = (int64_t *)ssd->v_bank_infos[bank].garbage_bitmap;
		assert((int64_t)gbitmap % sizeof(int64_t) == 0);

		start_idx = 0;
		end_idx = sc->BLOCK_NB - 1;
		free_idx = start_idx;

		for (word_idx = 0; word_idx <= (end_idx / sizeof(int64_t)); word_idx++) {
			word_val = *(gbitmap + word_idx);

			for (bit_pos = 0; bit_pos < sizeof(int64_t); bit_pos++) {
				if ((word_val & ((int64_t)1 << bit_pos))) {
					while (pmisc_meta->free_blk_list[free_idx] != NULL_VBN) {
						free_idx = (free_idx + 1) % sc->BLOCK_NB;
					}

					assert(pmisc_meta->free_blk_list[free_idx] == NULL_VBN);

					vbn = word_idx * sizeof(int64_t) + bit_pos;

					if (vbn >= sc->BLOCK_NB) assert(0);

					pmisc_meta->free_blk_list[free_idx] = vbn;
					pmisc_meta->num_free_blk++;

					pmisc_meta->garbage_last_idx = vbn;
					pmisc_meta->num_garbage_blks--;

					word_val &= ~((int64_t)1 << bit_pos);

					ssd->v_bank_infos[bank].block_assoc_table[vbn] = NULL_VBN;

					if (pmisc_meta->num_free_blk >= sc->BLOCK_NB) {
						*(gbitmap + word_idx) = word_val;
						goto finish_fill;
					}
				}
			}

			*(gbitmap + word_idx) = word_val;
			bit_pos = 0;
		}
	}
finish_fill:
	free_vbn = NULL_VBN;

	for (free_idx = 0; free_idx < sc->BLOCK_NB; free_idx++) {
		if (pmisc_meta->free_blk_list[free_idx] != NULL_VBN) break;
	}

found_free:
	assert(free_idx < sc->BLOCK_NB);

	free_vbn = pmisc_meta->free_blk_list[free_idx];

	if (free_vbn >= sc->BLOCK_NB) assert(0);

	pmisc_meta->free_blk_list[free_idx] = NULL_VBN;
	pmisc_meta->num_free_blk--;
	pmisc_meta->free_blk_last_idx = free_idx;

	*tt = SSD_BLOCK_ERASE(ssd, bank, free_vbn);

	return free_vbn;
}

int64_t get_num_bank(struct ssdstate *ssd, int64_t lpn) {
	struct ssdconf *sc = &(ssd->ssdparams);

	return lpn % sc->NUM_BANKS;
}

int64_t get_num_stripe(struct ssdstate *ssd, int64_t lpn) {
	struct ssdconf *sc = &(ssd->ssdparams);

	return lpn / sc->NUM_BANKS;
}

int64_t block_merge_copy(struct ssdstate *ssd, int64_t bank, struct act_blk_info *pact, int64_t dest_vbn, int64_t st_off, int64_t en_off) {
	struct ssdconf *sc = &(ssd->ssdparams);
	int64_t ncopied = 0, src_vbn, src_off, dest_off, lo_off;    //logical offset
	int64_t cur_need_to_emulate_tt = 0, max_need_to_emulate_tt = 0;
	int64_t src_vpn, dest_vpn;
	int64_t src_blk, src_page, dest_blk, dest_page, type = 0;

	assert(st_off < sc->PAGE_NB);
	assert(en_off < sc->PAGE_NB);

	for (lo_off = st_off; lo_off <= en_off; lo_off++) {

		if (pact->log_table[lo_off] != NULL_IDX) {
			src_vbn = pact->vbn;
			src_off = pact->log_table[lo_off];
			type = 2;
		}
		else {
			src_vbn = get_bmap(ssd, bank, pact->lbn);
			if (src_vbn == NULL_VBN) {
				type = 1;
				src_off = NULL_IDX;
			}
			else {
				src_off = lo_off;
				type = 2;
			}
		}

		dest_off = lo_off;

		src_vpn = src_vbn * sc->PAGE_NB + src_off;
		dest_vpn = dest_vbn * sc->PAGE_NB + dest_off;

		if (src_vbn == NULL_VBN) {

			dest_blk = dest_vpn / sc->PAGE_NB;
			dest_page = dest_vpn % sc->PAGE_NB;
			cur_need_to_emulate_tt = SSD_PAGE_WRITE(ssd, bank, dest_blk, dest_page, NULL);
			if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
		}
		else {
			int64_t vcount, iter;
			int64_t cpoff = pact->cpoff;
			assert(src_vbn != dest_vbn);

			vcount = get_vc(ssd, bank, src_vbn);

			src_blk = src_vpn / sc->PAGE_NB;
			src_page = src_vpn % sc->PAGE_NB;
			dest_blk = dest_vpn / sc->PAGE_NB;
			dest_page = dest_vpn % sc->PAGE_NB;

			if(type != 1) 
				type = 2;
			cur_need_to_emulate_tt = set_vpn(ssd, bank, pact->lbn * sc->PAGE_NB + lo_off, 0, pact, type, src_blk * sc->PAGE_NB + src_page);
			if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
			assert(cpoff == lo_off);

			dec_vc(ssd, bank, src_vbn, 1);
			ncopied++;
		}
		inc_vc(ssd, bank, dest_vbn, 1);
		prof_IOUB->SSR_padding_count += sc->LPAGE_PER_PPAGE;
		prof_IOUB->SSR_padding_cpbk += sc->LPAGE_PER_PPAGE;
	}
	return max_need_to_emulate_tt;
}

int64_t attach_page_table(struct ssdstate *ssd, int64_t bank, int64_t lbn) {
	struct ssdconf *sc = &(ssd->ssdparams);
	int64_t *pgt_seg;
	int64_t vbn, pgt_idx;

	if (get_assoc_type(ssd, bank, lbn) == ASSOC_PAGE) return 0;

	set_assoc_type(ssd, bank, lbn, ASSOC_PAGE);

	return 1;
}

int64_t *find_free_pgt_seg(struct ssdstate *ssd, int64_t bank, int64_t *pgt_idx) {
	struct ssdconf *sc = &(ssd->ssdparams);
	int64_t *page_mapping_table;
	int64_t idx, st_idx, en_idx, found;
	struct misc_metadata *pmisc_meta;

	pmisc_meta = &ssd->v_misc_metas[bank];
	page_mapping_table = ssd->v_bank_infos[bank].page_mapping_table;
	found = NULL_LPN;

	st_idx = pmisc_meta->pgt_last_idx + sc->PAGE_NB;
	en_idx = sc->PAGE_TABLE_SIZE / sc->NUM_BANKS - sc->PAGE_NB;

find_last_half:
	idx = st_idx;

	for(; idx <= en_idx; idx += sc->PAGE_NB) {
		if (page_mapping_table[idx] == PGT_SEG_FREE_MARK) {
			found = idx;
			break;
		}
	}

	if (found = NULL_LPN && st_idx != 0) {
		st_idx = 0;
		en_idx = pmisc_meta->pgt_last_idx;
		goto find_last_half;
	}

	assert(found != NULL_LPN);

	pmisc_meta->pgt_last_idx = found;
	*pgt_idx = found;

	return &page_mapping_table[found];

}

int64_t collect_garbage_blks(struct ssdstate *ssd, int64_t bank)
{
	struct ssdconf *sc = &(ssd->ssdparams);
	int64_t ncopied, next_sc_off;    // next scan offset
	struct summary_page vt_info, *psumm;
	struct misc_metadata *pmisc_meta;
	int64_t cur_need_to_emulate_tt = 0;
	int64_t max_need_to_emulate_tt = 0;
	int64_t tt = 0;
	int64_t vt_blk = 0;

	pmisc_meta = &ssd->v_misc_metas[bank];

	if ((pmisc_meta->num_free_blk + pmisc_meta->num_garbage_blks) >= FREE_THRESHOLD) {
		return max_need_to_emulate_tt;
	}

	vt_info.log_table = (int64_t *)malloc(sizeof(int64_t) * sc->PAGE_NB);

collect_more:
	vt_blk = get_victim_block(ssd, bank, &vt_info);
	if (vt_blk == NULL_VBN) {
		free(vt_info.log_table);
		return max_need_to_emulate_tt;
	}

	next_sc_off = 0;

continue_victim:
	tt = 0;

	psumm = (struct summary_page *)&ssd->v_misc_metas[bank].summ_pages[1];

	if (psumm->lbn == NULL_LBN) {
		cur_need_to_emulate_tt = summ_open(ssd, bank, psumm);
		if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
	}

	ncopied = compaction_copy(ssd, bank, &vt_info, &next_sc_off, sc->PAGE_NB - 1, psumm, &tt);
	if (tt > max_need_to_emulate_tt) max_need_to_emulate_tt = tt;

	if (next_sc_off > (sc->PAGE_NB - 2)) {
		// when the garbage block is full
		summ_close(ssd, bank, psumm);
	}

	// add the victim block to garbage
	printf("collect_garbage_blks: garbage, bank %lld vbn %lld\n", bank, vt_info.vbn);
	set_vc(ssd, bank, vt_info.vbn, 0);
	add_garbage(ssd, bank, vt_info.vbn);

	if (vt_info.cpoff < (sc->PAGE_NB - 1)) {
		goto continue_victim;
	}

	if (get_vc(ssd, bank, vt_info.vbn) != 0) {
		summ_close(ssd, bank, psumm);
	}
	else {
		ssd->v_bank_infos[bank].block_assoc_table[vt_info.vbn] = NULL_VBN;    //free block
	}

	if ((pmisc_meta->num_free_blk + pmisc_meta->num_garbage_blks) >= FREE_THRESHOLD) {
		free(vt_info.log_table);
		return max_need_to_emulate_tt;
	}

	goto collect_more;
}

int64_t get_victim_block(struct ssdstate *ssd, int64_t bank, struct summary_page *pvic) {
	struct ssdconf *sc = &(ssd->ssdparams);
	struct misc_metadata *pmisc_meta;
	struct bank_info *pbank_info;
	int64_t vc, min_vc;
	int64_t idx, min_idx;
	int64_t start_idx, end_idx;
	int64_t victim_vbn;
	int64_t assoc_type;

	pmisc_meta = &(ssd->v_misc_metas[bank]);
	pbank_info = &(ssd->v_bank_infos[bank]);

	min_vc = sc->PAGE_NB - 1;
	min_idx = NULL_VBN;

	start_idx = (pmisc_meta->gc_last_idx + 1) % sc->BLOCK_NB;
	end_idx = sc->BLOCK_NB - 1;

	for (idx = 0; idx < sc->BLOCK_NB; idx++) {
		vc = get_vc(ssd, bank, idx);
		assoc_type = pbank_info->block_assoc_table[idx];
		if (assoc_type == ASSOC_PAGE && vc >= 0 && vc < sc->PAGE_NB) {
			int valid_cnt = 0;
			for (int off = 0; off < sc->PAGE_NB - 1; off++) {
				valid_cnt += check_page_validity(ssd, bank, idx * sc->PAGE_NB + off, ssd->v_bank_infos[bank].inverse_mapping_table[idx * sc->PAGE_NB + off]);
			}
			set_vc(ssd, bank, idx, valid_cnt);
			pbank_info->block_assoc_table[idx] = NULL_VBN;
		}
		if (vc > 0 && vc < min_vc && assoc_type == ASSOC_PAGE) {
			min_vc = vc;
			min_idx = idx;
		}

	}

	if (min_idx == NULL_VBN) return NULL_VBN;
	assert(min_idx < sc->BLOCK_NB);

	pmisc_meta->gc_last_idx = min_idx;

	victim_vbn = min_idx;

	// pvic <- summary_page
	pvic->age = inc_actblk_seq(ssd, bank);
	pvic->assoc_type = ASSOC_PAGE;
	pvic->vbn = victim_vbn;
	pvic->cpoff = sc->PAGE_NB - 1;
	pvic->lbn = ASSOC_PAGE;

	for (int i = 0; i < sc->PAGE_NB - 1; i++) {
		int vpn = pvic->vbn * sc->PAGE_NB + i;
		int lpn = ssd->v_bank_infos[bank].inverse_mapping_table[vpn];
		pvic->log_table[i] = lpn;
	}

	return victim_vbn;

}

int64_t check_victim_info(struct ssdstate *ssd, int64_t bank, int64_t vbn, struct summary_page *pvic) {
	if ((pvic->vbn != vbn) || (pvic->assoc_type != ASSOC_PAGE)) {
		assert(0);
	}    
	return 0;
}

int64_t compaction_copy(struct ssdstate *ssd, int64_t bank, struct summary_page *vt_summ, int64_t *pnext_sc_off, int64_t cp_cnt, struct summary_page *dest_summ, int64_t *cur_need_to_emulate_tt) {
	struct ssdconf *sc = &(ssd->ssdparams);
	int64_t lpn, vt_vpn, dest_vpn;
	int64_t ncopied;
	int64_t sc_off;
	int64_t max_need_to_emulate_tt = 0;
	int32_t type = 2;


	sc_off = *pnext_sc_off;
	assert(sc_off < sc->PAGE_NB - 1);

	ncopied = 0;
	for (; sc_off < sc->PAGE_NB - 1; sc_off++) {
		lpn = vt_summ->log_table[sc_off];
		vt_vpn = vt_summ->vbn * sc->PAGE_NB + sc_off;
		dest_vpn = dest_summ->vbn * sc->PAGE_NB + dest_summ->cpoff;

		if (check_page_validity(ssd, bank, vt_vpn, lpn)) {
			if (dest_summ->cpoff > sc->PAGE_NB - 2) break;    // destination is full
			if (ncopied == cp_cnt) break;

#ifdef IOUB_COPYBACK
			type = 2;
			if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;

#else
			type = 0;
#endif

			set_vpn(ssd, bank, lpn, sc->LPAGE_PER_PPAGE - 1, (struct act_blk_info *)dest_summ, type, vt_vpn);

			dec_vc(ssd, bank, vt_summ->vbn, 1);
			inc_vc(ssd, bank, dest_summ->vbn, 1);

			ncopied++;

			if (get_vc(ssd, bank, vt_summ->vbn) == 0) {
				sc_off = sc->PAGE_NB;
				break;
			}
		}
	}

	*pnext_sc_off = ncopied;

	return ncopied;
}

int64_t check_page_validity(struct ssdstate *ssd, int64_t bank, int64_t vpn, int64_t lpn) {
	struct ssdconf *sc = &(ssd->ssdparams);
	int64_t lbn, off, pgt_idx, vbn;
	int64_t *page_mapping_table;

	if (lpn == NULL_LPN) return 0;

	page_mapping_table = ssd->v_bank_infos[bank].page_mapping_table;

	lbn = lpn / sc->PAGE_NB;
	off = lpn % sc->PAGE_NB;
	vbn = get_bmap(ssd, bank, lbn);
	pgt_idx = vbn * sc->PAGE_NB + off;

	return page_mapping_table[pgt_idx] == vpn;
}
