// File: vssim_config_manager.c
// Date: 2014. 12. 03.
// Author: Jinsoo Yoo (jedisty@hanyang.ac.kr)
// Copyright(c)2014
// Hanyang University, Seoul, Korea
// Embedded Software Systems Laboratory. All right reserved

//#include "common.h"
#include <assert.h>
#include "god.h"
#include "vssim_config_manager.h"

void INIT_SSD_CONFIG(struct ssdstate *ssd)
{
    struct ssdconf *sc = &(ssd->ssdparams);
	FILE* pfData;
	int i, j;
	pfData = fopen(ssd->conffile, "r");
	
	char* szCommand = NULL;
	
	szCommand = (char*)malloc(1024);
	memset(szCommand, 0x00, 1024);
	if(pfData!=NULL)
	{
		while(fscanf(pfData, "%s", szCommand)!=EOF)
		{
			if(strcmp(szCommand, "PAGE_SIZE") == 0)
			{
				fscanf(pfData, "%d", &sc->PAGE_SIZE);
			}
			else if(strcmp(szCommand, "PAGE_NB") == 0)
			{
				fscanf(pfData, "%d", &sc->PAGE_NB);
			}
			else if(strcmp(szCommand, "SECTOR_SIZE") == 0)
			{
				fscanf(pfData, "%d", &sc->SECTOR_SIZE);
			}	
			else if(strcmp(szCommand, "FLASH_NB") == 0)
			{
				fscanf(pfData, "%d", &sc->FLASH_NB);
			}	
			else if(strcmp(szCommand, "BLOCK_NB") == 0)
			{
				fscanf(pfData, "%d", &sc->BLOCK_NB);
			}					
			else if(strcmp(szCommand, "PLANES_PER_FLASH") == 0)
			{
				fscanf(pfData, "%d", &sc->PLANES_PER_FLASH);
			}
			else if(strcmp(szCommand, "REG_WRITE_DELAY") == 0)
			{
				fscanf(pfData, "%d", &sc->REG_WRITE_DELAY);
			}	
			else if(strcmp(szCommand, "CELL_PROGRAM_DELAY") == 0)
			{
				fscanf(pfData, "%d", &sc->CELL_PROGRAM_DELAY);
			}
			else if(strcmp(szCommand, "REG_READ_DELAY") == 0)
			{
				fscanf(pfData, "%d", &sc->REG_READ_DELAY);
			}
			else if(strcmp(szCommand, "CELL_READ_DELAY") == 0)
			{
				fscanf(pfData, "%d", &sc->CELL_READ_DELAY);
			}
			else if(strcmp(szCommand, "BLOCK_ERASE_DELAY") == 0)
			{
				fscanf(pfData, "%d", &sc->BLOCK_ERASE_DELAY);
			}
			else if(strcmp(szCommand, "CHANNEL_SWITCH_DELAY_R") == 0)
			{
				fscanf(pfData, "%d", &sc->CHANNEL_SWITCH_DELAY_R);
			}
			else if(strcmp(szCommand, "CHANNEL_SWITCH_DELAY_W") == 0)
			{
				fscanf(pfData, "%d", &sc->CHANNEL_SWITCH_DELAY_W);
			}
			else if(strcmp(szCommand, "DSM_TRIM_ENABLE") == 0)
			{
				fscanf(pfData, "%d", &sc->DSM_TRIM_ENABLE);
			}
			else if(strcmp(szCommand, "IO_PARALLELISM") == 0)
			{
				fscanf(pfData, "%d", &sc->IO_PARALLELISM);
			}
			else if(strcmp(szCommand, "CHANNEL_NB") == 0)
			{
				fscanf(pfData, "%d", &sc->CHANNEL_NB);
			}
			else if(strcmp(szCommand, "OVP") == 0)
			{
				fscanf(pfData, "%d", &sc->OVP);
			}
            else if (strcmp(szCommand, "GC_MODE") == 0)
            {
                fscanf(pfData, "%d", &sc->GC_MODE);
            }
#if defined FTL_MAP_CACHE || defined Polymorphic_FTL
			else if(strcmp(szCommand, "CACHE_IDX_SIZE") == 0)
			{
				fscanf(pfData, "%d", &sc->CACHE_IDX_SIZE);
			}
#endif
#ifdef FIRM_IO_BUFFER
			else if(strcmp(szCommand, "WRITE_BUFFER_FRAME_NB") == 0)
			{
				fscanf(pfData, "%u", &sc->WRITE_BUFFER_FRAME_NB);
			}
			else if(strcmp(szCommand, "READ_BUFFER_FRAME_NB") == 0)
			{
				fscanf(pfData, "%u", &sc->READ_BUFFER_FRAME_NB);
			}
#endif
#ifdef HOST_QUEUE
			else if(strcmp(szCommand, "HOST_QUEUE_ENTRY_NB") == 0)
			{
				fscanf(pfData, "%u", &sc->HOST_QUEUE_ENTRY_NB);
			}
#endif
#if defined FAST_FTL || defined LAST_FTL
			else if(strcmp(szCommand, "LOG_RAND_BLOCK_NB") == 0)
			{
				fscanf(pfData, "%d", &sc->LOG_RAND_BLOCK_NB);
			}	
			else if(strcmp(szCommand, "LOG_SEQ_BLOCK_NB") == 0)
			{
				fscanf(pfData, "%d", &sc->LOG_SEQ_BLOCK_NB);
			}	
#endif
			memset(szCommand, 0x00, 1024);
		}	
		fclose(pfData);

	} else {
        printf("Error: Cannot find configuration file for [%s]\n", ssd->ssdname);
        exit(EXIT_FAILURE);
    }

	/* Exception Handler */
	if(sc->FLASH_NB < sc->CHANNEL_NB){
		printf("ERROR[%s] Wrong CHANNEL_NB %d\n",__FUNCTION__, sc->CHANNEL_NB);
		return;
	}
	if(sc->PLANES_PER_FLASH != 1 && sc->PLANES_PER_FLASH % 2 != 0){
		printf("ERROR[%s] Wrong PLANAES_PER_FLASH %d\n", __FUNCTION__, sc->PLANES_PER_FLASH);
		return;
	}
#ifdef FIRM_IO_BUFFER
	if(sc->WRITE_BUFFER_FRAME_NB == 0 || sc->READ_BUFFER_FRAME_NB == 0){
		printf("ERROR[%s] Wrong parameter for SSD_IO_BUFFER",__FUNCTION__);
		return;
	}
#endif

	/* SSD Configuration */
	sc->SECTORS_PER_PAGE = sc->PAGE_SIZE / sc->SECTOR_SIZE;
	sc->PAGES_PER_FLASH = sc->PAGE_NB * sc->BLOCK_NB;
	sc->SECTOR_NB = (int64_t)sc->SECTORS_PER_PAGE * (int64_t)sc->PAGE_NB * (int64_t)sc->BLOCK_NB * (int64_t)sc->FLASH_NB;
    printf("VSSIM: sector_nb = %" PRId64 "\n", sc->SECTOR_NB);
#ifndef Polymorphic_FTL
	sc->WAY_NB = sc->FLASH_NB / sc->CHANNEL_NB;
#endif

	sc->NUM_BANKS = sc->FLASH_NB;
	sc->NUM_ACT_BLKS = 6;
	printf("FLASH_NB %d BLOCK_NB %d PAGE_NB %d\n", sc->FLASH_NB, sc->BLOCK_NB, sc->PAGE_NB);
	printf("SECTORS_PER_PER_PAGE: %d SECTOR_SIZE: %d PAGE_SIZE: %d\n", sc->SECTORS_PER_PAGE, sc->SECTOR_SIZE, sc->PAGE_SIZE);
	sc->GARBAGE_BITMAP_WORD = (sc->BLOCK_NB + sizeof(int64_t) - 1) / sizeof(int64_t);
//	sc->NUM_LBLOCKS = sc->BLOCK_NB;	//TODO:
//	sc->NUM_LPAGES = sc->PAGE_NB;		//TODO:
	
	sc->VC_ACTIVE_BLK = sc->PAGE_NB * 2;

	sc->LPAGE_SIZE = 4096;		
	sc->LPAGE_PER_PPAGE = sc->PAGE_SIZE / sc->LPAGE_SIZE;
	sc->SECTORS_PER_LPAGE = sc->SECTORS_PER_PAGE / sc->LPAGE_PER_PPAGE;

	sc->PAGE_TABLE_SIZE = sc->SECTOR_NB / sc->SECTORS_PER_LPAGE / sc->LPAGE_PER_PPAGE;

	/* Mapping Table */
	sc->BLOCK_MAPPING_ENTRY_NB = (int64_t)sc->BLOCK_NB * (int64_t)sc->FLASH_NB;
	sc->PAGES_IN_SSD = (int64_t)sc->PAGE_NB * (int64_t)sc->BLOCK_NB * (int64_t)sc->FLASH_NB;

#ifdef PAGE_MAP
	sc->PAGE_MAPPING_ENTRY_NB = (int64_t)sc->PAGE_NB * (int64_t)sc->BLOCK_NB * (int64_t)sc->FLASH_NB;
#endif

#if defined PAGE_MAP || defined BLOCK_MAP
	sc->EACH_EMPTY_TABLE_ENTRY_NB = (int64_t)sc->BLOCK_NB / (int64_t)sc->PLANES_PER_FLASH;
	sc->EMPTY_TABLE_ENTRY_NB = sc->FLASH_NB * sc->PLANES_PER_FLASH;
	sc->VICTIM_TABLE_ENTRY_NB = sc->FLASH_NB * sc->PLANES_PER_FLASH;

	sc->DATA_BLOCK_NB = sc->BLOCK_NB;
#endif

#if defined FAST_FTL || defined LAST_FTL
	sc->DATA_BLOCK_NB = sc->BLOCK_NB - sc->LOG_RAND_BLOCK_NB - sc->LOG_SEQ_BLOCK_NB;
	sc->DATA_MAPPING_ENTRY_NB = (int64_t)sc->FLASH_NB * (int64_t)sc->DATA_BLOCK_NB;

	sc->EACH_EMPTY_BLOCK_ENTRY_NB = (int64_t)sc->BLOCK_NB / (int64_t)sc->PLANES_PER_FLASH;
	sc->EMPTY_BLOCK_TABLE_NB = sc->FLASH_NB * sc->PLANES_PER_FLASH;
#endif

#ifdef DA_MAP
	if(sc->BM_START_SECTOR_NB < 0 || sc->BM_START_SECTOR_NB >= sc->SECTOR_NB){
		printf("ERROR[%s] BM_START_SECTOR_NB %d \n", __FUNCTION__, sc->BM_START_SECTOR_NB);
	}

	sc->DA_PAGE_MAPPING_ENTRY_NB = CALC_DA_PM_ENTRY_NB(); 
	sc->DA_BLOCK_MAPPING_ENTRY_NB = CALC_DA_BM_ENTRY_NB();
	sc->EACH_EMPTY_TABLE_ENTRY_NB = (int64_t)sc->BLOCK_NB / (int64_t)sc->PLANES_PER_FLASH;
	sc->EMPTY_TABLE_ENTRY_NB = sc->FLASH_NB * sc->PLANES_PER_FLASH;
	sc->VICTIM_TABLE_ENTRY_NB = sc->FLASH_NB * sc->PLANES_PER_FLASH;
#endif

	/* FAST Performance Test */
#ifdef FAST_FTL
	sc->SEQ_MAPPING_ENTRY_NB = (int64_t)sc->FLASH_NB * (int64_t)sc->LOG_SEQ_BLOCK_NB;
	sc->RAN_MAPPING_ENTRY_NB = (int64_t)sc->FLASH_NB * (int64_t)sc->LOG_RAND_BLOCK_NB;
	sc->RAN_LOG_MAPPING_ENTRY_NB = (int64_t)sc->FLASH_NB * (int64_t)sc->LOG_RAND_BLOCK_NB * (int64_t)sc->PAGE_NB;

	// PARAL_DEGREE = 4;
	sc->PARAL_DEGREE = sc->RAN_MAPPING_ENTRY_NB;
	if(sc->PARAL_DEGREE > sc->RAN_MAPPING_ENTRY_NB){
		printf("[INIT_SSD_CONFIG] ERROR PARAL_DEGREE \n");
		return;
	}
	sc->PARAL_COUNT = sc->PARAL_DEGREE * sc->PAGE_NB;
#endif

#ifdef LAST_FTL
	sc->SEQ_MAPPING_ENTRY_NB = (int64_t)sc->FLASH_NB * (int64_t)sc->LOG_SEQ_BLOCK_NB;
	sc->RAN_MAPPING_ENTRY_NB = (int64_t)sc->FLASH_NB * ((int64_t)sc->LOG_RAND_BLOCK_NB/2);
	sc->RAN_LOG_MAPPING_ENTRY_NB = (int64_t)sc->FLASH_NB * ((int64_t)sc->LOG_RAND_BLOCK_NB/2) * (int64_t)sc->PAGE_NB;

	sc->SEQ_THRESHOLD = sc->SECTORS_PER_PAGE * 4;
	sc->HOT_PAGE_NB_THRESHOLD = sc->PAGE_NB;

	sc->PARAL_DEGREE = sc->RAN_MAPPING_ENTRY_NB;
	if(sc->PARAL_DEGREE > sc->RAN_MAPPING_ENTRY_NB){
		printf("[INIT_SSD_CONFIG] ERROR PARAL_DEGREE \n");
		return;
	}
	sc->PARAL_COUNT = sc->PARAL_DEGREE * sc->PAGE_NB;
#endif

	/* Garbage Collection */
#if defined PAGE_MAP || defined BLOCK_MAP || defined DA_MAP
	sc->GC_THRESHOLD = 0.75; // 0.7 for 70%, 0.9 for 90%
	sc->GC_THRESHOLD_HARD = 0.78;
	sc->GC_THRESHOLD_BLOCK_NB = (int)((1-sc->GC_THRESHOLD) * (double)sc->BLOCK_MAPPING_ENTRY_NB);
	sc->GC_THRESHOLD_BLOCK_NB_HARD = (int)((1-sc->GC_THRESHOLD_HARD) * (double)sc->BLOCK_MAPPING_ENTRY_NB);
	sc->GC_THRESHOLD_BLOCK_NB_EACH = (int)((1-sc->GC_THRESHOLD) * (double)sc->EACH_EMPTY_TABLE_ENTRY_NB);
	if(sc->OVP != 0){
		sc->GC_VICTIM_NB = sc->FLASH_NB * sc->BLOCK_NB * sc->OVP / 100 / 2;
	}
	else{
		sc->GC_VICTIM_NB = 1;
	}
#endif

    /* Coperd: allocate GC_SLOT structure here */
    int num_gc_slots = 0;
    if (sc->GC_MODE == WHOLE_BLOCKING) {
        num_gc_slots = 1;
    } else if (sc->GC_MODE == CHANNEL_BLOCKING) {
        num_gc_slots = sc->CHANNEL_NB;
    } else if (sc->GC_MODE == CHIP_BLOCKING) {
        num_gc_slots = sc->FLASH_NB * sc->PLANES_PER_FLASH;
    } else {
        printf("Unsupported GC MODE: %d!\n", sc->GC_MODE);
        exit(EXIT_FAILURE);
    }

    assert(num_gc_slots != 0);
    ssd->gc_slot = calloc(num_gc_slots, sizeof(int64_t));

	/* Map Cache */
#ifdef FTL_MAP_CACHE
	sc->MAP_ENTRY_SIZE = sizeof(int64_t);
	sc->MAP_ENTRIES_PER_PAGE = sc->PAGE_SIZE / sc->MAP_ENTRY_SIZE;
	sc->MAP_ENTRY_NB = sc->PAGE_MAPPING_ENTRY_NB / sc->MAP_ENTRIES_PER_PAGE;	
#endif

	/* Polymorphic FTL */
#ifdef Polymorphic_FTL
	sc->WAY_NB = 1;

	sc->PHY_SPARE_SIZE = 436;
	sc->NR_PHY_BLOCKS = (int64_t)sc->FLASH_NB * (int64_t)sc->WAY_NB * (int64_t)sc->BLOCK_NB;
	sc->NR_PHY_PAGES = sc->NR_PHY_BLOCKS * (int64_t)sc->PAGE_NB;
	sc->NR_PHY_SECTORS = sc->NR_PHY_PAGES * (int64_t)sc->SECTORS_PER_PAGE;

	sc->NR_RESERVED_PHY_SUPER_BLOCKS = 4;
	sc->NR_RESERVED_PHY_BLOCKS = sc->FLASH_NB * sc->WAY_NB * sc->NR_RESERVED_PHY_SUPER_BLOCKS;
	sc->NR_RESERVED_PHY_PAGES = sc->NR_RESERVED_PHY_BLOCKS * sc->PAGE_NB;
#endif

    ssd->chnl_next_avail_time = (int64_t *)malloc(sizeof(int64_t) * sc->CHANNEL_NB);
    memset(ssd->chnl_next_avail_time, 0, sizeof(int64_t) * sc->CHANNEL_NB);
    ssd->chip_next_avail_time = (int64_t *)malloc(sizeof(int64_t) * sc->FLASH_NB);
    memset(ssd->chip_next_avail_time, 0, sizeof(int64_t) * sc->FLASH_NB);


	ssd->v_bad_blk_count = (int64_t *)malloc(sizeof(int64_t) * sc->NUM_BANKS);

	ssd->v_misc_metas = (struct misc_metadata *)malloc(sizeof(struct misc_metadata) * sc->NUM_BANKS);
	ssd->v_bank_infos = (struct bank_info *)malloc(sizeof(struct bank_info) * sc->NUM_BANKS);

	for(i = 0; i < sc->NUM_BANKS; i++) {
//		QTAILQ_INIT(&(ssd->bank_q[i].head));
		ssd->v_misc_metas[i].act_blks = (struct act_blk_info *)malloc(sizeof(struct act_blk_info) * sc->NUM_ACT_BLKS);
		ssd->v_misc_metas[i].summ_pages = (struct summary_page *)malloc(sizeof(struct summary_page) * 2);
		ssd->v_misc_metas[i].free_blk_list = (int64_t *)malloc(sizeof(int64_t) * sc->BLOCK_NB);

		ssd->v_bank_infos[i].garbage_bitmap = (int64_t *)malloc(sizeof(int64_t) * sc->GARBAGE_BITMAP_WORD);
		ssd->v_bank_infos[i].vcount_table = (int64_t *)malloc(sizeof(int64_t) * sc->BLOCK_NB);
		ssd->v_bank_infos[i].block_mapping_table = (int64_t *)malloc(sizeof(int64_t) * sc->BLOCK_NB);
		ssd->v_bank_infos[i].block_assoc_table = (int64_t *)malloc(sizeof(int64_t) * sc->BLOCK_NB);
		ssd->v_bank_infos[i].assoc_type_table = (int64_t *)malloc(sizeof(int64_t) * sc->BLOCK_NB);
		ssd->v_bank_infos[i].page_mapping_table = (int64_t *)malloc(sizeof(int64_t) * sc->PAGE_TABLE_SIZE);
		ssd->v_bank_infos[i].inverse_mapping_table = (int64_t *)malloc(sizeof(int64_t) * sc->BLOCK_NB * sc->PAGE_NB);

		for (j = 0; j < sc->NUM_ACT_BLKS; j++) {
			ssd->v_misc_metas[i].act_blks[j].log_table = (int64_t *)malloc(sizeof(int64_t) * sc->PAGE_NB);
			if(i == 0)
				ssd->v_misc_metas[i].act_blks[j].page_validity = (int8_t *)malloc(sizeof(int8_t) * sc->PAGE_NB*sc->NUM_BANKS);
			else
				ssd->v_misc_metas[i].act_blks[j].page_validity = ssd->v_misc_metas[0].act_blks[j].page_validity;
			actblk_init(ssd, i, &ssd->v_misc_metas[i].act_blks[j]);
			ssd->v_misc_metas[i].act_blks[j].slot_idx = j;
		}

		for (j = 0; j < 2; j++) {
			ssd->v_misc_metas[i].summ_pages[j].log_table = (int64_t *)malloc(sizeof(int64_t) * sc->PAGE_NB);
			ssd->v_misc_metas[i].summ_pages[j].page_validity = (int8_t *)malloc(sizeof(int8_t) * sc->PAGE_NB);
			summ_init(ssd, &ssd->v_misc_metas[i].summ_pages[j]);
		}

		int vblock = 0;
		for (j = 0; j < sc->BLOCK_NB; j++) {
			ssd->v_misc_metas[i].free_blk_list[j] = vblock;
			
			ssd->v_misc_metas[i].num_free_blk = sc->BLOCK_NB;
			ssd->v_misc_metas[i].free_blk_last_idx = sc->BLOCK_NB - 1;
			ssd->v_misc_metas[i].garbage_last_idx = vblock - 1;
			ssd->v_misc_metas[i].num_garbage_blks = 0;

			vblock++;
		}

		for(j = 0; j < sc->GARBAGE_BITMAP_WORD; j++) {
//			ssd->v_bank_infos[i].garbage_bitmap[j] = NULL_64BIT;
			ssd->v_bank_infos[i].garbage_bitmap[j] = 0;
		}

		for(j = 0; j < sc->BLOCK_NB; j++) {
			ssd->v_bank_infos[i].vcount_table[j] = 0;
			ssd->v_bank_infos[i].block_mapping_table[j] = NULL_VBN;
			ssd->v_bank_infos[i].assoc_type_table[j] = ASSOC_BLOCK;
			ssd->v_bank_infos[i].block_assoc_table[j] = NULL_VBN;
		}

		for(j = 0; j < sc->BLOCK_NB * sc->PAGE_NB; j++) {
			ssd->v_bank_infos[i].inverse_mapping_table[j] = NULL_64BIT;
		}

		for(j = 0; j < sc->PAGE_TABLE_SIZE; j++) {
			ssd->v_bank_infos[i].page_mapping_table[j] = NULL_64BIT;
		}

		for(j = 0; j < sc->PAGE_TABLE_SIZE / sc->NUM_BANKS; j += sc->PAGE_NB) {
//			printf("idx %d PAGE_TABLE_SIZE %d\n", j, sc->PAGE_TABLE_SIZE);
			ssd->v_bank_infos[i].page_mapping_table[j] = PGT_SEG_FREE_MARK;
		}

		ssd->v_misc_metas[i].pgt_last_idx = 0;

		ssd->v_misc_metas[i].act_free_pos = BITSEQ(sc->NUM_ACT_BLKS);

		ssd->v_misc_metas[i].act_blk_seq = 0;

	}

	ssd->ioub_start = 0;

	ssd->req_info = (struct req_info *)malloc(sizeof(struct req_info));

	ssd->padding_count = ssd->copyback_count = ssd->read_count = ssd->write_count = 0;
	ssd->padding_size = ssd->real_write_size = 0;
	ssd->close_merge_count = ssd->close_merge_size = 0;

	ssd->wb_idx = sc->NUM_ACT_BLKS;
	ssd->max_lpn_cnt = ssd->min_lpn_cnt = 1;
	ssd->max_wb_cnt = ssd->max_lpn_cnt * sc->LPAGE_PER_PPAGE;
	ssd->g_age = 0;
	ssd->wb = (int64_t **)malloc(sizeof(int64_t *) * ssd->wb_idx);
	ssd->wb_table = (int64_t *)malloc(sizeof(int64_t) * ssd->wb_idx);
	ssd->cur_wb_cnt = (int64_t *)malloc(sizeof(int64_t) * ssd->wb_idx);
	ssd->cur_lpn_cnt = (int64_t *)malloc(sizeof(int64_t) * ssd->wb_idx);
	ssd->age = (int64_t *)malloc(sizeof(int64_t) * ssd->wb_idx);
	for (i = 0; i < ssd->wb_idx; i++) {
		ssd->wb[i] = (int64_t *)malloc(sizeof(int64_t) * ssd->max_wb_cnt);
		ssd->age[i] = 0;
		for (int j = 0; j < ssd->max_wb_cnt; j++) {
			ssd->wb[i][j] = -1;
		}
		ssd->wb_table[i] = -1;
		ssd->cur_wb_cnt[i] = ssd->cur_lpn_cnt[i] = 0;
	}
	
	ssd->im_wb = (int64_t **)malloc(sizeof(int64_t*) * sc->LPAGE_PER_PPAGE);
	for (i = 0; i < sc->LPAGE_PER_PPAGE; i++) ssd->im_wb[i] = (int64_t *)malloc(sizeof(int64_t) * 2);	// src lpn & dst lpn
	ssd->im_wb_cnt = 0;

#if 1
	{
		ssd->n_page_mapping_lbn = 0;

		int64_t last_lpn = (PARTITION_SIZE_IN_SECTOR + METADATA_SIZE_IN_SECTOR) / sc->SECTORS_PER_PAGE;
		int64_t bank, lpn, bank_lpn, lbn;

		for (lpn = 0; lpn < last_lpn; lpn++) {
			bank = get_num_bank(ssd, lpn);
			bank_lpn = get_num_stripe(ssd, lpn);
			lbn = bank_lpn / sc->PAGE_NB;
			
			attach_page_table(ssd, bank, lbn);
			ssd->n_page_mapping_lbn = lbn + 1;
		}
	}
#endif

	free(szCommand);
}

