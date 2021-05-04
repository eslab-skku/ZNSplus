#ifndef __RAS_H__
#define __RAS_H__

#include <asm/errno.h>
#include <linux/uuid.h>
#include <linux/cper.h>

#ifdef CONFIG_DEBUG_FS
int ras_userspace_consumers(void);
void ras_debugfs_init(void);
int ras_add_daemon_trace(void);
#else
static inline int ras_userspace_consumers(void) { return 0; }
static inline void ras_debugfs_init(void) { return; }
static inline int ras_add_daemon_trace(void) { return 0; }
#endif

#ifdef CONFIG_RAS
void log_non_standard_event(const uuid_le *sec_type,
			    const uuid_le *fru_id, const char *fru_text,
			    const u8 sev, const u8 *err, const u32 len);
void log_arm_hw_error(struct cper_sec_proc_arm *err);
#else
static inline void
log_non_standard_event(const uuid_le *sec_type,
				   const uuid_le *fru_id, const char *fru_text,
				   const u8 sev, const u8 *err,
				   const u32 len) { return; }
static inline void
log_arm_hw_error(struct cper_sec_proc_arm *err) { return; }
#endif

#endif /* __RAS_H__ */
