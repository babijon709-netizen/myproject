// MainActivity.java — probe stage of the container plan.
//
// Nothing is patched here. The point is to find out, on the real device, whether a dual-space
// container is even possible on this phone: can we read the installed game's apks and libs, can
// the loader map them from any location, are hidden APIs reachable, does the game's Application
// class load from its own dex.
//
// Everything lands in one text report, so a single screenshot answers the design question.

package oxhk.sandbox;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.os.Bundle;
import android.os.Looper;
import android.util.Log;
import android.util.TypedValue;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public final class MainActivity extends Activity {
    private static final String GAME = "com.catsbit.oxidesurvivalisland";
    private static final String TAG = "oxhk.sandbox";

    static {
        System.loadLibrary("probe");
    }

    private static native String nativeProbe(String gameLib, String copy, byte[] libBytes);

    private TextView log;
    private Button run;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (14 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);
        root.setBackgroundColor(0xFF0E0E10);

        TextView title = new TextView(this);
        title.setText("oxhk sandbox — container probe\n" + GAME);
        title.setTextColor(0xFFFFD75E);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        root.addView(title, lp());

        run = new Button(this);
        run.setText("run probe");
        run.setAllCaps(false);
        run.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { go(); }
        });
        root.addView(run, lp());

        Button stage1 = new Button(this);
        stage1.setText("1. start game application");
        stage1.setAllCaps(false);
        stage1.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { stage1(); }
        });
        root.addView(stage1, lp());

        Button stage3 = new Button(this);
        stage3.setText("3. grant all-files access (for the game's data dir)");
        stage3.setAllCaps(false);
        stage3.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { allFiles(); }
        });
        root.addView(stage3, lp());

        Button logs = new Button(this);
        logs.setText("dump logcat");
        logs.setAllCaps(false);
        logs.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                append("\n== logcat (this process)");
                append(Logs.recent(4000));
            }
        });
        root.addView(logs, lp());

        Button stage2 = new Button(this);
        stage2.setText("2. launch unity");
        stage2.setAllCaps(false);
        stage2.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                startActivity(new Intent(MainActivity.this, UnityActivity.class));
            }
        });
        root.addView(stage2, lp());

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        Button share = new Button(this);
        share.setText("share log");
        share.setAllCaps(false);
        share.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { shareLog(); }
        });
        row.addView(share, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        Button copy = new Button(this);
        copy.setText("copy log");
        copy.setAllCaps(false);
        copy.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { copyLog(); }
        });
        row.addView(copy, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        root.addView(row, lp());

        ScrollView scroll = new ScrollView(this);
        log = new TextView(this);
        log.setTextColor(0xFFD6D6D6);
        log.setTextSize(TypedValue.COMPLEX_UNIT_SP, 9);
        log.setTypeface(android.graphics.Typeface.MONOSPACE);
        log.setTextIsSelectable(true);
        scroll.addView(log);
        root.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                0, 1f));

        setContentView(root);
        Logs.installCrashHandler(this);
        String crash = Logs.lastCrash(this);
        if (crash.length() > 0) {
            append("== last crash (previous run)");
            append(crash);
            Logs.clearCrash(this);
        }
        Sandbox.setLog(new Sandbox.Sink() {
            @Override public void line(final String s) { append(s); }
        });
        append("probe = measurements, 1/2 = the container itself");
    }

    private LinearLayout.LayoutParams lp() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private void append(final String s) {
        Log.i(TAG, s);
        if (Looper.myLooper() == Looper.getMainLooper()) {
            log.append(s + "\n");
        } else {
            runOnUiThread(new Runnable() {
                @Override public void run() { log.append(s + "\n"); }
            });
        }
    }

    private void copyLog() {
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        cm.setPrimaryClip(ClipData.newPlainText("oxhk", log.getText()));
        append("(copied)");
    }

    private void shareLog() {
        File f = new File(getExternalFilesDir(null), "probe.txt");
        try {
            FileOutputStream fos = new FileOutputStream(f);
            fos.write(log.getText().toString().getBytes("UTF-8"));
            fos.close();
        } catch (Throwable t) {
            append("write failed: " + t);
        }
        Intent i = new Intent(Intent.ACTION_SEND);
        i.setType("text/plain");
        i.putExtra(Intent.EXTRA_TEXT, log.getText().toString());
        i.putExtra(Intent.EXTRA_STREAM, android.net.Uri.fromFile(f));
        startActivity(Intent.createChooser(i, "share probe log"));
    }

    /** the game's data dir is /storage/emulated/0/Android/data/<game>/ — we may only write there
     *  with all-files access, and Unity computes persistentDataPath from the package name. */
    private void allFiles() {
        try {
            boolean ok = android.os.Environment.isExternalStorageManager();
            append("isExternalStorageManager: " + ok);
            if (!ok) {
                android.content.Intent i = new android.content.Intent(
                        android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        android.net.Uri.parse("package:" + getPackageName()));
                startActivity(i);
                append("grant it in the settings screen that just opened, then come back");
                return;
            }
            File dir = new File("/storage/emulated/0/Android/data/" + Sandbox.GAME + "/files");
            boolean made = dir.exists() || dir.mkdirs();
            File probe = new File(dir, "oxhk-write-test");
            boolean wrote = false;
            try {
                java.io.FileOutputStream fos = new java.io.FileOutputStream(probe);
                fos.write(42);
                fos.close();
                wrote = true;
                probe.delete();
            } catch (Throwable t) {
                append("write failed: " + t);
            }
            append("game data dir: " + dir + " exists=" + made + " writable=" + wrote);
        } catch (Throwable t) {
            append("allFiles failed: " + t);
        }
    }

    private void stage1() {
        try {
            append("\n== stage 1: game application");
            Sandbox.get(this).startApplication();
        } catch (Throwable t) {
            append("FAILED: " + t);
            Log.e(TAG, "stage 1", t);
        }
    }

    private void go() {
        run.setEnabled(false);
        run.setText("working…");
        try {
            append(probe());
        } catch (Throwable t) {
            append("FAILED: " + t);
            Log.e(TAG, "probe failed", t);
        } finally {
            run.setEnabled(true);
            run.setText("run probe");
        }
    }

    private String probe() throws Exception {
        StringBuilder sb = new StringBuilder();
        PackageManager pm = getPackageManager();

        PackageInfo pi;
        try {
            pi = pm.getPackageInfo(GAME, PackageManager.GET_SIGNING_CERTIFICATES
                    | PackageManager.GET_ACTIVITIES);
        } catch (PackageManager.NameNotFoundException e) {
            return "game not installed: " + GAME + "\n";
        }
        ApplicationInfo ai = pi.applicationInfo;

        sb.append("== game\n");
        sb.append("version     ").append(pi.versionName).append(" (")
                .append(pi.getLongVersionCode()).append(")\n");
        sb.append("app class   ").append(ai.className).append("\n");
        sb.append("sourceDir   ").append(ai.sourceDir).append('\n');
        sb.append("libDir      ").append(ai.nativeLibraryDir).append('\n');
        sb.append("installer   ");
        try {
            sb.append(pm.getInstallSourceInfo(GAME).getInstallingPackageName());
        } catch (Throwable t) {
            sb.append("?").append(t);
        }
        sb.append('\n');

        sb.append("signer      ");
        try {
            Signature[] s = pi.signingInfo.getApkContentsSigners();
            sb.append(sha256(s[0].toByteArray())).append(" (")
                    .append(s.length).append(" signer(s))");
        } catch (Throwable t) {
            sb.append("?").append(t);
        }
        sb.append('\n');

        List<File> splits = new ArrayList<File>();
        if (ai.splitSourceDirs != null) {
            for (String s : ai.splitSourceDirs) if (s != null) splits.add(new File(s));
        }
        sb.append("splits      ").append(splits.size()).append('\n');

        File arm64 = null;
        for (File f : splits) if (hasEntry(f, "lib/arm64-v8a/libil2cpp.so")) arm64 = f;
        sb.append("arm64 split ").append(arm64 == null ? "none" : arm64.getName()).append('\n');

        File libDir = new File(ai.nativeLibraryDir);
        String[] names = libDir.list();
        sb.append("libDir list ").append(names == null ? "denied" : names.length + " entries")
                .append('\n');
        sb.append("free space  ").append(mb(getFilesDir().getUsableSpace())).append(" (files)\n");
        sb.append("total libs  ");
        long libBytes = 0;
        if (names != null) for (String n : names) libBytes += new File(libDir, n).length();
        sb.append(mb(libBytes)).append('\n');

        // libmain.so is 6.6 KB and only needs system libs — the cheapest honest test
        byte[] libBytesArr = null;
        File copy = new File(getFilesDir(), "libmain.so");
        if (arm64 != null) {
            try {
                libBytesArr = readEntry(arm64, "lib/arm64-v8a/libmain.so");
                FileOutputStream fos = new FileOutputStream(copy);
                fos.write(libBytesArr);
                fos.close();
                sb.append("copied      ").append(libBytesArr.length).append(" B -> ")
                        .append(copy.getAbsolutePath()).append('\n');
            } catch (Throwable t) {
                sb.append("copy failed ").append(t).append('\n');
            }
        }
        if (libBytesArr == null) libBytesArr = new byte[0];

        sb.append("\n== loader\n");
        sb.append(nativeProbe(libDir.getAbsolutePath() + "/libmain.so", copy.getAbsolutePath(),
                libBytesArr));

        sb.append("\n== hidden api\n");
        sb.append(reflectTest("android.app.ActivityThread", "currentActivityThread"));
        sb.append(reflectTest("dalvik.system.VMRuntime", "getRuntime"));
        sb.append(hiddenApiBypass());

        sb.append("\n== game dex\n");
        try {
            ClassLoader cl = new dalvik.system.DexClassLoader(ai.sourceDir,
                    getCodeCacheDir().getAbsolutePath(), ai.nativeLibraryDir,
                    ClassLoader.getSystemClassLoader());
            Class<?> appClass = Class.forName(ai.className, false, cl);
            sb.append("app class   loaded, super=").append(appClass.getSuperclass().getName())
                    .append('\n');
            try {
                Class.forName(ai.className, true, cl);
                sb.append("<clinit>    ok (packer loaded its libs!)\n");
            } catch (Throwable t) {
                sb.append("<clinit>    ").append(t.getClass().getSimpleName()).append(": ")
                        .append(t.getMessage()).append('\n');
            }
        } catch (Throwable t) {
            sb.append("dex load    FAILED ").append(t.getClass().getSimpleName()).append(": ")
                    .append(t.getMessage()).append('\n');
        }
        return sb.toString();
    }

    private static String reflectTest(String cls, String method) {
        try {
            Class<?> c = Class.forName(cls);
            java.lang.reflect.Method m = c.getDeclaredMethod(method);
            m.setAccessible(true);
            return String.format("%-28s ok\n", cls.substring(cls.lastIndexOf('.') + 1));
        } catch (Throwable t) {
            return String.format("%-28s %s\n", cls.substring(cls.lastIndexOf('.') + 1),
                    t.getClass().getSimpleName() + ": " + t.getMessage());
        }
    }

    /** the standard Unsafe double-reflection trick: kill hidden-api enforcement for this process. */
    private static String hiddenApiBypass() {
        try {
            Class<?> unsafeClass = Class.forName("sun.misc.Unsafe");
            java.lang.reflect.Field f = unsafeClass.getDeclaredField("theUnsafe");
            f.setAccessible(true);
            Object unsafe = f.get(null);
            java.lang.reflect.Method forName = unsafeClass.getMethod("getDeclaredMethod",
                    Class.class, String.class, Class[].class);
            Class<?> vmRuntime = Class.forName("dalvik.system.VMRuntime");
            java.lang.reflect.Method setExemptions = (java.lang.reflect.Method) forName.invoke(
                    unsafe, vmRuntime, "setHiddenApiExemptions", new Class[]{String[].class});
            Object runtime = vmRuntime.getDeclaredMethod("getRuntime").invoke(null);
            setExemptions.invoke(runtime, new Object[]{new String[]{"L"}});
            return "unsafe exemption   ok\n";
        } catch (Throwable t) {
            return "unsafe exemption   " + t.getClass().getSimpleName() + ": " + t.getMessage() + "\n";
        }
    }

    private static boolean hasEntry(File apk, String name) {
        ZipFile z = null;
        try {
            z = new ZipFile(apk);
            return z.getEntry(name) != null;
        } catch (Throwable t) {
            return false;
        } finally {
            if (z != null) try { z.close(); } catch (Throwable ignored) {}
        }
    }

    private static byte[] readEntry(File apk, String name) throws Exception {
        ZipFile z = new ZipFile(apk);
        try {
            ZipEntry e = z.getEntry(name);
            if (e == null) throw new IllegalStateException("no " + name);
            InputStream in = z.getInputStream(e);
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) bos.write(buf, 0, n);
            in.close();
            return bos.toByteArray();
        } finally {
            z.close();
        }
    }

    private static String sha256(byte[] data) {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] d = md.digest(data);
            StringBuilder sb = new StringBuilder();
            for (byte x : d) sb.append(String.format("%02x", x));
            return sb.toString();
        } catch (Throwable t) {
            return "?" + t;
        }
    }

    private static String mb(long v) { return String.format("%.1f MB", v / 1048576.0); }
}
