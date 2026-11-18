// blacklist-tool: shows both copies of the pl:s function blacklist that sdb
// reads, and which one is in effect.
//
// sdb reads blacklist.json from two places and uses the one whose top-level
// "version" is higher (BCAT only wins if it's strictly higher):
//   1. System data <prefix>0828 (FunctionBlackList), installed content.
//      Changes only with a system update, rebootless ones included.
//   2. BCAT delivery cache <prefix>102E, directory "blacklist", file
//      "blacklist.json". bcat downloads it with no system update involved.
// <prefix> is 01000000000 on Switch 1 and 04000000000 on Switch 2.
//
// The BCAT copy is read two ways: through bcat:s (what sdb does), and straight
// from its save. If something holds a copy locked, the read fails; reboot and
// retry.
//
// Write Empty (or a payload from SD) overwrites the BCAT copy in the save with
// an empty list whose version is one above both copies, so sdb picks it over
// either. The whole save is backed up to SD first. If the save has no
// blacklist.json, it creates it and lists it in bcat's meta files. Restore
// Original puts the original back, or removes what Write Empty added.
//
// From bcat and sdb in 23.5.0:
// - /directories/<dir>/files.meta is a u32 version (1), then up to 100
//   0x80-byte entries: name[0x20], u64 data_id @0x20, u64 size @0x28,
//   digest[0x10] @0x30, the rest zero. bcat copies all of it from the server's
//   list when it downloads, and never hashes the file itself.
// - /directories.meta is a u32 version (1), then up to 100 0x40-byte
//   entries: name[0x20], then 0x20 bytes from the server's list (digest).
// - bcat's IDeliveryCacheFileService Open needs the name in files.meta (not
//   directories.meta, which only the directory listing uses), but
//   GetSize and Read use the real file. GetDigest returns the meta digest.
// - sdb's DeliveryCacheFile only does Open, GetSize, Read and Close. It never
//   asks for the digest. It rejects compatibility_version >= 2.
// So the file is written at its own size, the meta size is updated to match
// (only the directory listing reports it), and the digest is left alone.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>
#include "bcat.h"
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
#define SDB_ID      (PFX | 0x39)
#define BCAT_PROC_ID (PFX | 0x0C)

#define SD_DIR "sdmc:/switch/blacklist-tool"
#define SD_ORIGINAL SD_DIR "/bcat-original-blacklist.json"
#define SAVE_BLACKLIST "/directories/blacklist/files/blacklist.json"
#define SAVE_FILES_META "/directories/blacklist/files.meta"
#define SAVE_DIRS_META "/directories.meta"
#define SAVE_BL_DIR "/directories/blacklist"
#define META_ENTRY 0x80      // files.meta
#define DIR_META_ENTRY 0x40  // directories.meta
#define META_MAX 100
#define META_SIZE_OFF 0x28
// Written when Write Empty adds blacklist.json to a save that had none, so
// Restore Original knows to take it out again instead of restoring an original.
#define SD_CREATED SD_DIR "/bcat-created.txt"
// If present, Write Empty writes this file as is instead of an empty list.
#define SD_PAYLOAD SD_DIR "/payload.json"

#define RED    "\x1b[31;1m"
#define YELLOW "\x1b[33;1m"
#define GREEN  "\x1b[32;1m"
#define CYAN   "\x1b[36;1m"
#define RESET  "\x1b[0m"

// ---- output: every line goes to a buffer that the screen scrolls over ----

static char **lines;
static int nlines, cap_lines, scroll;
static char partial[512];

static void add_line(const char *s) {
    if (nlines == cap_lines) {
        cap_lines = cap_lines ? cap_lines * 2 : 256;
        lines = realloc(lines, sizeof *lines * cap_lines);
    }
    lines[nlines++] = strdup(s);
}

static void out(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    char *p = buf;
    for (char *nl; (nl = strchr(p, '\n')); p = nl + 1) {
        *nl = 0;
        strncat(partial, p, sizeof partial - strlen(partial) - 1);
        add_line(partial);
        partial[0] = 0;
    }
    strncat(partial, p, sizeof partial - strlen(partial) - 1);
}

static void clear_lines(void) {
    for (int i = 0; i < nlines; i++) free(lines[i]);
    nlines = 0;
    scroll = 0;
    partial[0] = 0;
}

static void err(const char *what, Result rc) {
    out("  %s: " RED "%04d-%04d" RESET " (0x%X)\n", what, 2000 + R_MODULE(rc), R_DESCRIPTION(rc), rc);
}

#define ROWS 41
#define MENU_COUNT 6

enum {
    MENU_VIEW, MENU_WRITE_EMPTY, MENU_WRITE_PAYLOAD, MENU_RESTORE, MENU_DUMP,
    MENU_SYNC
};

static const char *menu_labels[MENU_COUNT] = {
    "View", "Write Empty", "Write payload from SD", "Restore Original", "Dump",
    "Sync sysdata to BCAT"
};

enum { SCREEN_MAIN, SCREEN_VIEW, SCREEN_RESULT };
static int screen;
static int menu_sel;
static int menu_items = 4;
static int menu_map[MENU_COUNT];
static bool bcat_newer;
static bool have_payload;
// Where the main view stops and the records detail starts, then where this
// load's output ends. Anything after records_end is output from an action.
static int summary_end, records_end;
static int action_start, action_end;

static void print_range(int total) {
    if (total > ROWS)
        printf("  " CYAN "(%d-%d of %d)" RESET, scroll + 1,
               scroll + ROWS < total ? scroll + ROWS : total, total);
}

// Prints lines [first, end) from the current scroll offset.
static int print_lines(int first, int end) {
    int total = end - first;
    if (scroll > total - ROWS) scroll = total - ROWS;
    if (scroll < 0) scroll = 0;
    for (int i = first + scroll; i < end && i < first + scroll + ROWS; i++)
        printf("%s" RESET "\n", lines[i]);
    return total;
}

static void draw_main(void) {
    consoleClear();
    int total = print_lines(0, summary_end);
    printf("\x1b[43;1H");
    for (int i = 0; i < menu_items; i++) {
        int action = menu_map[i];
        if (menu_items > 4 && i == 4) printf("\x1b[44;1H");
        if (i == menu_sel) printf("\x1b[30;42m %s " RESET, menu_labels[action]);
        else printf(" %s ", menu_labels[action]);
        if (i + 1 < menu_items) printf("  ");
    }
    if (menu_items > 4) printf("\x1b[45;1H");
    else printf("\x1b[44;1H");
    printf(CYAN " < > choose   [A] select   [+] exit   up/down scroll" RESET);
    print_range(total);
    consoleUpdate(NULL);
}

static void draw_view(void) {
    consoleClear();
    int total = print_lines(summary_end, records_end);
    if (total <= 0) printf("No records were read.\n");
    printf("\x1b[43;1H" YELLOW "[B] back" RESET);
    printf("\x1b[44;1H" CYAN " up/down scroll   L/R page" RESET);
    print_range(total);
    consoleUpdate(NULL);
}

