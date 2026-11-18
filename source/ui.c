#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "ui.h"

// ---- output: every line goes to a buffer that the screen scrolls over ----

static char **lines;
static int nlines, cap_lines;
int scroll;
static char partial[512];

static void add_line(const char *s) {
    if (nlines == cap_lines) {
        cap_lines = cap_lines ? cap_lines * 2 : 256;
        lines = realloc(lines, sizeof *lines * cap_lines);
    }
    lines[nlines++] = strdup(s);
}

void out(const char *fmt, ...) {
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

void clear_lines(void) {
    for (int i = 0; i < nlines; i++) free(lines[i]);
    nlines = 0;
    scroll = 0;
    partial[0] = 0;
}

void err(const char *what, Result rc) {
    out("  %s: " RED "%04d-%04d" RESET " (0x%X)\n", what, 2000 + R_MODULE(rc), R_DESCRIPTION(rc), rc);
}

int ui_line_count(void) {
    return nlines;
}

const char *ui_line(int i) {
    return lines[i];
}

void print_range(int total) {
    if (total > ROWS)
        printf("  " CYAN "(%d-%d of %d)" RESET, scroll + 1,
               scroll + ROWS < total ? scroll + ROWS : total, total);
}

// Prints lines [first, end) from the current scroll offset.
int print_lines(int first, int end) {
    int total = end - first;
    if (scroll > total - ROWS) scroll = total - ROWS;
    if (scroll < 0) scroll = 0;
    for (int i = first + scroll; i < end && i < first + scroll + ROWS; i++)
        printf("%s" RESET "\n", lines[i]);
    return total;
}
