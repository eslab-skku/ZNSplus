#ifndef _IOUB_UTIL_H_
#define _IOUB_UTIL_H_

#include "vssim_config_manager.h"

#define IOUB_MAGIC				0xDEADFACE
#define HOST_UNIT				(4096)
#define DEVICE_UNIT				(512)
#define CMD_TRANSFER_UNIT		(32 * HOST_UNIT / DEVICE_UNIT)

#define IOUB_START_ADDR			(8224 * HOST_UNIT / DEVICE_UNIT)	// 30976
#define IOUB_END_ADDR			(9216 * HOST_UNIT / DEVICE_UNIT)


#define ADDR_READ_HEADER		(IOUB_START_ADDR + CMD_TRANSFER_UNIT * 0)	// 30976
#define ADDR_PROFILE_COMMAND		(IOUB_START_ADDR + CMD_TRANSFER_UNIT * 2)	// 31488
#define ADDR_TL_OPEN		(IOUB_START_ADDR + CMD_TRANSFER_UNIT * 4)	// 32000
#define ADDR_INTERNAL_COMPACTION		(IOUB_START_ADDR + CMD_TRANSFER_UNIT * 6)	// 32512
#define IOUB_MAX_IM_CNT			(512)

int64_t ioub_process_command(struct ssdstate *ssd, int64_t lba, int64_t num_sectors);
int64_t recv_profile_command(struct ssdstate *ssd);
int64_t recv_internal_merge(struct ssdstate *ssd);
int64_t recv_close_section(struct ssdstate *ssd);

typedef struct IOUBprofile {
	uint64_t NAND_read;
	uint64_t NAND_write;
	uint64_t NAND_copyback;
	uint64_t IM_count;
	uint64_t IM_read;
	uint64_t IM_write;
	uint64_t IM_copyback;
	uint64_t IZC_count;
	uint64_t IZC_total_time;
	uint64_t SSR_padding_count;
	uint64_t SSR_padding_lpn;
	uint64_t SSR_padding_cpbk;
	uint64_t SSR_back_padding;
	uint64_t SSR_pre_padding;
	uint64_t SSR_fg_padding;
	uint64_t CH_total_active;
	uint64_t CH_total_num;
} IOUBprofile;

extern struct IOUBprofile *prof_IOUB;

#endif
