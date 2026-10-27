// Logs.java — the container runs the game in its own process, so it can read its own logcat
// without READ_LOGS. That closes the "no adb" hole: everything the engine, Play Core and the
// packer say lands in one screen and can be shared out.

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

    /** last {@code maxLines} lines of this process' logcat. */
    public static String recent(int maxLines) {
        StringBuilder sb = new StringBuilder();
        java.lang.Process p = null;
        try {
            p = Runtime.getRuntime().exec(new String[]{
                    "/system/bin/logcat", "-d", "-v", "time", "-t", String.valueOf(maxLines),
                    "--pid=" + android.os.Process.myPid()});
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()), 65536);
            String line;
            while ((line = r.readLine()) != null) sb.append(line).append('\n');
            r.close();
            p.waitFor();
        } catch (Throwable t) {
            sb.append("logcat failed: ").append(t).append('\n');
        } finally {
            if (p != null) p.destroy();
        }
        if (sb.length() == 0) {
            // some roms filter by pid differently; fall back to a tag dump
            try {
                java.lang.Process q = Runtime.getRuntime().exec(new String[]{
                        "/system/bin/logcat", "-d", "-v", "time", "-t",
                        String.valueOf(maxLines), "-s", "oxhk.sandbox:V", "Unity:V"});
                BufferedReader r = new BufferedReader(new InputStreamReader(q.getInputStream()));
                String line;
                while ((line = r.readLine()) != null) sb.append(line).append('\n');
                r.close();
            } catch (Throwable ignored) {
            }
        }
        return sb.toString();
    }

    private static File crashFile(Context c) {
        return new File(c.getFilesDir(), "last-crash.txt");
    }

    /** install an uncaught-exception trap: a dead process leaves a file behind, its trace is lost. */
    public static void installCrashHandler(final Context c) {
        final Thread.UncaughtExceptionHandler prev = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
            @Override public void uncaughtException(Thread t, Throwable e) {
                try {
                    StringBuilder sb = new StringBuilder();
                    sb.append(t.getName()).append(' ').append(e).append('\n');
                    for (StackTraceElement el : e.getStackTrace()) sb.append("  at ").append(el).append('\n');
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
        });
    }

    public static String lastCrash(Context c) {
        File f = crashFile(c);
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

    public static void clearCrash(Context c) {
        crashFile(c).delete();
    }
}
