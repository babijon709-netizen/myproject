// Axml.java — rewrite one string inside a binary AndroidManifest.xml, in place.
//
// The only manifest change the injector needs is the <application android:name> value: point it
// at oxhk.App. That attribute is a string-pool reference, so instead of rebuilding the XML we
// rewrite the string's bytes where they live. The new name is shorter than the original
// (`dtGga4.sNBtq9.vRXcl4.pm7o47`), so it always fits; the tail is zero-filled and no offset
// moves. Nothing else in the manifest is touched.

package oxhk.injector;

import java.io.ByteArrayOutputStream;
import java.nio.charset.Charset;

public final class Axml {
    private static final int RES_STRING_POOL_TYPE = 0x0001;
    private static final int RES_XML_TYPE = 0x0003;
    private static final int RES_XML_START_ELEMENT_TYPE = 0x0102;
    private static final int RES_XML_RESOURCE_MAP_TYPE = 0x0180;

    private static final int ATTR_NAME = 0x01010003;   // android:name
    private static final Charset UTF8 = Charset.forName("UTF-8");

    private Axml() {}

    public static String originalApplicationName(byte[] axml) {
        try {
            Pool p = readPool(axml);
            int appNameIdx = findApplicationNameAttr(axml, p);
            if (appNameIdx >= 0) return p.get(appNameIdx);
        } catch (Throwable ignored) {
        }
        return null;
    }

    /** Returns a patched copy, or null when the attribute could not be found/changed. */
    public static byte[] patchApplicationName(byte[] axml, String newName) {
        byte[] out = axml.clone();
        try {
            Pool p = readPool(out);
            int idx = findApplicationNameAttr(out, p);
            if (idx < 0) return null;
            if (!p.rewrite(out, idx, newName)) return null;
            // read it back: the pool must still parse, the count must be unchanged and the
            // entry must now be the new name. Cheap insurance against a broken manifest.
            Pool check = readPool(out);
            if (check.stringCount != p.stringCount) return null;
            if (!newName.equals(check.get(idx))) return null;
            if (!"application".equals(findElement(check, "application"))) return null;
            return out;
        } catch (Throwable t) {
            return null;
        }
    }

    // ── string pool ─────────────────────────────────────────────────────────────────────
    static final class Pool {
        int chunkStart, headerSize, stringCount, offsetsBase, dataStart;
        boolean utf8;
        int[] offsets;

        String get(int i) {
            if (i < 0 || i >= stringCount) return null;
            return read(data, offsets[i]);
        }

        byte[] data;

        String read(byte[] buf, int off) {
            int p = dataStart + off;
            if (utf8) {
                int[] r1 = u8len(buf, p);
                int p2 = p + r1[1];
                int[] r2 = u8len(buf, p2);
                int start = p2 + r2[1];
                return new String(buf, start, r2[0], UTF8);
            } else {
                int n = ((buf[p] & 0xff) | ((buf[p + 1] & 0xff) << 8)) & 0x7fff;
                return new String(buf, p + 2, n * 2, Charset.forName("UTF-16LE"));
            }
        }

        boolean rewrite(byte[] buf, int i, String value) {
            int off = dataStart + offsets[i];
            int cap;
            if (utf8) {
                int[] r1 = u8len(buf, off);
                int p2 = off + r1[1];
                int[] r2 = u8len(buf, p2);
                int start = p2 + r2[1];
                cap = r2[0];                    // the string's own bytes — never past them
                byte[] enc = value.getBytes(UTF8);
                if (enc.length > cap) return false;
                if (enc.length != r2[0]) {
                    if (r2[1] != 1) return false;      // don't re-encode multi-byte lengths
                    buf[p2] = (byte) enc.length;
                }
                System.arraycopy(enc, 0, buf, start, enc.length);
                for (int k = start + enc.length; k < start + cap; k++) buf[k] = 0;
                return true;
            } else {
                int n = ((buf[off] & 0xff) | ((buf[off + 1] & 0xff) << 8)) & 0x7fff;
                int start = off + 2;
                if (value.length() > n) return false;
                buf[off] = (byte) (value.length() & 0xff);
                buf[off + 1] = (byte) ((value.length() >> 8) & 0x7f);
                for (int k = 0; k < value.length(); k++) {
                    char c = value.charAt(k);
                    buf[start + k * 2] = (byte) (c & 0xff);
                    buf[start + k * 2 + 1] = (byte) ((c >> 8) & 0xff);
                }
                for (int k = value.length(); k < n; k++) {
                    buf[start + k * 2] = 0;
                    buf[start + k * 2 + 1] = 0;
                }
                return true;
            }
        }

    }

