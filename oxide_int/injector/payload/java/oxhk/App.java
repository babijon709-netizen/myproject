// Replaces the game's Application in the patched manifest: same superclass as the original,
// so the packer's <clinit> (System.loadLibrary("loader")) still runs, and oxhk is installed
// right after the game's own onCreate.
package oxhk;

public final class App extends dtGga4.sNBtq9.vRXcl4.pm7o47 {
    @Override
    public void onCreate() {
        super.onCreate();
        Oxhk.install(this);
    }
}
