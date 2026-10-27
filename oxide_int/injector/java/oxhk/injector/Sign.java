// Sign.java — v1+v2+v3 signing on the device with apksig (the same lib apksigner uses).
//
// The keystore (PKCS12) is baked into the injector's assets at build time, so every patch is
// signed with the same key and updates install over each other. The certificate SHA-256 is what
// the game/server would see instead of the store signature.

package oxhk.injector;

import android.content.Context;
import android.content.res.AssetManager;

import com.android.apksig.ApkSigner;

import java.io.File;
import java.io.InputStream;
import java.security.KeyStore;
import java.security.PrivateKey;
import java.security.cert.Certificate;
import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

public final class Sign {
    private static final String STORE = "oxhk.p12";
    private static final char[] PASS = "oxhkpass".toCharArray();
    private static final String ALIAS = "oxhk";

    private Sign() {}

    public static void sign(Context ctx, File in, File out, int minSdk) throws Exception {
        KeyStore ks = KeyStore.getInstance("PKCS12");
        AssetManager am = ctx.getAssets();
        InputStream is = am.open(STORE);
        try {
            ks.load(is, PASS);
        } finally {
            is.close();
        }
        PrivateKey key = (PrivateKey) ks.getKey(ALIAS, PASS);
        Certificate[] chain = ks.getCertificateChain(ALIAS);
        if (key == null || chain == null) throw new IllegalStateException("keystore without key " + ALIAS);

        List<X509Certificate> certs = new ArrayList<X509Certificate>();
        for (Certificate c : chain) certs.add((X509Certificate) c);

        ApkSigner.SignerConfig cfg = new ApkSigner.SignerConfig.Builder(ALIAS, key, certs).build();
        new ApkSigner.Builder(Collections.singletonList(cfg))
                .setInputApk(in)
                .setOutputApk(out)
                .setV1SigningEnabled(true)
                .setV2SigningEnabled(true)
                .setV3SigningEnabled(true)
                .setMinSdkVersion(minSdk)
                .build()
                .sign();
    }

    public static String certSha256(Context ctx) {
        try {
            KeyStore ks = KeyStore.getInstance("PKCS12");
            InputStream is = ctx.getAssets().open(STORE);
            try {
                ks.load(is, PASS);
            } finally {
                is.close();
            }
            X509Certificate c = (X509Certificate) ks.getCertificate(ALIAS);
            byte[] d = java.security.MessageDigest.getInstance("SHA-256").digest(c.getEncoded());
            StringBuilder sb = new StringBuilder();
            for (byte b : d) sb.append(String.format("%02x", b));
            return sb.toString();
        } catch (Throwable t) {
            return "(unavailable: " + t + ")";
        }
    }
}
