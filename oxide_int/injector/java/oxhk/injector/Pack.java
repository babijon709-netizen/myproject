// Pack.java — rebuild the game's APKs with the payload inside, on the device, no tools.
//
// base.apk : manifest's android:name -> oxhk.App, payload dex added as classes<N+1>.dex
// split    : liboxhk.so added to lib/arm64-v8a/ (STORED)
//
// Old signature files are dropped, both outputs are re-signed afterwards (Sign.java). The APKs
// keep everything else byte for byte, so the packer's own files are untouched.

package oxhk.injector;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

public final class Pack {
    private static final Set<String> DROP = new HashSet<String>();

    static {
        DROP.add("META-INF/MANIFEST.MF");
    }

    private Pack() {}

    private static boolean isSignatureFile(String name) {
        String n = name.toUpperCase();
        if (!n.startsWith("META-INF/")) return false;
        return n.endsWith(".SF") || n.endsWith(".RSA") || n.endsWith(".DSA") || n.endsWith(".EC");
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
    public static String patchBase(File in, File out, byte[] payloadDex, String newAppClass) throws IOException {
        ZipFile z = new ZipFile(in);
        try {
            int slot = nextDexSlot(z);
            List<String> extraNames = new ArrayList<String>();
            List<byte[]> extraData = new ArrayList<byte[]>();

            ZipEntry manifest = z.getEntry("AndroidManifest.xml");
            if (manifest == null) throw new IOException("no AndroidManifest.xml in base.apk");
            byte[] axml = readAll(z.getInputStream(manifest));
            String orig = Axml.originalApplicationName(axml);
            byte[] patched = Axml.patchApplicationName(axml, newAppClass);
            if (patched == null) throw new IOException("could not patch <application android:name> ("
                    + orig + ")");

            extraNames.add("classes" + slot + ".dex");
            extraData.add(payloadDex);

            FileOutputStream fos = new FileOutputStream(out);
            ZipOutputStream zos = new ZipOutputStream(fos);
            try {
                Enumeration<? extends ZipEntry> en = z.entries();
                while (en.hasMoreElements()) {
                    ZipEntry e = en.nextElement();
                    String name = e.getName();
                    if (isSignatureFile(name) || DROP.contains(name)) continue;
                    if (name.startsWith("classes") && name.endsWith(".dex")) {
                        int s = -1;
                        String mid = name.substring("classes".length(), name.length() - 4);
                        if (!mid.isEmpty()) {
                            try { s = Integer.parseInt(mid); } catch (NumberFormatException ignored) {}
                        }
                        if (s == slot) continue;   // never collide
                    }
                    byte[] data = readAll(z.getInputStream(e));
                    if (name.equals("AndroidManifest.xml")) data = patched;
                    put(zos, name, data, false);
                }
                for (int i = 0; i < extraNames.size(); i++) put(zos, extraNames.get(i), extraData.get(i), false);
            } finally {
                zos.close();
            }
            return "classes" + slot + ".dex  (was " + orig + " -> " + newAppClass + ")";
        } finally {
            z.close();
        }
    }

    /** split_config.arm64_v8a.apk -> same split with liboxhk.so inside. */
    public static void patchSplit(File in, File out, byte[] lib) throws IOException {
        ZipFile z = new ZipFile(in);
        try {
            FileOutputStream fos = new FileOutputStream(out);
            ZipOutputStream zos = new ZipOutputStream(fos);
            try {
                Enumeration<? extends ZipEntry> en = z.entries();
                boolean had = false;
                while (en.hasMoreElements()) {
                    ZipEntry e = en.nextElement();
                    String name = e.getName();
                    if (isSignatureFile(name) || DROP.contains(name)) continue;
                    if (name.equals("lib/arm64-v8a/liboxhk.so")) { had = true; continue; }
                    put(zos, name, readAll(z.getInputStream(e)), false);
                }
                if (!had) put(zos, "lib/arm64-v8a/liboxhk.so", lib, true);
            } finally {
                zos.close();
            }
        } finally {
            z.close();
        }
    }

    // STORED for native libs (keeps them mmap-friendly), DEFLATED for everything else
    private static void put(ZipOutputStream zos, String name, byte[] data, boolean stored) throws IOException {
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
        InputStream in = new FileInputStream(f);
        return readAll(in);
    }
}
