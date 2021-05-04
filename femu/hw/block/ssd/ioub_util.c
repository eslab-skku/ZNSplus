#include "ioub_util.h"
#include "bast_util.h"

int64_t recv_meta_write(struct ssdstate *ssd, int64_t lba, int64_t num_sectors);

int64_t ioub_process_command(struct ssdstate *ssd, int64_t lba, int64_t num_sectors) {
	int64_t cur_need_to_emulate_tt = 0;
	struct ssdconf *sc = &(ssd->ssdparams);
	switch (lba) {
		case ADDR_PROFILE_COMMAND:
			cur_need_to_emulate_tt = recv_profile_command(ssd);
			break;
		case ADDR_TL_OPEN:
			cur_need_to_emulate_tt = recv_close_section(ssd);
			break;
		case ADDR_INTERNAL_COMPACTION:
			cur_need_to_emulate_tt = recv_internal_merge(ssd);
			break;
		default:
			cur_need_to_emulate_tt = recv_meta_write(ssd, lba, num_sectors);
			break;
	}
	
	return cur_need_to_emulate_tt;
}

int64_t recv_profile_command(struct ssdstate *ssd) {
	float chip_utilization = 0;
	printf("NAND_read	%llu chunks (16KB)\n", prof_IOUB->NAND_read);
	printf("NAND_write	%llu chunks (16KB)\n", prof_IOUB->NAND_write);
	printf("NAND_cpbk	%llu chunks (16KB)\n", prof_IOUB->NAND_copyback);
	printf("IZC_count	%llu LPNs (4KB)\n", prof_IOUB->IM_count);
	printf("IZC_read	%llu chunks (16KB)\n", prof_IOUB->IM_read);
	printf("IZC_write	%llu LPNs (4KB)\n", prof_IOUB->IM_write);
	printf("IZC_copyback	%llu LPNs (4KB)\n", prof_IOUB->IM_copyback);
	if(prof_IOUB->IZC_count == 0)
		printf("IZC_avg. time	0\n");
	else
		printf("IZC_avg. time	%llu ns\n", prof_IOUB->IZC_total_time / prof_IOUB->IZC_count);
	printf("SSR_PDcount	%llu LPNs (4KB)\n", prof_IOUB->SSR_padding_count);
	printf("SSR_PDR&W	%llu LPNs (4KB)\n", prof_IOUB->SSR_padding_lpn);
	printf("SSR_PDcpbk	%llu LPNs (4KB)\n", prof_IOUB->SSR_padding_cpbk);
	printf("SSR_BP		%llu LPNs (4KB)\n", prof_IOUB->SSR_back_padding);
	printf("SSR_PP		%llu LPNs (4KB)\n", prof_IOUB->SSR_pre_padding);
	printf("SSR_FD		%llu LPNs (4KB)\n", prof_IOUB->SSR_fg_padding);

	if(prof_IOUB->CH_total_num)
		chip_utilization = (float)prof_IOUB->CH_total_active / prof_IOUB->CH_total_num;
	printf("CH_util		%f chips/s\n", chip_utilization);
#ifndef DEBUG_CHIP_UTIL
	prof_IOUB->CH_total_active = 0;
	prof_IOUB->CH_total_num = 0;
#endif
	
	return 0;
}

int64_t recv_meta_write(struct ssdstate *ssd, int64_t lba, int64_t num_sectors) {
	struct ssdconf *sc = &(ssd->ssdparams);
	uint64_t bank, bank_lpn;
	uint64_t max_emulate_tt = 0, cur_emulate_tt;
	uint32_t iter, count = (num_sectors + sc->SECTORS_PER_PAGE - 1) / sc->SECTORS_PER_PAGE;
	uint32_t lpn = lba / sc->SECTORS_PER_PAGE;
	

	for(iter = 0; iter < count; iter++, lpn++) {
		//We assumed that GC does not occur in the meta area.
		//Therefore, it is not a problem even if it is transmitted only by writing to the first page within the bank.
		bank = lpn % sc->NUM_BANKS;
		cur_emulate_tt = SSD_PAGE_WRITE(ssd, bank, 0, 0, NULL);
		if(cur_emulate_tt > max_emulate_tt) max_emulate_tt = cur_emulate_tt;
	}

	return max_emulate_tt;

}

