// Pack.java — rebuild the game's APKs with the payload inside, on the device, no tooling.
//
// base.apk          : manifest's android:name -> oxhk.App, payload dex as classes<N+1>.dex
// arm64 split       : liboxhk.so added to lib/arm64-v8a/ (STORED)
// every other split : copied through untouched (still has to be re-signed, the app must be
//                     one certificate), streamed entry by entry so a 92 MB asset pack does not
//                     have to fit in the heap.
//
// Old signature files are dropped; both outputs get re-signed afterwards (Sign.java). Everything
// else is copied byte for byte, so the packer's own files are untouched.

package oxhk.injector;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Enumeration;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

public final class Pack {
    private Pack() {}

    private static boolean isSignatureFile(String name) {
        String n = name.toUpperCase();
        if (!n.startsWith("META-INF/")) return false;
        return n.endsWith(".SF") || n.endsWith(".RSA") || n.endsWith(".DSA") || n.endsWith(".EC")
                || n.equals("META-INF/MANIFEST.MF");
    }

    public static String baseApplicationName(File baseApk) throws IOException {
        ZipFile z = new ZipFile(baseApk);
        try {
            ZipEntry e = z.getEntry("AndroidManifest.xml");
            return Axml.originalApplicationName(readAll(z.getInputStream(e)));
        } finally {
            z.close();
        }
    }

    /** true when this split carries the ARM64 native lib set (that is the one we patch). */
    public static boolean hasArm64Il2Cpp(File apk) {
        ZipFile z = null;
        try {
            z = new ZipFile(apk);
            return z.getEntry("lib/arm64-v8a/libil2cpp.so") != null;
        } catch (Throwable t) {
            return false;
        } finally {
            if (z != null) try { z.close(); } catch (IOException ignored) {}
        }
    }

    public static int nextDexSlot(ZipFile z) {
        int max = 1;
        Enumeration<? extends ZipEntry> en = z.entries();
        while (en.hasMoreElements()) {
            String n = en.nextElement().getName();
            if (!n.startsWith("classes") || !n.endsWith(".dex")) continue;
            String mid = n.substring("classes".length(), n.length() - 4);
            if (mid.isEmpty()) continue;
            try {
                max = Math.max(max, Integer.parseInt(mid));
            } catch (NumberFormatException ignored) {
            }
        }
        return max + 1;
    }

    /** base.apk -> patched, unsigned, with the payload dex inside. */
    public static String patchBase(File in, File out, byte[] payloadDex, String newAppClass)
            throws IOException {
        ZipFile z = new ZipFile(in);
        try {
            int slot = nextDexSlot(z);

            ZipEntry manifest = z.getEntry("AndroidManifest.xml");
            if (manifest == null) throw new IOException("no AndroidManifest.xml in base.apk");
            byte[] axml = readAll(z.getInputStream(manifest));
            String orig = Axml.originalApplicationName(axml);
            byte[] patched = Axml.patchApplicationName(axml, newAppClass);
            if (patched == null)
                throw new IOException("could not patch <application android:name> (" + orig + ")");

            ZipOutputStream zos = new ZipOutputStream(new FileOutputStream(out));
            try {
                Enumeration<? extends ZipEntry> en = z.entries();
                while (en.hasMoreElements()) {
                    ZipEntry e = en.nextElement();
                    String name = e.getName();
                    if (isSignatureFile(name)) continue;
                    if (name.equals("AndroidManifest.xml")) {
                        putBytes(zos, name, patched, false);
                        continue;
                    }
                    if (name.startsWith("classes") && name.endsWith(".dex")) {
                        String mid = name.substring("classes".length(), name.length() - 4);
                        int s = -1;
                        if (!mid.isEmpty()) {
                            try { s = Integer.parseInt(mid); } catch (NumberFormatException ignored) {}
                        }
                        if (s == slot) continue;   // never collide with ours
                    }
                    copyThrough(zos, e, z.getInputStream(e));
                }
                putBytes(zos, "classes" + slot + ".dex", payloadDex, false);
            } finally {
                zos.close();
            }
            return "classes" + slot + ".dex  (" + orig + " -> " + newAppClass + ")";
        } finally {
            z.close();
        }
    }

    /** split -> same split with liboxhk.so inside (inject) or copied through as is. */
    public static void patchSplit(File in, File out, byte[] lib, boolean inject) throws IOException {
        ZipFile z = new ZipFile(in);
        try {
            ZipOutputStream zos = new ZipOutputStream(new FileOutputStream(out));
            try {
                Enumeration<? extends ZipEntry> en = z.entries();
                while (en.hasMoreElements()) {
                    ZipEntry e = en.nextElement();
                    String name = e.getName();
                    if (isSignatureFile(name)) continue;
                    if (inject && name.equals("lib/arm64-v8a/liboxhk.so")) continue;
                    copyThrough(zos, e, z.getInputStream(e));
                }
                if (inject) putBytes(zos, "lib/arm64-v8a/liboxhk.so", lib, true);
            } finally {
                zos.close();
            }
        } finally {
            z.close();
        }
    }

    // ── zip helpers ─────────────────────────────────────────────────────────────────────

    /** stream an unchanged entry: keeps its method and its (already known) crc/size. */
    private static void copyThrough(ZipOutputStream zos, ZipEntry e, InputStream in) throws IOException {
        try {
            ZipEntry o = new ZipEntry(e.getName());
            if (e.getMethod() == ZipEntry.STORED) {
                o.setMethod(ZipEntry.STORED);
                o.setSize(e.getSize());
                o.setCompressedSize(e.getSize());
                o.setCrc(e.getCrc());
            } else {
                o.setMethod(ZipEntry.DEFLATED);
            }
            zos.putNextEntry(o);
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) zos.write(buf, 0, n);
            zos.closeEntry();
        } finally {
            in.close();
        }
    }

    private static void putBytes(ZipOutputStream zos, String name, byte[] data, boolean stored)
            throws IOException {
        ZipEntry e = new ZipEntry(name);
        if (stored) {
            CRC32 crc = new CRC32();
            crc.update(data);
            e.setMethod(ZipEntry.STORED);
            e.setSize(data.length);
            e.setCompressedSize(data.length);
            e.setCrc(crc.getValue());
        } else {
            e.setMethod(ZipEntry.DEFLATED);
        }
        zos.putNextEntry(e);
        zos.write(data);
        zos.closeEntry();
    }

    static byte[] readAll(InputStream in) throws IOException {
        try {
            ByteArrayOutputStream bos = new ByteArrayOutputStream(Math.max(1024, in.available()));
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) bos.write(buf, 0, n);
            return bos.toByteArray();
        } finally {
            in.close();
        }
    }

    static byte[] readFile(File f) throws IOException {
        return readAll(new FileInputStream(f));
    }
}