static void draw_result(void) {
    consoleClear();
    int total = print_lines(action_start, action_end);
    if (total <= 0) printf("Done.\n");
    printf("\x1b[43;1H" YELLOW "[B] back" RESET);
    printf("\x1b[44;1H" CYAN " up/down scroll   L/R page" RESET);
    print_range(total);
    consoleUpdate(NULL);
}

// ---- state ----

typedef struct {
    const char *label;
    bool have;
    u8 *data;
    u64 len;
    JVal *json;
    bool has_version;
    unsigned long long version;
} Copy;

static Copy sysdata = {"system data"}, bcat_ipc = {"BCAT (bcat:s)"}, bcat_save = {"BCAT (save)"};
static bool killed_sdb, killed_bcat;
// The delivery cache save, for the write path. Set when the save is
// found, even if it couldn't be opened or has no blacklist.json.
static FsSaveDataInfo bcat_save_info;
static bool have_bcat_save_info;
static u8 fw_major, fw_minor, fw_micro;

static void copy_reset(Copy *c) {
    free(c->data);
    json_free(c->json);
    const char *l = c->label;
    memset(c, 0, sizeof *c);
    c->label = l;
}

static void copy_set(Copy *c, u8 *data, u64 len) {
    c->have = true;
    c->data = data;
    c->len = len;
    size_t pos = 0;
    c->json = json_parse((const char *)data, len, &pos);
    if (!c->json) {
        out("  " RED "Not valid JSON" RESET " (error near byte %zu)\n", pos);
        return;
    }
    c->has_version = json_u64(json_get(c->json, "version"), &c->version);
}

// ---- sources ----

static void get_fw(void) {
    SetSysFirmwareVersion fw;
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
        fw_major = fw.major; fw_minor = fw.minor; fw_micro = fw.micro;
    } else {
        u32 v = hosversionGet();
        fw_major = HOSVER_MAJOR(v); fw_minor = HOSVER_MINOR(v); fw_micro = HOSVER_MICRO(v);
    }
}

static void read_sysdata(void) {
    out(CYAN "System data %016llX" RESET "\n", (unsigned long long)SYSDATA_ID);
    Result rc = romfsMountFromDataArchive(SYSDATA_ID, NcmStorageId_BuiltInSystem, "bl828");
    if (R_FAILED(rc)) {
        err("Can't mount it", rc);
        if (!killed_sdb) out("  If sdb holds it, reboot and retry.\n");
        return;
    }
    FILE *f = fopen("bl828:/blacklist.json", "rb");
    if (!f) {
        out("  " RED "No blacklist.json in it" RESET "\n");
    } else {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        u8 *buf = n >= 0 && n < 16 * 1024 * 1024 ? malloc(n + 1) : NULL;
        if (buf && fread(buf, 1, n, f) == (size_t)n) {
            buf[n] = 0;
            out("  blacklist.json, %ld bytes\n", n);
            copy_set(&sysdata, buf, n);
        } else {
            free(buf);
            out("  " RED "Can't read blacklist.json" RESET "\n");
        }
        fclose(f);
    }
    romfsUnmount("bl828");
}

static void read_bcat_ipc(void) {
    out(CYAN "BCAT %016llX through bcat:s" RESET "\n", (unsigned long long)BCAT_ID);
    if (killed_bcat) { out("  Skipped, bcat is killed.\n"); return; }

    BcatStorage st;
    Result rc = bcatOpenStorage(&st, BCAT_ID);
    if (R_FAILED(rc)) {
        err("Can't open the delivery cache", rc);
        if (!killed_sdb) out("  If sdb holds it open, reboot and retry.\n");
        return;
    }
    out("  Opened via %s\n", st.service_name);

    static char dirs[64][0x20];
    s32 ndirs = 0;
    rc = bcatListDirectories(&st, dirs, 64, &ndirs);
    if (R_FAILED(rc)) err("Can't list directories", rc);
    else {
        out("  Directories (%d):", ndirs);
        for (s32 i = 0; i < ndirs && i < 64; i++) out(" %.32s", dirs[i]);
        out("%s\n", ndirs ? "" : " none, so nothing delivered");
    }

    static BcatDirEntry files[64];
    s32 nfiles = 0;
    rc = bcatListFiles(&st, "blacklist", files, 64, &nfiles);
    if (R_FAILED(rc)) {
        err("No \"blacklist\" directory", rc);
    } else {
        for (s32 i = 0; i < nfiles && i < 64; i++)
            out("  blacklist/%.32s, %llu bytes\n", files[i].name, (unsigned long long)files[i].size);
        u8 *data;
        u64 len;
        rc = bcatReadFile(&st, "blacklist", "blacklist.json", &data, &len);
        if (R_FAILED(rc)) err("Can't read blacklist/blacklist.json", rc);
        else copy_set(&bcat_ipc, data, len);
    }
    bcatCloseStorage(&st);
}

static const char *space_name(u8 s) {
    switch (s) {
    case FsSaveDataSpaceId_System: return "System";
    case FsSaveDataSpaceId_User: return "User";
    case FsSaveDataSpaceId_SdSystem: return "SdSystem";
    case FsSaveDataSpaceId_SdUser: return "SdUser";
    case FsSaveDataSpaceId_ProperSystem: return "ProperSystem";
    case FsSaveDataSpaceId_SafeMode: return "SafeMode";
    default: return "?";
    }
}

static const char *type_name(u8 t) {
    static const char *n[] = {"System", "Account", "Bcat", "Device", "Temporary", "Cache", "SystemBcat"};
    return t < 7 ? n[t] : "?";
}

static Result read_save_file(FsFileSystem *fs, const char *p, u8 **data, u64 *len) {
    char path[FS_MAX_PATH] = {0};
    strncpy(path, p, sizeof path - 1);
    FsFile f;
    Result rc = fsFsOpenFile(fs, path, FsOpenMode_Read, &f);
    if (R_FAILED(rc)) return rc;
    s64 size = 0;
    rc = fsFileGetSize(&f, &size);
    u8 *buf = NULL;
    if (R_SUCCEEDED(rc) && (size < 0 || size > 16 * 1024 * 1024 || !(buf = malloc(size + 1))))
        rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    u64 got = 0;
    if (R_SUCCEEDED(rc)) rc = fsFileRead(&f, 0, buf, size, FsReadOption_None, &got);
    fsFileClose(&f);
    if (R_FAILED(rc)) { free(buf); return rc; }
    buf[got] = 0;
    *data = buf;
    *len = got;
    return 0;
}

