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
// The BCAT copy is read two ways: through bcat:s (what sdb does, see bcat.h)
// and straight from its save (see bcatsave.h). If something holds a copy
// locked, the read fails; reboot and retry.
//
// Write Empty (or a payload from SD) overwrites the BCAT copy in the save with
// an empty list whose version is one above both copies, so sdb picks it over
// either. The whole save is backed up to SD first. If the save has no
// blacklist.json, it creates it and lists it in bcat's meta files. Restore
// Original puts the original back, or removes what Write Empty added.
//
// Layout: sources.c reads the copies, actions.c changes them, bcatsave.c does
// the save file IO and meta formats, ui.c holds the log the screens scroll
// over, json.c and bcat.c are standalone.

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>
#include "json.h"
#include "blacklist.h"
#include "ui.h"

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

    summary_end = ui_line_count();
    if (sysdata.json) { print_records(&sysdata); out("\n"); }
    if (bc && bc->json) { print_records(bc); out("\n"); }
    records_end = ui_line_count();

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
                    action_start = ui_line_count();
                    write_bcat(&pad, false);
                    action_end = ui_line_count();
                    screen = SCREEN_RESULT;
                    break;
                case MENU_WRITE_PAYLOAD:
                    action_start = ui_line_count();
                    write_bcat(&pad, true);
                    action_end = ui_line_count();
                    screen = SCREEN_RESULT;
                    break;
                case MENU_RESTORE:
                    action_start = ui_line_count();
                    restore_bcat(&pad);
                    action_end = ui_line_count();
                    screen = SCREEN_RESULT;
                    break;
                case MENU_DUMP:
                    action_start = ui_line_count();
                    save_sd();
                    action_end = ui_line_count();
                    screen = SCREEN_RESULT;
                    break;
                case MENU_SYNC:
                    action_start = ui_line_count();
                    sync_sysdata_bcat(&pad);
                    action_end = ui_line_count();
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
