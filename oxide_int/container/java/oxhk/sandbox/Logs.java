// Logs.java — the container runs the game in its own process, so it can read its own logcat
// without READ_LOGS: logd filters by uid, so everything our processes ever wrote comes back,
// including runs that already died. That closes the "no adb" hole.

package oxhk.sandbox;

import android.content.Context;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;

public final class Logs {
    private static final String TAG = "oxhk.sandbox";

    private Logs() {}

    public static String recent(int maxLines) {
        String out = exec(new String[]{"/system/bin/logcat", "-d", "-v", "time",
                "-b", "main,system,crash", "-t", String.valueOf(maxLines)});
        if (out.length() < 200) {
            String alt = exec(new String[]{"/system/bin/logcat", "-d", "-v", "time",
                    "-b", "all", "-t", String.valueOf(maxLines)});
            if (alt.length() > out.length()) out = alt;
        }
        if (out.length() == 0) out = "(logcat returned nothing — rom may block it)\n";
        return out;
    }

    private static String exec(String[] cmd) {
        StringBuilder sb = new StringBuilder();
        java.lang.Process p = null;
        try {
            p = Runtime.getRuntime().exec(cmd);
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()), 1 << 16);
            String line;
            while ((line = r.readLine()) != null) sb.append(line).append('\n');
            r.close();
            p.waitFor();
        } catch (Throwable t) {
            sb.append("logcat failed: ").append(t).append('\n');
        } finally {
            if (p != null) p.destroy();
        }
        return sb.toString();
    }

    public static File save(Context c, String name, String text) {
        try {
            File f = new File(c.getExternalFilesDir(null), name);
            FileOutputStream fos = new FileOutputStream(f);
            fos.write(text.getBytes("UTF-8"));
            fos.close();
            return f;
        } catch (Throwable t) {
            Log.e(TAG, "save failed", t);
            return null;
        }
    }

    /** the trace of the last container run, written by Sandbox.say() straight to disk. */
    public static String lastRun(Context c) {
        return read(new File(c.getFilesDir(), "last-run.txt"));
    }

    public static File runFile(Context c) {
        return new File(c.getFilesDir(), "last-run.txt");
    }

    private static String read(File f) {
        if (!f.exists()) return "";
        try {
            FileInputStream in = new FileInputStream(f);
            byte[] buf = new byte[(int) f.length()];
            int n = in.read(buf);
            in.close();
            return n > 0 ? new String(buf, 0, n, "UTF-8") : "";
        } catch (Throwable t) {
            return "?" + t;
        }
    }

    private static File crashFile(Context c) {
        return new File(c.getFilesDir(), "last-crash.txt");
    }

    public static void installCrashHandler(final Context c) {
        final Thread.UncaughtExceptionHandler prev = Thread.getDefaultUncaughtExceptionHandler();
        final Thread.UncaughtExceptionHandler ours = new Thread.UncaughtExceptionHandler() {
            @Override public void uncaughtException(Thread t, Throwable e) {
                // The game runs with a spoofed package name, so GMS/Firebase — which validate the
                // real caller uid against the package — throw SecurityException on their own
                // threads and Handler.dispatchMessage turns that into a process death.
                // Their failure is not ours to fix: log it and keep the process alive.
                if (foreign(t, e)) {
                    Log.w(TAG, "swallowed " + t.getName() + ": " + e
                            + " (gms identity mismatch)");
                    return;
                }
                try {
                    StringBuilder sb = new StringBuilder();
                    sb.append(t.getName()).append(' ').append(e).append('\n');
                    for (StackTraceElement el : e.getStackTrace())
                        sb.append("  at ").append(el).append('\n');
                    Throwable cause = e.getCause();
                    int depth = 0;
                    while (cause != null && depth++ < 4) {
                        sb.append("caused by ").append(cause).append('\n');
                        for (StackTraceElement el : cause.getStackTrace())
                            sb.append("  at ").append(el).append('\n');
                        cause = cause.getCause();
                    }
                    FileOutputStream fos = new FileOutputStream(crashFile(c));
                    fos.write(sb.toString().getBytes("UTF-8"));
                    fos.close();
                } catch (Throwable ignored) {
                }
                Log.e(TAG, "uncaught", e);
                if (prev != null) prev.uncaughtException(t, e);
            }
        };
        Thread.setDefaultUncaughtExceptionHandler(ours);
        Thread.currentThread().setUncaughtExceptionHandler(ours);   // main thread
    }

    /** is this exception only a consequence of our package spoof?
     *
     *  The frames are obfuscated (`m7.er`), and the only GMS marker is the source-file suffix
     *  (`:com.google.android.gms.dynamite_measurementdynamite@…`), so matching on
     *  StackTraceElement.getClassName() never hits. Unity also rethrows the failure as a plain
     *  java.lang.Error with the original text inside its message. Both cases need the flattened
     *  string, not the structured one. */
    private static boolean foreign(Thread t, Throwable e) {
        String text;
        try {
            text = Log.getStackTraceString(e);
        } catch (Throwable ignored) {
            text = String.valueOf(e) + "\n" + java.util.Arrays.toString(e.getStackTrace());
        }
        String[] markers = {
                "Unknown calling package name",
                "does not belong to",
                "cannot schedule job in",
                "com.google.android.gms",
                "dynamite_measurementdynamite",
                "com.google.firebase",
        };
        for (String m : markers) {
            if (text.contains(m)) return true;
        }
        String name = t.getName();
        return name.startsWith("Measurement") || name.startsWith("Firebase")
                || name.startsWith("GoogleApiHandler");
    }

    public static String lastCrash(Context c) {
        return read(crashFile(c));
    }

    public static void clearCrash(Context c) {
        crashFile(c).delete();
    }
}