// Lists saves that belong to the delivery ID and reads the first one that opens.
// The path inside is from bcat's strings (":/directories/%s/files/%s"), not traced.
static void read_bcat_save(void) {
    out(CYAN "BCAT %016llX save %016llX, read directly" RESET "\n", (unsigned long long)BCAT_ID,
        BCAT_SAVE_ID);
    static const FsSaveDataSpaceId spaces[] = {FsSaveDataSpaceId_User, FsSaveDataSpaceId_System, FsSaveDataSpaceId_SdUser};
    FsSaveDataInfo found[8];
    int nfound = 0;

    for (size_t s = 0; s < sizeof spaces / sizeof *spaces; s++) {
        FsSaveDataInfoReader r;
        Result rc = fsOpenSaveDataInfoReader(&r, spaces[s]);
        if (R_FAILED(rc)) continue;
        FsSaveDataInfo info;
        s64 n = 0;
        while (R_SUCCEEDED(fsSaveDataInfoReaderRead(&r, &info, 1, &n)) && n == 1) {
            if (info.system_save_data_id != BCAT_SAVE_ID && info.save_data_id != BCAT_SAVE_ID &&
                info.application_id != BCAT_ID) continue;
            out("  Found: space %s, type %s (%u), save %016llX, %llu bytes\n",
                space_name(info.save_data_space_id), type_name(info.save_data_type), info.save_data_type,
                (unsigned long long)info.save_data_id, (unsigned long long)info.size);
            if (nfound < 8) found[nfound++] = info;
        }
        fsSaveDataInfoReaderClose(&r);
    }
    have_bcat_save_info = false;
    if (!nfound) {
        out("  No save %016llX in User, System or SdUser.\n", BCAT_SAVE_ID);
        out("  bcat hasn't created the delivery cache on this console.\n");
        return;
    }
    bcat_save_info = found[0];
    have_bcat_save_info = true;

    for (int i = 0; i < nfound; i++) {
        FsSaveDataAttribute attr = {0};
        attr.application_id = found[i].application_id;
        attr.system_save_data_id = found[i].system_save_data_id;
        attr.save_data_type = found[i].save_data_type;
        attr.save_data_index = found[i].save_data_index;
        attr.uid = found[i].uid;
        FsFileSystem fs;
        Result rc = fsOpenReadOnlySaveDataFileSystem(&fs, found[i].save_data_space_id, &attr);
        // The read-only open may not take system saves. This handle is only
        // read from and never committed.
        bool sys = found[i].save_data_type == FsSaveDataType_System || found[i].save_data_type == FsSaveDataType_SystemBcat;
        if (R_FAILED(rc) && sys && !(R_MODULE(rc) == 2 && R_DESCRIPTION(rc) == 7))
            rc = fsOpenSaveDataFileSystemBySystemSaveDataId(&fs, found[i].save_data_space_id, &attr);
        if (R_FAILED(rc)) {
            err("Can't open it", rc);
            if (R_MODULE(rc) == 2 && R_DESCRIPTION(rc) == 7)
                out("  Something has it open. Reboot and retry.\n");
            continue;
        }
        FsDir d;
        char root[FS_MAX_PATH] = "/directories";
        if (R_SUCCEEDED(fsFsOpenDirectory(&fs, root, FsDirOpenMode_ReadDirs, &d))) {
            FsDirectoryEntry e;
            s64 n = 0;
            out("  /directories:");
            int count = 0;
            while (R_SUCCEEDED(fsDirRead(&d, &n, 1, &e)) && n == 1) { out(" %s", e.name); count++; }
            out("%s\n", count ? "" : " empty");
            fsDirClose(&d);
        }
        u8 *data;
        u64 len;
        rc = read_save_file(&fs, SAVE_BLACKLIST, &data, &len);
        if (R_FAILED(rc)) err("No " SAVE_BLACKLIST, rc);
        else {
            out("  blacklist.json, %llu bytes\n", (unsigned long long)len);
            copy_set(&bcat_save, data, len);
            bcat_save_info = found[i];
        }
        fsFsClose(&fs);
        if (bcat_save.have) break;
    }
}

static void print_version_u32(u32 v) {
    out("%u.%u.%u-%u (0x%08X)", (v >> 26) & 0x3F, (v >> 20) & 0x3F, (v >> 16) & 0xF, v & 0xFFFF, v);
}

// Installed version of 0x828 vs what the installed SystemUpdate lists for it.
// If the installed one is newer, it came in on its own as a Rebootless title.
static void ncm_info(void) {
    out(CYAN "Installed system titles (NCM)" RESET "\n");
    Result rc = ncmInitialize();
    if (R_FAILED(rc)) { err("ncm", rc); return; }
    NcmContentMetaDatabase db;
    rc = ncmOpenContentMetaDatabase(&db, NcmStorageId_BuiltInSystem);
    if (R_FAILED(rc)) { err("Can't open the system meta database", rc); ncmExit(); return; }

    NcmContentMetaKey bl = {0}, su = {0};
    bool have_bl = R_SUCCEEDED(rc = ncmContentMetaDatabaseGetLatestContentMetaKey(&db, &bl, SYSDATA_ID));
    if (have_bl) out("  FunctionBlackList %016llX installed: v 0x%08X\n", (unsigned long long)SYSDATA_ID, bl.version);
    else err("FunctionBlackList not found", rc);

    s32 total = 0, written = 0;
    NcmContentMetaKey keys[4];
    rc = ncmContentMetaDatabaseList(&db, &total, &written, keys, 4, NcmContentMetaType_SystemUpdate,
                                    0, 0, UINT64_MAX, NcmContentInstallType_Full);
    if (R_SUCCEEDED(rc) && written > 0) {
        su = keys[0];
        out("  SystemUpdate %016llX: ", (unsigned long long)su.id);
        print_version_u32(su.version);
        out("\n");
        static NcmContentMetaInfo infos[512];
        s32 n = 0;
        rc = ncmContentMetaDatabaseListContentMetaInfo(&db, &n, infos, 512, &su, 0);
        if (R_FAILED(rc)) err("Can't list its titles", rc);
        for (s32 i = 0; R_SUCCEEDED(rc) && i < n; i++) {
            if (infos[i].id != SYSDATA_ID && infos[i].id != REBOOTLESS_ID) continue;
            bool is_bl = infos[i].id == SYSDATA_ID;
            out("  It lists %s v 0x%08X, attributes 0x%X%s\n",
                is_bl ? "FunctionBlackList" : "RebootlessSystemUpdateVersion",
                infos[i].version, infos[i].attr, infos[i].attr & 2 ? " (Rebootless)" : "");
            if (is_bl && have_bl) {
                if (bl.version > infos[i].version)
                    out("  " YELLOW "Installed FunctionBlackList is newer than the SystemUpdate lists:"
                        RESET "\n  " YELLOW "it was updated on its own (Rebootless)." RESET "\n");
                else if (bl.version == infos[i].version)
                    out("  Installed FunctionBlackList matches the SystemUpdate.\n");
            }
        }
    } else if (R_FAILED(rc)) {
        err("Can't find the SystemUpdate meta", rc);
    }
    ncmContentMetaDatabaseClose(&db);
    ncmExit();

    SetSysRebootlessSystemUpdateVersion rv;
    rc = setsysGetRebootlessSystemUpdateVersion(&rv);
    if (R_SUCCEEDED(rc))
        out("  Rebootless version (Support page): \"%.32s\" (%u)\n", rv.display_version, rv.version);
    else err("Rebootless version", rc);
}

// pl:s 106 GetFunctionBlackListVersion: the version sdb has loaded.
static void pl_version(void) {
    out(CYAN "pl:s (sdb)" RESET "\n");
    if (killed_sdb) { out("  Skipped, sdb is killed.\n"); return; }
    Service pl;
    Result rc = smGetService(&pl, "pl:s");
    if (R_FAILED(rc)) { err("Can't get pl:s", rc); return; }
    u32 v = 0;
    rc = serviceDispatchOut(&pl, 106, v);
    if (R_FAILED(rc)) err("GetFunctionBlackListVersion", rc);
    else out("  Loaded blacklist version: " GREEN "%u" RESET "\n", v);
    serviceClose(&pl);
}

