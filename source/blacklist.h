// Shared IDs, paths, the blacklist copies sdb reads, and the functions that
// fill and change them. sources.c reads the copies, actions.c writes them.
#pragma once
#include <switch.h>
#include "json.h"

#ifdef __SWITCH2__
#define PFX 0x0400000000000000ULL
#define PLATFORM "Switch 2"
#else
#define PFX 0x0100000000000000ULL
#define PLATFORM "Switch 1"
#endif

#define SYSDATA_ID  (PFX | 0x828)   // FunctionBlackList
#define REBOOTLESS_ID (PFX | 0x826) // RebootlessSystemUpdateVersion
#define BCAT_ID     (PFX | 0x102E)  // delivery ID sdb opens
// bcat keeps that delivery's cache in this system save. From the table in
// bcat (23.5.0, 0x226460: delivery ID, save ID, sizes, flags 3). System save
// IDs aren't platform-prefixed; switchbrew lists the same ID on Switch 2.
#define BCAT_SAVE_ID 0x8000000000010005ULL
// sdb and bcat themselves, referenced in messages but never opened here.
#define SDB_ID      (PFX | 0x39)
#define BCAT_PROC_ID (PFX | 0x0C)

#define SD_DIR "sdmc:/switch/blacklist-tool"
#define SD_ORIGINAL SD_DIR "/bcat-original-blacklist.json"
#define SAVE_BLACKLIST "/directories/blacklist/files/blacklist.json"
#define SAVE_FILES_META "/directories/blacklist/files.meta"
#define SAVE_DIRS_META "/directories.meta"
#define SAVE_BL_DIR "/directories/blacklist"
// Written when Write Empty adds blacklist.json to a save that had none, so
// Restore Original knows to take it out again instead of restoring an original.
#define SD_CREATED SD_DIR "/bcat-created.txt"
// If present, Write Empty writes this file as is instead of an empty list.
#define SD_PAYLOAD SD_DIR "/payload.json"

// ---- state (sources.c) ----

// One blacklist copy: the bytes read and what json_parse made of them.
typedef struct {
    const char *label;
    bool have;
    u8 *data;
    u64 len;
    JVal *json;
    bool has_version;
    unsigned long long version;
} Copy;

extern Copy sysdata, bcat_ipc, bcat_save;
// Never set so far; message wording checks them.
extern bool killed_sdb, killed_bcat;
// The delivery cache save, for the write path. Set when the save is
// found, even if it couldn't be opened or has no blacklist.json.
extern FsSaveDataInfo bcat_save_info;
extern bool have_bcat_save_info;
extern u8 fw_major, fw_minor, fw_micro;

void copy_reset(Copy *c);                 // frees everything, keeps the label
void copy_set(Copy *c, u8 *data, u64 len); // takes ownership of data

// ---- sources.c: fill the copies and log what was found ----

void get_fw(void);
void read_sysdata(void);
void read_bcat_ipc(void);
void read_bcat_save(void);
void ncm_info(void);
void pl_version(void);

// ---- actions.c: the menu actions, each appending to the log ----

void save_sd(void);
void write_bcat(PadState *pad, bool use_payload);
void sync_sysdata_bcat(PadState *pad);
void restore_bcat(PadState *pad);
