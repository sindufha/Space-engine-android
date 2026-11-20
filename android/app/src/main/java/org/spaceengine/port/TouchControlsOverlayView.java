package org.spaceengine.port;

import android.content.Context;
import android.text.InputType;
import android.util.AttributeSet;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;

/**
 * Full-screen Landscape touch & collapsible mobile HUD overlay shown once SpaceEngine enters planetarium mode.
 */
public class TouchControlsOverlayView extends FrameLayout {

    private static final int VK_BACK = 0x08;
    private static final int VK_RETURN = 0x0D;
    private static final int VK_ESCAPE = 0x1B;
    private static final int VK_SPACE = 0x20;
    private static final int VK_A = 0x41;
    private static final int VK_C = 0x43;
    private static final int VK_D = 0x44;
    private static final int VK_G = 0x47;
    private static final int VK_J = 0x4A;
    private static final int VK_L = 0x4C;
    private static final int VK_S = 0x53;
    private static final int VK_W = 0x57;
    private static final int VK_Z = 0x5A;
    private static final int VK_F3 = 0x72;

    private final ScaleGestureDetector scaleDetector;
    private View hudBar;
    private boolean keyW, keyA, keyS, keyD;
    private Button toggleHudButton;
    private Button dragModeButton;
    private boolean hudExpanded = true;
    private boolean orbitDragMode = false;
    private int activeTouchButton = -1; // -1=none, 0=LMB, 3=RMB
    private int renderWidth = 0;
    private int renderHeight = 0;

    public void setRenderDimensions(int width, int height) {
        if (width > 0 && height > 0) {
            this.renderWidth = width;
            this.renderHeight = height;
        }
    }

    private float mapX(float rawX) {
        int vw = getWidth();
        if (vw > 0 && renderWidth > 0 && renderWidth != vw) {
            return rawX * ((float) renderWidth / (float) vw);
        }
        return rawX;
    }

    private float mapY(float rawY) {
        int vh = getHeight();
        if (vh > 0 && renderHeight > 0 && renderHeight != vh) {
            return rawY * ((float) renderHeight / (float) vh);
        }
        return rawY;
    }

    public TouchControlsOverlayView(Context context) {
        this(context, null);
    }

    public TouchControlsOverlayView(Context context, AttributeSet attrs) {
        this(context, attrs, 0);
    }

    public TouchControlsOverlayView(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        LayoutInflater.from(context).inflate(R.layout.view_touch_hud, this, true);
        setFocusable(true);
        setFocusableInTouchMode(true);
        bindButtons();

        scaleDetector = new ScaleGestureDetector(
            context,
            new ScaleGestureDetector.SimpleOnScaleGestureListener() {
                @Override
                public boolean onScale(ScaleGestureDetector detector) {
                    float factor = detector.getScaleFactor();
                    NativeEngineBridge.nativeSendTouch(
                        MotionEvent.ACTION_MOVE,
                        mapX(detector.getFocusX()),
                        mapY(detector.getFocusY()),
                        2,
                        (factor - 1.0f) * 3.0f);
                    return true;
                }
            });
    }

    private void bindButtons() {
        hudBar = findViewById(R.id.hudActionBar);
        toggleHudButton = findViewById(R.id.btnToggleHud);
        dragModeButton = findViewById(R.id.btnHudDragMode);

        toggleHudButton.setOnClickListener(v -> {
            hudExpanded = !hudExpanded;
            hudBar.setVisibility(hudExpanded ? View.VISIBLE : View.GONE);
        });

        if (dragModeButton != null) {
            dragModeButton.setOnClickListener(v -> {
                orbitDragMode = !orbitDragMode;
                dragModeButton.setText(orbitDragMode ? R.string.hud_mode_orbit : R.string.hud_mode_look);
            });
        }

        JoystickView stick = findViewById(R.id.hudJoystick);
        if (stick != null) {
            stick.setListener(this::onStickMoved);
        }
        View kb = findViewById(R.id.btnHudKeyboard);
        if (kb != null) {
            kb.setOnClickListener(v -> toggleKeyboard());
        }

        bindVirtualKeyButton(R.id.btnHudStop, VK_Z);
        bindVirtualKeyButton(R.id.btnHudCenter, VK_C);
        bindVirtualKeyButton(R.id.btnHudGoTo, VK_G);
        View find = findViewById(R.id.btnHudFind);
        if (find != null) {
            find.setOnClickListener(v -> {
                tapKey(VK_F3);
                // Find window has a text box: bring up the soft keyboard for it.
                postDelayed(this::showKeyboard, 250);
            });
        }
        bindVirtualKeyButton(R.id.btnHudTimeSlow, VK_J);
        bindVirtualKeyButton(R.id.btnHudPause, VK_SPACE);
        bindVirtualKeyButton(R.id.btnHudTimeFast, VK_L);
    }

