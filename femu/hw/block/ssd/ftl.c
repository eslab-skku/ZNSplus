// File: ftl.c
// Date: 2014. 12. 03.
// Author: Jinsoo Yoo (jedisty@hanyang.ac.kr)
// Copyright(c)2014
// Hanyang University, Seoul, Korea
// Embedded Software Systems Laboratory. All right reserved

#include "qemu/osdep.h"
#include "block/block_int.h"
#include "block/qapi.h"
#include "exec/memory.h"
#include "hw/block/block.h"
#include "hw/hw.h"
#include "hw/pci/msix.h"
#include "hw/pci/msi.h"
#include "hw/pci/pci.h"
#include "qapi/visitor.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/bitops.h"
#include "qemu/bitmap.h"
#include "qom/object.h"
#include "sysemu/sysemu.h"
#include "sysemu/block-backend.h"
#include <qemu/main-loop.h>
#include "block/block_int.h"
#include "god.h"
#include "common.h"
#include "ssd_io_manager.h"
#include "ftl_gc_manager.h"
#include "ssd.h"
#include "vssim_config_manager.h"
#include "bast_util.h"
#ifndef VSSIM_BENCH
//#include "qemu-kvm.h"
#endif

#ifdef FTL_GET_WRITE_WORKLOAD
FILE* fp_write_workload;
#endif
#ifdef FTL_IO_LATENCY
FILE* fp_ftl_w;
FILE* fp_ftl_r;
#endif

extern int64_t blocking_to;

void FTL_INIT(struct ssdstate *ssd)
{
    int g_init = ssd->g_init;

	if(g_init == 0){

		INIT_SSD_CONFIG(ssd);

		INIT_MAPPING_TABLE(ssd);
		INIT_INVERSE_MAPPING_TABLE(ssd);
		INIT_BLOCK_STATE_TABLE(ssd);
		INIT_VALID_ARRAY(ssd);
		INIT_EMPTY_BLOCK_LIST(ssd);
		INIT_VICTIM_BLOCK_LIST(ssd);
		INIT_PERF_CHECKER(ssd);
		
#ifdef FTL_MAP_CACHE
		INIT_CACHE();
#endif
#ifdef FIRM_IO_BUFFER
		INIT_IO_BUFFER();
#endif
#ifdef MONITOR_ON
		INIT_LOG_MANAGER();
#endif
		g_init = 1;
#ifdef FTL_GET_WRITE_WORKLOAD
		fp_write_workload = fopen("./data/p_write_workload.txt","a");
#endif
#ifdef FTL_IO_LATENCY
		fp_ftl_w = fopen("./data/p_ftl_w.txt","a");
		fp_ftl_r = fopen("./data/p_ftl_r.txt","a");
#endif
		SSD_IO_INIT(ssd);
	
	}
}

void FTL_TERM(struct ssdstate *ssd)
{
}

int64_t FTL_READ(struct ssdstate *ssd, int64_t sector_nb, unsigned int length)
{
	int64_t ret;

#ifdef GET_FTL_WORKLOAD
	FILE* fp_workload = fopen("./data/workload_ftl.txt","a");
	struct timeval tv;
	struct tm *lt;
	double curr_time;
	gettimeofday(&tv, 0);
	lt = localtime(&(tv.tv_sec));
	curr_time = lt->tm_hour*3600 + lt->tm_min*60 + lt->tm_sec + (double)tv.tv_usec/(double)1000000;
	//fprintf(fp_workload,"%lf %d %ld %u %x\n",curr_time, 0, sector_nb, length, 1);
	fprintf(fp_workload,"%lf %d %u %x\n",curr_time, sector_nb, length, 1);
	fclose(fp_workload);
#endif
#ifdef FTL_IO_LATENCY
	int64_t start_ftl_r, end_ftl_r;
	start_ftl_r = get_usec();
#endif
	ret = _FTL_READ(ssd, sector_nb, length);

	return ret;
}

int64_t FTL_WRITE(struct ssdstate *ssd, int64_t sector_nb, unsigned int length)
{
	struct ssdconf *sc = &(ssd->ssdparams);
	int64_t ret, i;
	int64_t cur_need_to_emulate_tt = 0, max_need_to_emulate_tt = 0;
#ifdef GET_FTL_WORKLOAD
	FILE* fp_workload = fopen("./data/workload_ftl.txt","a");
	struct timeval tv;
	struct tm *lt;
	double curr_time;
	gettimeofday(&tv, 0);
	lt = localtime(&(tv.tv_sec));
	curr_time = lt->tm_hour*3600 + lt->tm_min*60 + lt->tm_sec + (double)tv.tv_usec/(double)1000000;
	fprintf(fp_workload,"%lf %d %u %x\n",curr_time, sector_nb, length, 0);
	fclose(fp_workload);
#endif
#ifdef FTL_IO_LATENCY
	int64_t start_ftl_w, end_ftl_w;
	start_ftl_w = get_usec();
#endif

	ret = _FTL_WRITE(ssd, sector_nb, length);

#ifdef FTL_IO_LATENCY
	end_ftl_w = get_usec();
	if(length >= 128)
		fprintf(fp_ftl_w,"%ld\t%u\n", end_ftl_w - start_ftl_w, length);
#endif

    return ret;
}


