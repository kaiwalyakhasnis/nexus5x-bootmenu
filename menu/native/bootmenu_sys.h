// The system side of the boot menu, for its Kotlin UI (through cinterop):
// the partitions, the list of images, writing the recovery slot, rebooting.
//
// What it writes: only the partition whose GPT name is exactly "recovery",
// and only a file that is a boot image and fits. /cache is mounted read-only
// without journal replay; nothing else is opened for writing.
#ifndef BOOTMENU_SYS_H
#define BOOTMENU_SYS_H

#ifdef __cplusplus
extern "C" {
#endif

enum { BM_IMAGE = 0, BM_KEEP = 1, BM_BOOTLOADER = 2, BM_POWEROFF = 3 };

typedef struct {
  int kind;
  int current;        // already in the recovery slot
  long long size;
  char label[64];
  char error[48];     // why it can't be booted; empty if it can
} bm_entry;

// Finds the recovery and cache partitions (waiting up to 10 s for the eMMC),
// mounts /cache read-only. Then bm_scan() lists the entries.
void bm_init(void);
int bm_scan(void);                 // number of entries
const bm_entry* bm_get(int i);
int bm_default(void);              // the entry in the recovery slot, -1 if none
int bm_timeout(void);              // auto-boot seconds (bootmenu.timeout=), 0: wait for a choice
int bm_charger(void);              // powered on by a charger (androidboot.mode=charger)
int bm_battery(void);              // percent, -1 if unknown

// Writing an image into the recovery slot runs on its own thread; poll it.
enum { BM_IDLE = 0, BM_WRITING = 1, BM_CHECKING = 2, BM_DONE = 3, BM_FAILED = 4 };
int bm_flash_start(int i);         // 0 if started
int bm_flash_state(void);
int bm_flash_percent(void);
const char* bm_flash_error(void);

// Boots entry i: the recovery slot (only if i is in it), the bootloader, or
// power off. Returns only on failure.
int bm_boot(int i);

void bm_log(const char* msg);      // to the kernel log, as "bootmenu: <msg>"

#ifdef __cplusplus
}
#endif

#endif