int64_t recv_internal_merge(struct ssdstate *ssd) {
	struct ssdconf *sc = &(ssd->ssdparams);
	int32_t read_offset = 0;
	int32_t src_lpn, dst_lpn, src_page, dst_page;
	int32_t src_hlpn, dst_hlpn;
	int32_t prev_hlpn;
	int32_t read_page_cnt = 1, idx = 0;
	int64_t cur_need_to_emulate_tt = 0, tt = 0, max_need_to_emulate_tt = 0;
	int64_t bank, bank_lpn, src_vpn, dst_vpn;
	uint64_t src_bank, dst_bank;
	uint64_t src_bank_lpn, dst_bank_lpn;
	struct act_blk_info *pact = NULL, *src_pact_0, *src_pact_1;
	int is_copyback = 1;
	int32_t *read_hlpn_list;
	int32_t padding_count;
	uint32_t same_src = 1;
	uint64_t last_src_vpn = 0xffffffff;
	uint32_t last_src_bank = sc->NUM_BANKS, last_dst_bank;
	uint32_t comp = 0;


	while (1) {
		if (read_offset == IOUB_MAX_IM_CNT) {
			comp = 1;
			goto read_page;
		}

		src_lpn = ((int32_t *)ssd->req_info->p_data)[read_offset];
		dst_lpn = ((int32_t *)ssd->req_info->p_data)[512 + read_offset];

		if(src_lpn == NULL || dst_lpn == NULL) {
			comp = 1;
			goto read_page;
		}

		src_lpn += PARTITION_SIZE_IN_SECTOR / sc->SECTORS_PER_LPAGE;
		dst_lpn += PARTITION_SIZE_IN_SECTOR / sc->SECTORS_PER_LPAGE;

		src_page = src_lpn / sc->LPAGE_PER_PPAGE;
		dst_page = dst_lpn / sc->LPAGE_PER_PPAGE;

		src_bank = get_num_bank(ssd, src_page);
		src_bank_lpn = get_num_stripe(ssd, src_page);
		

		dst_bank = get_num_bank(ssd, dst_page);

		src_vpn = get_vpn(ssd, src_bank, src_bank_lpn);

		if(last_src_vpn == 0xffffffff) {
			same_src = 1;
		}
		else if(last_src_vpn != src_vpn || src_bank != last_src_bank) {
read_page:
#ifdef IOUB_DO_COPYBACK
			if(same_src == sc->LPAGE_PER_PPAGE && last_src_bank == last_dst_bank) {
				
			}
			else
#endif
			{
				prof_IOUB->IM_read++;
				cur_need_to_emulate_tt = SSD_PAGE_READ(ssd, last_src_bank, CALC_BLOCK(ssd, last_src_vpn), CALC_PAGE(ssd, last_src_vpn), NULL);
				if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;

			}

			if(comp)
				break;

			same_src = 1;
		} else {
			same_src++;
		}

		read_offset++;
		last_src_vpn = src_vpn;
		last_src_bank = src_bank;
		last_dst_bank = dst_bank;
	}
	read_offset = 0;
	same_src = 1;
	last_src_vpn = 0xffffffff;
	last_src_bank = sc->NUM_BANKS;

	while (1) {
		uint64_t off_in_page;
		if (read_offset == IOUB_MAX_IM_CNT)	break;

		src_lpn = ((int32_t *)ssd->req_info->p_data)[read_offset];
		dst_lpn = ((int32_t *)ssd->req_info->p_data)[512 + read_offset];

		if(src_lpn == NULL || dst_lpn == NULL) break;

		src_lpn += PARTITION_SIZE_IN_SECTOR / sc->SECTORS_PER_LPAGE;
		dst_lpn += PARTITION_SIZE_IN_SECTOR / sc->SECTORS_PER_LPAGE;

		src_page = src_lpn / sc->LPAGE_PER_PPAGE;
		dst_page = dst_lpn / sc->LPAGE_PER_PPAGE;

		src_bank = get_num_bank(ssd, src_page);
		src_bank_lpn = get_num_stripe(ssd, src_page);
		

		dst_bank = get_num_bank(ssd, dst_page);
		dst_bank_lpn = get_num_stripe(ssd, dst_page);

		pact = assign_new_write_vpn(ssd, dst_bank, dst_bank_lpn, &cur_need_to_emulate_tt);

		if(pact == NULL) assert(0);

		src_vpn = get_vpn(ssd, src_bank, src_bank_lpn);
		dst_vpn = pact->vbn * sc->PAGE_NB + pact->cpoff;
		
		off_in_page = dst_lpn % sc->LPAGE_PER_PPAGE;

		if(off_in_page == 0) {
			same_src = 1;
		}
		else if(src_vpn == last_src_vpn && last_src_bank == src_bank) {
			same_src++;
		}
		else {
			same_src = 1;
		}
		last_src_vpn = src_vpn;
		last_src_bank = src_bank;

#ifdef IOUB_DO_COPYBACK
		if(same_src == sc->LPAGE_PER_PPAGE && src_bank == dst_bank) {
			cur_need_to_emulate_tt = set_vpn(ssd, dst_bank, dst_bank_lpn, off_in_page, pact, 2, src_vpn);
			if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
			prof_IOUB->IM_copyback += sc->LPAGE_PER_PPAGE;
		} 
		else
#endif
		{
			cur_need_to_emulate_tt = set_vpn(ssd, dst_bank, dst_bank_lpn, off_in_page, pact, 1, src_vpn);
			if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
			if(off_in_page == sc->LPAGE_PER_PPAGE - 1)
				prof_IOUB->IM_write += sc->LPAGE_PER_PPAGE;
		}

		read_offset++;
		prof_IOUB->IM_count++;
	}


////////////////////////////////////////////////////////////////////////////////////////////////////////

	return max_need_to_emulate_tt;
}

