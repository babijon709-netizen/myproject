// oxhk/Oxhk.java — in-process overlay for Oxide (HyperHug). Loaded from the repacked APK,
// same process as the game, no root. Native side: liboxhk.so (base + RVA reads).
//
// Two entry points, the patcher wires whichever the manifest allows:
//   Oxhk.install(Application)  — from our generated Application subclass' onCreate
//   Oxhk.install(Activity)     — from the launcher activity's onCreate
//
// Everything runs on the Android UI thread; Unity's script loop lives there too, which is why
// the native side can call managed functions without any hooking.

package oxhk;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Build;
import android.os.SystemClock;
import android.util.Log;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.view.Window;
import android.widget.FrameLayout;

import org.json.JSONObject;

import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.util.Locale;

public final class Oxhk {
    private static final String TAG = "oxhk";
    private static final int MAX_ENT = 64;
    private static final int STRIDE = 10;

    // ── config, shared with the native side ──────────────────────────────────────────────
    static final int F_ESP = 1, F_BOXES = 1 << 1, F_CLEAN = 1 << 2;
    static final int F_AIM = 1 << 3, F_TEAM = 1 << 4, F_MENU = 1 << 5;

    static int flags = F_ESP | F_BOXES | F_TEAM | F_MENU;
    static float fovPx = 260f;
    static float smoothing = 4f;
    static float gain = 1f;
    static int touchMs = 40;
    static int headBone = 10;   // HumanBodyBones.Head
    static int cooldownMs = 30;

    private static boolean sInstalled;
    private static Activity sActivity;
    private static EspView sView;

    static native void nativeInit(String filesDir, int w, int h);
    static native void nativeSetConfig(int flags, float fovPx, float smoothing, float gain,
                                       int touchMs, int headBone, int cooldownMs);
    static native int nativeTick(float[] out, int w, int h);
    static native int nativeNames(String[] out);

    static {
        try {
            System.loadLibrary("oxhk");
        } catch (Throwable t) {
            Log.e(TAG, "loadLibrary failed", t);
        }
    }

    public static void install(Context ctx) {
        if (sInstalled) return;
        sInstalled = true;
        Log.i(TAG, "install from " + ctx.getClass().getName());
        if (ctx instanceof Activity) attach((Activity) ctx);
        Context app = ctx.getApplicationContext();
        if (app instanceof Application) {
            ((Application) app).registerActivityLifecycleCallbacks(new Application.ActivityLifecycleCallbacks() {
                public void onActivityCreated(Activity a, android.os.Bundle b) {}
                public void onActivityStarted(Activity a) {}
                public void onActivityResumed(Activity a) { attach(a); }
                public void onActivityPaused(Activity a) {}
                public void onActivityStopped(Activity a) {}
                public void onActivitySaveInstanceState(Activity a, android.os.Bundle b) {}
                public void onActivityDestroyed(Activity a) { if (a == sActivity) sActivity = null; }
            });
        }
    }