    private void bindHoldVirtualKeyButton(int viewId, final int vkCode) {
        View btn = findViewById(viewId);
        if (btn == null) {
            return;
        }
        btn.setOnTouchListener((v, event) -> {
            int action = event.getActionMasked();
            if (action == MotionEvent.ACTION_DOWN) {
                v.setPressed(true);
                NativeEngineBridge.nativeSendKey(vkCode, true);
                return true;
            } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
                v.setPressed(false);
                NativeEngineBridge.nativeSendKey(vkCode, false);
                return true;
            }
            return false;
        });
    }

    private void bindVirtualKeyButton(int viewId, final int vkCode) {
        View btn = findViewById(viewId);
        if (btn == null) {
            return;
        }
        btn.setOnClickListener(v -> {
            NativeEngineBridge.nativeSendKey(vkCode, true);
            NativeEngineBridge.nativeSendKey(vkCode, false);
        });
    }

    private void tapKey(int vk) {
        NativeEngineBridge.nativeSendKey(vk, true);
        NativeEngineBridge.nativeSendKey(vk, false);
    }

    private void setMoveKey(int vk, boolean wanted, boolean current) {
        if (wanted != current) {
            NativeEngineBridge.nativeSendKey(vk, wanted);
        }
    }

    private void onStickMoved(float x, float y) {
        // Hysteresis: engage above 0.35, release below 0.2.
        boolean w = y > (keyW ? 0.2f : 0.35f);
        boolean s = y < -(keyS ? 0.2f : 0.35f);
        boolean d = x > (keyD ? 0.2f : 0.35f);
        boolean a = x < -(keyA ? 0.2f : 0.35f);
        setMoveKey(VK_W, w, keyW);
        setMoveKey(VK_S, s, keyS);
        setMoveKey(VK_D, d, keyD);
        setMoveKey(VK_A, a, keyA);
        keyW = w;
        keyS = s;
        keyD = d;
        keyA = a;
    }

    private boolean keyboardShown = false;

    private void showKeyboard() {
        requestFocus();
        InputMethodManager imm = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        if (imm != null) {
            imm.restartInput(this);
            imm.showSoftInput(this, InputMethodManager.SHOW_FORCED);
            keyboardShown = true;
        }
    }

    private void hideKeyboard() {
        InputMethodManager imm = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        if (imm != null) {
            imm.hideSoftInputFromWindow(getWindowToken(), 0);
        }
        keyboardShown = false;
    }

    private void toggleKeyboard() {
        if (keyboardShown) {
            hideKeyboard();
        } else {
            showKeyboard();
        }
    }

    @Override
    public boolean onCheckIsTextEditor() {
        return true;
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT
            | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD;
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI
            | EditorInfo.IME_FLAG_NO_FULLSCREEN
            | EditorInfo.IME_ACTION_DONE;
        return new BaseInputConnection(this, false) {
            @Override
            public boolean commitText(CharSequence text, int newCursorPosition) {
                sendText(text);
                return true;
            }

            @Override
            public boolean setComposingText(CharSequence text, int newCursorPosition) {
                // Keyboards sometimes compose; forward finished characters immediately.
                sendText(text);
                return true;
            }

            @Override
            public boolean deleteSurroundingText(int beforeLength, int afterLength) {
                for (int i = 0; i < Math.max(1, beforeLength); i++) {
                    tapKey(VK_BACK);
                }
                return true;
            }

            @Override
            public boolean performEditorAction(int actionCode) {
                tapKey(VK_RETURN);
                return true;
            }

            @Override
            public boolean sendKeyEvent(KeyEvent event) {
                if (event.getAction() == KeyEvent.ACTION_DOWN) {
                    handleSoftKey(event);
                }
                return true;
            }
        };
    }

    private void sendText(CharSequence text) {
        if (text == null) {
            return;
        }
        for (int i = 0; i < text.length(); i++) {
            char ch = text.charAt(i);
            if (ch == '\n') {
                tapKey(VK_RETURN);
            } else {
                NativeEngineBridge.nativeSendChar(ch);
            }
        }
    }

    private void handleSoftKey(KeyEvent event) {
        int code = event.getKeyCode();
        if (code == KeyEvent.KEYCODE_DEL) {
            tapKey(VK_BACK);
        } else if (code == KeyEvent.KEYCODE_ENTER) {
            tapKey(VK_RETURN);
        } else if (code == KeyEvent.KEYCODE_ESCAPE) {
            tapKey(VK_ESCAPE);
        } else {
            int uc = event.getUnicodeChar();
            if (uc > 0) {
                NativeEngineBridge.nativeSendChar(uc);
            }
        }
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        handleSoftKey(event);
        return true;
    }

    @Override
    protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
        onStickMoved(0f, 0f);
    }

    @Override
    protected void onVisibilityChanged(View changedView, int visibility) {
        super.onVisibilityChanged(changedView, visibility);
        if (visibility != View.VISIBLE) {
            onStickMoved(0f, 0f);
        }
    }

    private void releaseActiveTouchButton(float x, float y) {
        if (activeTouchButton == 0) {
            NativeEngineBridge.nativeSendTouch(1, x, y, 1, 0.0f);
        } else if (activeTouchButton == 3) {
            NativeEngineBridge.nativeSendTouch(4, x, y, 1, 0.0f);
        }
        activeTouchButton = -1;
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        scaleDetector.onTouchEvent(event);
        int action = event.getActionMasked();
        int pointerCount = event.getPointerCount();
        float x = mapX(event.getX());
        float y = mapY(event.getY());

        if (pointerCount >= 2 || scaleDetector.isInProgress()) {
            releaseActiveTouchButton(x, y);
            return true;
        }

        if (action == MotionEvent.ACTION_DOWN) {
            activeTouchButton = orbitDragMode ? 3 : 0;
            NativeEngineBridge.nativeSendTouch(activeTouchButton, x, y, 1, 0.0f);
        } else if (action == MotionEvent.ACTION_MOVE) {
            NativeEngineBridge.nativeSendTouch(2, x, y, 1, 0.0f);
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            if (activeTouchButton != -1) {
                releaseActiveTouchButton(x, y);
            } else {
                NativeEngineBridge.nativeSendTouch(orbitDragMode ? 4 : 1, x, y, 1, 0.0f);
            }
        }
        return true;
    }
}
