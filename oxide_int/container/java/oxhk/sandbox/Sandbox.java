// Sandbox.java — the container core (stage 1: the game's own Application, inside our process).
//
// What the probe proved on the device:
//   * dlopen from /data/app/.../lib/arm64 works → the game's native libs do NOT have to be
//     copied into our apk, and /proc/self/maps shows the game's real paths
//   * the game's Application <clinit> runs in our process → the packer accepts us
//   * ActivityThread / VMRuntime are reflectable → the hidden-api wall is not in the way
//
// So: build a Resources/AssetManager that covers base + every split, load the game's dex with
// the game's lib dir as the library search path, present a Context that answers with the game's
// package name, and run the game's Application against it. The apks themselves stay untouched
// and store-signed.

package oxhk.sandbox;

import android.app.Application;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.util.DisplayMetrics;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

public final class Sandbox {
    public static final String GAME = "com.catsbit.oxidesurvivalisland";
    private static final String TAG = "oxhk.sandbox";

    public interface Sink { void line(String s); }

    /** answer with the game's package name, or with our own — the engine derives its data paths
     *  from it (getExternalFilesDir/getExternalCacheDir are called through JNI), so this decides
     *  whether Unity writes next to our apk or into the game's external dir. */
    public static volatile boolean SPOOF = true;

    private static Sandbox instance;
    private static Sink log;

    public static void setLog(Sink l) { log = l; }

    private static File traceFile;

    static void say(String s) {
        Log.i(TAG, s);
        Sink l = log;
        if (l != null) l.line(s);
        if (traceFile != null) {
            try {
                FileOutputStream fos = new FileOutputStream(traceFile, true);
                fos.write((s + "\n").getBytes("UTF-8"));
                fos.close();
            } catch (Throwable ignored) {
            }
        }
    }

    /** the trace also goes to disk: a process that dies takes its UI with it. */
    private static void openTrace(Context c) {
        if (traceFile != null) return;
        traceFile = new File(c.getFilesDir(), "last-run.txt");
        try {
            FileOutputStream fos = new FileOutputStream(traceFile, false);
            fos.write(("run " + new java.util.Date() + "\n").getBytes("UTF-8"));
            fos.close();
        } catch (Throwable ignored) {
        }
    }

    public static synchronized Sandbox get(Context host) {
        if (instance == null) {
            openTrace(host.getApplicationContext());
            instance = new Sandbox(host.getApplicationContext());
        }
        return instance;
    }

    private final Context host;
    PackageInfo info;
    ApplicationInfo ai;
    ClassLoader loader;
    AssetManager am;
    Resources res;
    Context ctx;
    Application app;

    private Sandbox(Context host) { this.host = host; }

    // ── stage 1 ────────────────────────────────────────────────────────────────────────────

    /** target info + classloader + resources. Cheap, safe, repeatable. */
    public synchronized void init() throws Exception {
        if (loader != null) return;
        say("== target");
        PackageManager pm = host.getPackageManager();
        info = pm.getPackageInfo(GAME, PackageManager.GET_ACTIVITIES
                | PackageManager.GET_SERVICES | PackageManager.GET_RECEIVERS);
        ai = info.applicationInfo;
        say("app class  " + ai.className);
        say("sourceDir  " + ai.sourceDir);
        say("libDir     " + ai.nativeLibraryDir);

        DisplayMetrics dm = host.getResources().getDisplayMetrics();
        Configuration cfg = new Configuration(host.getResources().getConfiguration());

        // Two ways to get an AssetManager that covers the game's apks. The manual one is what
        // every cloner does, but addAssetPath is a hidden API and on newer Android it can return
        // a cookie while nested lookups still fail — which is exactly what happened (the game's
        // top-level assets listed fine, assets/bin/Data/* was FileNotFound). The system-built one
        // (getResourcesForApplication) knows the package's splits, so it is tried first.
        say("== assets (" + (1 + splitCount()) + " apks)");
        String[] keys = {"assets/bin/Data/unity_app_guid", "bin/Data/unity_app_guid"};

        AssetManager system = null;
        try {
            Resources gr = pm.getResourcesForApplication(ai);
            system = gr.getAssets();
            say("system am  " + system + "  " + describe(system, keys));
        } catch (Throwable t) {
            say("system am  FAILED " + t);
        }

        AssetManager manual = AssetManager.class.getConstructor().newInstance();
        Method addAssetPath = AssetManager.class.getMethod("addAssetPath", String.class);
        int cookie = (Integer) addAssetPath.invoke(manual, ai.sourceDir);
        say("addAssetPath(base) -> " + cookie + (cookie != 0 ? " ok" : " FAILED"));
        for (File f : splits()) {
            int c = (Integer) addAssetPath.invoke(manual, f.getAbsolutePath());
            say("addAssetPath(" + f.getName() + ") -> " + c + (c != 0 ? " ok" : " FAILED"));
        }
        say("manual am  " + describe(manual, keys));

        boolean systemWorks = system != null && canOpen(system, keys);
        boolean manualWorks = canOpen(manual, keys);
        if (systemWorks) {
            am = system;
            say("using system AssetManager");
        } else if (manualWorks) {
            am = manual;
            say("using manual AssetManager");
        } else {
            // last resort: give the engine both — resources from the system one, assets from the
            // manual one, and let it tell us which call fails
            am = manual;
            say("NEITHER AssetManager can open the unity data — paths checked: " + topLevel(manual));
        }

        res = new Resources(am, dm, cfg);
        say("resources  " + res.getAssets() + ", config " + cfg.orientation);

        say("== dex");
        File odex = new File(host.getCodeCacheDir(), "sandbox");
        if (!odex.exists()) odex.mkdirs();
        long t0 = System.currentTimeMillis();
        loader = new dalvik.system.DexClassLoader(ai.sourceDir, odex.getAbsolutePath(),
                ai.nativeLibraryDir, ClassLoader.getSystemClassLoader());
        Class<?> appClass = Class.forName(ai.className, false, loader);
        say("app class  " + appClass.getName() + " extends "
                + appClass.getSuperclass().getName() + "  ("
                + (System.currentTimeMillis() - t0) + " ms)");

        ctx = new Ctx(host, this);

        say("== where the unity data lives");
        for (String dir : new String[]{"bin", "bin/Data", "assets/bin", "assets/bin/Data"}) {
            say("list(" + dir + ") -> " + listOf(am, dir));
        }

        say("== what unity will see");
        for (String n : new String[]{"assets/bin/Data/unity_app_guid",
                "assets/bin/Data/data.unity3d",
                "assets/bin/Data/Managed/Metadata/global-metadata.dat"}) {
            say("asset " + n + ": " + assetInfo(n));
        }
        say("top-level: " + topLevel());
        say("files      " + host.getFilesDir() + "  free " + mb(host.getFilesDir().getUsableSpace()));
        say("cache      " + host.getCacheDir() + "  free " + mb(host.getCacheDir().getUsableSpace()));
        say("extFiles   " + host.getExternalFilesDir(null));
        say("extCache   " + host.getExternalCacheDir());
        say("ok");
    }

