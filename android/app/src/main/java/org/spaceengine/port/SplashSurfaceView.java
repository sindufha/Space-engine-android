package org.spaceengine.port;

import android.content.Context;
import android.util.AttributeSet;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/**
 * Full-screen landscape SurfaceView backing SpaceEngine's native seSplashWindow
 * (L"SE Splash" / L"Banner") GDI DIB framebuffer while SpaceEngine initializes.
 */
public class SplashSurfaceView extends SurfaceView implements SurfaceHolder.Callback {

    public SplashSurfaceView(Context context) {
        super(context);
        init();
    }

    public SplashSurfaceView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    public SplashSurfaceView(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        init();
    }

    private void init() {
        getHolder().addCallback(this);
        setZOrderMediaOverlay(true);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        NativeEngineBridge.nativeOnSplashSurfaceCreated(
                holder.getSurface(),
                Math.max(1, getWidth()),
                Math.max(1, getHeight()));
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        NativeEngineBridge.nativeOnSplashSurfaceChanged(
                holder.getSurface(),
                width,
                height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        NativeEngineBridge.nativeOnSplashSurfaceDestroyed();
    }
}