int64_t _FTL_READ(struct ssdstate *ssd, int64_t sector_nb, unsigned int length)
{
    struct ssdconf *sc = &(ssd->ssdparams);
    int64_t SECTOR_NB = sc->SECTOR_NB;
    int64_t SECTORS_PER_PAGE = sc->SECTORS_PER_PAGE;
    int PLANES_PER_FLASH = sc->PLANES_PER_FLASH;
    int CHANNEL_NB = sc->CHANNEL_NB;
    int GC_MODE = sc->GC_MODE;
    int64_t *gc_slot = ssd->gc_slot;
    int64_t cur_need_to_emulate_tt = 0, max_need_to_emulate_tt = 0;

    int64_t curtime = get_usec();

    /* Coperd: FTL layer blocked reads statistics */
    ssd->nb_total_reads++;
    ssd->nb_total_rd_sz += length;


#ifdef FTL_DEBUG
	printf("[%s] Start\n", __FUNCTION__);
#endif

	if(sector_nb + length > SECTOR_NB){
		printf("Error[%s] Exceed Sector number\n", __FUNCTION__); 
		return FAIL;	
	}

	int64_t lpn;
	int64_t ppn;
	int64_t lba = sector_nb;
	unsigned int remain = length;
	unsigned int left_skip = sector_nb % SECTORS_PER_PAGE;
	unsigned int right_skip;
	unsigned int read_sects;

	unsigned int ret = FAIL;
	int read_page_nb = 0;
	int io_page_nb;

	nand_io_info* n_io_info = NULL;

    int num_flash = 0, num_blk = 0, num_channel = 0, num_plane = 0;
    int slot;
	int64_t bank, bank_lpn;

#ifdef FIRM_IO_BUFFER
	INCREASE_RB_FTL_POINTER(length);
#endif

	while(remain > 0){

		if(remain > SECTORS_PER_PAGE - left_skip){
			right_skip = 0;
		}
		else{
			right_skip = SECTORS_PER_PAGE - left_skip - remain;
		}
		read_sects = SECTORS_PER_PAGE - left_skip - right_skip;

		lpn = lba / (int64_t)SECTORS_PER_PAGE;
		bank = get_num_bank(ssd, lpn);
		bank_lpn = get_num_stripe(ssd, lpn);
		ppn = get_vpn(ssd, bank, bank_lpn);

		if(ppn == -1){
#ifdef FIRM_IO_BUFFER
			INCREASE_RB_LIMIT_POINTER();
#endif
		}

		lba += read_sects;
		remain -= read_sects;
		left_skip = 0;
	}

	ssd->io_alloc_overhead = ALLOC_IO_REQUEST(ssd, sector_nb, length, READ, &io_page_nb);

	remain = length;
	lba = sector_nb;
	left_skip = sector_nb % SECTORS_PER_PAGE;

    /* 
     * Coperd: since the whole I/O submission path is single threaded, it's
     * safe to do this. "blocking_to" means the time we will block the
     * current I/O to. It will be finally decided by gc timestamps according 
     * to the GC mode you are using.
     */
    blocking_to = 0;

	while(remain > 0){

		if(remain > SECTORS_PER_PAGE - left_skip){
			right_skip = 0;
		}
		else{
			right_skip = SECTORS_PER_PAGE - left_skip - remain;
		}
		read_sects = SECTORS_PER_PAGE - left_skip - right_skip;

		lpn = lba / (int64_t)SECTORS_PER_PAGE;
		bank = get_num_bank(ssd, lpn);
		bank_lpn = get_num_stripe(ssd, lpn);

#ifdef FTL_MAP_CACHE
		ppn = CACHE_GET_PPN(lpn);
#else
		ppn = get_vpn(ssd, bank, bank_lpn);
#endif

		if(ppn == -1){
#ifdef FTL_DEBUG
			printf("ERROR[%s] No Mapping info\n", __FUNCTION__);
#endif
            ppn = 0;
		}

		/* Read data from NAND page */
                n_io_info = CREATE_NAND_IO_INFO(ssd, read_page_nb, READ, io_page_nb, ssd->io_request_seq_nb);


        num_flash = CALC_FLASH(ssd, ppn);
        num_blk = CALC_BLOCK(ssd, ppn);
        num_channel = num_flash %  CHANNEL_NB;
        num_plane = num_flash * PLANES_PER_FLASH + num_blk % PLANES_PER_FLASH;
        if (GC_MODE == WHOLE_BLOCKING) {
            slot = 0;
        } else if (GC_MODE == CHANNEL_BLOCKING) {
            slot = num_channel;
        } else if (GC_MODE == CHIP_BLOCKING) {
            slot = num_plane;
        }

        if (gc_slot[slot] > blocking_to) {
            blocking_to = gc_slot[slot];
        }
		cur_need_to_emulate_tt = SSD_PAGE_READ(ssd, bank, num_blk, CALC_PAGE(ssd, ppn), n_io_info);

        if (cur_need_to_emulate_tt > max_need_to_emulate_tt) {
            max_need_to_emulate_tt = cur_need_to_emulate_tt;
        }

#ifdef FTL_DEBUG
		if(ret == SUCCESS){
			printf("\t read complete [%u]\n",ppn);
		}
		else if(ret == FAIL){
			printf("ERROR[%s] %u page read fail \n",__FUNCTION__, ppn);
		}
#endif
		read_page_nb++;

		lba += read_sects;
		remain -= read_sects;
		left_skip = 0;

	}

    if (blocking_to > curtime) {
        ssd->nb_blocked_reads++;
    }

	INCREASE_IO_REQUEST_SEQ_NB(ssd);

#ifdef FIRM_IO_BUFFER
	INCREASE_RB_LIMIT_POINTER();
#endif

#ifdef MONITOR_ON
	char szTemp[1024];
	sprintf(szTemp, "READ PAGE %d ", length);
	WRITE_LOG(szTemp);
#endif

#ifdef FTL_DEBUG
	printf("[%s] Complete\n", __FUNCTION__);
#endif

	return max_need_to_emulate_tt;
}