    /** stage 1b: the game's Application, attached and created. */
    public synchronized void startApplication() throws Exception {
        init();
        if (app != null) { say("application already running"); return; }
        say("== application");
        Class<?> appClass = Class.forName(ai.className, true, loader);
        Application a = (Application) appClass.newInstance();
        Method attach = Application.class.getDeclaredMethod("attach", Context.class);
        attach.setAccessible(true);
        attach.invoke(a, ctx);
        app = a;
        say("attached to sandbox context");
        try {
            a.onCreate();
            say("onCreate ok — packer + Application alive in our process");
        } catch (Throwable t) {
            say("onCreate threw: " + t.getClass().getName() + ": " + t.getMessage());
            Log.e(TAG, "application onCreate", t);
        }
    }

    private static String listOf(AssetManager a, String dir) {
        try {
            String[] l = a.list(dir);
            return l == null ? "null" : (l.length + " " + java.util.Arrays.toString(l));
        } catch (Throwable t) {
            return "FAILED " + t;
        }
    }

    private static String describe(AssetManager a, String[] keys) {
        StringBuilder sb = new StringBuilder();
        for (String k : keys) sb.append(k.startsWith("assets/") ? "  [" : "  (")
                .append(k).append(")=").append(canOpen(a, k));
        return sb.toString();
    }

    static boolean canOpen(AssetManager a, String... keys) {
        for (String k : keys) {
            java.io.InputStream in = null;
            try {
                in = a.open(k);
                return true;
            } catch (Throwable ignored) {
            } finally {
                if (in != null) try { in.close(); } catch (Throwable ignored) {}
            }
        }
        return false;
    }

    /** can the merged AssetManager actually open that asset? */
    String assetInfo(String name) {
        java.io.InputStream in = null;
        try {
            in = am.open(name);
            byte[] buf = new byte[1 << 16];
            int n = 0, r;
            while ((r = in.read(buf)) > 0) n += r;
            return n + " B (readable)";
        } catch (Throwable t) {
            return "FAILED " + t.getClass().getSimpleName() + ": " + t.getMessage();
        } finally {
            if (in != null) try { in.close(); } catch (Throwable ignored) {}
        }
    }

    static String mb(long v) { return String.format("%.1f MB", v / 1048576.0); }

    static String topLevel(AssetManager a) {
        try {
            return java.util.Arrays.toString(a.list(""));
        } catch (Throwable t) {
            return "?" + t;
        }
    }

    String topLevel() {
        try {
            String[] l = am.list("");
            return java.util.Arrays.toString(l);
        } catch (Throwable t) {
            return "?" + t;
        }
    }

    // ── helpers ────────────────────────────────────────────────────────────────────────────

    public ClassLoader loader() throws Exception { init(); return loader; }
    public Resources resources() throws Exception { init(); return res; }
    public AssetManager assets() throws Exception { init(); return am; }
    public ApplicationInfo appInfo() throws Exception { init(); return ai; }
    public Application application() throws Exception { init(); return app; }

    List<File> splits() {
        List<File> out = new ArrayList<File>();
        if (ai != null && ai.splitSourceDirs != null) {
            for (String s : ai.splitSourceDirs) if (s != null) out.add(new File(s));
        }
        return out;
    }

    int splitCount() {
        return ai != null && ai.splitSourceDirs != null ? ai.splitSourceDirs.length : 0;
    }

    /** Context handed to the game: answers with the game's identity, not ours. */
    static final class Ctx extends ContextWrapper {
        private final Sandbox s;

        Ctx(Context base, Sandbox s) {
            super(base);
            this.s = s;
        }

        @Override public String getPackageName() { return SPOOF ? GAME : super.getPackageName(); }
        @Override public ClassLoader getClassLoader() { return s.loader; }
        @Override public Resources getResources() { return s.res; }
        @Override public AssetManager getAssets() { return s.am; }
        @Override public ApplicationInfo getApplicationInfo() { return s.ai; }
        @Override public Context getApplicationContext() { return s.app != null ? s.app : this; }
        @Override public String getOpPackageName() { return SPOOF ? GAME : super.getOpPackageName(); }
    }
}
