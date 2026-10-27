// Install.java — hand one or two APKs to PackageInstaller in a single session (base + split),
// so the system treats them as one install. No adb needed.

package oxhk.injector;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;

import java.io.File;
import java.io.FileInputStream;
import java.io.OutputStream;

public final class Install {
    private Install() {}

    public static void install(Context ctx, File... apks) throws Exception {
        PackageInstaller pi = ctx.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params =
                new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setInstallLocation(android.content.pm.PackageInfo.INSTALL_LOCATION_AUTO);

        int id = pi.createSession(params);
        PackageInstaller.Session session = pi.openSession(id);
        try {
            for (File apk : apks) {
                OutputStream os = session.openWrite(apk.getName(), 0, apk.length());
                FileInputStream fis = new FileInputStream(apk);
                try {
                    byte[] buf = new byte[256 * 1024];
                    int n;
                    while ((n = fis.read(buf)) > 0) os.write(buf, 0, n);
                    session.fsync(os);
                } finally {
                    try { fis.close(); } catch (Throwable ignored) {}
                    os.close();
                }
            }
            Intent cb = new Intent(ctx, MainActivity.class).setAction(MainActivity.ACTION_INSTALL_RESULT);
            PendingIntent pi2 = PendingIntent.getActivity(ctx, id, cb,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
            session.commit(pi2.getIntentSender());
        } finally {
            session.close();
        }
    }
}
