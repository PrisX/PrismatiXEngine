package com.prismatix.player;

import org.libsdl.app.SDLActivity;

/**
 * Hosts the PrismatiX Player. UNVERIFIED: not yet run on a device; see
 * docs/android.md.
 *
 * <p>SDL3 is linked statically into libmain.so, so only "main" is loaded
 * here. Android 6+ resolves libmain's shared FFmpeg dependencies from the
 * app's native library directory automatically.
 */
public class PrismatiXActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }
}
