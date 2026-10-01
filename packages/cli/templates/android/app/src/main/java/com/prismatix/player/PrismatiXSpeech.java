package com.prismatix.player;

import android.content.Context;
import android.speech.tts.TextToSpeech;

/**
 * Text-to-speech for the Player's self-voicing setting. The
 * native SpeechService creates one instance through JNI and calls speak()
 * and stop() from the SDL thread; TextToSpeech is thread-safe for those.
 *
 * <p>The engine binds asynchronously. Text spoken before it is ready is
 * dropped, the same as on a desktop without a voice.
 */
public final class PrismatiXSpeech implements TextToSpeech.OnInitListener {
    private final TextToSpeech mEngine;
    private volatile boolean mReady;

    public PrismatiXSpeech(Context context) {
        mEngine = new TextToSpeech(context.getApplicationContext(), this);
    }

    @Override
    public void onInit(int status) {
        mReady = status == TextToSpeech.SUCCESS;
    }

    public boolean isReady() {
        return mReady;
    }

    public void speak(String text, boolean interrupt) {
        if (!mReady || text == null || text.isEmpty()) return;
        mEngine.speak(text, interrupt ? TextToSpeech.QUEUE_FLUSH : TextToSpeech.QUEUE_ADD,
                null, "prismatix");
    }

    public void stop() {
        if (mReady) mEngine.stop();
    }

    public void shutdown() {
        mReady = false;
        mEngine.shutdown();
    }
}
