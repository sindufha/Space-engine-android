package org.spaceengine.port;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.util.DisplayMetrics;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Full-screen Landscape Launcher Activity hosting the GL SurfaceView, native seSplashWindow
 * loading screen SurfaceView, PE/asset selectors, and immersive landscape touch overlay.
 */
public class LauncherActivity extends Activity
    implements SurfaceHolder.Callback, NativeEngineBridge.EngineCallbacks {

    private static final String PREFS_NAME = "spaceengine_launcher";
    private static final String KEY_EXE_PATH = "exe_path";
    private static final String KEY_GAME_DIR = "game_dir";

    private static final int REQ_PICK_EXE = 1001;
    private static final int REQ_PICK_GAME_DIR = 1002;
    private static final int REQ_STORAGE_PERM = 1003;
    private static final int REQ_MANAGE_ALL_FILES = 1004;

    private final NativeEngineBridge engineBridge = new NativeEngineBridge();
    private final ExecutorService ioExecutor = Executors.newSingleThreadExecutor();

    private SurfaceView glSurfaceView;
    private SplashSurfaceView splashSurfaceView;
    private View launcherContainer;
    private TouchControlsOverlayView touchOverlay;

    private View storageBanner;
    private Button btnGrantStorage;
    private Button btnSelectExe;
    private Button btnSelectGameDir;
    private Button btnStart;
    private ProgressBar launchProgress;
    private TextView txtExeStatus;
    private TextView txtGameDirStatus;
    private TextView txtEngineStatus;
    private TextView txtLandscapeInfo;

    private String selectedExePath;
    private String selectedGameDir;
    private boolean exeValid;
    private boolean gameDirValid;
    private boolean launchBusy;

    private int surfaceWidth = 1200;
    private int surfaceHeight = 540;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        NativeEngineBridge.nativeSetRuntimePaths(getApplicationInfo().nativeLibraryDir);
        org.libsdl.app.SDL.initialize();
        org.libsdl.app.SDL.setupJNI();
        org.libsdl.app.SDL.setContext(getApplicationContext());

        setContentView(R.layout.activity_launcher);
        applyImmersiveFullscreen();
        engineBridge.setCallbacks(this);

        initViews();
        initDefaultLandscapeMetrics();
        restoreSavedSelections();
        updateStoragePermissionBanner();
        refreshStartButtonState();
    }

    @Override
    protected void onResume() {
        super.onResume();
        applyImmersiveFullscreen();
        updateStoragePermissionBanner();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applyImmersiveFullscreen();
        }
    }

    @Override
    protected void onDestroy() {
        NativeEngineBridge.nativeShutdown();
        org.libsdl.app.SDLAudioManager.release(getApplicationContext());
        ioExecutor.shutdownNow();
        super.onDestroy();
    }

    @SuppressWarnings("deprecation")
    private void applyImmersiveFullscreen() {
        Window window = getWindow();
        if (window == null) {
            return;
        }
        WindowManager.LayoutParams lp = window.getAttributes();
        lp.layoutInDisplayCutoutMode =
            WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        window.setAttributes(lp);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false);
            WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                controller.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            window.getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        }
    }

    private void initViews() {
        glSurfaceView = findViewById(R.id.glSurfaceView);
        splashSurfaceView = findViewById(R.id.splashSurfaceView);
        launcherContainer = findViewById(R.id.launcherContainer);
        touchOverlay = findViewById(R.id.touchOverlay);

        storageBanner = findViewById(R.id.storageBanner);
        btnGrantStorage = findViewById(R.id.btnGrantStorage);
        btnSelectExe = findViewById(R.id.btnSelectExe);
        btnSelectGameDir = findViewById(R.id.btnSelectGameDir);
        btnStart = findViewById(R.id.btnStart);
        launchProgress = findViewById(R.id.launchProgress);

        txtExeStatus = findViewById(R.id.txtExeStatus);
        txtGameDirStatus = findViewById(R.id.txtGameDirStatus);
        txtEngineStatus = findViewById(R.id.txtEngineStatus);
        txtLandscapeInfo = findViewById(R.id.txtLandscapeInfo);

        glSurfaceView.getHolder().addCallback(this);
        glSurfaceView.setOnTouchListener((v, event) -> {
            int action = event.getActionMasked();
            int nativeAction = -1;
            if (action == android.view.MotionEvent.ACTION_DOWN) nativeAction = 0;
            else if (action == android.view.MotionEvent.ACTION_UP) nativeAction = 1;
            else if (action == android.view.MotionEvent.ACTION_MOVE) nativeAction = 2;
            else if (action == android.view.MotionEvent.ACTION_CANCEL) nativeAction = 3;
            if (nativeAction >= 0 && v.getWidth() > 0 && v.getHeight() > 0) {
                float sx = event.getX() * ((float) surfaceWidth / (float) v.getWidth());
                float sy = event.getY() * ((float) surfaceHeight / (float) v.getHeight());
                NativeEngineBridge.nativeSendTouch(nativeAction, sx, sy, 0, 0f);
                return true;
            }
            return false;
        });

        btnGrantStorage.setOnClickListener(v -> requestStorageAccess());
        btnSelectExe.setOnClickListener(v -> openExePicker());
        btnSelectGameDir.setOnClickListener(v -> openGameDirPicker());
        btnStart.setOnClickListener(v -> startSpaceEngine());
    }

    private void applyScaledLandscapeSize(int viewW, int viewH) {
        if (viewW <= 0 || viewH <= 0) {
            return;
        }
        if (viewW < viewH) {
            int tmp = viewW;
            viewW = viewH;
            viewH = tmp;
        }

        int rw = viewW;
        int rh = viewH;
        // Downscale high-DPI mobile screens (> 600px height) to ~540p in landscape
        // while keeping exact aspect ratio; SurfaceFlinger upscales the framebuffer cleanly.
        if (viewH > 600) {
            rh = 540;
            rw = ((int) Math.round((double) viewW * 540.0 / (double) viewH)) & ~1;
            if (rw <= rh) {
                rw = rh + 2;
            }
        }
        surfaceWidth = Math.max(rw, 2);
        surfaceHeight = Math.max(rh, 2);
        if (glSurfaceView != null && glSurfaceView.getHolder() != null) {
            glSurfaceView.getHolder().setFixedSize(surfaceWidth, surfaceHeight);
        }
        if (touchOverlay != null) {
            touchOverlay.setRenderDimensions(surfaceWidth, surfaceHeight);
        }
        txtLandscapeInfo.setText(getString(R.string.landscape_viewport_fmt, surfaceWidth, surfaceHeight));
    }

    private void initDefaultLandscapeMetrics() {
        DisplayMetrics dm = new DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(dm);
        applyScaledLandscapeSize(Math.max(dm.widthPixels, 1), Math.max(dm.heightPixels, 1));
    }

    private void restoreSavedSelections() {
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        String savedExe = prefs.getString(KEY_EXE_PATH, null);
        String savedDir = prefs.getString(KEY_GAME_DIR, null);

        if (savedExe != null && !savedExe.isEmpty()) {
            validateExePathAsync(savedExe);
        }
        if (savedDir != null && !savedDir.isEmpty()) {
            validateGameDirAsync(savedDir);
        }
    }

    private boolean hasFullStorageAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            return Environment.isExternalStorageManager();
        }
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)
            == PackageManager.PERMISSION_GRANTED;
    }

    private void updateStoragePermissionBanner() {
        boolean granted = hasFullStorageAccess();
        storageBanner.setVisibility(granted ? View.GONE : View.VISIBLE);
    }

    private void requestStorageAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
            intent.setData(Uri.parse("package:" + getPackageName()));
            startActivityForResult(intent, REQ_MANAGE_ALL_FILES);
        } else {
            requestPermissions(
                new String[] {
                    Manifest.permission.READ_EXTERNAL_STORAGE,
                    Manifest.permission.WRITE_EXTERNAL_STORAGE
                },
                REQ_STORAGE_PERM);
        }
    }

    private void openExePicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, REQ_PICK_EXE);
    }

    private void openGameDirPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(
            Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, REQ_PICK_GAME_DIR);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        updateStoragePermissionBanner();

        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        Uri uri = data.getData();

        if (requestCode == REQ_PICK_EXE) {
            handleSelectedExeUri(uri);
        } else if (requestCode == REQ_PICK_GAME_DIR) {
            handleSelectedGameTreeUri(uri);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        updateStoragePermissionBanner();
    }

    private void handleSelectedExeUri(final Uri uri) {
        exeValid = false;
        selectedExePath = null;
        txtExeStatus.setText(R.string.status_validating_exe);
        refreshStartButtonState();

        ioExecutor.execute(() -> {
            String directPath = tryResolveDocumentPath(uri, false);
            if (directPath != null && new File(directPath).canRead()) {
                String res = NativeEngineBridge.nativeValidateExe(directPath, -1);
                applyExeValidationResult(directPath, res);
                return;
            }

            try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r")) {
                if (pfd == null) {
                    applyExeValidationResult(null, "ERR|Unable to open selected file descriptor");
                    return;
                }
                String res = NativeEngineBridge.nativeValidateExe(null, pfd.getFd());
                if (!res.startsWith("OK|")) {
                    applyExeValidationResult(null, res);
                    return;
                }
                File staged = stageExeIntoInternalStorage(uri);
                applyExeValidationResult(staged.getAbsolutePath(), res);
            } catch (IOException e) {
                applyExeValidationResult(null, "ERR|Failed staging .exe: " + e.getMessage());
            }
        });
    }

    private File stageExeIntoInternalStorage(Uri uri) throws IOException {
        File runtimeDir = new File(getFilesDir(), "runtime");
        if (!runtimeDir.exists() && !runtimeDir.mkdirs()) {
            throw new IOException("Cannot create runtime directory");
        }
        File dst = new File(runtimeDir, "SpaceEngine.exe");
        try (InputStream in = getContentResolver().openInputStream(uri);
             FileOutputStream out = new FileOutputStream(dst)) {
            if (in == null) {
                throw new IOException("Null input stream for selected .exe");
            }
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
        }
        return dst;
    }

    private void handleSelectedGameTreeUri(final Uri treeUri) {
        String resolvedDir = tryResolveDocumentPath(treeUri, true);
        if (resolvedDir == null) {
            gameDirValid = false;
            selectedGameDir = null;
            txtGameDirStatus.setText(R.string.err_tree_path_resolve);
            refreshStartButtonState();
            return;
        }
        validateGameDirAsync(resolvedDir);
    }

    private void validateExePathAsync(final String path) {
        txtExeStatus.setText(R.string.status_validating_exe);
        ioExecutor.execute(() -> {
            String res = NativeEngineBridge.nativeValidateExe(path, -1);
            applyExeValidationResult(path, res);
        });
    }

    private void validateGameDirAsync(final String dirPath) {
        txtGameDirStatus.setText(R.string.status_validating_dir);
        ioExecutor.execute(() -> {
            String res = NativeEngineBridge.nativeValidateGameDir(dirPath);
            applyGameDirValidationResult(dirPath, res);
        });
    }

    private void applyExeValidationResult(final String path, final String nativeResult) {
        final boolean ok = nativeResult != null && nativeResult.startsWith("OK|");
        final String msg = stripResultPrefix(nativeResult);
        runOnUiThread(() -> {
            exeValid = ok;
            selectedExePath = ok ? path : null;
            txtExeStatus.setText(ok ? (path + "\n" + msg) : msg);
            txtExeStatus.setTextColor(getColor(ok ? R.color.status_ok : R.color.status_err));
            if (ok && path != null) {
                getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit().putString(KEY_EXE_PATH, path).apply();
            }
            refreshStartButtonState();
        });
    }

    private void applyGameDirValidationResult(final String dirPath, final String nativeResult) {
        final boolean ok = nativeResult != null && nativeResult.startsWith("OK|");
        final String msg = stripResultPrefix(nativeResult);
        runOnUiThread(() -> {
            gameDirValid = ok;
            selectedGameDir = ok ? dirPath : null;
            txtGameDirStatus.setText(ok ? (dirPath + "\n" + msg) : msg);
            txtGameDirStatus.setTextColor(getColor(ok ? R.color.status_ok : R.color.status_err));
            if (ok && dirPath != null) {
                getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit().putString(KEY_GAME_DIR, dirPath).apply();
            }
            refreshStartButtonState();
        });
    }

    private static String stripResultPrefix(String raw) {
        if (raw == null) {
            return "";
        }
        int bar = raw.indexOf('|');
        return (bar >= 0 && bar + 1 < raw.length()) ? raw.substring(bar + 1) : raw;
    }

    private String tryResolveDocumentPath(Uri uri, boolean isTree) {
        if ("file".equalsIgnoreCase(uri.getScheme())) {
            return uri.getPath();
        }
        if (!"com.android.externalstorage.documents".equals(uri.getAuthority())) {
            return null;
        }
        String docId = isTree
            ? DocumentsContract.getTreeDocumentId(uri)
            : DocumentsContract.getDocumentId(uri);
        if (docId == null) {
            return null;
        }
        int colon = docId.indexOf(':');
        if (colon < 0) {
            return null;
        }
        String volume = docId.substring(0, colon);
        String rel = docId.substring(colon + 1);
        String base = "primary".equalsIgnoreCase(volume)
            ? Environment.getExternalStorageDirectory().getAbsolutePath()
            : ("/storage/" + volume);
        return rel.isEmpty() ? base : (base + "/" + rel);
    }

    private void refreshStartButtonState() {
        boolean canStart = exeValid && gameDirValid && !launchBusy;
        btnStart.setEnabled(canStart);
        btnSelectExe.setEnabled(!launchBusy);
        btnSelectGameDir.setEnabled(!launchBusy);
        if (!launchBusy) {
            btnStart.setText(R.string.btn_start);
            launchProgress.setVisibility(View.GONE);
        }
    }

    private void startSpaceEngine() {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        if (!exeValid || !gameDirValid || launchBusy) {
            return;
        }
        launchBusy = true;
        refreshStartButtonState();
        btnStart.setText(R.string.status_starting);
        launchProgress.setVisibility(View.VISIBLE);
        txtEngineStatus.setText(R.string.status_loading_splash);
        txtEngineStatus.setTextColor(getColor(R.color.text_secondary));

        String internalDir = getFilesDir().getAbsolutePath();
        String res = NativeEngineBridge.nativeStartEngine(
            engineBridge,
            selectedExePath,
            selectedGameDir,
            internalDir,
            surfaceWidth,
            surfaceHeight);

        if (res == null || !res.startsWith("OK|")) {
            onEngineError(stripResultPrefix(res));
            return;
        }

        // Hide the Launcher card and activate the landscape splash SurfaceView immediately so
        // SpaceEngine's own loading banner paints full-screen without delay.
        launcherContainer.setVisibility(View.GONE);
        splashSurfaceView.setVisibility(View.VISIBLE);
    }

    @Override
    public void onEngineStageChanged(final int stage, final String message) {
        runOnUiThread(() -> {
            if (stage == 100) {
                launchBusy = false;
                refreshStartButtonState();
                txtEngineStatus.setText(message);
                txtEngineStatus.setTextColor(getColor(R.color.status_ok));
                return;
            }
            btnStart.setText(getString(R.string.btn_loading_stage_fmt, stage));
            txtEngineStatus.setText(message);
        });
    }

    @Override
    public void onSplashVisibilityChanged(final boolean visible) {
        runOnUiThread(() -> {
            if (visible) {
                launcherContainer.setVisibility(View.GONE);
                splashSurfaceView.setVisibility(View.VISIBLE);
            } else {
                splashSurfaceView.setVisibility(View.GONE);
            }
        });
    }

    @Override
    public void onGameSurfaceVisible() {
        runOnUiThread(() -> launcherContainer.setVisibility(View.GONE));
    }

    @Override
    public void onPlanetariumReady() {
        runOnUiThread(() -> {
            launchBusy = false;
            launchProgress.setVisibility(View.GONE);
            splashSurfaceView.setVisibility(View.GONE);
            launcherContainer.setVisibility(View.GONE);
            touchOverlay.setVisibility(View.VISIBLE);
        });
    }

    @Override
    public void onEngineError(final String message) {
        runOnUiThread(() -> {
            launchBusy = false;
            splashSurfaceView.setVisibility(View.GONE);
            touchOverlay.setVisibility(View.GONE);
            launcherContainer.setVisibility(View.VISIBLE);
            refreshStartButtonState();
            txtEngineStatus.setText(message);
            txtEngineStatus.setTextColor(getColor(R.color.status_err));
        });
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        int w = glSurfaceView.getWidth();
        int h = glSurfaceView.getHeight();
        if (w > 0 && h > 0) {
            applyScaledLandscapeSize(w, h);
        }
        NativeEngineBridge.nativeOnSurfaceCreated(holder.getSurface(), surfaceWidth, surfaceHeight);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        if (width > 0 && height > 0) {
            if (width < height) {
                int tmp = width;
                width = height;
                height = tmp;
            }
            surfaceWidth = width;
            surfaceHeight = height;
            if (touchOverlay != null) {
                touchOverlay.setRenderDimensions(surfaceWidth, surfaceHeight);
            }
            txtLandscapeInfo.setText(getString(R.string.landscape_viewport_fmt, surfaceWidth, surfaceHeight));
        }
        NativeEngineBridge.nativeOnSurfaceChanged(surfaceWidth, surfaceHeight);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        NativeEngineBridge.nativeOnSurfaceDestroyed();
    }
}
