#include <SDL2/SDL.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>

#include "host/import.h"

int mh_launch_main(int, char **);
/* SDL invokes this in its native thread after the Android surface is ready. */
int SDL_main(int argc, char **argv)
{
    /*
     * Where this session keeps its files. With a ROM named, that is beside
     * the ROM, so a device's state sits with its firmware. With none -- which
     * is how the app starts now, straight into its list of devices -- it is
     * the app's own storage, because there is no ROM to sit beside yet.
     */
    if (argc >= 3 && !strcmp(argv[1], "--rom")) {
        char *dir = strdup(argv[2]);
        char *slash = dir ? strrchr(dir, '/') : NULL;
        if (!slash) { free(dir); return 2; }
        *slash = 0;
        if (chdir(dir)) { free(dir); return 2; }
        free(dir);
    } else {
        const char *files = SDL_AndroidGetInternalStoragePath();
        if (files && chdir(files)) return 2;
    }
    freopen("session.log", "w", stderr);
    setvbuf(stderr, NULL, _IOLBF, 0);
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight Portrait PortraitUpsideDown");
    SDL_setenv("MH_FULLSCREEN", "1", 1);
    /*
     * Where devices live. An app has no home directory -- HOME is "/" here,
     * which is not ours to write to -- so the usual desktop places do not
     * exist and the store has to be told. This is the app's own storage,
     * which is private to it and removed when it is uninstalled.
     */
    {
        const char *files = SDL_AndroidGetInternalStoragePath();
        char devices[1024];
        if (files && snprintf(devices, sizeof(devices), "%s/devices", files) <
            (int)sizeof(devices))
            SDL_setenv("MH_DEVICES_DIR", devices, 0);
    }
    int result = mh_launch_main(argc, argv);
    if (result) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MagicHat",
        "The emulator could not start or save successfully. Open the session log from the ROM list for details.", NULL);
    return result;
}

/*
 * What Android can still ask for, which is only to close.
 *
 * There used to be several: power, save, rotate, each relayed as a user event
 * and handled beside the display. They are on the control rail now, drawn by
 * the emulator itself and identical to the desktop's, so the only thing left
 * that Android owns is the back gesture. Menus handle it first; without a
 * menu, the frontend closes and saves the machine on the way out.
 */
JNIEXPORT void JNICALL Java_org_magichat_app_EmulatorActivity_command(
    JNIEnv *env, jclass cls, jint code)
{
    (void)env; (void)cls;
    SDL_Event event = {0};
    event.type = code == 1 ? SDL_KEYDOWN : SDL_QUIT;
    if (code == 1) event.key.keysym.sym = SDLK_AC_BACK;
    SDL_PushEvent(&event);
}

/*
 * Importing a firmware image, which on Android is the platform's job.
 *
 * An app can see its own storage and almost nothing else, so the file the
 * person downloaded is not one this program can find by looking. The system's
 * document picker is the way in: it hands back exactly the file they chose,
 * and the Java side copies it somewhere we own before telling us where.
 *
 * It is asynchronous because it has to be. The picker is another activity; it
 * covers the emulator, the person browses for as long as they browse, and the
 * answer arrives whenever it arrives. The devices list keeps drawing in the
 * meantime and collects the path when there is one.
 */
static char g_imported[4096], g_import_error[256];
static bool g_have_import, g_import_pending, g_discard_import;
static mh_import_kind g_import_kind;
static SDL_SpinLock g_import_lock;

bool mh_import_available(void) { return true; }
bool mh_import_pending(mh_import_kind kind) {
    SDL_AtomicLock(&g_import_lock);
    bool pending = g_import_pending && g_import_kind == kind && !g_discard_import;
    SDL_AtomicUnlock(&g_import_lock);
    return pending;
}
bool mh_import_request(mh_import_kind kind)
{
    SDL_AtomicLock(&g_import_lock);
    if (g_import_pending || g_have_import) {
        SDL_AtomicUnlock(&g_import_lock); return false;
    }
    g_import_kind = kind; g_import_pending = true; g_discard_import = false;
    g_imported[0] = g_import_error[0] = 0;
    SDL_AtomicUnlock(&g_import_lock);
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    bool ok = false;
    if (env && activity) {
        jclass cls = (*env)->GetObjectClass(env, activity);
        jmethodID method = (*env)->GetMethodID(env, cls, "requestImport", "(I)V");
        if (method) { (*env)->CallVoidMethod(env, activity, method, (jint)kind); ok = true; }
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); ok = false; }
        (*env)->DeleteLocalRef(env, cls);
        (*env)->DeleteLocalRef(env, activity);
    }
    if (!ok) {
        SDL_AtomicLock(&g_import_lock);
        g_import_pending = false;
        SDL_AtomicUnlock(&g_import_lock);
    }
    return ok;
}
bool mh_import_result(mh_import_kind kind, char *path, size_t path_size,
                       char *error, size_t error_size)
{
    SDL_AtomicLock(&g_import_lock);
    bool ready = g_have_import && g_import_kind == kind;
    if (ready) {
        snprintf(path, path_size, "%s", g_imported);
        snprintf(error, error_size, "%s", g_import_error);
        g_have_import = false;
    }
    SDL_AtomicUnlock(&g_import_lock);
    return ready;
}
void mh_import_cancel(mh_import_kind kind)
{
    SDL_AtomicLock(&g_import_lock);
    if (g_import_kind == kind) { g_discard_import = true; g_have_import = false; }
    SDL_AtomicUnlock(&g_import_lock);
}
JNIEXPORT void JNICALL Java_org_magichat_app_EmulatorActivity_imported(
    JNIEnv *env, jclass cls, jstring path, jstring error)
{
    (void)cls;
    const char *p = path ? (*env)->GetStringUTFChars(env, path, NULL) : NULL;
    const char *e = error ? (*env)->GetStringUTFChars(env, error, NULL) : NULL;
    SDL_AtomicLock(&g_import_lock);
    snprintf(g_imported, sizeof(g_imported), "%s", p ? p : "");
    snprintf(g_import_error, sizeof(g_import_error), "%s", e ? e : "");
    g_import_pending = false;
    g_have_import = !g_discard_import;
    SDL_AtomicUnlock(&g_import_lock);
    if (p) (*env)->ReleaseStringUTFChars(env, path, p);
    if (e) (*env)->ReleaseStringUTFChars(env, error, e);
}
