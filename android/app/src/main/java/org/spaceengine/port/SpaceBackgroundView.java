package org.spaceengine.port;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.util.AttributeSet;
import android.view.View;

import java.util.Random;

/**
 * Custom deep-space background for the portrait launcher:
 * starfield, glowing galaxy/nebula core, and a ringed planet.
 */
public class SpaceBackgroundView extends View {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private float[] starX;
    private float[] starY;
    private float[] starR;
    private int[] starA;
    private int cachedW = 0;
    private int cachedH = 0;

    public SpaceBackgroundView(Context context) {
        super(context);
    }

    public SpaceBackgroundView(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    public SpaceBackgroundView(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
    }

    private void ensureStars(int w, int h) {
        if (w <= 0 || h <= 0 || (w == cachedW && h == cachedH && starX != null)) {
            return;
        }
        cachedW = w;
        cachedH = h;
        int count = 140;
        starX = new float[count];
        starY = new float[count];
        starR = new float[count];
        starA = new int[count];
        Random rng = new Random(42095L);
        for (int i = 0; i < count; i++) {
            starX[i] = rng.nextFloat() * w;
            starY[i] = rng.nextFloat() * h;
            starR[i] = 0.8f + rng.nextFloat() * 2.0f;
            starA[i] = 90 + rng.nextInt(165);
        }
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        int w = getWidth();
        int h = getHeight();
        if (w <= 0 || h <= 0) return;

        ensureStars(w, h);

        // Deep space vertical gradient
        paint.setShader(new LinearGradient(
                0, 0, 0, h,
                new int[]{0xFF040711, 0xFF0A1128, 0xFF050814},
                new float[]{0f, 0.52f, 1f},
                Shader.TileMode.CLAMP));
        canvas.drawRect(0, 0, w, h, paint);

        // Galaxy / Nebula glow (upper-left)
        float gx = w * 0.25f;
        float gy = h * 0.22f;
        float gr = Math.max(w, h) * 0.55f;
        paint.setShader(new RadialGradient(
                gx, gy, gr,
                new int[]{0x554E7BFF, 0x288C52FF, 0x00000000},
                new float[]{0f, 0.42f, 1f},
                Shader.TileMode.CLAMP));
        canvas.drawCircle(gx, gy, gr, paint);

        // Secondary warm galaxy core (lower-right)
        float gx2 = w * 0.82f;
        float gy2 = h * 0.72f;
        float gr2 = Math.max(w, h) * 0.42f;
        paint.setShader(new RadialGradient(
                gx2, gy2, gr2,
                new int[]{0x38FF9D5C, 0x185B3FD9, 0x00000000},
                new float[]{0f, 0.45f, 1f},
                Shader.TileMode.CLAMP));
        canvas.drawCircle(gx2, gy2, gr2, paint);
        paint.setShader(null);

        // Stars
        for (int i = 0; i < starX.length; i++) {
            paint.setColor(Color.argb(starA[i], 225, 236, 255));
            canvas.drawCircle(starX[i], starY[i], starR[i], paint);
        }

        // Ringed Planet (upper-right)
        float px = w * 0.78f;
        float py = h * 0.20f;
        float pr = Math.min(w, h) * 0.16f;

        // Back ring
        canvas.save();
        canvas.rotate(-22f, px, py);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(pr * 0.16f);
        paint.setColor(0x558FB8FF);
        RectF ringRect = new RectF(px - pr * 1.95f, py - pr * 0.48f, px + pr * 1.95f, py + pr * 0.48f);
        canvas.drawArc(ringRect, 180f, 180f, false, paint);
        canvas.restore();

        // Planet body
        paint.setStyle(Paint.Style.FILL);
        paint.setShader(new RadialGradient(
                px - pr * 0.35f, py - pr * 0.35f, pr * 1.45f,
                new int[]{0xFF88B4FF, 0xFF3359A8, 0xFF0D1733},
                new float[]{0f, 0.55f, 1f},
                Shader.TileMode.CLAMP));
        canvas.drawCircle(px, py, pr, paint);
        paint.setShader(null);

        // Front ring
        canvas.save();
        canvas.rotate(-22f, px, py);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(pr * 0.16f);
        paint.setColor(0x88B8D4FF);
        canvas.drawArc(ringRect, 0f, 180f, false, paint);
        canvas.restore();
        paint.setStyle(Paint.Style.FILL);

        // Small moon
        float mx = w * 0.16f;
        float my = h * 0.78f;
        float mr = Math.min(w, h) * 0.045f;
        paint.setShader(new RadialGradient(
                mx - mr * 0.3f, my - mr * 0.3f, mr * 1.3f,
                new int[]{0xFFD6E0F5, 0xFF5A6887, 0xFF141B2D},
                new float[]{0f, 0.6f, 1f},
                Shader.TileMode.CLAMP));
        canvas.drawCircle(mx, my, mr, paint);
        paint.setShader(null);
    }
}
