package com.prismatix.player;

import org.libsdl.app.SDLActivity;

/**
 * Hosts the PrismatiX Player. See docs/shipping.md (Android).
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

    /**
     * The manifest's screenOrientation (distribution.android.orientation)
     * decides. SDL would otherwise switch a resizable window to FULL_USER
     * and let a landscape game turn portrait.
     */
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
    }
}
