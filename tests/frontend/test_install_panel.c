/*
 * test_install_panel.c — choosing a package and starting it on its way.
 *
 * The transfer itself is the guest's business and takes minutes; what this
 * checks is the part a person touches, which is the part that was silently
 * wrong twice. A machine with no serial link must say so rather than opening
 * a picker that leads nowhere, and a refusal written into a variable nobody
 * draws is a button that does nothing -- the same bug the devices list had.
 *
 * The board here is a stand-in: it answers the install operation and hands
 * back a real link, so the panel is exercised exactly as it is by a
 * DataRover, without waiting for one to boot.
 */
#include "frontend/sdl/install_panel.h"
#include "frontend/sdl/picker.h"
#include "host/pclink.h"
#include "util/fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails;
#define CHECK(x) do{ if(!(x)){ fprintf(stderr,"line %d: %s\n",__LINE__,#x); fails++; } }while(0)
static int find(const mrc_ui_row *r, unsigned n, const char *l) {
    for (unsigned i=0;i<n;i++) if (r[i].label && !strcmp(r[i].label,l)) return r[i].id;
    return -1;
}
/* A board that can take a package, and one that cannot. */
static mrc_pclink *taken;
static struct mrc_pclink *fake_install(void *b, const char *path) {
    (void)b; taken = mrc_pclink_open(path); return taken;
}
static void put(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) exit(1);
    fwrite(data, 1, n, f);
    fclose(f);
}

int main(void) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/mrc-install-XXXXXX", mrc_temp_dir());
    if (!mrc_mkdtemp(dir)) return 1;
    char pkg[256], notes[256], emc[256];
    snprintf(pkg, sizeof(pkg), "%s/Reversi.pkg", dir);
    snprintf(emc, sizeof(emc), "%s/WebBrowser40.mc2", dir);
    snprintf(notes, sizeof(notes), "%s/notes.txt", dir);
    static unsigned char body[40192];
    for (size_t i = 0; i < sizeof(body); i++) body[i] = (unsigned char)(i * 7 + 3);
    /* Every package starts with the same eight bytes, whatever it is named. */
    static const unsigned char signature[8] = {0,'S','A','L','T','C','O','D'};
    memcpy(body, signature, sizeof(signature));
    put(pkg, body, sizeof(body));
    put(notes, "not a package", 13);
    put(emc, body, sizeof(body));       /* the same container, another name */
    SDL_setenv("SDL_VIDEODRIVER","dummy",1);
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window *w = SDL_CreateWindow("x",0,0,900,600,0);
    SDL_Renderer *r = SDL_CreateRenderer(w,-1,SDL_RENDERER_SOFTWARE);
    mrc_ui *ui=NULL; mrc_ui_open(&ui,r,w);
    mrc_ui_set_buttons(ui, 1u<<MRC_UI_ICON_INSTALL);
    mrc_setenv("HOME", dir);

    /* A machine with no link says so rather than offering a dead picker. */
    mrc_runtime_ops bare = {0};
    mrc_runtime nolink = { .ops = &bare };
    mrc_install_panel_open(ui, &nolink);
    const mrc_ui_row *rows = mrc_ui_panel_rows(ui);
    unsigned n = mrc_ui_panel_row_count(ui);
    int choose = find(rows,n,"Choose a package\xe2\x80\xa6");
    CHECK(choose >= 0);
    CHECK(mrc_install_panel_row(ui,&nolink,choose));
    rows = mrc_ui_panel_rows(ui); n = mrc_ui_panel_row_count(ui);
    CHECK(rows[0].kind == MRC_UI_ROW_HEADING);
    CHECK(rows[0].label && strstr(rows[0].label,"no link"));
    mrc_install_panel_close(ui);

    /* A machine that can: choose a package and the transfer starts. */
    mrc_runtime_ops ops = { .install = fake_install };
    mrc_runtime m = { .ops = &ops };
    mrc_install_panel_open(ui, &m);
    rows = mrc_ui_panel_rows(ui); n = mrc_ui_panel_row_count(ui);
    choose = find(rows,n,"Choose a package\xe2\x80\xa6");
    CHECK(choose >= 0);
    CHECK(mrc_install_panel_row(ui,&m,choose));
    rows = mrc_ui_panel_rows(ui); n = mrc_ui_panel_row_count(ui);
    int pkgrow = find(rows,n,"Reversi.pkg");
    CHECK(pkgrow >= 0);
    CHECK(find(rows,n,"notes.txt") < 0);      /* filtered */
    /* Packages are published under several extensions and are all the same
     * container; the list shows them and the signature is what decides. */
    CHECK(find(rows,n,"WebBrowser40.mc2") >= 0);
    CHECK(mrc_install_panel_row(ui,&m,pkgrow));
    rows = mrc_ui_panel_rows(ui); n = mrc_ui_panel_row_count(ui);
    CHECK(taken != NULL);
    CHECK(rows[0].kind == MRC_UI_ROW_HEADING);
    CHECK(rows[0].label && strstr(rows[0].label,"Storeroom"));  /* waiting text */
    CHECK(n >= 3);
    CHECK(rows[1].value && strstr(rows[1].value,"of 40192 bytes"));
    mrc_install_panel_close(ui);
    if (taken) mrc_pclink_close(taken);
    mrc_ui_close(ui);
    SDL_Quit();
    char clean[300];
    snprintf(clean, sizeof(clean), "rm -rf '%s'", dir);
    if (system(clean)) { /* a temp directory left behind is not a failure */ }
    if (fails) fprintf(stderr,"%d install panel checks failed\n",fails);
    else printf("install panel checks passed\n");
    return fails?1:0;
}
