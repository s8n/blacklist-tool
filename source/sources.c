// Reads the blacklist copies sdb sees: system data 0x828, the delivery cache
// through bcat:s, and the delivery cache save read directly. Also the
// installed-title info around them (NCM, pl:s, firmware version).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include "bcat.h"
#include "json.h"
#include "blacklist.h"
#include "bcatsave.h"
#include "ui.h"

// ---- state ----

Copy sysdata = {"system data"}, bcat_ipc = {"BCAT (bcat:s)"}, bcat_save = {"BCAT (save)"};
bool killed_sdb, killed_bcat;
FsSaveDataInfo bcat_save_info;
bool have_bcat_save_info;
u8 fw_major, fw_minor, fw_micro;

void copy_reset(Copy *c) {
    free(c->data);
    json_free(c->json);
    const char *l = c->label;
    memset(c, 0, sizeof *c);
    c->label = l;
}

void copy_set(Copy *c, u8 *data, u64 len) {
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

void get_fw(void) {
    SetSysFirmwareVersion fw;
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
        fw_major = fw.major; fw_minor = fw.minor; fw_micro = fw.micro;
    } else {
        u32 v = hosversionGet();
        fw_major = HOSVER_MAJOR(v); fw_minor = HOSVER_MINOR(v); fw_micro = HOSVER_MICRO(v);
    }
}

void read_sysdata(void) {
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

void read_bcat_ipc(void) {
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

// Lists saves that belong to the delivery ID and reads the first one that opens.
// The path inside is from bcat's strings (":/directories/%s/files/%s"), not traced.
void read_bcat_save(void) {
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
void ncm_info(void) {
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
void pl_version(void) {
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
