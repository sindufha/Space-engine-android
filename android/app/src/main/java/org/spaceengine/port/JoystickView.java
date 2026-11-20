package org.spaceengine.port;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.View;

/** Virtual analog stick. Reports normalized x (right +) and y (up +) in [-1, 1]. */
public class JoystickView extends View {

    public interface Listener {
        void onMove(float x, float y);
    }

    private final Paint basePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint ringPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint knobPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint knobRing = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint hintPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private Listener listener;
    private int activePointer = -1;
    private float knobX = 0f; // normalized
    private float knobY = 0f;

    public JoystickView(Context context) {
        this(context, null);
    }

    public JoystickView(Context context, AttributeSet attrs) {
        super(context, attrs);
        float d = getResources().getDisplayMetrics().density;
        basePaint.setStyle(Paint.Style.FILL);
        basePaint.setColor(0x66101826);
        ringPaint.setStyle(Paint.Style.STROKE);
        ringPaint.setStrokeWidth(2f * d);
        ringPaint.setColor(0xCCFFFFFF);
        knobPaint.setStyle(Paint.Style.FILL);
        knobPaint.setColor(0xCC5AA9FF);
        knobRing.setStyle(Paint.Style.STROKE);
        knobRing.setStrokeWidth(2f * d);
        knobRing.setColor(0xFFFFFFFF);
        hintPaint.setColor(0xCCFFFFFF);
        hintPaint.setTextAlign(Paint.Align.CENTER);
        hintPaint.setTextSize(14f * d);
        hintPaint.setFakeBoldText(true);
    }

    public void setListener(Listener l) {
        listener = l;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        float w = getWidth();
        float h = getHeight();
        float cx = w / 2f;
        float cy = h / 2f;
        float r = Math.min(w, h) / 2f - ringPaint.getStrokeWidth();
        canvas.drawCircle(cx, cy, r, basePaint);
        canvas.drawCircle(cx, cy, r, ringPaint);
        float off = r * 0.78f;
        float ty = hintPaint.getTextSize() / 3f;
        canvas.drawText("\u25B2", cx, cy - off + ty, hintPaint);
        canvas.drawText("\u25BC", cx, cy + off + ty, hintPaint);
        canvas.drawText("\u25C0", cx - off, cy + ty, hintPaint);
        canvas.drawText("\u25B6", cx + off, cy + ty, hintPaint);
        float kr = r * 0.38f;
        float kx = cx + knobX * (r - kr);
        float ky = cy + knobY * (r - kr);
        canvas.drawCircle(kx, ky, kr, knobPaint);
        canvas.drawCircle(kx, ky, kr, knobRing);
    }

    private void update(MotionEvent e, int index) {
        float w = getWidth();
        float h = getHeight();
        float r = Math.min(w, h) / 2f;
        float dx = (e.getX(index) - w / 2f) / r;
        float dy = (e.getY(index) - h / 2f) / r;
        float len = (float) Math.sqrt(dx * dx + dy * dy);
        if (len > 1f) {
            dx /= len;
            dy /= len;
        }
        knobX = dx;
        knobY = dy;
        invalidate();
        if (listener != null) {
            listener.onMove(dx, -dy);
        }
    }

    private void reset() {
        activePointer = -1;
        knobX = 0f;
        knobY = 0f;
        invalidate();
        if (listener != null) {
            listener.onMove(0f, 0f);
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                if (activePointer == -1) {
                    int idx = e.getActionIndex();
                    activePointer = e.getPointerId(idx);
                    update(e, idx);
                }
                return true;
            case MotionEvent.ACTION_MOVE: {
                int idx = e.findPointerIndex(activePointer);
                if (idx >= 0) {
                    update(e, idx);
                }
                return true;
            }
            case MotionEvent.ACTION_POINTER_UP:
                if (e.getPointerId(e.getActionIndex()) == activePointer) {
                    reset();
                }
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                reset();
                return true;
            default:
                return true;
        }
    }

    @Override
    protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
        reset();
    }
}
