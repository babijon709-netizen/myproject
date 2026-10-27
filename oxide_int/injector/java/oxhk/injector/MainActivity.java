// MainActivity.java — one screen: what will happen, a button, and a log.
//
// Flow (no root, no adb, no PC):
//   installed game apks -> copy out -> patch manifest + add payload dex to base,
//   add liboxhk.so to the split -> sign both with the baked key -> PackageInstaller session.
//
// Target-specific by design: TARGET_PACKAGE and the original Application class name are baked in.

package oxhk.injector;

import android.app.Activity;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.InputStream;
import java.util.Arrays;

public final class MainActivity extends Activity {
    public static final String ACTION_INSTALL_RESULT = "oxhk.injector.INSTALL_RESULT";

    private static final String TARGET_PACKAGE = "com.catsbit.oxidesurvivalisland";
    private static final String NEW_APP_CLASS = "oxhk.App";
    private static final int MIN_SDK = 26;

    private TextView log;
    private Button go;
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
        root.addView(title, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView info = new TextView(this);
        info.setText("signing cert sha-256 (what the game will see):\n" + Sign.certSha256(this));
        info.setTextColor(0xFF9A9A9A);
        info.setTextSize(TypedValue.COMPLEX_UNIT_SP, 9);
        root.addView(info, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        go = new Button(this);
        go.setText("patch + install");
        go.setAllCaps(false);
        go.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { run(); }
        });
        root.addView(go, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        ScrollView scroll = new ScrollView(this);
        log = new TextView(this);
        log.setTextColor(0xFFD0D0D0);
        log.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10);
        log.setTypeface(android.graphics.Typeface.MONOSPACE);
        scroll.addView(log);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        root.addView(scroll, lp);

        setContentView(root);

        if (ACTION_INSTALL_RESULT.equals(getIntent().getAction())) {
            int status = getIntent().getIntExtra("android.content.pm.extra.STATUS", -1);
            String msg = getIntent().getStringExtra("android.content.pm.extra.STATUS_MESSAGE");
            append(status >= 0 ? "installer: " + (status == 0 ? "ok" : "status " + status) + " " + msg
                               : "installer: no status returned");
        }
    }

    private void append(String s) {
        android.util.Log.i("oxhk.injector", s);
        final String line = s;
        ui.post(new Runnable() {
            @Override public void run() {
                log.append(line + "\n");
            }
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
                    android.util.Log.e("oxhk.injector", "patch failed", t);
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
        PackageInfo pi;
        try {
            pi = pm.getPackageInfo(TARGET_PACKAGE, 0);
        } catch (PackageManager.NameNotFoundException e) {
            append("game not installed: " + TARGET_PACKAGE);
            return;
        }
        ApplicationInfo ai = pi.applicationInfo;
        File base = new File(ai.sourceDir);
        append("base:  " + base + "  (" + base.length() + " B)");
        if (base.length() < 1024) throw new IllegalStateException("cannot read the game's base.apk: " + base);

        File split = null;
        if (ai.splitSourceDirs != null) {
            for (String s : ai.splitSourceDirs) {
                if (s == null) continue;
                append("split: " + s + "  (" + new File(s).length() + " B)");
                if (split == null) split = new File(s);
            }
        }
        if (split == null) throw new IllegalStateException("no split installed — expected split_config.arm64_v8a");
        append("splits: " + Arrays.toString(ai.splitSourceDirs));

        File work = new File(getCacheDir(), "oxhk");
        deleteTree(work);
        if (!work.mkdirs()) throw new IllegalStateException("cannot create " + work);

        byte[] payloadDex = readAsset("payload/classes.dex");
        byte[] payloadLib = readAsset("payload/liboxhk.so");
        append("payload: dex " + payloadDex.length + " B, lib " + payloadLib.length + " B");

        File outBaseZip = new File(work, "base-patched.apk");
        File outSplitZip = new File(work, "split-patched.apk");
        String summary = Pack.patchBase(base, outBaseZip, payloadDex, NEW_APP_CLASS);
        append("base patched: " + summary);
        Pack.patchSplit(split, outSplitZip, payloadLib);
        append("split patched: lib/arm64-v8a/liboxhk.so");

        File signedBase = new File(work, "base-oxhk.apk");
        File signedSplit = new File(work, "split_config.arm64_v8a-oxhk.apk");
        append("signing…");
        Sign.sign(this, outBaseZip, signedBase, MIN_SDK);
        Sign.sign(this, outSplitZip, signedSplit, MIN_SDK);
        append("signed: " + signedBase.length() + " B / " + signedSplit.length() + " B");

        append("handing over to PackageInstaller (it will ask to uninstall the store build first — "
                + "the signature changed)");
        Install.install(this, signedBase, signedSplit);
    }

    private byte[] readAsset(String name) throws Exception {
        InputStream is = getAssets().open(name);
        try {
            return Pack.readAll(is);
        } finally {
            is.close();
        }
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }
}