    private static void attach(final Activity a) {
        if (a == null || a == sActivity) return;
        a.runOnUiThread(() -> {
            try {
                sActivity = a;
                loadConfig(a);
                if (sView == null) sView = new EspView(a);
                ViewGroup decor = (ViewGroup) a.getWindow().getDecorView();
                if (sView.getParent() == null) {
                    decor.addView(sView, new FrameLayout.LayoutParams(
                            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
                }
                sView.bringToFront();
                pushConfig();
                nativeInit(a.getFilesDir().getAbsolutePath(), decor.getWidth(), decor.getHeight());
                Log.i(TAG, "overlay attached to " + a.getClass().getName());
            } catch (Throwable t) {
                Log.e(TAG, "attach failed", t);
            }
        });
    }

    static void pushConfig() {
        try {
            nativeSetConfig(flags, fovPx, smoothing, gain, touchMs, headBone, cooldownMs);
        } catch (Throwable ignored) {
        }
    }

    static void saveConfig(Context ctx) {
        try {
            JSONObject o = new JSONObject();
            o.put("flags", flags);
            o.put("fovPx", fovPx);
            o.put("smoothing", smoothing);
            o.put("gain", gain);
            o.put("touchMs", touchMs);
            o.put("headBone", headBone);
            o.put("cooldownMs", cooldownMs);
            File f = new File(ctx.getFilesDir(), "oxhk.json");
            try (FileWriter w = new FileWriter(f)) { w.write(o.toString()); }
        } catch (Throwable t) {
            Log.e(TAG, "save failed", t);
        }
    }

    static void loadConfig(Context ctx) {
        File f = new File(ctx.getFilesDir(), "oxhk.json");
        if (!f.exists()) return;
        try (FileReader r = new FileReader(f)) {
            StringBuilder sb = new StringBuilder();
            char[] buf = new char[512];
            int n;
            while ((n = r.read(buf)) > 0) sb.append(buf, 0, n);
            JSONObject o = new JSONObject(sb.toString());
            flags = o.optInt("flags", flags);
            fovPx = (float) o.optDouble("fovPx", fovPx);
            smoothing = (float) o.optDouble("smoothing", smoothing);
            gain = (float) o.optDouble("gain", gain);
            touchMs = o.optInt("touchMs", touchMs);
            headBone = o.optInt("headBone", headBone);
            cooldownMs = o.optInt("cooldownMs", cooldownMs);
        } catch (Throwable t) {
            Log.e(TAG, "load failed", t);
        }
    }

    // ── touch synthesis: swipe on the game's own view, nothing written to game memory ─────
    static View gameView() {
        Activity a = sActivity;
        if (a == null) return null;
        Window wnd = a.getWindow();
        if (wnd == null) return null;
        View decor = wnd.getDecorView();
        if (!(decor instanceof ViewGroup)) return null;
        ViewGroup g = (ViewGroup) decor;
        for (int i = 0; i < g.getChildCount(); i++) {
            View c = g.getChildAt(i);
            if (c != sView) return c;
        }
        return null;
    }

    static void swipe(float dx, float dy) {
        View target = gameView();
        if (target == null) return;
        int w = target.getWidth(), h = target.getHeight();
        if (w <= 0 || h <= 0) return;
        float cx = w * 0.5f, cy = h * 0.5f;

        long down = SystemClock.uptimeMillis();
        long span = Math.max(1, touchMs);

        MotionEvent d = MotionEvent.obtain(down, down, MotionEvent.ACTION_DOWN, cx, cy, 0);
        d.setSource(InputDevice.SOURCE_TOUCHSCREEN);
        target.dispatchTouchEvent(d);
        d.recycle();

        MotionEvent m = MotionEvent.obtain(down, down + span / 2, MotionEvent.ACTION_MOVE, cx + dx, cy + dy, 0);
        m.setSource(InputDevice.SOURCE_TOUCHSCREEN);
        target.dispatchTouchEvent(m);
        m.recycle();

        MotionEvent u = MotionEvent.obtain(down, down + span, MotionEvent.ACTION_UP, cx + dx, cy + dy, 0);
        u.setSource(InputDevice.SOURCE_TOUCHSCREEN);
        target.dispatchTouchEvent(u);
        u.recycle();
    }

    // ── overlay view ─────────────────────────────────────────────────────────────────────
    static final class EspView extends View implements Runnable {
        private final float[] out = new float[4 + MAX_ENT * STRIDE];
        private final String[] names = new String[MAX_ENT];
        private final Paint box = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint panel = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint panelText = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF menuRect = new RectF();
        private final RectF closeRect = new RectF();
        private final RectF badgeRect = new RectF();
        private final RectF[] rowRects = new RectF[16];

        private int count;
        private long lastNames;
        private int tab;                 // 0 esp, 1 aim, 2 cfg
        private boolean dragging;
        private float dragOffX, dragOffY;
        private float menuX = -1, menuY = -1;
        private final float density;
        private final float rowH;
        private final float menuW;

        EspView(Context ctx) {
            super(ctx);
            setWillNotDraw(false);
            density = ctx.getResources().getDisplayMetrics().density;
            rowH = 26f * density;
            menuW = 168f * density;

            box.setStyle(Paint.Style.STROKE);
            box.setStrokeWidth(1.6f * density);
            text.setColor(Color.WHITE);
            text.setTextSize(11f * density);
            text.setTypeface(Typeface.MONOSPACE);
            text.setShadowLayer(2f, 0f, 1f, Color.BLACK);
            panel.setColor(0xCC101010);
            panelText.setColor(0xFFE8E8E8);
            panelText.setTextSize(11.5f * density);
            panelText.setTypeface(Typeface.MONOSPACE);
            fill.setStyle(Paint.Style.FILL);
            for (int i = 0; i < rowRects.length; i++) rowRects[i] = new RectF();
        }

        @Override protected void onAttachedToWindow() {
            super.onAttachedToWindow();
            postOnAnimation(this);
        }

        @Override protected void onDetachedFromWindow() {
            removeCallbacks(this);
            super.onDetachedFromWindow();
        }

        @Override public void run() {
            try {
                tick();
            } catch (Throwable t) {
                Log.e(TAG, "tick failed", t);
            }
            invalidate();
            postOnAnimation(this);
        }

        private void tick() {
            int w = getWidth(), h = getHeight();
            if (w <= 0 || h <= 0) return;
            count = nativeTick(out, w, h);

            if (out[0] > 0.5f && (flags & F_AIM) != 0 && Math.abs(out[1]) + Math.abs(out[2]) > 0.5f) {
                swipe(out[1], out[2]);
            }
            long now = SystemClock.uptimeMillis();
            if (now - lastNames > 250) {
                lastNames = now;
                int n = nativeNames(names);
                if (n < names.length) {
                    for (int i = n; i < names.length; i++) names[i] = null;
                }
            }
        }

        // ── input: only the menu area consumes touches, everything else passes to the game ──
        @Override public boolean onTouchEvent(MotionEvent e) {
            layoutMenu();
            float x = e.getX(), y = e.getY();
            int action = e.getActionMasked();

            if (action == MotionEvent.ACTION_DOWN) {
                if ((flags & F_MENU) == 0) {
                    if (badgeRect.contains(x, y)) { flags |= F_MENU; pushConfig(); invalidate(); return true; }
                    return false;
                }
                if (closeRect.contains(x, y)) { flags &= ~F_MENU; pushConfig(); invalidate(); return true; }
                if (menuRect.contains(x, y)) {
                    if (y - menuRect.top < rowH) {           // title bar: drag
                        dragging = true;
                        dragOffX = x - menuRect.left;
                        dragOffY = y - menuRect.top;
                        return true;
                    }
                    for (int i = 0; i < rowRects.length; i++) {
                        if (rowRects[i].contains(x, y)) {
                            onRow(i);
                            return true;
                        }
                    }
                    return true;
                }
                return false;
            }
            if (action == MotionEvent.ACTION_MOVE && dragging) {
                menuX = Math.max(0, Math.min(getWidth() - menuW, x - dragOffX));
                menuY = Math.max(0, Math.min(getHeight() - rowH * 2, y - dragOffY));
                layoutMenu();
                invalidate();
                return true;
            }
            if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
                if (dragging) { dragging = false; return true; }
                return menuRect.contains(x, y);
            }
            return menuRect.contains(x, y);
        }

        private boolean onRow(int i) {
            if (i == 3) { tab = 0; return true; }
            if (i == 4) { tab = 1; return true; }
            if (i == 5) { tab = 2; return true; }
            if (i < 3) { tab = i; return true; }

            int slot = i - 6;
            if (tab == 0) {
                if (slot == 0) flags ^= F_ESP;
                else if (slot == 1) flags ^= F_BOXES;
                else if (slot == 2) flags ^= F_CLEAN;
                else if (slot == 3) flags ^= F_TEAM;
            } else if (tab == 1) {
                if (slot == 0) flags ^= F_AIM;
                else if (slot == 1) fovPx = clamp(fovPx + 40f, 40f, 1400f);
                else if (slot == 2) smoothing = clamp(smoothing + 1f, 1f, 20f);
                else if (slot == 3) gain = clamp(gain + 0.1f, 0.2f, 3f);
                else if (slot == 4) touchMs = (int) clamp(touchMs + 20, 10, 300);
                else if (slot == 5) cooldownMs = (int) clamp(cooldownMs + 10, 0, 300);
            } else {
                if (slot == 0) { saveConfig(getContext()); return true; }
                if (slot == 1) { loadConfig(getContext()); }
                if (slot == 2) {
                    flags = F_ESP | F_BOXES | F_TEAM | F_MENU;
                    fovPx = 260f; smoothing = 4f; gain = 1f; touchMs = 40; cooldownMs = 30;
                }
                if (slot == 3) { flags &= ~F_MENU; }  // badge in the top-left corner re-opens it
            }
            pushConfig();
            return true;
        }

        private static float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

        private void layoutMenu() {
            float x = menuX < 0 ? 12f * density : menuX;
            float y = menuY < 0 ? 90f * density : menuY;
            float h = rowH * 11;
            menuRect.set(x, y, x + menuW, y + h);
            closeRect.set(menuRect.right - rowH, menuRect.top, menuRect.right, menuRect.top + rowH);
            for (int i = 0; i < rowRects.length; i++) rowRects[i].setEmpty();
        }

        @Override protected void onDraw(Canvas c) {
            int n = Math.min(count, MAX_ENT);
            int w = getWidth(), h = getHeight();
            if (w <= 0 || h <= 0) return;

            drawFov(c, w, h);
            if ((flags & F_ESP) != 0) {
                for (int i = 0; i < n; i++) {
                    int b = 4 + i * STRIDE;
                    boolean team = out[b + 9] > 0.5f;
                    box.setColor(team ? 0xFF4FA3FF : 0xFFFF4444);
                    float x1 = out[b], y1 = out[b + 1], x2 = out[b + 2], y2 = out[b + 3];
                    float hx = out[b + 4], hy = out[b + 5];
                    float dist = out[b + 6], hp = out[b + 7], hpMax = out[b + 8];
                    if ((flags & F_CLEAN) == 0) {
                        c.drawRect(x1, y1, x2, y2, box);
                        float frac = hpMax > 0f ? Math.max(0f, Math.min(1f, hp / hpMax)) : 0f;
                        fill.setColor(0xAA22C55E);
                        c.drawRect(x1 - 4f * density, y2, x1 - 1f * density, y2 - (y2 - y1) * frac, fill);
                    }
                    fill.setColor(team ? 0xFF4FA3FF : 0xFFFF4444);
                    c.drawCircle(hx, hy, 3f * density, fill);
                    String label = String.format(Locale.US, "%.0fm", dist);
                    if ((flags & F_CLEAN) == 0 && hpMax > 0f) {
                        label = String.format(Locale.US, "%.0fm %.0fhp", dist, hp);
                    }
                    c.drawText(label, x1, y2 + 12f * density, text);
                    String nm = i < names.length ? names[i] : null;
                    if (nm != null && (flags & F_CLEAN) == 0) c.drawText(nm, x1, y1 - 4f * density, text);
                }
            }
            if ((flags & F_MENU) != 0) {
                drawMenu(c, n);
            } else {
                badgeRect.set(8f * density, 70f * density, 8f * density + 34f * density, 70f * density + 22f * density);
                fill.setColor(0x66000000);
                c.drawRoundRect(badgeRect, 5f * density, 5f * density, fill);
                panelText.setColor(0xFFB8B8B8);
                c.drawText("oxhk", badgeRect.left + 5f * density, badgeRect.bottom - 7f * density, panelText);
            }
        }

        private void drawFov(Canvas c, int w, int h) {
            if ((flags & F_AIM) == 0) return;
            Paint p = box;
            p.setColor(0x66FFCC33);
            c.drawCircle(w * 0.5f, h * 0.5f, fovPx, p);
        }

        private void drawMenu(Canvas c, int n) {
            layoutMenu();
            c.drawRoundRect(menuRect, 6f * density, 6f * density, panel);

            String[] tabs = {"ESP", "AIM", "CFG"};
            String[] rows;
            if (tab == 0) {
                rows = new String[]{
                        "esp: " + onOff((flags & F_ESP) != 0),
                        "boxes: " + onOff((flags & F_BOXES) != 0),
                        "clean: " + onOff((flags & F_CLEAN) != 0),
                        "teamcheck: " + onOff((flags & F_TEAM) != 0),
                        "", ""};
            } else if (tab == 1) {
                rows = new String[]{
                        "aim: " + onOff((flags & F_AIM) != 0),
                        "fov: " + (int) fovPx,
                        "smooth: " + (int) smoothing,
                        "gain: " + String.format(Locale.US, "%.1f", gain),
                        "touchms: " + touchMs,
                        String.format(Locale.US, "cooldown: %d  (aiming %d)", cooldownMs, n)};
            } else {
                rows = new String[]{"save cfg", "load cfg", "reset", "close menu", "", ""};
            }

            float y = menuRect.top + rowH;
            panelText.setColor(0xFFFFD75E);
            c.drawText("oxhk 11978", menuRect.left + 8f * density, y - rowH * 0.3f, panelText);
            panelText.setColor(0xFFB0B0B0);
            c.drawText("x", closeRect.centerX() - 4f * density, y - rowH * 0.3f, panelText);

            // tab strip
            float tx = menuRect.left + 8f * density;
            for (int i = 0; i < tabs.length; i++) {
                panelText.setColor(i == tab ? 0xFFFFD75E : 0xFF8A8A8A);
                c.drawText(tabs[i], tx, menuRect.top + rowH * 1.7f, panelText);
                rowRects[3 + i].set(tx - 4f * density, menuRect.top + rowH * 1.1f,
                        tx + 34f * density, menuRect.top + rowH * 1.95f);
                tx += 44f * density;
            }

            float ry = menuRect.top + rowH * 2.2f;
            for (int i = 0; i < rows.length; i++) {
                if (rows[i].isEmpty()) continue;
                panelText.setColor(0xFFE8E8E8);
                c.drawText(rows[i], menuRect.left + 8f * density, ry + rowH * 0.65f, panelText);
                rowRects[6 + i].set(menuRect.left, ry, menuRect.right, ry + rowH);
                ry += rowH;
            }
        }

        private static String onOff(boolean b) { return b ? "on" : "off"; }
    }
}
