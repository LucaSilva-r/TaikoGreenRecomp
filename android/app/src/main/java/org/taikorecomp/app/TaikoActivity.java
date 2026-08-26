package org.taikorecomp.app;

import android.app.ActivityManager;
import android.content.Context;
import android.os.Bundle;
import android.view.View;

import java.io.File;

import org.libsdl.app.SDLActivity;

public final class TaikoActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        File files = getExternalFilesDir(null);
        File vfs = null;
        File shaderCache = null;
        if (files != null) {
            vfs = new File(files, "vfs");
            vfs.mkdirs();
            shaderCache = new File(files, "shader-cache");
            shaderCache.mkdirs();
        }
        super.onCreate(savedInstanceState);

        // SDL imports SDL_ENV manifest metadata during SDL_Init. Taiko's
        // headless proof starts its guest loader before SDL_Init, so publish
        // the same environment as soon as SDL's native library is loaded.
        if (vfs != null) {
            nativeSetenv("PS3_VFS_ROOT", vfs.getAbsolutePath());
        }
        if (shaderCache != null) {
            nativeSetenv("TAIKO_SHADER_CACHE", shaderCache.getAbsolutePath());
            ActivityManager manager = (ActivityManager)
                getSystemService(Context.ACTIVITY_SERVICE);
            ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
            if (manager != null) {
                manager.getMemoryInfo(memory);
            }
            // Android DXC peaked beyond 1.9 GiB on the 4 GiB Redmi proof
            // device while compiling one shader. On constrained devices,
            // save misses as HLSL for host compilation instead of inviting an
            // LMK kill. Devices with at least 7 GiB may compile misses locally.
            if (manager == null || memory.totalMem < 7L * 1024L * 1024L * 1024L) {
                nativeSetenv("TAIKO_SHADER_CACHE_READONLY", "1");
            }
        }
        nativeSetenv("PS3_VFS_LAYOUT", "usrdir");
        nativeSetenv("TAIKO_DNS_LOOPBACK", "1");
        nativeSetenv("TAIKO_OFFLINE_COMPLETE", "1");
        nativeSetenv("TAIKO_BOOT_VBLANK_HZ", "120");
        nativeSetenv("TAIKO_PERF_OVERLAY", "1");
        nativeSetenv("SDL_ORIENTATIONS", "LandscapeLeft LandscapeRight");

        getWindow().getDecorView().setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
            View.SYSTEM_UI_FLAG_FULLSCREEN |
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
            View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }
}
