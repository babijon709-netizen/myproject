// MainActivity.java — one screen: what will happen, two buttons, and a log.
//
// Flow (no root, no adb, no PC):
//   installed game apks -> patch base (manifest + payload dex) and the arm64 split (liboxhk.so),
//   copy the remaining splits through -> sign all of them with the baked key ->
//   PackageInstaller session containing base + every split, which is what the system needs for
//   a split app.
//
// Target-specific by design: TARGET_PACKAGE is baked in, the original Application class is read
// from the manifest itself and only the replacement name is fixed (NEW_APP_CLASS).

package oxhk.injector;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.TypedValue;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;

public final class MainActivity extends Activity {
    public static final String ACTION_INSTALL_RESULT = "oxhk.injector.INSTALL_RESULT";

    private static final String TARGET_PACKAGE = "com.catsbit.oxidesurvivalisland";
    private static final String NEW_APP_CLASS = "oxhk.App";
    private static final int MIN_SDK = 25;      // game's minSdk, decides v1/v2 in apksig

    private TextView log;
    private Button go;
    private Button uninstall;
    private final Handler ui = new Handler(Looper.getMainLooper());

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (16 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);
        root.setBackgroundColor(0xFF101012);

        TextView title = new TextView(this);
        title.setText("oxhk injector\n" + TARGET_PACKAGE + "  ->  " + NEW_APP_CLASS);
        title.setTextColor(0xFFFFD75E);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        root.addView(title, lp());

        TextView info = new TextView(this);
        info.setText("signing cert sha-256 (what the game will see):\n" + Sign.certSha256(this));
        info.setTextColor(0xFF9A9A9A);
        info.setTextSize(TypedValue.COMPLEX_UNIT_SP, 9);
        root.addView(info, lp());

        go = new Button(this);
        go.setText("patch + install");
        go.setAllCaps(false);
        go.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { run(); }
        });
        root.addView(go, lp());

        uninstall = new Button(this);
        uninstall.setText("uninstall game (needed once: signature changes)");
        uninstall.setAllCaps(false);
        uninstall.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                try {
                    startActivity(new Intent(Intent.ACTION_DELETE, Uri.parse("package:" + TARGET_PACKAGE)));
                } catch (Throwable t) {
                    append("uninstall intent failed: " + t);
                }
            }
        });
        root.addView(uninstall, lp());

        ScrollView scroll = new ScrollView(this);
        log = new TextView(this);
        log.setTextColor(0xFFD0D0D0);
        log.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10);
        log.setTypeface(android.graphics.Typeface.MONOSPACE);
        scroll.addView(log);
        root.addView(scroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        setContentView(root);

        if (ACTION_INSTALL_RESULT.equals(getIntent().getAction())) {
            int status = getIntent().getIntExtra("android.content.pm.extra.STATUS", Integer.MIN_VALUE);
            String msg = getIntent().getStringExtra("android.content.pm.extra.STATUS_MESSAGE");
            append("installer status=" + status + (status == 0 ? " (ok)" : "") + " " + (msg == null ? "" : msg));
        }
    }

    private LinearLayout.LayoutParams lp() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private void append(String s) {
        Log.i("oxhk.injector", s);
        final String line = s;
        ui.post(new Runnable() {
            @Override public void run() { log.append(line + "\n"); }
        });
    }

    private void run() {
        go.setEnabled(false);
        go.setText("working…");
        new Thread(new Runnable() {
            @Override public void run() {
                try {
                    patch();
                } catch (Throwable t) {
                    append("FAILED: " + t);
                    Log.e("oxhk.injector", "patch failed", t);
                } finally {
                    ui.post(new Runnable() {
                        @Override public void run() {
                            go.setEnabled(true);
                            go.setText("patch + install");
                        }
                    });
                }
            }
        }).start();
    }

    private void patch() throws Exception {
        PackageManager pm = getPackageManager();
        ApplicationInfo ai;
        try {
            ai = pm.getApplicationInfo(TARGET_PACKAGE, 0);
        } catch (PackageManager.NameNotFoundException e) {
            append("game not installed: " + TARGET_PACKAGE);
            return;
        }

        File base = new File(ai.sourceDir);
        if (!base.exists() || base.length() < 1024)
            throw new IllegalStateException("cannot read " + base);
        append("base:  " + base.getName() + "  " + mb(base.length()));

        List<File> splits = new ArrayList<File>();
        if (ai.splitSourceDirs != null) {
            for (String s : ai.splitSourceDirs) {
                if (s == null) continue;
                File f = new File(s);
                splits.add(f);
                append("split: " + f.getName() + "  " + mb(f.length()));
            }
        }

        File arm64 = null;
        for (File f : splits) {
            if (Pack.hasArm64Il2Cpp(f)) { arm64 = f; break; }
        }
        if (arm64 == null && !splits.isEmpty()) {
            // fall back to the name, in case the lib set moved between builds
            for (File f : splits) {
                if (f.getName().contains("arm64_v8a")) { arm64 = f; break; }
            }
        }
        if (arm64 == null) throw new IllegalStateException("no arm64 split with libil2cpp.so");
        append("patch target: " + arm64.getName() + "   (splits total: " + splits.size() + ")");

        File work = new File(getCacheDir(), "oxhk");
        deleteTree(work);
        if (!work.mkdirs()) throw new IllegalStateException("cannot create " + work);

        long need = base.length() + 192L * 1024 * 1024;
        for (File f : splits) need += f.length();
        long free = work.getUsableSpace();
        append("cache free " + mb(free) + ", need about " + mb(need));
        if (free < need) throw new IllegalStateException("not enough space in app cache (" + work + ")");

        byte[] payloadDex = readAsset("payload/classes.dex");
        byte[] payloadLib = readAsset("payload/liboxhk.so");
        append("payload: dex " + payloadDex.length + " B, lib " + payloadLib.length + " B");

        List<File> signed = new ArrayList<File>();

        long t0 = System.currentTimeMillis();
        File outBase = new File(work, "base.apk");
        String summary = Pack.patchBase(base, outBase, payloadDex, NEW_APP_CLASS);
        append("base patched: " + summary + "  " + el(t0));

        List<File> staged = new ArrayList<File>();
        for (File f : splits) {
            File o = new File(work, f.getName());
            boolean inject = f.equals(arm64);
            t0 = System.currentTimeMillis();
            Pack.patchSplit(f, o, payloadLib, inject);
            staged.add(o);
            append((inject ? "split + liboxhk.so: " : "split copied: ") + f.getName() + "  " + el(t0));
        }

        t0 = System.currentTimeMillis();
        File sb = new File(work, "base-oxhk.apk");
        Sign.sign(this, outBase, sb, MIN_SDK);
        signed.add(sb);
        append("signed base  " + mb(sb.length()));

        for (int i = 0; i < staged.size(); i++) {
            File out = new File(work, splits.get(i).getName().replace(".apk", "-oxhk.apk"));
            t0 = System.currentTimeMillis();
            Sign.sign(this, staged.get(i), out, MIN_SDK);
            signed.add(out);
            append("signed " + out.getName() + "  " + mb(out.length()) + "  " + el(t0));
        }

        append("handing " + signed.size() + " apks to PackageInstaller in one session");
        Install.install(this, signed.toArray(new File[0]));
    }

    private byte[] readAsset(String name) throws Exception {
        InputStream is = getAssets().open(name);
        try {
            return Pack.readAll(is);
        } finally {
            is.close();
        }
    }

    private static String mb(long v) { return String.format("%.1f MB", v / 1048576.0); }

    private static String el(long t0) { return "(" + (System.currentTimeMillis() - t0) + " ms)"; }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }
}
