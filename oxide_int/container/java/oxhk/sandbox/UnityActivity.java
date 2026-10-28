// UnityActivity.java — stage 2: the game's engine, booted from our own activity.
//
// This is why the container does not need the Activity.attach() surgery other cloners do: the
// activity that hosts Unity is *ours*, so it can answer with the game's identity — package name,
// assets (base + all splits merged), resources, classloader, ApplicationInfo — and the engine
// finds everything where it expects it, through its own virtual calls. The game's apk is never
// modified and keeps its store signature.
//
// This build is Unity 6 (2023.x) layout, read out of the game's own dex:
//   UnityPlayerActivity.onCreate:
//       mUnityPlayer = new UnityPlayerForActivityOrService(this, this);
//       setContentView(mUnityPlayer.getFrameLayout());
//   UnityPlayer's only constructor is (Context, a.o /*enum: context type*/, IUnityPlayerLifecycleEvents)
// so we construct the same subclass, pass a Proxy for IUnityPlayerLifecycleEvents (a 2-method
// interface) and forward the lifecycle by name — onResume/onPause/destroy here, resume/pause/quit
// on older Unity, both are tried.

package oxhk.sandbox;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.Bundle;
import android.util.Log;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;

public final class UnityActivity extends Activity {
    private static final String TAG = "oxhk.sandbox";

    private Sandbox s;
    private Object player;          // UnityPlayer / UnityPlayerForActivityOrService
    private Object lifecycle;       // IUnityPlayerLifecycleEvents proxy
    private FrameLayout root;       // engine view + our overlay on top
    private final StringBuilder trace = new StringBuilder();

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        Logs.installCrashHandler(this);
        Sandbox.setLog(new Sandbox.Sink() {
            @Override public void line(String l) { trace.append(l).append('\n'); }
        });
        s = Sandbox.get(this);

        root = new FrameLayout(this);
        setContentView(root);

