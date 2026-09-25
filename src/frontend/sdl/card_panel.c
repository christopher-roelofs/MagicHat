#include "frontend/sdl/card_panel.h"
#include "frontend/sdl/picker.h"
#include "host/import.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define ROW_SLOT1 0x6001
#define ROW_SLOT2 0x6002
#define ROW_CREATE 0x6003
#define ROW_TYPE_SRAM 0x6010
#define ROW_TYPE_NE2000 0x6011
#define ROW_EJECT1 0x6020
#define ROW_EJECT2 0x6021
#define ROW_IMPORT_SRAM 0x6022

static mrc_ui_row rows[5];
static bool showing;
static mrc_picker *picker;
static unsigned picker_slot;
static char last_directory[4096];
static char card_directory[4096];
static bool creating;
static bool type_selecting;
static unsigned type_slot;
static unsigned create_slot;
static char create_name[128];
static char notice[160];
static char display_values[2][256];
static bool create_name_edited;
static void begin_create(mrc_ui *, mrc_runtime *);
static void finish_create(mrc_ui *, mrc_runtime *);

static const char *card_display_name(mrc_runtime *m, unsigned slot)
{
    const char *path = m && m->ops->card_name
                     ? m->ops->card_name(m->board, slot) : NULL;
    if (!path || !*path) return "inserted — eject";
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

static bool ensure_card_directory(void)
{
    struct stat st;
    if (mkdir("cards", 0700) < 0 && access("cards", F_OK) < 0)
        return false;
    if (stat("cards", &st) < 0 || !S_ISDIR(st.st_mode)) return false;
    if (!realpath("cards", card_directory)) return false;
    return true;
}

static void default_name(void)
{
    for (unsigned n = 1; n < 100000; n++) {
        snprintf(create_name, sizeof(create_name), "mc%u", n);
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", card_directory, create_name);
        if (access(path, F_OK) != 0) return;
    }
    snprintf(create_name, sizeof(create_name), "mc%u", 100000u);
}

static void build(mrc_ui *ui, mrc_runtime *m)
{
    unsigned count = 0;
    if (notice[0]) {
        rows[count++] = (mrc_ui_row){ .kind = MRC_UI_ROW_HEADING,
                                      .label = notice };
    }
    if (creating) {
        rows[count++] = (mrc_ui_row){ .kind = MRC_UI_ROW_HEADING,
                                      .label = "Type a name, then choose Create" };
        rows[count++] = (mrc_ui_row){ .kind = MRC_UI_ROW_CHOICE,
                                      .label = create_name,
                                      .value = "name" };
        rows[count++] = (mrc_ui_row){ .kind = MRC_UI_ROW_ACTION,
                                      .label = "Create",
                                      .value = create_slot ? "slot 2" : "slot 1",
                                      .id = ROW_CREATE };
        mrc_ui_open_panel(ui, "New memory card", rows, count);
        showing = true;
        return;
    }
    if (type_selecting) {
        rows[count++] = (mrc_ui_row){ .kind = MRC_UI_ROW_ACTION,
                                      .label = "SRAM memory card",
                                      .value = "writable storage",
                                      .id = ROW_TYPE_SRAM };
        if (mrc_import_available())
            rows[count++] = (mrc_ui_row){ .kind = MRC_UI_ROW_ACTION,
                .label = "Import SRAM card copy…", .value = "from Android storage",
                .id = ROW_IMPORT_SRAM };
        if (m && m->ops->card_insert_ne2000)
            rows[count++] = (mrc_ui_row){ .kind = MRC_UI_ROW_ACTION,
                                          .label = "NE2000 network card",
                                          .value = "Internet",
                                          .id = ROW_TYPE_NE2000 };
        mrc_ui_open_panel(ui, "Choose PC Card", rows, count);
        showing = true;
        return;
    }
    for (unsigned slot = 0; slot < 2; slot++) {
        bool present = m && m->ops->card_present &&
                      m->ops->card_present(m->board, slot);
        if (present) {
            const char *type = m->ops->card_type
                             ? m->ops->card_type(m->board, slot) : NULL;
            snprintf(display_values[slot], sizeof(display_values[slot]),
                     "%s — %s", type ? type : "card",
                     card_display_name(m, slot));
        }
        rows[count++] = (mrc_ui_row){
            .kind = MRC_UI_ROW_ACTION,
            .label = slot ? "Card 2" : "Card 1",
            .value = present ? display_values[slot] : "empty — insert",
            .id = slot ? ROW_SLOT2 : ROW_SLOT1,
            .action_count = present ? 1 : 0,
            .action_icon = { MRC_UI_ICON_EJECT },
            .action_id = { slot ? ROW_EJECT2 : ROW_EJECT1 },
        };
    }
    rows[count++] = (mrc_ui_row){ .kind = MRC_UI_ROW_ACTION,
                                  .label = "Create card…",
                                  .value = "new writable SRAM image",
                                  .id = ROW_CREATE };
    mrc_ui_open_panel(ui, "PC Cards", rows, count);
    showing = true;
}

void mrc_card_panel_open(mrc_ui *ui, mrc_runtime *m)
{
    if (picker) { mrc_picker_close(picker); picker = NULL; }
    if (creating) SDL_StopTextInput();
    creating = false;
    type_selecting = false;
    notice[0] = 0;
    if (!ensure_card_directory())
        snprintf(notice, sizeof(notice), "could not create cards directory");
    build(ui, m);
}

void mrc_card_panel_close(mrc_ui *ui)
{
    mrc_import_cancel(MRC_IMPORT_CARD);
    if (picker) {
        const char *at = mrc_picker_directory(picker);
        if (at) snprintf(last_directory, sizeof(last_directory), "%s", at);
        mrc_picker_close(picker); picker = NULL;
    }
    if (creating) SDL_StopTextInput();
    creating = false;
    type_selecting = false;
    if (mrc_ui_panel_open(ui)) mrc_ui_close_panel(ui);
    showing = false;
}

bool mrc_card_panel_showing(void) { return showing; }

bool mrc_card_panel_back(mrc_ui *ui, mrc_runtime *m)
{
    if (!showing) return false;
    if (creating) {
        SDL_StopTextInput();
        creating = false;
        notice[0] = 0;
        build(ui, m);
        return true;
    }
    if (type_selecting) {
        mrc_import_cancel(MRC_IMPORT_CARD);
        type_selecting = false;
        build(ui, m);
        return true;
    }
    if (!picker) return false;
    const char *at = mrc_picker_directory(picker);
    if (at) snprintf(last_directory, sizeof(last_directory), "%s", at);
    mrc_picker_close(picker); picker = NULL;
    type_slot = picker_slot;
    type_selecting = true;
    build(ui, m);
    return true;
}

static unsigned first_empty_slot(mrc_runtime *m)
{
    for (unsigned slot = 0; slot < 2; slot++)
        if (!m->ops->card_present(m->board, slot)) return slot;
    return 2;
}

static void begin_create(mrc_ui *ui, mrc_runtime *m)
{
    if (!card_directory[0] && !ensure_card_directory()) {
        snprintf(notice, sizeof(notice), "could not create cards directory");
        build(ui, m);
        return;
    }
    create_slot = first_empty_slot(m);
    if (create_slot > 1) {
        snprintf(notice, sizeof(notice), "both card slots are occupied");
        build(ui, m);
        return;
    }
    default_name();
    create_name_edited = false;
    notice[0] = 0;
    creating = true;
    SDL_StartTextInput();
    build(ui, m);
}

static void finish_create(mrc_ui *ui, mrc_runtime *m)
{
    if (!create_name[0] || !strcmp(create_name, ".") || !strcmp(create_name, "..")) {
        snprintf(notice, sizeof(notice), "Enter a valid card name");
        build(ui, m);
        return;
    }
    char path[4096];
    if (snprintf(path, sizeof(path), "%s/%s", card_directory, create_name) >= (int)sizeof(path)) {
        snprintf(notice, sizeof(notice), "Card path is too long");
        build(ui, m);
        return;
    }
    if (access(path, F_OK) == 0) {
        snprintf(notice, sizeof(notice), "That name already exists; choose another");
        build(ui, m);
        return;
    }
    if (!m->ops->card_insert_sram(m->board, create_slot, path)) {
        snprintf(notice, sizeof(notice), "could not create %s", create_name);
        build(ui, m);
        return;
    }
    SDL_StopTextInput();
    creating = false;
    notice[0] = 0;
    build(ui, m);
}

bool mrc_card_panel_event(mrc_ui *ui, mrc_runtime *m, const SDL_Event *e)
{
    if (!showing || !creating) return false;
    if (e->type == SDL_TEXTINPUT) {
        size_t have = strlen(create_name);
        size_t add = strlen(e->text.text);
        if (!create_name_edited) {
            create_name[0] = 0;
            have = 0;
            create_name_edited = true;
        }
        if (have + add < sizeof(create_name) - 1 &&
            !strpbrk(e->text.text, "/\\"))
            strncat(create_name, e->text.text, sizeof(create_name) - have - 1);
        build(ui, m);
        return true;
    }
    if (e->type != SDL_KEYDOWN) return false;
    if (e->key.keysym.sym == SDLK_BACKSPACE) {
        create_name_edited = true;
        size_t n = strlen(create_name);
        if (n) {
            do { --n; } while (n && ((unsigned char)create_name[n] & 0xc0) == 0x80);
            create_name[n] = 0;
        }
        build(ui, m);
        return true;
    }
    if (e->key.keysym.sym == SDLK_RETURN || e->key.keysym.sym == SDLK_KP_ENTER) {
        finish_create(ui, m);
        return true;
    }
    if (e->key.keysym.sym == SDLK_ESCAPE || e->key.keysym.sym == SDLK_AC_BACK)
        return false;  /* shared menu Back handling */
    return true;
}

bool mrc_card_panel_row(mrc_ui *ui, mrc_runtime *m, int id)
{
    if (!showing) return false;
    if (mrc_import_pending(MRC_IMPORT_CARD)) return true;
    if (picker) {
        if (!mrc_picker_row(picker, id)) return false;
        const char *chosen = mrc_picker_taken(picker);
        if (!chosen) return true;
        char path[4096];
        snprintf(path, sizeof(path), "%s", chosen);
        const char *at = mrc_picker_directory(picker);
        if (at) snprintf(last_directory, sizeof(last_directory), "%s", at);
        mrc_picker_close(picker); picker = NULL;
        if (!m || !m->ops->card_insert_sram ||
            !m->ops->card_insert_sram(m->board, picker_slot, path))
            snprintf(notice, sizeof(notice), "Could not attach that SRAM card");
        else notice[0] = 0;
        build(ui, m);
        return true;
    }
    if (id == ROW_CREATE) {
        if (creating) finish_create(ui, m);
        else if (m && m->ops->card_present && m->ops->card_insert_sram)
            begin_create(ui, m);
        return true;
    }
    if (type_selecting) {
        if (id == ROW_IMPORT_SRAM) {
            picker_slot = type_slot;
            snprintf(notice, sizeof(notice), "%s", mrc_import_request(MRC_IMPORT_CARD)
                ? "Choose a card to copy into local storage"
                : "Another file import is still finishing");
            build(ui, m);
            return true;
        }
        if (id == ROW_TYPE_SRAM) {
            type_selecting = false;
            picker_slot = type_slot;
            picker = mrc_picker_open(ui, "Choose SRAM card image",
                                     card_directory[0] ? card_directory : NULL,
                                     NULL, 0);
            if (!picker) build(ui, m);
            return true;
        }
        if (id == ROW_TYPE_NE2000) {
            type_selecting = false;
            if (!m->ops->card_insert_ne2000(m->board, type_slot))
                snprintf(notice, sizeof(notice), "could not attach NE2000");
            build(ui, m);
            return true;
        }
        return false;
    }
    if (id == ROW_EJECT1 || id == ROW_EJECT2) {
        unsigned slot = id == ROW_EJECT2;
        if (!m || !m->ops->card_eject || !m->ops->card_eject(m->board, slot))
            snprintf(notice, sizeof(notice), "Could not eject card %u", slot + 1);
        else snprintf(notice, sizeof(notice), "Card %u ejected", slot + 1);
        build(ui, m);
        return true;
    }
    if (id != ROW_SLOT1 && id != ROW_SLOT2) return false;
    picker_slot = id == ROW_SLOT2;
    bool present = m && m->ops->card_present &&
                   m->ops->card_present(m->board, picker_slot);
    if (present) {
        snprintf(notice, sizeof(notice), "Use the eject button to remove card %u", picker_slot + 1);
        build(ui, m);
        return true;
    }
    if (!m || (!m->ops->card_insert_sram && !m->ops->card_insert_ne2000))
        return true;
    type_selecting = true;
    type_slot = picker_slot;
    build(ui, m);
    return true;
}

void mrc_card_panel_tick(mrc_ui *ui, mrc_runtime *m)
{
    if (!showing) return;
    char path[4096], error[256];
    if (!mrc_import_result(MRC_IMPORT_CARD, path, sizeof(path), error, sizeof(error))) return;
    if (error[0]) snprintf(notice, sizeof(notice), "%s", error);
    else if (!path[0]) snprintf(notice, sizeof(notice), "File selection cancelled");
    else if (!m || !m->ops->card_insert_sram ||
             !m->ops->card_insert_sram(m->board, picker_slot, path))
        snprintf(notice, sizeof(notice), "Could not attach the imported card");
    else { type_selecting = false; snprintf(notice, sizeof(notice), "Imported copy mounted; original file unchanged"); }
    build(ui, m);
}
