#include "frontend/sdl/display.h"
#include <stdio.h>
#include "frontend/sdl/presentation.h"
#include "host/state.h"
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s (%s)\n",__LINE__,#x,SDL_GetError()); return 1; } } while(0)
static unsigned saves, releases;
static char saved_to[256];
static bool save_state(void *ctx, const char *path)
{ (void)ctx; saves++; snprintf(saved_to,sizeof(saved_to),"%s",path); return true; }
static bool pen(void *ctx, bool down, unsigned x, unsigned y)
{ (void)ctx; (void)x; (void)y; if (!down) releases++; return true; }
/* Where a device's state lands, which is the same rule the retained store
 * used: the firmware path with .state in place of its extension. */
static int check_paths(void)
{
    char out[64];
    CHECK(mh_state_path_for("roms/PIC-2000.rom",out,sizeof(out)) &&
          !strcmp(out,"roms/PIC-2000.state"));
    /* No extension at all, and a dot in a directory along the way is not one. */
    CHECK(mh_state_path_for("roms/PIC2000",out,sizeof(out)) &&
          !strcmp(out,"roms/PIC2000.state"));
    CHECK(mh_state_path_for("a.b/rom",out,sizeof(out)) &&
          !strcmp(out,"a.b/rom.state"));
    /* Launching a device from its own state would save over it with a
     * machine that never booted. */
    CHECK(!mh_state_path_for("roms/PIC-2000.state",out,sizeof(out)));
    CHECK(!mh_state_path_for("roms/PIC-2000.rom",out,8));
    return 0;
}

int main(void)
{
    if (check_paths()) return 1;
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    CHECK(SDL_Init(SDL_INIT_VIDEO)==0);
    mh_sdl_display d;
    CHECK(mh_sdl_display_open(&d,"display test",4,4,24));
    CHECK(mh_sdl_display_resize(&d,2,2));
    const uint8_t levels[]={0,1,2,3}, gray[]={255,170,85,0};
    uint32_t expected[4];
    mh_sdl_display_gray(expected,gray,4,MH_FRAME_GRAY8);
    mh_sdl_display_gray(d.pixels,levels,4,MH_FRAME_LCD2);
    for(unsigned i=0;i<4;i++) CHECK(d.pixels[i]==expected[i]);
    CHECK(expected[0]==0xffffffff && expected[3]==0xff000000);
    CHECK(mh_sdl_display_present(&d,2,2,false));
    uint32_t screen[16];
    CHECK(SDL_RenderReadPixels(d.renderer,NULL,SDL_PIXELFORMAT_ARGB8888,screen,16)==0);
    CHECK(screen[0]==expected[0] && screen[3]==expected[1]);
    CHECK(screen[12]==expected[2] && screen[15]==expected[3]);
    /* Non-square panel in a portrait window: verify actual rendered corners
     * and their inverse touch mapping, including letterbox rejection. */
    CHECK(mh_sdl_display_resize(&d,3,2));
    uint32_t colors[]={0xffff0000,0xff00ff00,0xff0000ff,
                       0xff00ffff,0xffff00ff,0xffffff00};
    SDL_memcpy(d.pixels,colors,sizeof(colors));
    SDL_SetWindowSize(d.window,12,18);
    const char *rotations[]={"0","90","180","270"};
    for(unsigned r=0;r<4;r++) {
        SDL_setenv("MH_DISPLAY_ROTATION",rotations[r],1);
        CHECK(mh_sdl_display_present(&d,3,2,false));
        uint32_t portrait[12*18];
        CHECK(SDL_RenderReadPixels(d.renderer,NULL,SDL_PIXELFORMAT_ARGB8888,
                                   portrait,12*4)==0);
        SDL_Rect dst;
        mh_sdl_panel_rect(d.renderer,3,2,false,0,0,&dst);
        for(unsigned y=0;y<2;y++) for(unsigned x=0;x<3;x++) {
            int ox,oy; unsigned px,py;
            mh_sdl_panel_to_output(&dst,x,y,3,2,&ox,&oy);
            CHECK(portrait[oy*12+ox]==colors[y*3+x]);
            CHECK(mh_sdl_panel_point(&dst,ox,oy,3,2,&px,&py));
            CHECK(px==x && py==y);
        }
        unsigned px,py;
        CHECK(!mh_sdl_panel_point(&dst,dst.x-1,dst.y,3,2,&px,&py));
        CHECK(!mh_sdl_panel_point(&dst,dst.x+dst.w,dst.y,3,2,&px,&py));
    }
    SDL_setenv("MH_DISPLAY_ROTATION","0",1);
    CHECK(mh_sdl_display_resize(&d,2,2));
    /*
     * Going to the background saves the machine.
     *
     * On a tablet this is the likeliest way a session ends -- the system can
     * take the process at any point afterwards without asking again -- so it
     * is the save that matters most, and it saves the whole machine to the
     * file this device keeps it in.
     */
    mh_runtime_ops ops = {.pen=pen,.save_state=save_state};
    mh_runtime runtime = {.ops=&ops};

    /* A device with nowhere to keep its state is not offered the button, and
     * backgrounding does not quietly write one somewhere. */
    mh_state_set_path(NULL);
    CHECK(!mh_sdl_can_save_state(&runtime));
    mh_state_set_path("/nonexistent-directory/device.state");
    CHECK(mh_sdl_can_save_state(&runtime));
    mh_runtime_ops cannot = {.pen=pen};
    mh_runtime no_save = {.ops=&cannot};
    CHECK(!mh_sdl_can_save_state(&no_save));

    Uint64 wall=SDL_GetPerformanceCounter();
    SDL_Event event = {.type=SDL_APP_WILLENTERBACKGROUND};
    CHECK(mh_sdl_display_event(&d,&event,&runtime,&wall));
    CHECK(d.backgrounded && saves==1 && releases==1);
    CHECK(!strcmp(saved_to,"/nonexistent-directory/device.state"));
    /* Already in the background: nothing further to save. */
    CHECK(mh_sdl_display_event(&d,&event,&runtime,&wall));
    CHECK(saves==1);
    Uint64 old_wall=wall;
    SDL_Delay(2);
    event.type=SDL_APP_DIDENTERFOREGROUND;
    CHECK(mh_sdl_display_event(&d,&event,&runtime,&wall));
    CHECK(!d.backgrounded && wall>old_wall);
    event.type=SDL_RENDER_DEVICE_RESET;
    CHECK(mh_sdl_display_event(&d,&event,&runtime,&wall));
    CHECK(mh_sdl_display_present(&d,2,2,false));
    mh_state_set_path(NULL);
    uint32_t *old=d.pixels;
    CHECK(mh_sdl_display_resize(&d,2,2) && d.pixels==old);
    CHECK(!mh_sdl_display_resize(&d,UINT32_MAX,2) && d.pixels==old);
    CHECK(!mh_sdl_display_resize(&d,0,2) && d.pixels==old);
    /* Exercise the LCD cell-size transitions that previously freed twice. */
    for(unsigned i=0;i<30;i++) {
        unsigned size=2+i%5;
        CHECK(mh_sdl_display_resize(&d,size,size));
        CHECK(d.width==size && d.height==size);
        CHECK(mh_sdl_display_present(&d,2,2,true));
    }
    mh_sdl_display_close(&d);
    CHECK(!d.window && !d.renderer && !d.texture && !d.pixels);
    mh_sdl_display_close(&d);
    SDL_Quit();
    return 0;
}
