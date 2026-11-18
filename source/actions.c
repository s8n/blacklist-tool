// The menu actions: dump the copies to SD, write the BCAT one (empty list or
// payload from SD), sync the system data copy into it, restore the original.
// Each one appends to the log; confirmation screens print straight to the
// console and never enter it.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>
#include "json.h"
#include "blacklist.h"
#include "bcatsave.h"
#include "ui.h"

void save_sd(void) {
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
        for (int i = 0; i < ui_line_count(); i++) {
            for (const char *p = ui_line(i); *p; p++) {
                if (*p == '\x1b') { while (*p && *p != 'm') p++; if (!*p) break; continue; }
                fputc(*p, f);
            }
            fputc('\n', f);
        }
        fclose(f);
        saved++;
    }
    out("\n" GREEN "Saved %d file(s) to " SD_DIR "/%s-*" RESET "\n", saved, stamp);
    scroll = ui_line_count();
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

void write_bcat(PadState *pad, bool use_payload) {
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

void sync_sysdata_bcat(PadState *pad) {
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

void restore_bcat(PadState *pad) {
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
