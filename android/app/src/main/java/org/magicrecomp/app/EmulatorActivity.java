package org.magicrecomp.app;

import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Build;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.provider.OpenableColumns;
import org.libsdl.app.SDLActivity;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.UUID;

/*
 * The emulator, full screen, with nothing of Android's around it.
 *
 * There used to be a toolbar across the top with a "Magic Cap controls"
 * button, and a dialog behind it offering power, save, rotate and close. All
 * four now live on the control rail the emulator draws itself, which is the
 * same rail the desktop has -- so the toolbar was a second set of controls
 * that had to be kept in step with the first, and the strip it sat in was
 * taken out of the guest's screen on a device that has little enough of it.
 *
 * What is left is the surface and the back gesture, which closes cleanly so
 * the machine is saved.
 */
public class EmulatorActivity extends SDLActivity {
    private static native void command(int code);
    /* The emulator calls this back with the imported image, or null. */
    private static native void imported(String path, String error);
    private static final int PICK_FILE = 100;
    @Override protected String[] getLibraries() { return new String[]{"SDL2", "mcap"}; }
    @Override protected String[] getArguments() {
        ArrayList<String> args = new ArrayList<>();
        /*
         * A ROM only when something asked for one. With none, the emulator
         * opens its own list of devices, which is how this app starts now:
         * there is no separate screen in front of it.
         */
        String rom = getIntent().getStringExtra("rom");
        if (rom != null) { args.add("--rom"); args.add(rom); }
        args.add("--gui");
        // The fastest engine that is exactly equivalent on this device:
        // native execution for MIPS and cached blocks for 68k.
        args.add("--cpu-engine"); args.add("auto");
        if (getIntent().getBooleanExtra("temporary", false)) args.add("--temporary");
        if (getIntent().getBooleanExtra("fresh", false)) args.add("--fresh");
        return args.toArray(new String[0]);
    }
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (Build.VERSION.SDK_INT >= 33)
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT, this::close);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        /*
         * The guest gets the whole screen, with the system bars out of the
         * way. Padding the layout to clear them instead left a strip of the
         * window's own background along the bottom -- white, under a machine
         * that is mostly black, and taken out of a panel that is small
         * enough already. The rail the emulator draws is its own controls;
         * Android's belong to Android and can be swiped back when wanted.
         */
        getWindow().setBackgroundDrawable(new ColorDrawable(Color.BLACK));
        hideSystemBars();
    }
    /*
     * The system's document picker, which is the only way to reach a file the
     * person downloaded: an app sees its own storage and little else, so no
     * amount of browsing from inside the emulator would find it. Called from
     * native code when the devices list is asked for a new device.
     */
    @SuppressWarnings("unused")   /* called through JNI */
    public void requestImport(int kind) {
        runOnUiThread(() -> {
            try {
                startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
                    .addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"), PICK_FILE + kind);
            } catch (Exception e) {
                imported(null, "Could not open the system file picker");
            }
        });
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        final int kind = request - PICK_FILE;
        if (kind < 0 || kind > 2) return;
        if (result != RESULT_OK || data == null || data.getData() == null) {
            imported(null, null);           /* they changed their mind */
            return;
        }
        Uri uri = data.getData();
        /*
         * Copied on a thread of its own: an image is megabytes and the
         * emulator is drawing. It lands in storage this app owns, which is
         * where the device it becomes will copy it from.
         */
        new Thread(() -> {
            String folder = kind == 0 ? "roms" : kind == 1 ? "packages" : "cards";
            File directory = new File(new File(getFilesDir(), folder),
                                      UUID.randomUUID().toString());
            File partial = new File(directory, "import.part");
            try {
                String fallback = kind == 0 ? "Magic Cap.rom" : kind == 1 ? "package.pkg" : "card.sram";
                String name = fallback;
                try (Cursor c = getContentResolver().query(uri,
                        new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                    if (c != null && c.moveToFirst()) name = c.getString(0);
                }
                if (name == null || name.isEmpty() || name.equals(".") || name.equals("..")) name = fallback;
                name = name.replace('/', '_').replace('\\', '_');
                if (name.length() > 100) name = name.substring(0, 100);
                if (kind == 0 && !name.endsWith(".rom") && !name.endsWith(".image")) name += ".rom";
                if (!directory.mkdirs()) throw new IOException("Cannot create import folder");
                try (InputStream input = getContentResolver().openInputStream(uri);
                     FileOutputStream output = new FileOutputStream(partial)) {
                    if (input == null) throw new IOException("Cannot read selected file");
                    byte[] buffer = new byte[65536]; long size = 0; int count;
                    while ((count = input.read(buffer)) != -1) {
                        size += count;
                        if (size > 64L * 1024 * 1024) throw new IOException("File exceeds 64 MiB");
                        output.write(buffer, 0, count);
                    }
                    if (size < (kind == 0 ? 1024 : 1)) throw new IOException("Selected file is too small");
                    output.getFD().sync();
                }
                File done = new File(directory, name);
                Files.move(partial.toPath(), done.toPath());
                imported(done.getAbsolutePath(), null);
            } catch (Exception e) {
                partial.delete(); directory.delete();
                imported(null, e.getMessage() == null ? "Could not import selected file" : e.getMessage());
            }
        }, "file-import").start();
    }

    /*
     * Immersive, and sticky: a swipe brings the bars back for a moment and
     * they withdraw again, rather than permanently reclaiming a strip of the
     * guest's screen.
     */
    private void hideSystemBars() {
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            WindowInsetsController bars = getWindow().getInsetsController();
            if (bars != null) {
                bars.hide(WindowInsets.Type.systemBars());
                bars.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }

    /* The bars come back after a dialog or a task switch; put them away. */
    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (focused) hideSystemBars();
    }

    /*
     * Back first navigates the native menus. With no menu open it closes
     * the emulator cleanly, saving the machine on the way out.
     */
    private void close() {
        if (!isFinishing()) command(1);
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
            if (event.getAction() == KeyEvent.ACTION_UP) command(1);
            return true;
        }
        return super.dispatchKeyEvent(event);
    }
    @Override public void onBackPressed() { command(1); }
    @Override protected void onDestroy() {
        super.onDestroy(); // SDL joins the native thread, including its final state save.
        android.os.Process.killProcess(android.os.Process.myPid());
    }
}