#ifdef DA_MAP
int64_t CALC_DA_PM_ENTRY_NB(void)
{
	int64_t ret_page_nb = (int64_t)sc->BM_START_SECTOR_NB / sc->SECTORS_PER_PAGE;
	if((sc->BM_START_SECTOR_NB % sc->SECTORS_PER_PAGE) != 0)
		ret_page_nb += 1;

	int64_t block_nb = ret_page_nb / sc->PAGE_NB;
	if((ret_page_nb % sc->PAGE_NB) != 0)
		block_nb += 1;

	ret_page_nb = block_nb * sc->PAGE_NB;
	bm_start_sect_nb = ret_page_nb * sc->SECTORS_PER_PAGE;

	return ret_page_nb;
}

int64_t CALC_DA_BM_ENTRY_NB(void)
{
	int64_t total_page_nb = (int64_t)sc->PAGE_NB * sc->BLOCK_NB * sc->FLASH_NB;
	int64_t ret_block_nb = ((int64_t)total_page_nb - sc->DA_PAGE_MAPPING_ENTRY_NB)/(int64_t)sc->PAGE_NB;

	int64_t temp_total_page_nb = ret_block_nb * sc->PAGE_NB + sc->DA_PAGE_MAPPING_ENTRY_NB;
	if(temp_total_page_nb != total_page_nb){
		printf("ERROR[%s] %ld != %ld\n", __FUNCTION__, total_page_nb, temp_total_page_nb);
	}

	return ret_block_nb;
}
#endif
