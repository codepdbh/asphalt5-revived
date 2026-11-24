package com.androidevolved.asphalt5;

/** Entry points of libasphalt5.so (native/game_host.c). Call only on the GL thread. */
final class GameNative {
    static {
        System.loadLibrary("asphalt5");
    }

    private GameNative() {
    }

    static native boolean nativeBoot(String enginePath, String dataDir, String saveDir,
                                     Class<?> bridge, int language,
                                     int logicalWidth, int logicalHeight);

    static native void nativeSurfaceChanged(int width, int height);

    static native void nativeRender();

    /** action: 0 pressed, 1 moved, 2 released. Coordinates in surface pixels. */
    static native void nativeTouch(int action, float x, float y, int pointer);

    static native void nativeKey(boolean down, int keyCode);

    static native void nativeAccelerometer(float x, float y, float z);

    static native void nativePause(boolean paused);

    static native int nativeCanInterrupt();

    static native void nativeMusicCompleted(int sound);

    /** Asphalt5.nativeCheckwifi: enables the local Wi-Fi multiplayer menu. */
    static native void nativeCheckWifi(boolean active);

    /** Name shown to other players in the local Wi-Fi lobby. */
    static native void nativeSetPlayerName(String name);
}
