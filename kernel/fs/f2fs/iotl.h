#ifndef _LINUX_F2FS_IOTL_H
#define _LINUX_F2FS_IOTL_H

#define IOTL_MAGIC      0xDEADFACE
#define IOTL_START_ADDR 8224 // excpet partition table
#define IOTL_ADDR_CNT   (16 * 1024 * 2 - IOTL_START_ADDR) // use first 16MB space

// Each command use 32 sectors of address space for necessary data transfer
#define CMD_TRANSFER_UNIT       32
#define ADDR_READ_HEADER        (IOTL_START_ADDR + CMD_TRANSFER_UNIT * 0) // 4128
#define ADDR_WRITE_HEADER       (IOTL_START_ADDR + CMD_TRANSFER_UNIT * 2) // 4192
#define ADDR_CLOSE_SECTION      (IOTL_START_ADDR + CMD_TRANSFER_UNIT * 4) // 4256
#define ADDR_INTERNAL_MERGE     (IOTL_START_ADDR + CMD_TRANSFER_UNIT * 6) // 4320
#define ADDR_FLUSH     (IOTL_START_ADDR + CMD_TRANSFER_UNIT * 8) // 4384
#define ADDR_PROF     (IOTL_START_ADDR + CMD_TRANSFER_UNIT * 2) // 4192

struct iotl_info {
    u32 magic;
    u32 count;

    // Filesystem Layout
#define HDR_FS_START_NAME fs_start_sector
    u32 fs_start_sector;
    u32 fs_sectors_per_block;
    u32 fs_blocks_per_seg;
    u32 fs_segs_per_sec;
    u32 fs_seg0_blkaddr;
    u32 fs_start_segno;
    u32 fs_num_active_section;
    u32 fs_num_meta_sects;

    // Device Part
#define HDR_DEV_START_NAME dev_sectors_per_page
    u32 dev_sectors_per_page;
    u32 dev_pages_per_block;
    u32 dev_pages_per_section;   // offset calc.
    u32 dev_num_parallel_units;
    u32 dev_num_active_blocks;

    u32 reserved[3];
};

#ifdef FTL_OPT_IOTL_SUPPORT
///////////////////// OPENSSD /////////////////////////////////////////

enum {
    CURSEG_HOT_DATA = 0,
    CURSEG_WARM_DATA,
    CURSEG_COLD_DATA,
    CURSEG_HOT_NODE,
    CURSEG_WARM_NODE,
    CURSEG_COLD_NODE,
    NR_CURSEG_TYPE,
};

#define NULL_SEGNO			((unsigned int)(~0))
#define NULL_SECNO			((unsigned int)(~0))

#define GET_SEGOFF_FROM_SEG0(ih, blk_addr) \
    ((blk_addr) - (ih)->fs_seg0_blkaddr)
#define GET_SEGNO_FROM_SEG0(ih, blk_addr) \
    (GET_SEGOFF_FROM_SEG0(ih, blk_addr) / (ih)->fs_blocks_per_seg)
#define GET_R2L_SEGNO(ih, segno) ((segno) + (ih)->fs_start_segno)
#define GET_L2R_SEGNO(ih, segno) ((segno) - (ih)->fs_start_segno)
#define START_BLOCK(ih, segno) \
    ((ih)->fs_seg0_blkaddr + \
     GET_R2L_SEGNO(ih, segno) * (ih)->fs_blocks_per_seg)
#define GET_SEGNO(ih, blk_addr) \
    GET_L2R_SEGNO(ih, GET_SEGNO_FROM_SEG0(ih, blk_addr))

int iotl_process_command(UINT32 const lba, UINT32 const num_sectors, int direction);

static inline int iotl_check_cmdaddr(UINT32 const lba)
{
    return (lba >= IOTL_START_ADDR && lba < (IOTL_START_ADDR + IOTL_ADDR_CNT));
}

/////////////////////////////////////////////////////////////////////
#else
///////////////////// F2FS /////////////////////////////////////////


#define iotl_msg(fmt, ...) printk(KERN_INFO "%s: "fmt, "F2FS-iotl", ##__VA_ARGS__)
#define iotl_err(fmt, ...) printk(KERN_EMERG "%s: "fmt, "F2FS-iotl", ##__VA_ARGS__)

#if 0
#define iotl_dbg(fmt, ...) printk(KERN_INFO "%s: "fmt, "F2FS-iotl", ##__VA_ARGS__)
#else
#define iotl_dbg(fmt, ...)
#endif

/////////////////////////////////////////////////////////////////////
#endif
#endif /* _LINUX_F2FS_IOTL_H */
