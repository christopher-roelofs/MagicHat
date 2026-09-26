#include "frontend/sdl/card_panel.h"
#include "util/fs.h"
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
static int find(mh_ui *ui,const char *label) {
    const mh_ui_row *r=mh_ui_panel_rows(ui);
    for(unsigned i=0;i<mh_ui_panel_row_count(ui);++i)
        if(!strcmp(r[i].label,label))return r[i].id;
    return -1;
}
int main(void) {
    char temp[512],cwd[4096];
    snprintf(temp,sizeof(temp),"%s/mh-card-panel-XXXXXX",mh_temp_dir());
    CHECK(getcwd(cwd,sizeof(cwd))!=NULL);
    CHECK(mh_mkdtemp(temp)!=NULL);CHECK(chdir(temp)==0);
    SDL_setenv("SDL_VIDEODRIVER","dummy",1);CHECK(SDL_Init(SDL_INIT_VIDEO)==0);
    SDL_Window *w=SDL_CreateWindow("test",0,0,900,600,0);
    SDL_Renderer *r=SDL_CreateRenderer(w,-1,SDL_RENDERER_SOFTWARE);
    mh_ui *ui=NULL;CHECK(mh_ui_open(&ui,r,w));mh_ui_set_buttons(ui,255);
    mh_runtime_ops ops={.card_present=present,.card_name=name,.card_insert_sram=insert,.card_eject=eject};
    mh_runtime m={.ops=&ops};
    mh_card_panel_open(ui,&m);
    CHECK(mh_card_panel_row(ui,&m,find(ui,"Create card…")));
    SDL_Event e;SDL_zero(e);e.type=SDL_TEXTINPUT;strcpy(e.text.text,"café");
    CHECK(mh_card_panel_event(ui,&m,&e));
    SDL_zero(e);e.type=SDL_KEYDOWN;e.key.keysym.sym=SDLK_BACKSPACE;
    CHECK(mh_card_panel_event(ui,&m,&e));CHECK(find(ui,"caf")>=0);
    e.key.keysym.sym=SDLK_ESCAPE;CHECK(!mh_card_panel_event(ui,&m,&e));
    CHECK(mh_ui_event(ui,&e));CHECK(mh_ui_take_row(ui)==MH_UI_ROW_BACK);
    fail_insert=true;CHECK(mh_card_panel_row(ui,&m,find(ui,"Create")));
    CHECK(strstr(mh_ui_panel_rows(ui)[0].label,"could not create")!=NULL);
    mh_card_panel_close(ui);CHECK(!mh_card_panel_showing());CHECK(!SDL_IsTextInputActive());
    e.key.keysym.sym=SDLK_a;CHECK(!mh_card_panel_event(ui,&m,&e));
    fail_insert=false;mounted[0]=true;mh_card_panel_open(ui,&m);
    CHECK(mh_card_panel_row(ui,&m,find(ui,"Card 1")));
    CHECK(mounted[0] && ejections==0);
    const mh_ui_row *rows=mh_ui_panel_rows(ui);
    for(unsigned i=0;i<mh_ui_panel_row_count(ui);++i)
        if(!strcmp(rows[i].label,"Card 1")) {
            CHECK(rows[i].action_count==1);
            CHECK(rows[i].action_icon[0]==MH_UI_ICON_EJECT);
            int id=rows[i].action_id[0];CHECK(mh_card_panel_row(ui,&m,id));break;
        }
    CHECK(!mounted[0] && ejections==1);
    FILE *f=fopen("cards/test.sram","wb");CHECK(f!=NULL);if(f)fclose(f);
    CHECK(mh_card_panel_row(ui,&m,find(ui,"Card 1")));
    CHECK(mh_card_panel_row(ui,&m,find(ui,"SRAM memory card")));
    CHECK(mh_card_panel_row(ui,&m,find(ui,"test.sram")));
    CHECK(mounted[0] && strstr(selected,"/cards/test.sram"));
    mh_card_panel_close(ui);mh_ui_close(ui);SDL_DestroyRenderer(r);SDL_DestroyWindow(w);SDL_Quit();
    unlink("cards/test.sram");rmdir("cards");CHECK(chdir(cwd)==0);rmdir(temp);
    return failures?1:0;
}
