#include "frontend/sdl/card_panel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); ++failures; } } while(0)
static bool mounted[2], fail_insert;
static unsigned ejections;
static char selected[4096];
static bool present(void *b,unsigned s) { (void)b; return mounted[s]; }
static const char *name(void *b,unsigned s) { (void)b;(void)s;return "Example card"; }
static bool insert(void *b,unsigned s,const char *path) {
    (void)b; snprintf(selected,sizeof(selected),"%s",path);
    if(fail_insert) return false;
    mounted[s]=true; return true;
}
static bool eject(void *b,unsigned s) { (void)b;mounted[s]=false;++ejections;return true; }
static int find(mrc_ui *ui,const char *label) {
    const mrc_ui_row *r=mrc_ui_panel_rows(ui);
    for(unsigned i=0;i<mrc_ui_panel_row_count(ui);++i)
        if(!strcmp(r[i].label,label))return r[i].id;
    return -1;
}
int main(void) {
    char temp[]="/tmp/mrc-card-panel-XXXXXX",cwd[4096];
    CHECK(getcwd(cwd,sizeof(cwd))!=NULL);
    CHECK(mkdtemp(temp)!=NULL);CHECK(chdir(temp)==0);
    SDL_setenv("SDL_VIDEODRIVER","dummy",1);CHECK(SDL_Init(SDL_INIT_VIDEO)==0);
    SDL_Window *w=SDL_CreateWindow("test",0,0,900,600,0);
    SDL_Renderer *r=SDL_CreateRenderer(w,-1,SDL_RENDERER_SOFTWARE);
    mrc_ui *ui=NULL;CHECK(mrc_ui_open(&ui,r,w));mrc_ui_set_buttons(ui,255);
    mrc_runtime_ops ops={.card_present=present,.card_name=name,.card_insert_sram=insert,.card_eject=eject};
    mrc_runtime m={.ops=&ops};
    mrc_card_panel_open(ui,&m);
    CHECK(mrc_card_panel_row(ui,&m,find(ui,"Create card…")));
    SDL_Event e;SDL_zero(e);e.type=SDL_TEXTINPUT;strcpy(e.text.text,"café");
    CHECK(mrc_card_panel_event(ui,&m,&e));
    SDL_zero(e);e.type=SDL_KEYDOWN;e.key.keysym.sym=SDLK_BACKSPACE;
    CHECK(mrc_card_panel_event(ui,&m,&e));CHECK(find(ui,"caf")>=0);
    e.key.keysym.sym=SDLK_ESCAPE;CHECK(!mrc_card_panel_event(ui,&m,&e));
    CHECK(mrc_ui_event(ui,&e));CHECK(mrc_ui_take_row(ui)==MRC_UI_ROW_BACK);
    fail_insert=true;CHECK(mrc_card_panel_row(ui,&m,find(ui,"Create")));
    CHECK(strstr(mrc_ui_panel_rows(ui)[0].label,"could not create")!=NULL);
    mrc_card_panel_close(ui);CHECK(!mrc_card_panel_showing());CHECK(!SDL_IsTextInputActive());
    e.key.keysym.sym=SDLK_a;CHECK(!mrc_card_panel_event(ui,&m,&e));
    fail_insert=false;mounted[0]=true;mrc_card_panel_open(ui,&m);
    CHECK(mrc_card_panel_row(ui,&m,find(ui,"Card 1")));
    CHECK(mounted[0] && ejections==0);
    const mrc_ui_row *rows=mrc_ui_panel_rows(ui);
    for(unsigned i=0;i<mrc_ui_panel_row_count(ui);++i)
        if(!strcmp(rows[i].label,"Card 1")) {
            CHECK(rows[i].action_count==1);
            CHECK(rows[i].action_icon[0]==MRC_UI_ICON_EJECT);
            int id=rows[i].action_id[0];CHECK(mrc_card_panel_row(ui,&m,id));break;
        }
    CHECK(!mounted[0] && ejections==1);
    FILE *f=fopen("cards/test.sram","wb");CHECK(f!=NULL);if(f)fclose(f);
    CHECK(mrc_card_panel_row(ui,&m,find(ui,"Card 1")));
    CHECK(mrc_card_panel_row(ui,&m,find(ui,"SRAM memory card")));
    CHECK(mrc_card_panel_row(ui,&m,find(ui,"test.sram")));
    CHECK(mounted[0] && strstr(selected,"/cards/test.sram"));
    mrc_card_panel_close(ui);mrc_ui_close(ui);SDL_DestroyRenderer(r);SDL_DestroyWindow(w);SDL_Quit();
    unlink("cards/test.sram");rmdir("cards");CHECK(chdir(cwd)==0);rmdir(temp);
    return failures?1:0;
}
