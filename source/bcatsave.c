#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <switch.h>
#include "blacklist.h"
#include "ui.h"
#include "bcatsave.h"

Result read_save_file(FsFileSystem *fs, const char *p, u8 **data, u64 *len) {
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

bool write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    return (fclose(f) == 0) && ok;
}

bool fs_not_found(Result rc) { return R_MODULE(rc) == 2 && R_DESCRIPTION(rc) == 1; }
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

Result open_bcat_save_rw(FsFileSystem *fs) {
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
int backup_tree(FsFileSystem *fs, const char *src, const char *dst) {
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

// ---- bcat meta files ----

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

Result write_save_blacklist(FsFileSystem *fs, const u8 *data, u64 len) {
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

Result remove_save_blacklist(FsFileSystem *fs) {
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