// ---- records ----

static const char *func_name(unsigned long long f) {
    static const char *n[] = {"?", "LocalNetworkMode", "SecureLdnLocalCommunication",
                              "SecureLp2pLocalCommunication", "InsecureLp2pLocalCommunication", "Launch"};
    return f < 6 ? n[f] : "?";
}

static void print_records(const Copy *c) {
    const JVal *recs = json_get(c->json, "records");
    unsigned long long cv = 0;
    json_u64(json_get(c->json, "compatibility_version"), &cv);
    out(CYAN "Records in %s" RESET " (version %llu, compatibility_version %llu): ", c->label,
        c->has_version ? c->version : 0ULL, cv);
    if (!recs || recs->type != J_ARR) { out(RED "no records array" RESET "\n"); return; }
    out("%d\n", recs->n);

    for (int i = 0; i < recs->n; i++) {
        const JVal *r = &recs->items[i];
        unsigned long long id = 0, raw = 0;
        const char *kind = "application";
        if (!json_u64(json_get(r, "application_id"), &id)) {
            kind = "program";
            json_u64(json_get(r, "program_id"), &id);
        }
        const JVal *fn = json_get(r, "function_id");
        const char *fname = fn && fn->type == J_STR ? fn->str
                          : json_u64(json_get(r, "function_id_raw"), &raw) ? func_name(raw) : "?";
        const JVal *ty = json_get(r, "type");
        const char *tname = ty && ty->type == J_STR ? ty->str : NULL;
        unsigned long long traw = 0;
        bool have_traw = json_u64(json_get(r, "type_raw"), &traw);
        if (!tname) tname = have_traw ? (traw == 1 ? "ApplicationUpdateRequired" : traw == 2 ? "Prohibited"
                                       : traw == 3 ? "SystemUpdateRequired" : "?") : "?";

        unsigned long long av = 0, ma = 0, mi = 0, mc = 0;
        bool have_av = json_u64(json_get(json_get(r, "application"), "version"), &av);
        const JVal *sys = json_get(r, "system");
        bool have_sys = sys && json_u64(json_get(sys, "major"), &ma);
        json_u64(json_get(sys, "minor"), &mi);
        json_u64(json_get(sys, "micro"), &mc);

        bool sysreq = !strcmp(tname, "SystemUpdateRequired") || (!ty && traw == 3);
        out("  %3d %s %016llX %s %s%s" RESET "\n", i, kind, id, fname,
            sysreq ? YELLOW : "", tname);
        out("      ");
        if (have_av) out("app v%llu (%llu.%llu.%llu)  ", av, av >> 16, (av >> 8) & 0xFF, av & 0xFF);
        if (have_sys) out("system %llu.%llu.%llu", ma, mi, mc);
        if (sysreq && have_sys) {
            // sdb drops a SystemUpdateRequired record only when the console is
            // above its system version (FUN_00088160, debug-record path).
            int cmp = fw_major != ma ? (fw_major < ma ? -1 : 1)
                    : fw_minor != mi ? (fw_minor < mi ? -1 : 1)
                    : fw_micro != mc ? (fw_micro < mc ? -1 : 1) : 0;
            out("%s", cmp <= 0 ? RED "  applies to this firmware" RESET : GREEN "  firmware is above it" RESET);
        }
        out("\n");
    }
}

// ---- top level ----

static void load(void) {
    copy_reset(&sysdata);
    copy_reset(&bcat_ipc);
    copy_reset(&bcat_save);
    clear_lines();

    get_fw();
    out("blacklist-tool v1.1 (" PLATFORM "), firmware %u.%u.%u%s%s\n", fw_major, fw_minor, fw_micro,
        killed_sdb ? ", " YELLOW "sdb killed" RESET : "", killed_bcat ? ", " YELLOW "bcat killed" RESET : "");
    out("\n");
    read_sysdata();
    out("\n");
    read_bcat_ipc();
    out("\n");
    read_bcat_save();
    out("\n");
    ncm_info();
    out("\n");
    pl_version();
    out("\n");

    const Copy *bc = bcat_ipc.have ? &bcat_ipc : bcat_save.have ? &bcat_save : NULL;
    out(CYAN "Summary" RESET "\n");
    out("  System data version: %s", sysdata.has_version ? "" : "-");
    if (sysdata.has_version) out("%llu", sysdata.version);
    out("\n  BCAT version:        %s", bc && bc->has_version ? "" : "-");
    if (bc && bc->has_version) out("%llu", bc->version);
    out("\n");
    if (bcat_ipc.has_version && bcat_save.has_version && bcat_ipc.version != bcat_save.version)
        out("  " YELLOW "bcat:s and the save disagree" RESET "\n");
    if (sysdata.has_version && bc && bc->has_version)
        out("  In effect: %s\n", bc->version > sysdata.version ? YELLOW "BCAT copy" RESET : "system data copy");
    else if (sysdata.has_version && (!bc || !bc->have))
        out("  In effect: system data copy (no BCAT copy read)\n");
    out("\n");

    summary_end = nlines;
    if (sysdata.json) { print_records(&sysdata); out("\n"); }
    if (bc && bc->json) { print_records(bc); out("\n"); }
    records_end = nlines;

    // The sync option only makes sense when BCAT is the newer copy.
    bcat_newer = sysdata.has_version && bc && bc->has_version && bc->version > sysdata.version;

    struct stat st;
    have_payload = stat(SD_PAYLOAD, &st) == 0;

    menu_items = 0;
    menu_map[menu_items++] = MENU_VIEW;
    menu_map[menu_items++] = MENU_WRITE_EMPTY;
    if (have_payload) menu_map[menu_items++] = MENU_WRITE_PAYLOAD;
    menu_map[menu_items++] = MENU_RESTORE;
    menu_map[menu_items++] = MENU_DUMP;
    if (bcat_newer) menu_map[menu_items++] = MENU_SYNC;
    if (menu_sel >= menu_items) menu_sel = menu_items - 1;
}

static bool write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    return (fclose(f) == 0) && ok;
}

static void save_sd(void) {
    mkdir("sdmc:/switch", 0777);
    mkdir(SD_DIR, 0777);
    char stamp[32], path[160];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);

    const Copy *copies[] = {&sysdata, &bcat_ipc, &bcat_save};
    const char *names[] = {"sysdata", "bcat", "bcat-save"};
    int saved = 0;
    for (int i = 0; i < 3; i++) {
        if (!copies[i]->have) continue;
        snprintf(path, sizeof path, SD_DIR "/%s-%s-blacklist.json", stamp, names[i]);
        saved += write_file(path, copies[i]->data, copies[i]->len);
    }

    // Report without the colour codes.
    snprintf(path, sizeof path, SD_DIR "/%s-report.txt", stamp);
    FILE *f = fopen(path, "w");
    if (f) {
        for (int i = 0; i < nlines; i++) {
            for (const char *p = lines[i]; *p; p++) {
                if (*p == '\x1b') { while (*p && *p != 'm') p++; if (!*p) break; continue; }
                fputc(*p, f);
            }
            fputc('\n', f);
        }
        fclose(f);
        saved++;
    }
    out("\n" GREEN "Saved %d file(s) to " SD_DIR "/%s-*" RESET "\n", saved, stamp);
    scroll = nlines;
}

