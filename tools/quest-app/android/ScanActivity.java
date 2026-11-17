package __PACKAGE__;

import android.Manifest;
import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.ImageFormat;
import android.graphics.Typeface;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.provider.Settings;
import android.text.InputFilter;
import android.text.InputType;
import android.util.Log;
import android.util.Size;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import com.google.zxing.BarcodeFormat;
import com.google.zxing.BinaryBitmap;
import com.google.zxing.DecodeHintType;
import com.google.zxing.PlanarYUVLuminanceSource;
import com.google.zxing.ReaderException;
import com.google.zxing.common.HybridBinarizer;
import com.google.zxing.qrcode.QRCodeReader;

import java.nio.ByteBuffer;
import java.util.Collections;
import java.util.EnumMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * The VisIVO VR app's first screen (issue #383): a 2-D panel that reads the
 * QR code of the desktop's Open on Headset dialog with the headset's
 * passthrough camera, or takes the code typed, then starts the immersive
 * part (LauncherActivity → the loader, which pairs and enters VR by itself).
 *
 * The camera: Horizon OS's Passthrough Camera API, plain Camera2 on a
 * Quest 3 / 3S with Horizon OS v74 or later. Meta documents either
 * android.permission.CAMERA or horizonos.permission.HEADSET_CAMERA as
 * enough, but on a Quest 3 the camera list showed no passthrough camera
 * until HEADSET_CAMERA was granted in the settings (issue #386). So the
 * camera starts only with HEADSET_CAMERA; both are asked for; when the
 * system grants neither (or shows no dialog), the panel names the setting
 * and opens the app's settings page, and checks again when it is back on
 * top. "No camera" is said only when nothing is missing. The forward RGB
 * cameras carry the vendor tag com.meta.extra_metadata.camera_source = 0;
 * the left one (position 0) is used. Only the luminance plane is read: the
 * QR decoder (ZXing) needs nothing else, and the viewfinder is grey.
 * Nothing is kept or sent: each frame is decoded and dropped.
 */
public class ScanActivity extends Activity {
    static final String HEADSET_CAMERA = "horizonos.permission.HEADSET_CAMERA";
    static final String TAG = "VisIVO-VR";
    static final int ASK = 383;
    /** Where the user turns the permission on (Horizon OS's settings). */
    static final String SETTING = "Settings ▸ Privacy & safety ▸ App permissions ▸ Headset cameras";
    /** The viewfinder's refresh period (ms): enough to aim. */
    static final long PREVIEW_MS = 100;

    private ImageView viewfinder;
    private TextView status;
    private Button settingsButton;
    private EditText codeField;
    private String loaderStart;

    // Set on the UI thread, read on the camera's (volatile).
    private volatile HandlerThread cameraThread, decodeThread;
    private volatile Handler cameraHandler, decodeHandler;
    private volatile CameraDevice device;
    private volatile CameraCaptureSession session;
    private volatile ImageReader reader;
    private final AtomicBoolean decoding = new AtomicBoolean(false);
    private final QRCodeReader qr = new QRCodeReader();
    private final Map<DecodeHintType, Object> hints = new EnumMap<>(DecodeHintType.class);
    private long lastPreview;
    private boolean asking, launched;
    private String lastProblem = "";
    private long lastProblemAt;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        loaderStart = getString(R.string.launchUrl);
        hints.put(DecodeHintType.POSSIBLE_FORMATS, Collections.singletonList(BarcodeFormat.QR_CODE));
        hints.put(DecodeHintType.TRY_HARDER, Boolean.TRUE);
        setContentView(buildUi());
    }

    private int dp(float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }

    private TextView text(String s, float sp, int color) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        t.setLineSpacing(0, 1.15f);
        return t;
    }

    private ViewGroup buildUi() {
        final int fg = 0xFFE8ECF1, muted = 0xFFA9B4C2;
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.HORIZONTAL);
        root.setBackgroundColor(0xFF101418);
        root.setPadding(dp(28), dp(24), dp(28), dp(24));

        viewfinder = new ImageView(this);
        viewfinder.setBackgroundColor(0xFF000000);
        viewfinder.setScaleType(ImageView.ScaleType.FIT_CENTER);
        viewfinder.setContentDescription("Camera view");
        LinearLayout.LayoutParams vp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1.15f);
        vp.setMarginEnd(dp(24));
        root.addView(viewfinder, vp);

        LinearLayout side = new LinearLayout(this);
        side.setOrientation(LinearLayout.VERTICAL);
        root.addView(side, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f));

        TextView title = text("VisIVO VR", 30, fg);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        side.addView(title);
        TextView how = text("On the computer, in VisIVO Visual Analytics: Tools → Views → Open in VR Headset… "
                + "shows a QR code. Look at it from about 30 to 50 cm: the cube opens in VR by itself.", 17, fg);
        how.setPadding(0, dp(10), 0, dp(14));
        side.addView(how);

        status = text("Starting the camera…", 17, 0xFF8FD3FF);
        status.setPadding(0, 0, 0, dp(12));
        side.addView(status);

        // Shown when the cameras' permission is missing: the app's own page
        // of the headset's settings (Permissions), checked again on return.
        settingsButton = new Button(this);
        settingsButton.setText("Open app settings");
        settingsButton.setOnClickListener(v -> openAppSettings());
        settingsButton.setVisibility(android.view.View.GONE);
        LinearLayout.LayoutParams sp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        sp.bottomMargin = dp(18);
        side.addView(settingsButton, sp);

        side.addView(text("Or type the code:", 16, muted));
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        codeField = new EditText(this);
        codeField.setHint("ABC-234");
        codeField.setTextSize(TypedValue.COMPLEX_UNIT_SP, 24);
        codeField.setTextColor(fg);
        codeField.setHintTextColor(0xFF5D6B7A);
        codeField.setSingleLine(true);
        codeField.setFilters(new InputFilter[] { new InputFilter.LengthFilter(9) });
        codeField.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        codeField.setImeOptions(EditorInfo.IME_ACTION_GO);
        codeField.setOnEditorActionListener((v, actionId, event) -> {
            if (actionId == EditorInfo.IME_ACTION_GO || actionId == EditorInfo.IME_ACTION_DONE) {
                connectTyped();
                return true;
            }
            return false;
        });
        row.addView(codeField, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        Button connect = new Button(this);
        connect.setText("Connect");
        connect.setOnClickListener(v -> connectTyped());
        row.addView(connect);
        side.addView(row);

        String host = Uri.parse(loaderStart).getHost();
        TextView foot = text("Pairing through " + (host == null ? loaderStart : host)
                + ". The camera image is read on the headset and never kept or sent.", 13, muted);
        foot.setPadding(0, dp(18), 0, 0);
        side.addView(foot);
        return root;
    }

    private void say(String s, boolean error) {
        status.setText(s);
        status.setTextColor(error ? 0xFFFF9C8F : 0xFF8FD3FF);
    }

    private void connectTyped() {
        PairCode.Result r = PairCode.typed(codeField.getText().toString());
        if (!r.ok()) { say(r.problem, true); return; }
        launch(r.code);
    }

    /** The immersive part: the loader with the code; this panel goes. */
    private void launch(String code) {
        if (launched) return;
        launched = true;
        stopCamera();
        say("Code " + code.substring(0, 3) + "-" + code.substring(3) + ": opening VR…", false);
        Intent i = new Intent(Intent.ACTION_VIEW, Uri.parse(PairCode.loaderLaunch(loaderStart, code)),
                this, LauncherActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(i);
        finishAndRemoveTask();
    }

    // ── Permission ─────────────────────────────────────────────────────────

    private boolean granted(String permission) {
        return checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED;
    }

    /** The cameras' permissions not granted yet, HEADSET_CAMERA first. */
    private String[] missingPermissions() {
        java.util.List<String> out = new java.util.ArrayList<>(2);
        if (!granted(HEADSET_CAMERA)) out.add(HEADSET_CAMERA);
        if (!granted(Manifest.permission.CAMERA)) out.add(Manifest.permission.CAMERA);
        return out.toArray(new String[0]);
    }

    @Override
    protected void onResume() {
        super.onResume();
        launched = false;
        Log.i(TAG, "permissions: HEADSET_CAMERA " + granted(HEADSET_CAMERA)
                + ", CAMERA " + granted(Manifest.permission.CAMERA));
        if (granted(HEADSET_CAMERA)) {
            settingsButton.setVisibility(android.view.View.GONE);
            startCamera();
        } else if (!asking) {
            // Asked once per screen: a refusal is not asked again at every
            // resume (the settings button stays); back from the settings
            // page, this resume checks again.
            asking = true;
            say("Asking to use the headset's cameras…", false);
            requestPermissions(missingPermissions(), ASK);
        } else {
            permissionMissing();
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode != ASK) return;
        for (int i = 0; i < permissions.length && i < grantResults.length; i++) {
            Log.i(TAG, "permission " + permissions[i] + ": "
                    + (grantResults[i] == PackageManager.PERMISSION_GRANTED ? "granted" : "refused"));
        }
        // Refused, or no dialog shown (the result then arrives refused at
        // once): name the setting. Granted: onResume follows and starts.
        if (!granted(HEADSET_CAMERA)) permissionMissing();
    }

    /** The permission is missing: say where to turn it on, offer the app's settings page. */
    private void permissionMissing() {
        say("To read the QR code, VisIVO VR needs permission to use the headset's cameras. In "
                + SETTING + " (and Camera, if listed) turn on VisIVO VR, or press Open app settings and "
                + "allow the cameras under Permissions. Or type the code below.", true);
        settingsButton.setVisibility(android.view.View.VISIBLE);
    }

    private void openAppSettings() {
        Intent i = new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                Uri.fromParts("package", getPackageName(), null));
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            startActivity(i);
        } catch (ActivityNotFoundException e) {
            Log.i(TAG, "no application details settings page: " + e);
            say("The headset's settings could not be opened from here: open " + SETTING
                    + " and turn on VisIVO VR. Or type the code below.", true);
        }
    }

    // ── The camera ─────────────────────────────────────────────────────────

    @Override
    protected void onPause() {
        stopCamera();
        super.onPause();
    }

    /** The passthrough camera to read: source 0 (passthrough RGB), the left
     *  one first; else any camera facing forward; null when there is none. */
    static String pickCamera(CameraManager cm) throws CameraAccessException {
        String passthrough = null, back = null;
        CameraCharacteristics.Key<Integer> source = null, position = null;
        if (Build.VERSION.SDK_INT >= 29) {
            source = new CameraCharacteristics.Key<>("com.meta.extra_metadata.camera_source", Integer.class);
            position = new CameraCharacteristics.Key<>("com.meta.extra_metadata.position", Integer.class);
        }
        String[] ids = cm.getCameraIdList();
        Log.i(TAG, "cameras: " + ids.length);
        for (String id : ids) {
            CameraCharacteristics c = cm.getCameraCharacteristics(id);
            Integer src = vendor(c, source), pos = vendor(c, position);
            Log.i(TAG, "camera " + id + ": camera_source " + src + ", position " + pos
                    + ", facing " + c.get(CameraCharacteristics.LENS_FACING));
            if (src != null && src == 0) {
                if (pos != null && pos == 0) return id;
                if (passthrough == null) passthrough = id;
            }
            Integer facing = c.get(CameraCharacteristics.LENS_FACING);
            if (back == null && facing != null && facing == CameraCharacteristics.LENS_FACING_BACK) back = id;
        }
        return passthrough != null ? passthrough : back;
    }

    static Integer vendor(CameraCharacteristics c, CameraCharacteristics.Key<Integer> key) {
        if (key == null) return null;
        try { return c.get(key); } catch (RuntimeException e) { return null; }
    }

    /** 1280×960 when offered (a QR code a hand's width away still has about
     *  3 pixels per module), else the largest size up to 1280 wide. */
    static Size pickSize(CameraCharacteristics c) {
        StreamConfigurationMap map = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        Size best = null;
        Size[] sizes = map == null ? null : map.getOutputSizes(ImageFormat.YUV_420_888);
        if (sizes != null) {
            for (Size s : sizes) {
                if (s.getWidth() == 1280 && s.getHeight() == 960) return s;
                if (s.getWidth() <= 1280 && (best == null || s.getWidth() * s.getHeight() > best.getWidth() * best.getHeight())) best = s;
            }
        }
        return best != null ? best : new Size(640, 480);
    }

    private void startCamera() {
        if (device != null || cameraThread != null) return;
        cameraThread = new HandlerThread("visivo-camera");
        cameraThread.start();
        cameraHandler = new Handler(cameraThread.getLooper());
        decodeThread = new HandlerThread("visivo-qr");
        decodeThread.start();
        decodeHandler = new Handler(decodeThread.getLooper());
        CameraManager cm = (CameraManager) getSystemService(CAMERA_SERVICE);
        try {
            String id = cm == null ? null : pickCamera(cm);
            Log.i(TAG, "camera picked: " + id);
            if (id == null) {
                stopCamera();
                if (missingPermissions().length > 0) {
                    // A permission is still missing (Meta documents either as
                    // enough, a Quest 3 showed otherwise): not "no camera".
                    permissionMissing();
                } else {
                    say("This headset has no camera VisIVO VR can use (the passthrough camera needs a Quest 3 "
                            + "or 3S with Horizon OS v74 or later, and passthrough turned on): type the code instead.",
                            true);
                }
                return;
            }
            Size size = pickSize(cm.getCameraCharacteristics(id));
            reader = ImageReader.newInstance(size.getWidth(), size.getHeight(), ImageFormat.YUV_420_888, 2);
            reader.setOnImageAvailableListener(this::onImage, cameraHandler);
            cm.openCamera(id, new CameraDevice.StateCallback() {
                @Override public void onOpened(CameraDevice d) {
                    if (cameraThread == null) { d.close(); return; }
                    device = d;
                    startSession();
                }
                @Override public void onDisconnected(CameraDevice d) {
                    d.close();
                    if (device == d) device = null;
                    runOnUiThread(() -> say("The camera was taken by another app: type the code, or open VisIVO VR again.", true));
                }
                @Override public void onError(CameraDevice d, int error) {
                    d.close();
                    if (device == d) device = null;
                    runOnUiThread(() -> say("The camera could not be opened (error " + error + "): type the code instead.", true));
                }
            }, cameraHandler);
        } catch (CameraAccessException | SecurityException | IllegalArgumentException e) {
            say("The camera could not be opened (" + e.getClass().getSimpleName() + "): type the code instead.", true);
            stopCamera();
        }
    }

    private void startSession() {
        try {
            final CameraDevice d = device;
            final ImageReader rd = reader;
            if (d == null || rd == null) return;
            d.createCaptureSession(Collections.singletonList(rd.getSurface()), new CameraCaptureSession.StateCallback() {
                @Override public void onConfigured(CameraCaptureSession s) {
                    if (device != d) { s.close(); return; }
                    session = s;
                    try {
                        CaptureRequest.Builder b = d.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                        b.addTarget(rd.getSurface());
                        b.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO);
                        s.setRepeatingRequest(b.build(), null, cameraHandler);
                        runOnUiThread(() -> say("Looking for the QR code…", false));
                    } catch (CameraAccessException | IllegalStateException e) {
                        runOnUiThread(() -> say("The camera stopped: type the code instead.", true));
                    }
                }
                @Override public void onConfigureFailed(CameraCaptureSession s) {
                    runOnUiThread(() -> say("The camera could not be started: type the code instead.", true));
                }
            }, cameraHandler);
        } catch (CameraAccessException | IllegalStateException e) {
            runOnUiThread(() -> say("The camera could not be started: type the code instead.", true));
        }
    }

    /** Closes the camera on its own thread (a frame being read there is
     *  done first), then ends both threads. */
    private void stopCamera() {
        final CameraCaptureSession s = session;
        final CameraDevice d = device;
        final ImageReader rd = reader;
        final HandlerThread ct = cameraThread, dt = decodeThread;
        session = null;
        device = null;
        reader = null;
        cameraThread = decodeThread = null;
        cameraHandler = decodeHandler = null;
        Runnable close = () -> {
            try { if (s != null) s.close(); } catch (RuntimeException ignored) { }
            try { if (d != null) d.close(); } catch (RuntimeException ignored) { }
            try { if (rd != null) rd.close(); } catch (RuntimeException ignored) { }
            if (ct != null) ct.quitSafely();
        };
        if (ct == null || !new Handler(ct.getLooper()).post(close)) close.run();
        if (dt != null) dt.quitSafely();
        decoding.set(false);
    }

    /** A frame: its luminance, to the decoder when it is free and to the
     *  viewfinder every PREVIEW_MS. On the camera's thread. */
    private void onImage(ImageReader r) {
        Image img;
        try { img = r.acquireLatestImage(); } catch (IllegalStateException e) { return; }
        if (img == null) return;
        byte[] lum = null;
        int w = 0, h = 0;
        try {
            long now = SystemClock.uptimeMillis();
            boolean preview = now - lastPreview >= PREVIEW_MS;
            boolean decode = !decoding.get();
            if (!preview && !decode) return;
            w = img.getWidth();
            h = img.getHeight();
            Image.Plane y = img.getPlanes()[0];
            ByteBuffer buf = y.getBuffer();
            int rowStride = y.getRowStride();
            lum = new byte[w * h];
            if (rowStride == w) {
                buf.get(lum, 0, w * h);
            } else {
                for (int row = 0; row < h; row++) {
                    buf.position(row * rowStride);
                    buf.get(lum, row * w, w);
                }
            }
            if (preview) {
                lastPreview = now;
                final Bitmap bmp = greyBitmap(lum, w, h);
                runOnUiThread(() -> viewfinder.setImageBitmap(bmp));
            }
            if (decode && decodeHandler != null && decoding.compareAndSet(false, true)) {
                final byte[] frame = lum;
                final int fw = w, fh = h;
                decodeHandler.post(() -> decode(frame, fw, fh));
            }
        } catch (RuntimeException ignored) {
            // A frame that could not be read: the next one will be.
        } finally {
            try { img.close(); } catch (RuntimeException ignored) { }
        }
    }

    /** The viewfinder: the luminance at half size, grey. */
    static Bitmap greyBitmap(byte[] lum, int w, int h) {
        int bw = w / 2, bh = h / 2;
        int[] px = new int[bw * bh];
        for (int y = 0; y < bh; y++) {
            int src = 2 * y * w, dst = y * bw;
            for (int x = 0; x < bw; x++) {
                int g = lum[src + 2 * x] & 0xff;
                px[dst + x] = 0xff000000 | (g << 16) | (g << 8) | g;
            }
        }
        return Bitmap.createBitmap(px, bw, bh, Bitmap.Config.ARGB_8888);
    }

    /** One frame through the QR decoder. On the decoder's thread. */
    private void decode(byte[] lum, int w, int h) {
        String text = null;
        try {
            PlanarYUVLuminanceSource src = new PlanarYUVLuminanceSource(lum, w, h, 0, 0, w, h, false);
            text = qr.decode(new BinaryBitmap(new HybridBinarizer(src)), hints).getText();
        } catch (ReaderException | RuntimeException ignored) {
            // No QR code in this frame.
        } finally {
            qr.reset();
            decoding.set(false);
        }
        if (text == null) return;
        final PairCode.Result res = PairCode.scanned(text, loaderStart);
        runOnUiThread(() -> {
            if (res.ok()) { launch(res.code); return; }
            if (res.problem.isEmpty()) return;
            long now = SystemClock.uptimeMillis();
            if (res.problem.equals(lastProblem) && now - lastProblemAt < 3000) return;
            lastProblem = res.problem;
            lastProblemAt = now;
            say(res.problem, true);
        });
    }
}