        try {
            s.init();
            s.startApplication();
            boot();
        } catch (Throwable t) {
            Log.e(TAG, "boot failed", t);
            android.widget.TextView tv = new android.widget.TextView(this);
            tv.setText("boot failed:\n\n" + t + "\n\n— trace —\n" + trace
                    + "\nsee logcat (tag oxhk.sandbox)");
            tv.setTextColor(0xFFFF6666);
            tv.setTextSize(10);
            tv.setPadding(24, 24, 24, 24);
            root.addView(tv);
        }
    }

    // ── boot ──────────────────────────────────────────────────────────────────────────────

    private void boot() throws Exception {
        log("pkg=" + getPackageName());
        log("files=" + getFilesDir());
        log("external=" + getExternalFilesDir(null));
        log("libs=" + getApplicationInfo().nativeLibraryDir);
        log("codePath=" + getPackageCodePath());
        try {
            android.net.ConnectivityManager cm = (android.net.ConnectivityManager)
                    s.host().getSystemService(CONNECTIVITY_SERVICE);
            android.net.Network n = cm.getActiveNetwork();
            log("network=" + n + " caps="
                    + (n == null ? "-" : cm.getNetworkCapabilities(n)));
        } catch (Throwable t) {
            log("network check failed (missing permission?): " + t);
        }
        log("resPath=" + getPackageResourcePath());
        ClassLoader cl = s.loader();

        // IUnityPlayerLifecycleEvents: two methods, so a Proxy is enough
        try {
            Class<?> iface = Class.forName("com.unity3d.player.IUnityPlayerLifecycleEvents", true, cl);
            lifecycle = Proxy.newProxyInstance(cl, new Class<?>[]{iface}, new InvocationHandler() {
                @Override public Object invoke(Object p, Method m, Object[] a) {
                    Log.i(TAG, "lifecycle: " + m.getName());
                    return null;
                }
            });
            log("lifecycle proxy: " + iface.getName());
        } catch (Throwable t) {
            log("lifecycle proxy skipped: " + t);
            lifecycle = this;   // UnityPlayerActivity itself implements the interface
        }

        Class<?> cls = Class.forName("com.unity3d.player.UnityPlayerForActivityOrService",
                true, cl);
        Class<?> iface = null;
        try {
            iface = Class.forName("com.unity3d.player.IUnityPlayerLifecycleEvents", true, cl);
        } catch (Throwable ignored) {
        }

        // (Context, IUnityPlayerLifecycleEvents) is what UnityPlayerActivity uses
        for (Constructor<?> c : cls.getConstructors()) {
            Class<?>[] p = c.getParameterTypes();
            if (p.length != 2) continue;
            if (!p[0].isInstance(this) || (iface != null && p[1] != iface)) continue;
            c.setAccessible(true);
            player = c.newInstance(this, lifecycle);
            log("engine: " + cls.getSimpleName() + "(" + p[0].getSimpleName() + ", "
                    + p[1].getSimpleName() + ")");
            break;
        }
        if (player == null) throw new IllegalStateException("no usable constructor in "
                + cls.getName() + " " + java.util.Arrays.toString(cls.getConstructors()));

        View view = viewOf(player);
        root.addView(view, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        root.setKeepScreenOn(true);
        log("engine view: " + view.getClass().getName() + " — waiting for il2cpp");

        // the ESP: same payload as the injected build, but nothing to hook — the overlay is a
        // View in a window we own, and liboxhk.so ships inside this apk.
        try {
            oxhk.Oxhk.install(this);
            log("oxhk overlay installed");
        } catch (Throwable t) {
            log("oxhk overlay failed: " + t);
        }
    }

    private View viewOf(Object p) throws Exception {
        for (String name : new String[]{"getFrameLayout", "getView"}) {
            try {
                Method m = p.getClass().getMethod(name);
                Object v = m.invoke(p);
                if (v instanceof View) return (View) v;
            } catch (NoSuchMethodException ignored) {
            }
        }
        if (p instanceof View) return (View) p;
        throw new IllegalStateException("no view out of " + p.getClass().getName());
    }

    private void log(String s) throws Exception {
        Log.i(TAG, s);
        Sandbox.say(s);
    }

    /** call by name, trying the Unity 6 names first and the older ones after. */
    private void call(String... names) {
        if (player == null) return;
        for (String name : names) {
            try {
                Method m = player.getClass().getMethod(name);
                m.invoke(player);
                return;
            } catch (NoSuchMethodException ignored) {
            } catch (Throwable t) {
                Log.w(TAG, "UnityPlayer." + name + " failed: " + t);
                return;
            }
        }
    }

    // ── lifecycle, mirroring UnityPlayerActivity ─────────────────────────────────────────

    @Override protected void onStart() {
        super.onStart();
        call("onStart", "start");
    }

    @Override protected void onResume() {
        super.onResume();
        call("onResume", "resume");
    }

    @Override protected void onPause() {
        call("onPause", "pause");
        super.onPause();
    }

    @Override protected void onStop() {
        call("onStop", "stop");
        super.onStop();
    }

    @Override protected void onDestroy() {
        call("destroy", "quit");
        super.onDestroy();
    }

    @Override public void onWindowFocusChanged(boolean has) {
        super.onWindowFocusChanged(has);
        if (player == null) return;
        try {
            player.getClass().getMethod("windowFocusChanged", boolean.class)
                    .invoke(player, has);
        } catch (Throwable ignored) {
        }
    }

    @Override public void onConfigurationChanged(Configuration c) {
        super.onConfigurationChanged(c);
        if (player == null) return;
        try {
            player.getClass().getMethod("configurationChanged", Configuration.class)
                    .invoke(player, c);
        } catch (Throwable ignored) {
        }
    }

    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        if (player != null) {
            try {
                Object r = player.getClass().getMethod("injectEvent", KeyEvent.class)
                        .invoke(player, e);
                if (Boolean.TRUE.equals(r)) return true;
            } catch (Throwable ignored) {
            }
        }
        return super.dispatchKeyEvent(e);
    }

    @Override protected void onSaveInstanceState(Bundle out) {
        // the engine view has no ids worth saving; skip the hierarchy walk
    }

    // ── the game's identity, answered by our own activity ────────────────────────────────

    @Override public String getPackageName() {
        return Sandbox.SPOOF ? Sandbox.GAME : super.getPackageName();
    }

    @Override public String getOpPackageName() {
        return Sandbox.SPOOF ? Sandbox.GAME : super.getOpPackageName();
    }

    @Override public ClassLoader getClassLoader() {
        try {
            return s.loader();
        } catch (Throwable t) {
            return super.getClassLoader();
        }
    }

    @Override public Resources getResources() {
        try {
            return s.resources();
        } catch (Throwable t) {
            return super.getResources();
        }
    }

    @Override public AssetManager getAssets() {
        try {
            return s.assets();
        } catch (Throwable t) {
            return super.getAssets();
        }
    }

    @Override public String getPackageCodePath() {
        try {
            return s.appInfo().sourceDir;
        } catch (Throwable t) {
            return super.getPackageCodePath();
        }
    }

    @Override public String getPackageResourcePath() {
        try {
            return s.appInfo().sourceDir;
        } catch (Throwable t) {
            return super.getPackageResourcePath();
        }
    }

    @Override public ApplicationInfo getApplicationInfo() {
        try {
            return s.appInfo();
        } catch (Throwable t) {
            return super.getApplicationInfo();
        }
    }

    @Override public Context getApplicationContext() {
        try {
            Application a = s.application();
            if (a != null) return a;
        } catch (Throwable ignored) {
        }
        return super.getApplicationContext();
    }
}
