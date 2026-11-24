package com.androidevolved.asphalt5;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.graphics.Rect;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.Environment;
import android.provider.Settings;
import android.widget.Button;
import android.widget.LinearLayout;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Log;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.util.Collections;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public final class GameActivity extends Activity implements SensorEventListener {
    private static final String TAG = "Asphalt5";
    private static final long FRAME_NANOS = 1_000_000_000L / 60;

    private GLSurfaceView view;
    private SensorManager sensors;
    private volatile boolean failed;
    private WifiManager.MulticastLock multicastLock;
    private ConnectivityManager.NetworkCallback networkCallback;

    /** Original Gameloft location on shared internal storage. */
    static File sharedDataDir() {
        return new File(Environment.getExternalStorageDirectory(), "gameloft/games/asphalt5");
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        WindowManager.LayoutParams attributes = getWindow().getAttributes();
        attributes.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        getWindow().setAttributes(attributes);
        tryStart();
    }

    /** Starts the game once storage access and data are available. */
    private void tryStart() {
        if (view != null) return;
        if (!Environment.isExternalStorageManager()) {
            showPermissionRequest();
            return;
        }
        File data = sharedDataDir();
        if (!new File(data, "gamecfg.bar").isFile()) {
            showMessage("Faltan los datos del juego.\n\nCopia el contenido de la carpeta asphalt5 a la "
                    + "memoria interna, en:\n" + data.getAbsolutePath()
                    + "\n\nCon ADB:\nadb push asphalt5 /sdcard/gameloft/games/");
            return;
        }
        File engine;
        try {
            engine = extractEngine();
        } catch (IOException error) {
            Log.e(TAG, "engine extraction failed", error);
            showMessage("No se pudo preparar el motor: " + error.getMessage());
            return;
        }
        File saves = new File(getFilesDir(), "save");
        if (!saves.isDirectory() && !saves.mkdirs()) Log.w(TAG, "cannot create " + saves);
        GameBridge.activity = this;
        GameBridge.dataDir = data.getAbsolutePath();

        view = new GLSurfaceView(this);
        view.setEGLContextClientVersion(1);
        view.setEGLConfigChooser(8, 8, 8, 8, 24, 8);
        view.setPreserveEGLContextOnPause(true);
        int[] logical = logicalSize();
        Log.i(TAG, "logical canvas " + logical[0] + "x" + logical[1]);
        view.setRenderer(new Renderer(engine, data, saves, logical[0], logical[1]));
        view.setFocusable(true);
        view.setFocusableInTouchMode(true);
        setContentView(view);
        view.requestFocus();
        sensors = (SensorManager) getSystemService(Context.SENSOR_SERVICE);
        hideSystemBars();
    }

    /**
     * The engine lays out its UI for the resolution Java reports, as it did on
     * 800x480, 854x480 and 1024x600 devices. Keep its 480-line layout and
     * widen it to the screen's aspect ratio so the image fills the display.
     */
    private int[] logicalSize() {
        Rect bounds = getWindowManager().getMaximumWindowMetrics().getBounds();
        int width = Math.max(bounds.width(), bounds.height());
        int height = Math.min(bounds.width(), bounds.height());
        int logicalHeight = 480;
        int logicalWidth = Math.round(logicalHeight * (float) width / height) & ~1;
        logicalWidth = Math.max(640, Math.min(1280, logicalWidth));
        return new int[] {logicalWidth, logicalHeight};
    }

    /**
     * Asphalt5.isWifiActive: Wi-Fi transport, or this phone hosting a hotspot
     * (an active wlan/ap/swlan interface with an IPv4 address).
     */
    boolean isWifiActive() {
        ConnectivityManager connectivity = getSystemService(ConnectivityManager.class);
        for (Network network : connectivity.getAllNetworks()) {
            NetworkCapabilities caps = connectivity.getNetworkCapabilities(network);
            if (caps != null && caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) return true;
        }
        try {
            for (NetworkInterface nif : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                String name = nif.getName();
                if (!nif.isUp() || nif.isLoopback()
                        || !(name.startsWith("wlan") || name.startsWith("ap") || name.startsWith("swlan"))) {
                    continue;
                }
                for (InetAddress address : Collections.list(nif.getInetAddresses())) {
                    if (address instanceof Inet4Address) return true;
                }
            }
        } catch (Exception error) {
            Log.w(TAG, "interface scan failed", error);
        }
        return false;
    }

    /** Device name from Settings (e.g. "Galaxy S25 Ultra"), else the model. */
    String playerName() {
        String name = Settings.Global.getString(getContentResolver(), Settings.Global.DEVICE_NAME);
        if (name == null || name.trim().isEmpty()) name = android.os.Build.MODEL;
        name = name.replaceAll("[^ -~]", "").trim();   // printable ASCII only
        if (name.isEmpty()) name = "Player";
        return name.length() > 15 ? name.substring(0, 15) : name;
    }

    void reportWifi() {
        boolean active = isWifiActive();
        Log.i(TAG, "wifi active: " + active);
        queueGameEvent(() -> GameNative.nativeCheckWifi(active));
    }

    private void startNetworking() {
        WifiManager wifi = getApplicationContext().getSystemService(WifiManager.class);
        if (multicastLock == null && wifi != null) {
            // Without it many phones drop the broadcast packets of game discovery.
            multicastLock = wifi.createMulticastLock("asphalt5-lan");
            multicastLock.setReferenceCounted(false);
        }
        if (multicastLock != null) multicastLock.acquire();
        if (networkCallback == null) {
            networkCallback = new ConnectivityManager.NetworkCallback() {
                @Override
                public void onAvailable(Network network) {
                    reportWifi();
                }

                @Override
                public void onLost(Network network) {
                    reportWifi();
                }
            };
            getSystemService(ConnectivityManager.class).registerDefaultNetworkCallback(networkCallback);
        }
        reportWifi();
    }

    private void stopNetworking() {
        if (multicastLock != null && multicastLock.isHeld()) multicastLock.release();
        if (networkCallback != null) {
            getSystemService(ConnectivityManager.class).unregisterNetworkCallback(networkCallback);
            networkCallback = null;
        }
    }

    private void showPermissionRequest() {
        Button grant = new Button(this);
        grant.setText("Permitir acceso a los archivos");
        grant.setOnClickListener(v -> {
            Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            startActivity(intent);
        });
        TextView text = new TextView(this);
        text.setTextSize(18);
        text.setText("Asphalt 5 lee sus datos de la memoria interna, en\n"
                + sharedDataDir().getAbsolutePath()
                + "\n\nAndroid exige el permiso \"Acceso a todos los archivos\" para leer esa carpeta.");
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(48, 48, 48, 48);
        layout.addView(text);
        layout.addView(grant);
        setContentView(layout);
    }

    private void showMessage(String text) {
        TextView message = new TextView(this);
        message.setPadding(48, 48, 48, 48);
        message.setTextSize(18);
        message.setText(text);
        setContentView(message);
    }

    private File extractEngine() throws IOException {
        File engine = new File(getFilesDir(), "libasphalt5-armeabi.so");
        long expected;
        try (android.content.res.AssetFileDescriptor fd = getAssets().openFd("engine.bin")) {
            expected = fd.getLength();
        }
        if (engine.length() == expected) return engine;
        File temporary = new File(getFilesDir(), "engine.tmp");
        try (InputStream in = getAssets().open("engine.bin");
                OutputStream out = new FileOutputStream(temporary)) {
            in.transferTo(out);
        }
        if (!temporary.renameTo(engine)) throw new IOException("rename failed");
        return engine;
    }

    private void hideSystemBars() {
        getWindow().setDecorFitsSystemWindows(false);
        WindowInsetsController controller = getWindow().getInsetsController();
        if (controller != null) {
            controller.hide(WindowInsets.Type.systemBars());
            controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    void queueGameEvent(Runnable event) {
        GLSurfaceView v = view;
        if (v != null && !failed) v.queueEvent(event);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (view == null) return super.onTouchEvent(event);
        int masked = event.getActionMasked();
        int index = event.getActionIndex();
        switch (masked) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                queueTouch(0, event.getX(index), event.getY(index), event.getPointerId(index));
                break;
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < event.getPointerCount(); i++) {
                    queueTouch(1, event.getX(i), event.getY(i), event.getPointerId(i));
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL:
                queueTouch(2, event.getX(index), event.getY(index), event.getPointerId(index));
                break;
            default:
                break;
        }
        return true;
    }

    private void queueTouch(int action, float x, float y, int pointer) {
        queueGameEvent(() -> GameNative.nativeTouch(action, x, y, pointer));
    }

    private static boolean systemKey(int keyCode) {
        return keyCode == KeyEvent.KEYCODE_VOLUME_UP || keyCode == KeyEvent.KEYCODE_VOLUME_DOWN
                || keyCode == KeyEvent.KEYCODE_CAMERA;
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (view == null || systemKey(keyCode)) return super.onKeyDown(keyCode, event);
        if (event.getRepeatCount() == 0) queueGameEvent(() -> GameNative.nativeKey(true, keyCode));
        return true;
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (view == null || systemKey(keyCode)) return super.onKeyUp(keyCode, event);
        queueGameEvent(() -> GameNative.nativeKey(false, keyCode));
        return true;
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        float x = event.values[0], y = event.values[1], z = event.values[2];
        queueGameEvent(() -> GameNative.nativeAccelerometer(x, y, z));
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (view == null) tryStart();   // returning from the permission screen
        if (view == null) return;
        hideSystemBars();
        view.onResume();
        queueGameEvent(() -> GameNative.nativePause(false));
        GameBridge.resumeAllAudio();
        startNetworking();
        Sensor accelerometer = sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
        if (accelerometer != null) sensors.registerListener(this, accelerometer, SensorManager.SENSOR_DELAY_GAME);
    }

    @Override
    protected void onPause() {
        if (view != null) {
            sensors.unregisterListener(this);
            stopNetworking();
            GameBridge.pauseAllAudio();
            queueGameEvent(() -> GameNative.nativePause(true));
            view.onPause();
        }
        super.onPause();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && view != null) hideSystemBars();
    }

    private final class Renderer implements GLSurfaceView.Renderer {
        private final File engine;
        private final File data;
        private final File saves;
        private final int logicalWidth;
        private final int logicalHeight;
        private long nextFrame;

        Renderer(File engine, File data, File saves, int logicalWidth, int logicalHeight) {
            this.engine = engine;
            this.data = data;
            this.saves = saves;
            this.logicalWidth = logicalWidth;
            this.logicalHeight = logicalHeight;
        }

        @Override
        public void onSurfaceCreated(GL10 gl, EGLConfig config) {
            if (failed) return;
            GameNative.nativeSetPlayerName(playerName());
            if (!GameNative.nativeBoot(engine.getAbsolutePath(), data.getAbsolutePath(),
                    saves.getAbsolutePath(), GameBridge.class, 0, logicalWidth, logicalHeight)) {
                failed = true;
                runOnUiThread(() -> showMessage("El motor no pudo arrancar. Revisa logcat (Asphalt5)."));
                return;
            }
            GameNative.nativeCheckWifi(isWifiActive());
        }

        @Override
        public void onSurfaceChanged(GL10 gl, int width, int height) {
            if (!failed) GameNative.nativeSurfaceChanged(width, height);
        }

        @Override
        public void onDrawFrame(GL10 gl) {
            if (failed) return;
            GameNative.nativeRender();
            // The engine's smoothing assumes a bounded frame rate (60 FPS cap).
            long now = System.nanoTime();
            if (nextFrame == 0 || now - nextFrame > FRAME_NANOS * 4) nextFrame = now;
            nextFrame += FRAME_NANOS;
            long wait = nextFrame - System.nanoTime();
            if (wait > 0) SystemClock.sleep(wait / 1_000_000L);
        }
    }
}