static bool wait_for(PadState *pad, u64 *pressed, u64 mask) {
    while (appletMainLoop()) {
        padUpdate(pad);
        u64 down = padGetButtonsDown(pad);
        if (down & mask) { *pressed = down; return true; }
        consoleUpdate(NULL);
    }
    return false;
}

// ---- write/restore the BCAT copy ----

static bool confirm(PadState *pad, bool ban_warning) {
    if (ban_warning) {
        printf("\n" RED "Warning: writing over the BCAT blacklist may lead to this console" RESET "\n");
        printf(RED "being banned. We simply do not know, but you assume the risk." RESET "\n");
    }
    printf("\n[A] %s  [B] cancel\n", ban_warning ? "press again to confirm" : "go ahead");
    consoleUpdate(NULL);
    u64 down = 0;
    return wait_for(pad, &down, HidNpadButton_A | HidNpadButton_B) && (down & HidNpadButton_A);
}

static Result open_bcat_save_rw(FsFileSystem *fs) {
    FsSaveDataAttribute attr = {0};
    attr.application_id = bcat_save_info.application_id;
    attr.system_save_data_id = bcat_save_info.system_save_data_id;
    attr.save_data_type = bcat_save_info.save_data_type;
    attr.save_data_index = bcat_save_info.save_data_index;
    attr.uid = bcat_save_info.uid;
    if (bcat_save_info.save_data_type == FsSaveDataType_System || bcat_save_info.save_data_type == FsSaveDataType_SystemBcat)
        return fsOpenSaveDataFileSystemBySystemSaveDataId(fs, bcat_save_info.save_data_space_id, &attr);
    return fsOpenSaveDataFileSystem(fs, bcat_save_info.save_data_space_id, &attr);
}

// Copies every file in the save to dst on SD, listing each one. Returns the
// file count, or -1 if anything couldn't be copied.
static int backup_tree(FsFileSystem *fs, const char *src, const char *dst) {
    mkdir(dst, 0777);
    char path[FS_MAX_PATH] = {0};
    strncpy(path, src[0] ? src : "/", sizeof path - 1);
    FsDir d;
    Result rc = fsFsOpenDirectory(fs, path, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &d);
    if (R_FAILED(rc)) { err(path, rc); return -1; }
    // Read the listing first so only one directory is open at a time.
    FsDirectoryEntry *list = malloc(64 * sizeof *list);
    int count = 0;
    s64 n = 0;
    while (list && count < 64 && R_SUCCEEDED(fsDirRead(&d, &n, 1, &list[count])) && n == 1) count++;
    fsDirClose(&d);
    if (!list) return -1;

    int total = 0;
    for (int i = 0; i < count && total >= 0; i++) {
        char s[FS_MAX_PATH], t[FS_MAX_PATH + 64];
        snprintf(s, sizeof s, "%s/%s", src, list[i].name);
        snprintf(t, sizeof t, "%s/%s", dst, list[i].name);
        if (list[i].type == FsDirEntryType_Dir) {
            int sub = backup_tree(fs, s, t);
            total = sub < 0 ? -1 : total + sub;
            continue;
        }
        u8 *data;
        u64 len;
        rc = read_save_file(fs, s, &data, &len);
        if (R_FAILED(rc)) { err(s, rc); total = -1; break; }
        bool ok = write_file(t, data, len);
        free(data);
        if (!ok) { out("  " RED "Can't write %s" RESET "\n", t); total = -1; break; }
        out("    %s, %llu bytes\n", s, (unsigned long long)len);
        total++;
    }
    free(list);
    return total;
}

static bool fs_not_found(Result rc) { return R_MODULE(rc) == 2 && R_DESCRIPTION(rc) == 1; }
static bool fs_exists(Result rc) { return R_MODULE(rc) == 2 && R_DESCRIPTION(rc) == 2; }

static Result make_dir(FsFileSystem *fs, const char *p) {
    char path[FS_MAX_PATH] = {0};
    strncpy(path, p, sizeof path - 1);
    Result rc = fsFsCreateDirectory(fs, path);
    return fs_exists(rc) ? 0 : rc;
}

// Replaces a file's contents, creating it if it's missing.
static Result write_whole_file(FsFileSystem *fs, const char *p, const void *data, u64 len) {
    FsFile f;
    char path[FS_MAX_PATH] = {0};
    strncpy(path, p, sizeof path - 1);
    Result rc = fsFsOpenFile(fs, path, FsOpenMode_Write | FsOpenMode_Read, &f);
    if (fs_not_found(rc)) {
        rc = fsFsCreateFile(fs, path, 0, 0);
        if (R_SUCCEEDED(rc)) rc = fsFsOpenFile(fs, path, FsOpenMode_Write | FsOpenMode_Read, &f);
    }
    if (R_FAILED(rc)) return rc;
    s64 size = 0;
    rc = fsFileGetSize(&f, &size);
    if (R_SUCCEEDED(rc) && size != (s64)len) rc = fsFileSetSize(&f, len);
    if (R_SUCCEEDED(rc) && len) rc = fsFileWrite(&f, 0, data, len, FsWriteOption_Flush);
    fsFileClose(&f);
    return rc;
}

static bool file_equals(FsFileSystem *fs, const char *p, const void *data, u64 len) {
    u8 *back;
    u64 back_len;
    if (R_FAILED(read_save_file(fs, p, &back, &back_len))) return false;
    bool same = back_len == len && !memcmp(back, data, len);
    free(back);
    return same;
}

// A bcat meta file: u32 version 1, then fixed-size entries starting with a
// 0x20-byte name. files.meta entries are 0x80 bytes, directories.meta 0x40.
typedef struct {
    const char *path;
    u32 esize;
    u8 *buf;
    u64 len;
    bool changed;
} Meta;

