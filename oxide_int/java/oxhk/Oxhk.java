// oxhk/Oxhk.java — in-process ESP overlay for Oxide (HyperHug). Lives inside the repacked APK,
// same process as the game, no root, no SYSTEM_ALERT_WINDOW: the overlay is a plain View added
// to the game's own window, and touches are only captured on the menu itself.
//
// Entry points (the repack script wires the second one via a generated Application subclass):
//   Oxhk.install(Context) — call from Application.onCreate() or the launcher activity's onCreate

package oxhk;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.SystemClock;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
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

    // ── config ───────────────────────────────────────────────────────────────────────────
    static final int F_ESP  = 1 << 0;
    static final int F_BOX  = 1 << 1;
    static final int F_NAME = 1 << 2;
    static final int F_HP   = 1 << 3;
    static final int F_DIST = 1 << 4;
    static final int F_TEAM = 1 << 5;
    static final int F_MENU = 1 << 6;

    static int flags = F_ESP | F_BOX | F_HP | F_DIST | F_TEAM | F_MENU;
    static int headBone = 10;      // HumanBodyBones.Head
    static float maxDist = 0f;     // 0 = everything

    // the packer (libloader.so) walks modules and files during boot; stay out of the process
    // until that is over. 0 = attach right away, 2500-4000 = after the loader's startup scans.
    static final int ATTACH_DELAY_MS = 0;

    private static boolean sInstalled;
    private static Activity sActivity;
    private static EspView sView;

    static native void nativeInit(String filesDir, int w, int h);
    static native void nativeSetConfig(int flags, int headBone, float maxDist);
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
            ((Application) app).registerActivityLifecycleCallbacks(
                    new Application.ActivityLifecycleCallbacks() {
                        public void onActivityCreated(Activity a, android.os.Bundle b) {}
                        public void onActivityStarted(Activity a) {}
                        public void onActivityResumed(Activity a) { attach(a); }
                        public void onActivityPaused(Activity a) {}
                        public void onActivityStopped(Activity a) {}
                        public void onActivitySaveInstanceState(Activity a, android.os.Bundle b) {}
                        public void onActivityDestroyed(Activity a) {
                            if (a == sActivity) sActivity = null;
                        }
                    });
        }
    }

    // anonymous classes, not lambdas: the dex is compiled with android.jar as bootclasspath,
    // where java.lang.invoke.LambdaMetafactory does not exist.
    private static void attach(final Activity a) {
        if (a == null || a == sActivity) return;
        a.runOnUiThread(new Runnable() {
            @Override public void run() {
                try {
                    sActivity = a;
                    loadConfig(a);
                    if (sView == null) sView = new EspView(a);
                    final ViewGroup decor = (ViewGroup) a.getWindow().getDecorView();
                    Runnable add = new Runnable() {
                        @Override public void run() {
                            try {
                                if (sView.getParent() == null) {
                                    decor.addView(sView, new FrameLayout.LayoutParams(
                                            ViewGroup.LayoutParams.MATCH_PARENT,
                                            ViewGroup.LayoutParams.MATCH_PARENT));
                                }
                                sView.bringToFront();
                                pushConfig();
                                nativeInit(a.getFilesDir().getAbsolutePath(),
                                           decor.getWidth(), decor.getHeight());
                                Log.i(TAG, "overlay attached to " + a.getClass().getName());
                            } catch (Throwable t) {
                                Log.e(TAG, "attach(run) failed", t);
                            }
                        }
                    };
                    if (ATTACH_DELAY_MS > 0) decor.postDelayed(add, ATTACH_DELAY_MS);
                    else add.run();
                } catch (Throwable t) {
                    Log.e(TAG, "attach failed", t);
                }
            }
        });
    }

    static void pushConfig() {
        try {
            nativeSetConfig(flags, headBone, maxDist);
        } catch (Throwable ignored) {
        }
    }

    private static File cfgFile(Context ctx) { return new File(ctx.getFilesDir(), "oxhk.json"); }

    static void saveConfig(Context ctx) {
        try {
            JSONObject o = new JSONObject();
            o.put("flags", flags);
            o.put("headBone", headBone);
            o.put("maxDist", maxDist);
            try (FileWriter w = new FileWriter(cfgFile(ctx))) { w.write(o.toString()); }
        } catch (Throwable t) {
            Log.e(TAG, "save failed", t);
        }
    }

    static void loadConfig(Context ctx) {
        File f = cfgFile(ctx);
        if (!f.exists()) return;
        try {
            StringBuilder sb = new StringBuilder();
            try (FileReader r = new FileReader(f)) {
                char[] buf = new char[512];
                int n;
                while ((n = r.read(buf)) > 0) sb.append(buf, 0, n);
            }
            JSONObject o = new JSONObject(sb.toString());
            flags = o.optInt("flags", flags);
            headBone = o.optInt("headBone", headBone);
            maxDist = (float) o.optDouble("maxDist", maxDist);
        } catch (Throwable t) {
            Log.e(TAG, "load failed", t);
        }
    }

    // ── overlay ──────────────────────────────────────────────────────────────────────────
    static final class EspView extends View implements Runnable {
        private final float[] out = new float[1 + MAX_ENT * STRIDE];
        private final String[] names = new String[MAX_ENT];

        private final Paint box = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint panel = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint panelText = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);

        private final RectF menu = new RectF();
        private final RectF badge = new RectF();
        private final RectF[] rows = new RectF[12];

        private final float d;
        private final float rowH;
        private final float menuW = 172f;

        private int count;
        private long lastNames;
        private boolean dragging;
        private float dragX, dragY;
        private float menuX = -1, menuY = -1;

        EspView(Context ctx) {
            super(ctx);
            setWillNotDraw(false);
            d = ctx.getResources().getDisplayMetrics().density;
            rowH = 27f * d;

            box.setStyle(Paint.Style.STROKE);
            box.setStrokeWidth(1.6f * d);

            text.setColor(Color.WHITE);
            text.setTextSize(11f * d);
            text.setTypeface(Typeface.MONOSPACE);
            text.setShadowLayer(2f, 0f, 1f, Color.BLACK);

            panel.setColor(0xCC0E0E10);
            panelText.setColor(0xFFE6E6E6);
            panelText.setTextSize(12f * d);
            panelText.setTypeface(Typeface.MONOSPACE);

            fill.setStyle(Paint.Style.FILL);
            for (int i = 0; i < rows.length; i++) rows[i] = new RectF();
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
                int w = getWidth(), h = getHeight();
                if (w > 0 && h > 0) count = nativeTick(out, w, h);
                long now = SystemClock.uptimeMillis();
                if (now - lastNames > 300) {
                    lastNames = now;
                    if ((flags & F_ESP) != 0 && (flags & F_NAME) != 0) {
                        int n = nativeNames(names);
                        for (int i = n; i < names.length; i++) names[i] = null;
                    }
                }
            } catch (Throwable t) {
                Log.e(TAG, "tick failed", t);
            }
            invalidate();
            postOnAnimation(this);
        }

        // ── input: the menu consumes touches, everything else goes to the game ───────────
        @Override public boolean onTouchEvent(MotionEvent e) {
            layout();
            final float x = e.getX(), y = e.getY();
            final int action = e.getActionMasked();

            if (action == MotionEvent.ACTION_DOWN) {
                if ((flags & F_MENU) == 0) {
                    if (badge.contains(x, y)) {
                        flags |= F_MENU;
                        pushConfig();
                        invalidate();
                        return true;
                    }
                    return false;
                }
                if (!menu.contains(x, y)) {
                    if (badge.contains(x, y)) { flags &= ~F_MENU; pushConfig(); invalidate(); return true; }
                    return false;
                }
                if (y - menu.top < rowH) {          // title bar drags the menu
                    dragging = true;
                    dragX = x - menu.left;
                    dragY = y - menu.top;
                    return true;
                }
                for (int i = 0; i < rows.length; i++) {
                    if (rows[i].contains(x, y)) { onRow(i); return true; }
                }
                return true;
            }
            if (action == MotionEvent.ACTION_MOVE && dragging) {
                menuX = Math.max(0, Math.min(getWidth() - menuW * d, x - dragX));
                menuY = Math.max(0, Math.min(getHeight() - rowH * 2, y - dragY));
                layout();
                invalidate();
                return true;
            }
            if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
                if (dragging) { dragging = false; return true; }
                return menu.contains(x, y);
            }
            return menu.contains(x, y);
        }

        private void onRow(int i) {
            switch (i) {
                case 0: flags ^= F_ESP; break;
                case 1: flags ^= F_BOX; break;
                case 2: flags ^= F_NAME; break;
                case 3: flags ^= F_HP; break;
                case 4: flags ^= F_DIST; break;
                case 5: flags ^= F_TEAM; break;
                case 6: maxDist = maxDist <= 0f ? 100f : (maxDist >= 300f ? 0f : maxDist * 2f); break;
                case 7: saveConfig(getContext()); break;
                case 8: loadConfig(getContext()); break;
                case 9: flags &= ~F_MENU; break;
                default: return;
            }
            pushConfig();
            invalidate();
        }

        private void layout() {
            final float x = menuX < 0 ? 14f * d : menuX;
            final float y = menuY < 0 ? 96f * d : menuY;
            menu.set(x, y, x + menuW * d, y + rowH * 11.5f);
            badge.set(10f * d, 88f * d, 10f * d + 36f * d, 88f * d + 22f * d);
            for (int i = 0; i < rows.length; i++) {
                rows[i].set(menu.left, menu.top + rowH * (1.4f + i), menu.right,
                            menu.top + rowH * (2.4f + i));
            }
        }

        @Override protected void onDraw(Canvas c) {
            final int n = Math.min(count, MAX_ENT);
            final int w = getWidth(), h = getHeight();
            if (w <= 0 || h <= 0) return;

            if ((flags & F_ESP) != 0) {
                for (int i = 0; i < n; i++) {
                    final int b = 1 + i * STRIDE;
                    final boolean team = out[b + 9] > 0.5f;
                    final int col = team ? 0xFF4FA3FF : 0xFFFF4D4D;

                    final float x1 = out[b], y1 = out[b + 1], x2 = out[b + 2], y2 = out[b + 3];
                    final float hx = out[b + 4], hy = out[b + 5];
                    final float dist = out[b + 6], hp = out[b + 7], hpMax = out[b + 8];

                    if ((flags & F_BOX) != 0) {
                        box.setColor(col);
                        c.drawRect(x1, y1, x2, y2, box);
                    }

                    fill.setColor(col);
                    c.drawCircle(hx, hy, 3f * d, fill);

                    if ((flags & F_HP) != 0 && hpMax > 0f) {
                        float frac = Math.max(0f, Math.min(1f, hp / hpMax));
                        fill.setColor(0xAA33D17A);
                        c.drawRect(x1 - 5f * d, y2, x1 - 2f * d, y2 - (y2 - y1) * frac, fill);
                    }

                    if ((flags & F_NAME) != 0 && i < names.length && names[i] != null) {
                        c.drawText(names[i], x1, y1 - 4f * d, text);
                    }
                    if ((flags & F_DIST) != 0) {
                        c.drawText(String.format(Locale.US, "%.0fm", dist), x1, y2 + 12f * d, text);
                    }
                }
            }

            if ((flags & F_MENU) != 0) drawMenu(c);
            else drawBadge(c);
        }

        private void drawBadge(Canvas c) {
            fill.setColor(0x66000000);
            c.drawRoundRect(badge, 5f * d, 5f * d, fill);
            panelText.setColor(0xFFB9B9B9);
            c.drawText("oxhk", badge.left + 5f * d, badge.bottom - 7f * d, panelText);
        }

        private void drawMenu(Canvas c) {
            c.drawRoundRect(menu, 7f * d, 7f * d, panel);

            panelText.setColor(0xFFFFD75E);
            c.drawText("oxhk esp", menu.left + 10f * d, menu.top + rowH * 0.85f, panelText);
            panelText.setColor(0xFF8C8C8C);
            c.drawText("v11978  " + enemiesLabel(), menu.right - 92f * d, menu.top + rowH * 0.85f, panelText);

            panelText.setColor(0xFFE6E6E6);
            final int base = (int) (menu.top + rowH * 2.1f);
            c.drawText(row("esp", (flags & F_ESP) != 0), menu.left + 10f * d, base, panelText);
            c.drawText(row("boxes", (flags & F_BOX) != 0), menu.left + 10f * d, base + rowH, panelText);
            c.drawText(row("names", (flags & F_NAME) != 0), menu.left + 10f * d, base + rowH * 2, panelText);
            c.drawText(row("health", (flags & F_HP) != 0), menu.left + 10f * d, base + rowH * 3, panelText);
            c.drawText(row("distance", (flags & F_DIST) != 0), menu.left + 10f * d, base + rowH * 4, panelText);
            c.drawText(row("team check", (flags & F_TEAM) != 0), menu.left + 10f * d, base + rowH * 5, panelText);
            c.drawText("range: " + (maxDist <= 0f ? "all" : String.format(Locale.US, "%.0fm", maxDist)),
                    menu.left + 10f * d, base + rowH * 6, panelText);
            panelText.setColor(0xFF9E9E9E);
            c.drawText("save", menu.left + 10f * d, base + rowH * 7, panelText);
            c.drawText("reload", menu.left + 10f * d, base + rowH * 8, panelText);
            c.drawText("close", menu.left + 10f * d, base + rowH * 9, panelText);
        }

        private String enemiesLabel() {
            int enemy = 0;
            for (int i = 0; i < Math.min(count, MAX_ENT); i++) {
                if (out[1 + i * STRIDE + 9] <= 0.5f) enemy++;
            }
            return enemy + "e";
        }

        private static String row(String label, boolean on) {
            return label + ": " + (on ? "on" : "off");
        }
    }
}