int64_t recv_close_section(struct ssdstate *ssd) {
	struct ssdconf *sc = &(ssd->ssdparams);
	int32_t src_lbn, dst_lbn, bank, bank_lpn, lpn, sec_type;
	struct act_blk_info *pact = NULL;	
	int64_t cur_need_to_emulate_tt = 0, max_need_to_emulate_tt = 0;
	int8_t *valid_bitmap;
	int32_t page_offset;

	sec_type = ssd->req_info->p_data[1];
	src_lbn = ssd->req_info->p_data[2];
	dst_lbn = ssd->req_info->p_data[3];

	if (dst_lbn == -1) return 0;
	if (src_lbn == dst_lbn) return 0;

	if (src_lbn != -1) {
		src_lbn += ssd->n_page_mapping_lbn;
		dst_lbn += ssd->n_page_mapping_lbn;

		for (bank = 0; bank < sc->NUM_BANKS; bank++) {
			pact = actblk_find(ssd, bank, src_lbn);
	
			if(pact == NULL) assert(0);

			assert(pact->lbn != NULL_LBN);
			assert(pact->assoc_type == ASSOC_BLOCK);

			cur_need_to_emulate_tt = actblk_close(ssd, bank, pact);
			if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
			cur_need_to_emulate_tt = actblk_open(ssd, bank, dst_lbn, pact);
			if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
		}
		
		pact = actblk_find(ssd, 0, dst_lbn);
		memset(pact->page_validity, 0, sizeof(int8_t) * sc->PAGE_NB*sc->NUM_BANKS);

		for(int32_t offset_in_section = 0; offset_in_section < sc->PAGE_NB * sc->LPAGE_PER_PPAGE * sc->NUM_BANKS; offset_in_section++)
		{
			int32_t bitmap_offset = offset_in_section >> 5;		// A >> 5 == A / 32
			int32_t byte_offset = offset_in_section % 32;
			int32_t offset_in_byte = offset_in_section % 8;
			byte_offset = byte_offset >> 3;

			if(offset_in_section % sc->LPAGE_PER_PPAGE == 0)
				pact->page_validity[offset_in_section / sc->LPAGE_PER_PPAGE] = 0;

			if (((int32_t*)ssd->req_info->p_data)[4 + bitmap_offset] & (1 << ((byte_offset + 1) * 8  - offset_in_byte - 1)))
			{
				pact->page_validity[offset_in_section / sc->LPAGE_PER_PPAGE] |= (int8_t)1 << (offset_in_byte % sc->LPAGE_PER_PPAGE);
			}
			if(offset_in_section % sc->LPAGE_PER_PPAGE == sc->LPAGE_PER_PPAGE - 1)
			{
				int32_t bank_lpn = offset_in_section / sc->LPAGE_PER_PPAGE;
			}
		}

	}
	else {
		dst_lbn += ssd->n_page_mapping_lbn;

		printf("recv_close_section2: bank %d src_lbn %d dst_lbn %d\n", bank, src_lbn, dst_lbn);
		for (bank = 0; bank < sc->NUM_BANKS; bank++) {
			pact = actblk_find(ssd, bank, dst_lbn);
			if (pact == NULL) {
				pact = actblk_find_free_slot(ssd, bank);
				if (pact == NULL) {
					pact = actblk_find_victim(ssd, bank);
					cur_need_to_emulate_tt = actblk_close(ssd, bank, pact);
					if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
				}
				cur_need_to_emulate_tt = actblk_open(ssd, bank, dst_lbn, pact);
				if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
			}
			else {
				if (((int32_t *)ssd->req_info->p_data)[4] == 0) {
					cur_need_to_emulate_tt = actblk_close(ssd, bank, pact);
					if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
					cur_need_to_emulate_tt = actblk_open(ssd, bank, dst_lbn, pact);
					if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
				}
			}
		}
		pact = actblk_find(ssd, 0, dst_lbn);
		memset(pact->page_validity, 0, sizeof(int8_t) * sc->PAGE_NB*sc->NUM_BANKS);

		for(int32_t offset_in_section = 0; offset_in_section < sc->PAGE_NB * sc->LPAGE_PER_PPAGE * sc->NUM_BANKS; offset_in_section++)
		{
			int32_t bitmap_offset = offset_in_section >> 5;		// A >> 5 == A / 32
			int32_t byte_offset = offset_in_section % 32;
			int32_t offset_in_byte = offset_in_section % 8;
			byte_offset = byte_offset >> 3;

			if(offset_in_section % sc->LPAGE_PER_PPAGE == 0)
				pact->page_validity[offset_in_section / sc->LPAGE_PER_PPAGE] = 0;

			if (((int32_t*)ssd->req_info->p_data)[4 + bitmap_offset] & (1 << ((byte_offset + 1) * 8  - offset_in_byte - 1)))
			{
				pact->page_validity[offset_in_section / sc->LPAGE_PER_PPAGE] |= (int8_t)1 << (offset_in_byte % sc->LPAGE_PER_PPAGE);
			}
			if(offset_in_section % sc->LPAGE_PER_PPAGE == sc->LPAGE_PER_PPAGE - 1)
			{
				int32_t bank_lpn = offset_in_section / sc->LPAGE_PER_PPAGE;
			}
		}
	}

	if (ssd->ioub_start < sc->NUM_ACT_BLKS)
		ssd->ioub_start++;

	return max_need_to_emulate_tt;
}