static Result meta_load(FsFileSystem *fs, Meta *m) {
    Result rc = read_save_file(fs, m->path, &m->buf, &m->len);
    if (fs_not_found(rc)) {
        // A missing meta is an empty list; bcat treats it the same way.
        m->buf = calloc(1, 4);
        if (!m->buf) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        m->buf[0] = 1;
        m->len = 4;
        m->changed = true;
        return 0;
    }
    if (R_FAILED(rc)) { err(m->path, rc); return rc; }
    u32 ver = 0;
    if (m->len >= 4) memcpy(&ver, m->buf, 4);
    if (m->len < 4 || ver != 1 || (m->len - 4) % m->esize || (m->len - 4) / m->esize > META_MAX) {
        out("  " RED "%s is %llu bytes, version %u: not the expected format." RESET "\n",
            m->path, (unsigned long long)m->len, ver);
        free(m->buf);
        m->buf = NULL;
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    return 0;
}

static int meta_count(const Meta *m) { return (m->len - 4) / m->esize; }

static u8 *meta_find(Meta *m, const char *name) {
    for (u64 off = 4; off < m->len; off += m->esize)
        if (!strncasecmp((const char *)m->buf + off, name, 0x20)) return m->buf + off;
    return NULL;
}

// Appends a zeroed entry with this name. NULL if the list is full.
static u8 *meta_add(Meta *m, const char *name) {
    if (meta_count(m) >= META_MAX) return NULL;
    u8 *b = realloc(m->buf, m->len + m->esize);
    if (!b) return NULL;
    m->buf = b;
    u8 *e = b + m->len;
    memset(e, 0, m->esize);
    strncpy((char *)e, name, 0x1f);
    m->len += m->esize;
    m->changed = true;
    return e;
}

static void meta_remove(Meta *m, const char *name) {
    u8 *e = meta_find(m, name);
    if (!e) return;
    memmove(e, e + m->esize, m->buf + m->len - (e + m->esize));
    m->len -= m->esize;
    m->changed = true;
}

static Result commit_and_check(FsFileSystem *fs, Meta *dirs, Meta *files, const u8 *data, u64 len) {
    Result rc = fsFsCommit(fs);
    if (R_FAILED(rc)) return rc;
    bool ok = (!dirs->changed || file_equals(fs, dirs->path, dirs->buf, dirs->len)) &&
              (!files->changed || file_equals(fs, files->path, files->buf, files->len)) &&
              (!data || file_equals(fs, SAVE_BLACKLIST, data, len));
    return ok ? 0 : MAKERESULT(Module_Libnx, LibnxError_IoError);
}

// Writes blacklist.json and makes sure bcat's meta lists it, in one commit.
// bcat:s only serves a file that files.meta lists. An existing entry keeps
// its data_id and digest and only gets the new size; nothing that reads this
// file checks the digest. A missing entry is added with both zero, and the
// directory is added to directories.meta the same way.
static Result write_save_blacklist(FsFileSystem *fs, const u8 *data, u64 len) {
    Meta dirs = {SAVE_DIRS_META, DIR_META_ENTRY}, files = {SAVE_FILES_META, META_ENTRY};
    Result rc = meta_load(fs, &dirs);
    if (R_SUCCEEDED(rc)) rc = meta_load(fs, &files);
    if (R_SUCCEEDED(rc) && !meta_find(&dirs, "blacklist")) {
        out("  Adding \"blacklist\" to directories.meta\n");
        if (!meta_add(&dirs, "blacklist")) rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }
    u8 *e = NULL;
    if (R_SUCCEEDED(rc) && !(e = meta_find(&files, "blacklist.json"))) {
        out("  Adding blacklist.json to files.meta\n");
        if (!(e = meta_add(&files, "blacklist.json"))) rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }
    if (R_SUCCEEDED(rc)) {
        u64 old_size, data_id;
        memcpy(&old_size, e + META_SIZE_OFF, 8);
        memcpy(&data_id, e + 0x20, 8);
        out("  files.meta entry: data_id %016llX, size %llu -> %llu\n",
            (unsigned long long)data_id, (unsigned long long)old_size, (unsigned long long)len);
        if (old_size != len) {
            memcpy(e + META_SIZE_OFF, &len, 8);
            files.changed = true;
        }
    }
    if (R_SUCCEEDED(rc)) rc = make_dir(fs, "/directories");
    if (R_SUCCEEDED(rc)) rc = make_dir(fs, SAVE_BL_DIR);
    if (R_SUCCEEDED(rc)) rc = make_dir(fs, SAVE_BL_DIR "/files");
    if (R_SUCCEEDED(rc)) rc = write_whole_file(fs, SAVE_BLACKLIST, data, len);
    if (R_SUCCEEDED(rc) && files.changed) rc = write_whole_file(fs, files.path, files.buf, files.len);
    if (R_SUCCEEDED(rc) && dirs.changed) rc = write_whole_file(fs, dirs.path, dirs.buf, dirs.len);
    if (R_SUCCEEDED(rc)) rc = commit_and_check(fs, &dirs, &files, data, len);
    free(dirs.buf);
    free(files.buf);
    return rc;
}

// Undoes write_save_blacklist on a save that had no blacklist.json: deletes
// the file and its files.meta entry, and the directory too if that leaves
// it empty.
static Result remove_save_blacklist(FsFileSystem *fs) {
    Meta dirs = {SAVE_DIRS_META, DIR_META_ENTRY}, files = {SAVE_FILES_META, META_ENTRY};
    Result rc = meta_load(fs, &dirs);
    if (R_SUCCEEDED(rc)) rc = meta_load(fs, &files);
    if (R_SUCCEEDED(rc)) {
        files.changed = false;
        dirs.changed = false;
        meta_remove(&files, "blacklist.json");
        char path[FS_MAX_PATH] = SAVE_BLACKLIST;
        rc = fsFsDeleteFile(fs, path);
        if (fs_not_found(rc)) rc = 0;
    }
    if (R_SUCCEEDED(rc) && meta_count(&files) == 0) {
        out("  Removing the now empty blacklist directory\n");
        char path[FS_MAX_PATH] = SAVE_BL_DIR;
        rc = fsFsDeleteDirectoryRecursively(fs, path);
        if (fs_not_found(rc)) rc = 0;
        files.changed = false; // deleted along with the directory
        meta_remove(&dirs, "blacklist");
    } else if (R_SUCCEEDED(rc) && files.changed) {
        rc = write_whole_file(fs, files.path, files.buf, files.len);
    }
    if (R_SUCCEEDED(rc) && dirs.changed) rc = write_whole_file(fs, dirs.path, dirs.buf, dirs.len);
    if (R_SUCCEEDED(rc)) rc = commit_and_check(fs, &dirs, &files, NULL, 0);
    free(dirs.buf);
    free(files.buf);
    return rc;
}

static Result open_save_for_writing(FsFileSystem *fs) {
    if (!have_bcat_save_info) {
        out("  " RED "Save %016llX (delivery cache for %016llX) wasn't found," RESET "\n",
            BCAT_SAVE_ID, (unsigned long long)BCAT_ID);
        out("  " RED "nothing to write to." RESET "\n");
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }
    Result rc = open_bcat_save_rw(fs);
    if (R_FAILED(rc)) {
        err("Can't open the save for writing", rc);
        if (!killed_bcat) out("  Reboot and try again.\n");
    }
    return rc;
}

// Copies the save to SD and keeps the current blacklist for Restore Original.
// Returns false when anything failed and nothing should be written.
static bool backup_bcat_before_write(FsFileSystem *fs, bool existed, const u8 *cur, u64 cur_len) {
    mkdir("sdmc:/switch", 0777);
    mkdir(SD_DIR, 0777);
    char stamp[32], dir[128];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    snprintf(dir, sizeof dir, SD_DIR "/%s-bcat-save", stamp);
    out("  Backing up the save to %s:\n", dir);
    int n = backup_tree(fs, "", dir);
    if (n < 0) {
        out("  " RED "Backup failed, not writing anything." RESET "\n");
        return false;
    }
    out("  %d file(s) backed up.\n", n);

    // What Restore Original goes back to: the first original kept, or a note
    // that there was no file.
    struct stat st;
    if (existed) {
        if (stat(SD_ORIGINAL, &st) != 0) {
            if (!write_file(SD_ORIGINAL, cur, cur_len)) {
                out("  " RED "Can't write " SD_ORIGINAL ", not writing anything." RESET "\n");
                return false;
            }
            out("  Original kept at " SD_ORIGINAL "\n");
        } else {
            out("  " SD_ORIGINAL " already exists, left as is.\n");
        }
    } else {
        static const char note[] = "blacklist-tool added blacklist.json to a BCAT save that had none.\n"
                                   "Restore Original removes it again.\n";
        if (!write_file(SD_CREATED, note, sizeof note - 1)) {
            out("  " RED "Can't write " SD_CREATED ", not writing anything." RESET "\n");
            return false;
        }
    }
    return true;
}

static void write_bcat(PadState *pad, bool use_payload) {
    out("\n" CYAN "%s" RESET "\n", use_payload ? "Write payload from SD" : "Write empty BCAT list");
    if (!sysdata.has_version) {
        out("  " RED "System data version unknown, can't pick a version above it." RESET "\n");
        return;
    }
    FsFileSystem fs;
    if (R_FAILED(open_save_for_writing(&fs))) return;

    // Read what's there now through this handle: the earlier read-only open
    // may have failed while bcat held the save.
    u8 *cur = NULL;
    u64 cur_len = 0;
    Result rc = read_save_file(&fs, SAVE_BLACKLIST, &cur, &cur_len);
    bool existed = R_SUCCEEDED(rc);
    if (!existed && !fs_not_found(rc)) {
        err("Can't read " SAVE_BLACKLIST, rc);
        fsFsClose(&fs);
        return;
    }
    size_t pos;
    JVal *cur_json = existed ? json_parse((const char *)cur, cur_len, &pos) : NULL;
    unsigned long long cur_v = 0;
    bool have_cur_v = cur_json && json_u64(json_get(cur_json, "version"), &cur_v);
    const JVal *cur_recs = json_get(cur_json, "records");

    // The version to beat: sdb only takes the BCAT copy if it's higher.
    unsigned long long top = sysdata.version;
    if (have_cur_v && cur_v > top) top = cur_v;
    if (bcat_ipc.has_version && bcat_ipc.version > top) top = bcat_ipc.version;
    unsigned long long v = top + 1, cv = 1; // sdb only accepts compatibility_version 0 or 1
    char *body = NULL;
    u64 len = 0;
    JVal *payload = NULL;
    int nrecs = 0;

    FILE *pf = NULL;
    if (use_payload) {
        pf = fopen(SD_PAYLOAD, "rb");
        if (!pf) {
            out("  " RED "Can't open " SD_PAYLOAD "." RESET "\n");
            goto done;
        }
    }
    if (pf) {
        fseek(pf, 0, SEEK_END);
        long n = ftell(pf);
        fseek(pf, 0, SEEK_SET);
        body = n > 0 && n < 1024 * 1024 ? malloc(n + 1) : NULL;
        if (body && fread(body, 1, n, pf) == (size_t)n) { body[n] = 0; len = n; }
        fclose(pf);
        if (!len) {
            out("  " RED "Can't read " SD_PAYLOAD RESET "\n");
            goto done;
        }
        payload = json_parse(body, len, &pos);
        const JVal *recs = json_get(payload, "records");
        bool have_v = json_u64(json_get(payload, "version"), &v);
        bool have_cv = json_u64(json_get(payload, "compatibility_version"), &cv);
        if (!payload) {
            out("  " RED SD_PAYLOAD " isn't valid JSON (error near byte %zu)." RESET "\n", pos);
            goto done;
        }
        if (!recs || recs->type != J_ARR || !have_v || !have_cv) {
            out("  " RED SD_PAYLOAD " needs compatibility_version, version and a records array." RESET "\n");
            goto done;
        }
        if (cv > 1) {
            out("  " RED "compatibility_version is %llu; sdb rejects anything above 1." RESET "\n", cv);
            goto done;
        }
        if (v <= top) {
            out("  " RED "payload.json has version %llu, but sdb would only use it above %llu." RESET "\n", v, top);
            out("  Set \"version\" to %llu or more.\n", top + 1);
            goto done;
        }
        nrecs = recs->n;
    } else {
        body = malloc(256);
        if (!body) goto done;
        len = snprintf(body, 256,
            "{\n    \"compatibility_version\": %llu,\n    \"version\": %llu,\n    \"records\": []\n}\n", cv, v);
    }

    consoleClear();
    printf(YELLOW "%s the BCAT blacklist?" RESET "\n\n", existed ? "Overwrite" : "Add");
    printf("Save %016llX, %s\n", (unsigned long long)bcat_save_info.save_data_id, SAVE_BLACKLIST);
    if (existed)
        printf("Now: version %s%llu, %d record(s), %llu bytes\n", have_cur_v ? "" : "? ", cur_v,
               cur_recs && cur_recs->type == J_ARR ? cur_recs->n : 0, (unsigned long long)cur_len);
    else
        printf("Now: no blacklist.json. It's created, and listed in files.meta\n"
               "     and directories.meta (data_id and digest zero).\n");
    printf("New: %s, version %llu, compatibility_version %llu,\n     %d record(s), %llu bytes.\n",
           payload ? SD_PAYLOAD : "empty list", v, cv, nrecs, (unsigned long long)len);
    if (payload) {
        const JVal *recs = json_get(payload, "records");
        for (int i = 0; i < recs->n && i < 5; i++) {
            const JVal *id = json_get(&recs->items[i], "application_id");
            if (!id) id = json_get(&recs->items[i], "program_id");
            const JVal *fn = json_get(&recs->items[i], "function_id"), *ty = json_get(&recs->items[i], "type");
            printf("     %s %s %s\n", id && id->type == J_STR ? id->str : "?",
                   fn && fn->type == J_STR ? fn->str : "?", ty && ty->type == J_STR ? ty->str : "?");
        }
        if (recs->n > 5) printf("     ...\n");
    }
    printf("\n");
    if (!existed) {
        printf("bcat may remove or replace it the next time it syncs with Nintendo,\n");
        printf("since the server's list doesn't match it (not verified).\n\n");
    }
    printf("The whole save will be copied to " SD_DIR "/ first.\n");
    if (!confirm(pad, true)) {
        out("  Cancelled.\n");
        goto done;
    }

    if (!backup_bcat_before_write(&fs, existed, cur, cur_len)) goto done;

    rc = write_save_blacklist(&fs, (const u8 *)body, len);
    if (R_FAILED(rc)) { err("Write failed", rc); goto done; }
    out("  " GREEN "Wrote version %llu and read it back." RESET "\n", v);
    out("  Reboot, then run this again: pl:s should report version %llu\n", v);

done:
    json_free(payload);
    free(body);
    json_free(cur_json);
    free(cur);
    fsFsClose(&fs);
}

static void sync_sysdata_bcat(PadState *pad) {
    out("\n" CYAN "Sync system data to BCAT" RESET "\n");
    const Copy *bc = bcat_ipc.have ? &bcat_ipc : bcat_save.have ? &bcat_save : NULL;
    if (!sysdata.have || !sysdata.json || !sysdata.has_version) {
        out("  " RED "The system data copy has no readable version." RESET "\n");
        return;
    }
    if (!bc || !bc->has_version || bc->version <= sysdata.version) {
        out("  " RED "BCAT isn't a higher version than system data." RESET "\n");
        return;
    }

    FsFileSystem fs;
    if (R_FAILED(open_save_for_writing(&fs))) return;

    // Read what's there now through this handle, as write_bcat does.
    u8 *cur = NULL;
    u64 cur_len = 0;
    Result rc = read_save_file(&fs, SAVE_BLACKLIST, &cur, &cur_len);
    bool existed = R_SUCCEEDED(rc);
    if (!existed && !fs_not_found(rc)) {
        err("Can't read " SAVE_BLACKLIST, rc);
        fsFsClose(&fs);
        return;
    }

    consoleClear();
    printf(YELLOW "Sync system data to the BCAT blacklist?" RESET "\n\n");
    printf("System data: version %llu, %llu bytes\n",
           sysdata.version, (unsigned long long)sysdata.len);
    printf("BCAT now:    version %llu\n", bc->version);
    printf("The BCAT copy will get the system data content and version.\n");
    printf("After the reboot, sdb will use the system data copy.\n\n");
    printf("%s\n\n", existed ? "The current BCAT copy and the whole save are kept on SD."
                             : "The whole save is kept on SD. BCAT had no blacklist.json.");
    printf("The whole save will be copied to " SD_DIR "/ first.\n");
    if (!confirm(pad, true)) {
        out("  Cancelled.\n");
        goto done;
    }

    if (!backup_bcat_before_write(&fs, existed, cur, cur_len)) goto done;

    rc = write_save_blacklist(&fs, sysdata.data, sysdata.len);
    if (R_FAILED(rc)) { err("Write failed", rc); goto done; }
    out("  " GREEN "Wrote the system data copy to BCAT and read it back." RESET "\n");
    out("  Reboot, then run this again: both copies should report version %llu\n", sysdata.version);

done:
    free(cur);
    fsFsClose(&fs);
}

static void restore_bcat(PadState *pad) {
    out("\n" CYAN "Restore the BCAT list" RESET "\n");
    struct stat st;
    bool created = stat(SD_CREATED, &st) == 0;
    u8 *data = NULL;
    long n = -1;
    if (!created) {
        FILE *f = fopen(SD_ORIGINAL, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            n = ftell(f);
            fseek(f, 0, SEEK_SET);
            data = n > 0 && n < 16 * 1024 * 1024 ? malloc(n) : NULL;
            if (data && fread(data, 1, n, f) != (size_t)n) { free(data); data = NULL; }
            fclose(f);
        }
        if (!data) {
            out("  " RED "Nothing to restore: no " SD_ORIGINAL RESET "\n");
            out("  " RED "and no " SD_CREATED RESET "\n");
            return;
        }
    }

    FsFileSystem fs;
    if (R_FAILED(open_save_for_writing(&fs))) { free(data); return; }

    consoleClear();
    printf(YELLOW "Restore the BCAT blacklist?" RESET "\n\n");
    printf("Save %016llX\n\n", (unsigned long long)bcat_save_info.save_data_id);
    if (created) {
        printf("Write Empty added blacklist.json to this save. This deletes it and its\n");
        printf("files.meta entry, and the blacklist directory if nothing else is in it.\n");
    } else {
        printf("Writes " SD_ORIGINAL " (%ld bytes)\n", n);
        printf("to %s and sets its size in files.meta to match.\n", SAVE_BLACKLIST);
    }
    if (!confirm(pad, false)) {
        out("  Cancelled.\n");
    } else {
        Result rc = created ? remove_save_blacklist(&fs) : write_save_blacklist(&fs, data, n);
        if (R_FAILED(rc)) {
            err("Restore failed", rc);
        } else {
            if (created) remove(SD_CREATED);
            out("  " GREEN "Restored. Reboot to have sdb load it." RESET "\n");
        }
    }
    fsFsClose(&fs);
    free(data);
}

int main(int argc, char **argv) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    setsysInitialize();

    load();
    screen = SCREEN_MAIN;
    scroll = 0;
    draw_main();

    u64 held_since = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        u64 held = padGetButtons(&pad);
        bool redraw = false;

        if (down & HidNpadButton_Plus) break;

        if (screen == SCREEN_MAIN) {
            if (down & HidNpadButton_Left) {
                menu_sel = (menu_sel + menu_items - 1) % menu_items;
                redraw = true;
            }
            if (down & HidNpadButton_Right) {
                menu_sel = (menu_sel + 1) % menu_items;
                redraw = true;
            }
            if (down & HidNpadButton_A) {
                scroll = 0;
                switch (menu_map[menu_sel]) {
                case MENU_VIEW:
                    screen = SCREEN_VIEW;
                    break;
                case MENU_WRITE_EMPTY:
                    action_start = nlines;
                    write_bcat(&pad, false);
                    action_end = nlines;
                    screen = SCREEN_RESULT;
                    break;
                case MENU_WRITE_PAYLOAD:
                    action_start = nlines;
                    write_bcat(&pad, true);
                    action_end = nlines;
                    screen = SCREEN_RESULT;
                    break;
                case MENU_RESTORE:
                    action_start = nlines;
                    restore_bcat(&pad);
                    action_end = nlines;
                    screen = SCREEN_RESULT;
                    break;
                case MENU_DUMP:
                    action_start = nlines;
                    save_sd();
                    action_end = nlines;
                    screen = SCREEN_RESULT;
                    break;
                case MENU_SYNC:
                    action_start = nlines;
                    sync_sysdata_bcat(&pad);
                    action_end = nlines;
                    screen = SCREEN_RESULT;
                    break;
                }
                redraw = true;
            }
        } else if (down & HidNpadButton_B) {
            // Coming back from an action, reread so the summary and View show
            // what's on the console now.
            if (screen == SCREEN_RESULT) load();
            screen = SCREEN_MAIN;
            scroll = 0;
            redraw = true;
        }

        // Up/down scroll a line (repeating when held), L/R a page.
        u64 now = armGetSystemTick();
        u64 dir = held & (HidNpadButton_AnyUp | HidNpadButton_AnyDown);
        if (down & (HidNpadButton_AnyUp | HidNpadButton_AnyDown)) held_since = now;
        bool step = (down & dir) || (dir && armTicksToNs(now - held_since) > 300000000ULL);
        if (step) { scroll += (dir & HidNpadButton_AnyUp) ? -1 : 1; redraw = true; }
        if (down & HidNpadButton_L) { scroll -= ROWS; redraw = true; }
        if (down & HidNpadButton_R) { scroll += ROWS; redraw = true; }

        if (redraw) {
            if (screen == SCREEN_VIEW) draw_view();
            else if (screen == SCREEN_RESULT) draw_result();
            else draw_main();
        } else {
            consoleUpdate(NULL);
        }
    }

    setsysExit();
    consoleExit(NULL);
    return 0;
}