    private static int[] u8len(byte[] b, int p) {
        int v = b[p] & 0xff;
        if ((v & 0x80) == 0) return new int[]{v, 1};
        return new int[]{((v & 0x7f) << 8) | (b[p + 1] & 0xff), 2};
    }

    private static Pool readPool(byte[] axml) {
        if (le16(axml, 0) != RES_XML_TYPE) throw new IllegalArgumentException("not axml");
        Pool p = new Pool();
        p.data = axml;
        int off = le16(axml, 2);
        while (off + 8 <= axml.length) {
            int type = le16(axml, off);
            int hsize = le16(axml, off + 2);
            int size = le32(axml, off + 4);
            if (size <= 0 || off + size > axml.length) break;
            if (type == RES_STRING_POOL_TYPE) {
                p.chunkStart = off;
                p.headerSize = hsize;
                p.stringCount = le32(axml, off + 8);
                int styles = le32(axml, off + 12);
                int flags = le32(axml, off + 16);
                int stringsStart = le32(axml, off + 20);
                p.utf8 = (flags & (1 << 8)) != 0;
                p.offsetsBase = off + hsize;
                p.dataStart = off + stringsStart;
                p.offsets = new int[p.stringCount];
                for (int i = 0; i < p.stringCount; i++) p.offsets[i] = le32(axml, p.offsetsBase + i * 4);
                return p;
            }
            off += size;
        }
        throw new IllegalArgumentException("no string pool");
    }

    /** Walk start-element chunks, find <application android:name="...">, return the pool index. */
    private static int findApplicationNameAttr(byte[] axml, Pool pool) {
        int off = le16(axml, 2);
        while (off + 8 <= axml.length) {
            int type = le16(axml, off);
            int hsize = le16(axml, off + 2);
            int size = le32(axml, off + 4);
            if (size <= 0 || off + size > axml.length) break;

            if (type == RES_XML_START_ELEMENT_TYPE) {
                int nameIdx = le32(axml, off + 20);
                String elem = pool.get(nameIdx);
                if ("application".equals(elem)) {
                    int attrStart = le16(axml, off + 24);
                    int attrSize = le16(axml, off + 26);
                    int attrCount = le16(axml, off + 28);
                    int base = off + hsize + attrStart;
                    for (int i = 0; i < attrCount; i++) {
                        int a = base + i * attrSize;
                        int nsIdx = le32(axml, a);
                        int an = le32(axml, a + 4);
                        int raw = le32(axml, a + 8);
                        int nRes = le32(axml, a + 12) >>> 24;   // data type
                        String attrName = an >= 0 && an < pool.stringCount ? pool.get(an) : null;
                        boolean isName = ATTR_NAME == an
                                || "name".equals(attrName);
                        if (!isName) continue;
                        if (raw >= 0 && raw < pool.stringCount) return raw;
                        if (nRes == 0x03) return le32(axml, a + 16);   // TYPE_STRING fallback
                    }
                }
            }
            off += size;
        }
        // last resort: find the known original name anywhere in the pool
        for (int i = 0; i < pool.stringCount; i++) {
            String s = pool.get(i);
            if ("dtGga4.sNBtq9.vRXcl4.pm7o47".equals(s)) return i;
        }
        return -1;
    }

    private static String findElement(Pool pool, String wanted) {
        byte[] axml = pool.data;
        int off = le16(axml, 2);
        while (off + 8 <= axml.length) {
            int type = le16(axml, off);
            int size = le32(axml, off + 4);
            if (size <= 0 || off + size > axml.length) break;
            if (type == RES_XML_START_ELEMENT_TYPE) {
                String e = pool.get(le32(axml, off + 20));
                if (wanted.equals(e)) return e;
            }
            off += size;
        }
        return null;
    }

    static int le16(byte[] b, int o) { return (b[o] & 0xff) | ((b[o + 1] & 0xff) << 8); }

    static int le32(byte[] b, int o) {
        return (b[o] & 0xff) | ((b[o + 1] & 0xff) << 8) | ((b[o + 2] & 0xff) << 16) | ((b[o + 3] & 0xff) << 24);
    }
}
