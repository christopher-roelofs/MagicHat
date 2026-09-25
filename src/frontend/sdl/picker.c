#include "frontend/sdl/picker.h"
#include "util/fs.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define ENTRIES_MAX 512
#define PATH_CAP    4096
#define NAME_CAP    256

/*
 * Row ids. The rail hands back whatever id a row carried, and the caller may
 * be running panels of its own, so these sit in a range of their own and
 * mrc_picker_row answers only for those.
 */
#define PICKER_ID_BASE 0x7000
#define PICKER_ID_UP   (PICKER_ID_BASE - 1)

typedef struct {
    char name[NAME_CAP];
    bool directory;
} entry;

struct mrc_picker {
    mrc_ui     *ui;
    const char *title;
    char        dir[PATH_CAP];
    char        taken[PATH_CAP];
    bool        have_taken;

    entry       entries[ENTRIES_MAX];
    unsigned    count;

    /*
     * The rows the rail draws. It borrows this array rather than copying it,
     * so it lives here for as long as the picker does, and the labels point
     * into the entries beside them.
     */
    mrc_ui_row  rows[ENTRIES_MAX + 1];
    unsigned    row_count;
    char        up_label[PATH_CAP + 8];

    const char *suffixes[8];
    char        suffix_store[8][16];
    unsigned    suffix_count;
};

static bool is_dir(const char *path)
{
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

/* A child of the current directory. A root ("/", or "C:/" on Windows)
 * already ends in its separator, so it is not doubled. */
static bool child_path(const mrc_picker *p, const char *name, char *out, size_t size)
{
    size_t n = strlen(p->dir);
    if (n && p->dir[n - 1] == '/') n--;
    return snprintf(out, size, "%.*s/%s", (int)n, p->dir, name) < (int)size;
}

static bool wanted(const mrc_picker *p, const char *name)
{
    if (!p->suffix_count) return true;
    size_t n = strlen(name);
    for (unsigned i = 0; i < p->suffix_count; i++) {
        size_t s = strlen(p->suffixes[i]);
        if (n >= s && !strcasecmp(name + n - s, p->suffixes[i])) return true;
    }
    return false;
}

/* Directories first, then files, each alphabetically without case -- the
 * order a person scans a list in. */
static int before(const entry *a, const entry *b)
{
    if (a->directory != b->directory) return a->directory ? 1 : 0;
    return strcasecmp(a->name, b->name) < 0;
}

static void build_rows(mrc_picker *p)
{
    p->row_count = 0;
    /* The way out, unless there is nowhere further out to go. */
    if (!mrc_path_is_root(p->dir)) {
        snprintf(p->up_label, sizeof(p->up_label), "\xe2\x86\x91  %s", p->dir);
        p->rows[p->row_count++] = (mrc_ui_row){
            .kind = MRC_UI_ROW_ACTION, .label = p->up_label,
            .id = PICKER_ID_UP,
        };
    }
    for (unsigned i = 0; i < p->count; i++) {
        p->rows[p->row_count++] = (mrc_ui_row){
            .kind = MRC_UI_ROW_ACTION,
            .label = p->entries[i].name,
            .value = p->entries[i].directory ? "\xe2\x80\xba" : NULL,
            .id = (int)(PICKER_ID_BASE + i),
        };
    }
    if (!p->count)
        p->rows[p->row_count++] = (mrc_ui_row){
            .kind = MRC_UI_ROW_HEADING, .label = "nothing here to open",
        };
    mrc_ui_open_panel(p->ui, p->title, p->rows, p->row_count);
}

static void read_dir(mrc_picker *p)
{
    p->count = 0;
    DIR *d = opendir(p->dir);
    if (d) {
        const struct dirent *e;
        while ((e = readdir(d)) && p->count < ENTRIES_MAX) {
            if (e->d_name[0] == '.') continue;   /* including . and .. */
            char full[PATH_CAP];
            if (!child_path(p, e->d_name, full, sizeof(full)))
                continue;
            bool dir = is_dir(full);
            if (!dir && !wanted(p, e->d_name)) continue;
            entry *slot = &p->entries[p->count++];
            snprintf(slot->name, sizeof(slot->name), "%s", e->d_name);
            slot->directory = dir;
        }
        closedir(d);
    }
    for (unsigned i = 1; i < p->count; i++)
        for (unsigned j = i; j && before(&p->entries[j], &p->entries[j - 1]); j--) {
            entry t = p->entries[j];
            p->entries[j] = p->entries[j - 1];
            p->entries[j - 1] = t;
        }
    build_rows(p);
}

/* Somewhere readable to start, so the picker always opens on something. */
static void settle(mrc_picker *p, const char *start)
{
    const char *candidates[5];
    unsigned n = 0;
    if (start && *start) candidates[n++] = start;
    const char *home = getenv("HOME");
    if (home && *home) candidates[n++] = home;
#ifdef _WIN32
    const char *profile = getenv("USERPROFILE");
    if (profile && *profile) candidates[n++] = profile;
#endif
    candidates[n++] = ".";
    candidates[n++] = "/";
    for (unsigned i = 0; i < n; i++) {
        char resolved[PATH_CAP];
        if (!mrc_realpath(candidates[i], resolved, sizeof(resolved))) continue;
        if (!is_dir(resolved)) continue;
        snprintf(p->dir, sizeof(p->dir), "%s", resolved);
        return;
    }
    snprintf(p->dir, sizeof(p->dir), "/");
}

mrc_picker *mrc_picker_open(mrc_ui *ui, const char *title, const char *start,
                            const char *const *suffixes, unsigned suffix_count)
{
    if (!ui) return NULL;
    mrc_picker *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->ui = ui;
    p->title = title ? title : "Open";
    if (suffix_count > 8) suffix_count = 8;
    for (unsigned i = 0; i < suffix_count; i++) {
        snprintf(p->suffix_store[i], sizeof(p->suffix_store[i]), "%s", suffixes[i]);
        p->suffixes[i] = p->suffix_store[i];
    }
    p->suffix_count = suffix_count;
    settle(p, start);
    read_dir(p);
    return p;
}

void mrc_picker_close(mrc_picker *p)
{
    if (!p) return;
    if (mrc_ui_panel_open(p->ui)) mrc_ui_close_panel(p->ui);
    free(p);
}

static void go_to(mrc_picker *p, const char *path)
{
    char resolved[PATH_CAP];
    if (!mrc_realpath(path, resolved, sizeof(resolved)) || !is_dir(resolved)) return;
    snprintf(p->dir, sizeof(p->dir), "%s", resolved);
    read_dir(p);
}

bool mrc_picker_row(mrc_picker *p, int id)
{
    if (!p) return false;
    if (id == PICKER_ID_UP) {
        char parent[PATH_CAP];
        snprintf(parent, sizeof(parent), "%s/..", p->dir);
        go_to(p, parent);
        return true;
    }
    if (id < PICKER_ID_BASE || id >= (int)(PICKER_ID_BASE + p->count))
        return false;
    const entry *e = &p->entries[id - PICKER_ID_BASE];
    char full[PATH_CAP];
    if (!child_path(p, e->name, full, sizeof(full)))
        return true;
    if (e->directory) { go_to(p, full); return true; }
    snprintf(p->taken, sizeof(p->taken), "%s", full);
    p->have_taken = true;
    return true;
}

const char *mrc_picker_taken(mrc_picker *p)
{
    if (!p || !p->have_taken) return NULL;
    p->have_taken = false;
    return p->taken;
}

const char *mrc_picker_directory(const mrc_picker *p)
{
    return p ? p->dir : NULL;
}