int64_t _FTL_WRITE(struct ssdstate *ssd, int64_t sector_nb, unsigned int length)
{
	struct ssdconf *sc = &(ssd->ssdparams);
	int64_t SECTOR_NB = sc->SECTOR_NB;
	int64_t SECTORS_PER_PAGE = sc->SECTORS_PER_PAGE;
	int64_t SECTORS_PER_LPAGE = sc->SECTORS_PER_LPAGE;
	int PLANES_PER_FLASH = sc->PLANES_PER_FLASH;
	int CHANNEL_NB = sc->CHANNEL_NB;
	int64_t *gc_slot = ssd->gc_slot;
	int GC_MODE = sc->GC_MODE;
	int EMPTY_TABLE_ENTRY_NB = sc->EMPTY_TABLE_ENTRY_NB;
	int64_t cur_need_to_emulate_tt = 0, max_need_to_emulate_tt = 0;
	int64_t curtime = get_usec();
#ifdef IOUB_COMMAND
//Page mapping to not act
	if (sector_nb >= IOUB_START_ADDR && sector_nb < PARTITION_SIZE_IN_SECTOR+METADATA_SIZE_IN_SECTOR) {
		cur_need_to_emulate_tt = ioub_process_command(ssd, sector_nb, length);
		return cur_need_to_emulate_tt;
	}
#endif

	if (ssd->in_warmup_stage == 0) {
		ssd->nb_total_writes++;
		ssd->nb_total_wr_sz += length;
	}

#ifdef FTL_DEBUG
	printf("[%s] Start\n", __FUNCTION__);
#endif

#ifdef FTL_GET_WRITE_WORKLOAD
	fprintf(fp_write_workload,"%d\t%u\n", sector_nb, length);
#endif

	int io_page_nb;

	if(sector_nb + length > SECTOR_NB){
		printf("ERROR[%s] Exceed Sector number sector_nb %d length %d SECTOR_NB %d\n", __FUNCTION__, sector_nb, length, SECTOR_NB);
		return FAIL;
	}
	else{
		ssd->io_alloc_overhead = ALLOC_IO_REQUEST(ssd, sector_nb, length, WRITE, &io_page_nb);
	}

	int64_t lba = sector_nb;
	int64_t lpn;
	int64_t new_vpn;
	int64_t old_vpn;

	unsigned int remain = length;
	unsigned int left_skip = sector_nb % SECTORS_PER_LPAGE;
	unsigned int right_skip;
	unsigned int write_sects;
	unsigned int sect_offset;

	unsigned int ret = FAIL;
	int write_page_nb=0;
	nand_io_info* n_io_info = NULL;

	int num_channel = 0, num_flash = 0, num_blk = 0, num_plane = 0;
	int slot;
	struct act_blk_info *pact = NULL;
	int64_t bank, bank_lpn, page_offset, column_cnt, off_in_page;
	int64_t type = 1;

	/* 
	 * Coperd: since the whole I/O submission path is single threaded, it's
	 * safe to do this. "blocking_to" means the time we will block the
	 * current I/O to. It will be finally decided by gc timestamps according 
	 * to the GC mode you are using.
	 */
	blocking_to = 0;


	while(remain > 0){

		if(remain > SECTORS_PER_LPAGE - left_skip){
			right_skip = 0;
		}
		else{
			right_skip = SECTORS_PER_LPAGE - left_skip - remain;
		}

		write_sects = SECTORS_PER_LPAGE - left_skip - right_skip;

#ifdef FIRM_IO_BUFFER
		INCREASE_WB_FTL_POINTER(write_sects);
#endif

		lpn = lba / (int64_t)SECTORS_PER_PAGE;
		bank = get_num_bank(ssd, lpn);
		bank_lpn = get_num_stripe(ssd, lpn);
		off_in_page = (lba / SECTORS_PER_LPAGE) % sc->LPAGE_PER_PPAGE;

		pact = assign_new_write_vpn(ssd, bank, bank_lpn, &cur_need_to_emulate_tt);
		if (pact == NULL) {
			goto fail_assign_new_write_vpn;
		}

		if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;
		new_vpn = pact->vbn * sc->PAGE_NB + pact->cpoff;

		old_vpn = get_vpn(ssd, bank, bank_lpn);

		n_io_info = CREATE_NAND_IO_INFO(ssd, write_page_nb, WRITE, io_page_nb, ssd->io_request_seq_nb);

		num_flash = CALC_FLASH(ssd, new_vpn);
		num_blk = CALC_BLOCK(ssd, new_vpn);
		num_channel = num_flash % CHANNEL_NB;
		num_plane = num_flash * PLANES_PER_FLASH + num_blk % PLANES_PER_FLASH;
		if (GC_MODE == WHOLE_BLOCKING) {
			slot = 0;
		} else if (GC_MODE == CHANNEL_BLOCKING) {
			slot = num_channel;
		} else if (GC_MODE == CHIP_BLOCKING) {
			slot = num_plane;
		}

		if (gc_slot[slot] > blocking_to) {
			blocking_to = gc_slot[slot];
		}

		if (old_vpn != NULL_VPN) {
			if (left_skip || right_skip) 
				type = 0;
		}
		else
			type = 1;

		write_page_nb++;

		cur_need_to_emulate_tt = set_vpn(ssd, bank, bank_lpn, off_in_page, pact, type, old_vpn);
		if (cur_need_to_emulate_tt > max_need_to_emulate_tt) max_need_to_emulate_tt = cur_need_to_emulate_tt;

#ifdef FTL_DEBUG
		if(ret == SUCCESS){
			printf("\twrite complete [%d, %d, %d]\n",bank, CALC_BLOCK(new_vpn),CALC_PAGE(new_vpn));
		}
		else if(ret == FAIL){
			printf("ERROR[%s] %d page write fail \n",__FUNCTION__, new_vpn);
		}
#endif
		lba += write_sects;
		remain -= write_sects;
		left_skip = 0;

	}
	if (blocking_to > curtime) {
		ssd->nb_blocked_writes++;
	}

	INCREASE_IO_REQUEST_SEQ_NB(ssd);

		int64_t gc_tt = collect_garbage_blks(ssd, bank);
		if (gc_tt > 0) {
			printf("[_FTL_WRITE] collect_garbage_blks tt %lu\n", gc_tt);
			cur_need_to_emulate_tt = gc_tt;
		}
		if (cur_need_to_emulate_tt > max_need_to_emulate_tt) {
			max_need_to_emulate_tt = cur_need_to_emulate_tt;
		}
fail_assign_new_write_vpn:
#ifdef FIRM_IO_BUFFER
	INCREASE_WB_LIMIT_POINTER();
#endif

#ifdef MONITOR_ON
	char szTemp[1024];
	sprintf(szTemp, "WRITE PAGE %d ", length);
	WRITE_LOG(szTemp);
	sprintf(szTemp, "WB CORRECT %d", write_page_nb);
	WRITE_LOG(szTemp);
#endif

#ifdef FTL_DEBUG
	printf("[%s] End\n", __FUNCTION__);
#endif

	return max_need_to_emulate_tt; 
}
