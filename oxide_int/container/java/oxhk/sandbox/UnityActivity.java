// UnityActivity.java — stage 2: the game's engine, booted from our own activity.
//
// This is why the container does not need the Activity.attach() surgery other cloners do: the
// activity that hosts Unity is *ours*, so it can simply answer with the game's identity —
// package name, assets (base + all splits merged), resources, classloader, ApplicationInfo —
// and `new UnityPlayer(this)` then finds everything where it expects it, through its own
// virtual calls. The game's apk is never modified and keeps its store signature.

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
import android.widget.FrameLayout;

import java.lang.reflect.Constructor;
import java.lang.reflect.Method;

public final class UnityActivity extends Activity {
    private static final String TAG = "oxhk.sandbox";

    private Sandbox s;
    private Object player;          // com.unity3d.player.UnityPlayer (a ViewGroup)
    private FrameLayout root;       // player + our overlay on top

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        Sandbox.setLog(null);
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
            tv.setText("boot failed:\n\n" + t + "\n\nsee logcat (tag oxhk.sandbox)");
            tv.setTextColor(0xFFFF6666);
            root.addView(tv);
        }
    }

    private void boot() throws Exception {
        ClassLoader cl = s.loader();
        Class<?> up = Class.forName("com.unity3d.player.UnityPlayer", true, cl);
        Constructor<?>[] ctors = up.getConstructors();
        player = null;
        Throwable last = null;
        for (Constructor<?> c : ctors) {
            Class<?>[] p = c.getParameterTypes();
            if (p.length != 1) continue;
            Object arg = null;
            if (p[0].isInstance(this)) arg = this;
            else if (p[0].isInstance(s.ctx)) arg = s.ctx;
            if (arg == null) continue;
            try {
                c.setAccessible(true);
                player = c.newInstance(arg);
                Log.i(TAG, "UnityPlayer(" + p[0].getSimpleName() + ") ok");
                break;
            } catch (Throwable t) {
                last = t;
                Log.e(TAG, "UnityPlayer(" + p[0].getSimpleName() + ") failed", t);
            }
        }
        if (player == null) throw last != null ? new IllegalStateException(last) :
                new IllegalStateException("no usable UnityPlayer constructor in "
                        + java.util.Arrays.toString(ctors));

        if (player instanceof View) {
            root.addView((View) player, new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        } else {
            Method getView = player.getClass().getMethod("getView");
            root.addView((View) getView.invoke(player), new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        }
        log("unity booted: " + player.getClass().getName());

        // unity wants its window visible; drop the system bars like its own activity does
        root.setKeepScreenOn(true);
    }

    private void log(String s) {
        Log.i(TAG, s);
    }

    private void call(String name, Class<?>[] types, Object... args) {
        if (player == null) return;
        try {
            Method m = player.getClass().getMethod(name, types);
            m.invoke(player, args);
        } catch (Throwable t) {
            Log.w(TAG, "UnityPlayer." + name + " failed: " + t);
        }
    }

    // lifecycle, mirroring UnityPlayerActivity
    @Override protected void onResume() {
        super.onResume();
        call("resume", new Class[0]);
    }

    @Override protected void onPause() {
        call("pause", new Class[0]);
        super.onPause();
    }

    @Override protected void onDestroy() {
        call("quit", new Class[0]);
        super.onDestroy();
    }

    @Override public void onWindowFocusChanged(boolean has) {
        super.onWindowFocusChanged(has);
        call("windowFocusChanged", new Class[]{boolean.class}, has);
    }

    @Override public void onConfigurationChanged(Configuration c) {
        super.onConfigurationChanged(c);
        call("configurationChanged", new Class[]{Configuration.class}, c);
    }

    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        if (player != null) {
            try {
                Method m = player.getClass().getMethod("injectEvent", KeyEvent.class);
                Object r = m.invoke(player, e);
                if (Boolean.TRUE.equals(r)) return true;
            } catch (Throwable ignored) {
            }
        }
        return super.dispatchKeyEvent(e);
    }

    @Override protected void onSaveInstanceState(Bundle out) {
        // the plugin view has no view ids to save; skip the hierarchy walk
    }

    // ── the game's identity, answered by our own activity ────────────────────────────────

    @Override public String getPackageName() {
        return Sandbox.GAME;
    }

    @Override public String getOpPackageName() {
        return Sandbox.GAME;
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
