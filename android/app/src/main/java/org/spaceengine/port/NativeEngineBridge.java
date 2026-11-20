package org.spaceengine.port;

import android.view.Surface;

/** JNI bridge between the Landscape Android Launcher UI and native libSpaceEngine.so. */
public final class NativeEngineBridge {

    static {
        System.loadLibrary("SDL2");
        System.loadLibrary("SpaceEngine");
    }

    /** Lifecycle callbacks dispatched from native engine threads. */
    public interface EngineCallbacks {
        void onEngineStageChanged(int stage, String message);
        void onSplashVisibilityChanged(boolean visible);
        void onGameSurfaceVisible();
        void onPlanetariumReady();
        void onEngineError(String message);
    }

    private volatile EngineCallbacks callbacks;

    public void setCallbacks(EngineCallbacks callbacks) {
        this.callbacks = callbacks;
    }

    /** Invoked from JNI during engine initialization stages. */
    public void onEngineStageChanged(int stage, String message) {
        EngineCallbacks cb = callbacks;
        if (cb != null) {
            cb.onEngineStageChanged(stage, message);
        }
    }

    /** Invoked from JNI when native seSplashWindow visibility changes. */
    public void onSplashVisibilityChanged(boolean visible) {
        EngineCallbacks cb = callbacks;
        if (cb != null) {
            cb.onSplashVisibilityChanged(visible);
        }
    }

    /** Invoked from JNI when main engine GL window becomes visible. */
    public void onGameSurfaceVisible() {
        EngineCallbacks cb = callbacks;
        if (cb != null) {
            cb.onGameSurfaceVisible();
        }
    }

    /** Invoked from JNI when loading stage [0x14077b250] == 7 and first frame swaps. */
    public void onPlanetariumReady() {
        EngineCallbacks cb = callbacks;
        if (cb != null) {
            cb.onPlanetariumReady();
        }
    }

    /** Invoked from JNI when native startup encounters a fatal error. */
    public void onEngineError(String message) {
        EngineCallbacks cb = callbacks;
        if (cb != null) {
            cb.onEngineError(message);
        }
    }

    public static native void nativeSetRuntimePaths(String nativeLibDir);

    public static native boolean nativeIsRecompLinked();

    public static native String nativeValidateExe(String path, int fd);

    public static native String nativeValidateGameDir(String dirPath);

    public static native String nativeStartEngine(
        NativeEngineBridge callbackBridge,
        String exePath,
        String gameDir,
        String internalDir,
        int surfaceWidth,
        int surfaceHeight);

    public static native void nativeOnSurfaceCreated(Surface surface, int width, int height);

    public static native void nativeOnSurfaceChanged(int width, int height);

    public static native void nativeOnSurfaceDestroyed();

    public static native void nativeOnSplashSurfaceCreated(Surface surface, int width, int height);

    public static native void nativeOnSplashSurfaceChanged(Surface surface, int width, int height);

    public static native void nativeOnSplashSurfaceDestroyed();

    public static native void nativeSendTouch(int action, float x, float y, int pointerCount, float pinchDelta);

    public static native void nativeSendKey(int keyCode, boolean down);

    public static native void nativeSendChar(int codepoint);

    public static native void nativeShutdown();
}
