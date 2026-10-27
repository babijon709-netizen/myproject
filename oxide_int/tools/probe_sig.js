// tools/probe_sig.js — answers "who checks the signature" without reversing libloader.
//
//   frida -U -f com.catsbit.oxidesurvivalisland -l tools/probe_sig.js --no-pause
//
// run it twice: once on the store build (baseline, learn what the game normally asks the PM)
// and once on the repacked build. diff the two logs.
//
// what you get:
//   getPackageInfo with flags 64 / 0x40 → GET_SIGNATURES: somebody is looking at certs
//   stack                                 → who exactly (java or a JNI caller from native)
//   dlopen                                → which libs the loader pulls, incl. libil2cpp
//   loadLibrary                           → java-side native loads, order matters

function stack(n) {
  return Java.use('android.util.Log').getStackTraceString(Java.use('java.lang.Throwable').$new()).split('\n').slice(1, n || 12).join('\n');
}

Java.perform(function () {
  try {
    var PM = Java.use('android.app.ApplicationPackageManager');
    PM.getPackageInfo.overloads.forEach(function (m) {
      var sig = m.argumentTypes.map(function (t) { return t.className; }).join(',');
      m.implementation = function () {
        var args = Array.prototype.slice.call(arguments);
        var r = m.apply(this, args);
        var flags = typeof args[1] === 'number' ? args[1] : (args[1] ? args[1].getValue() : -1);
        var interesting = typeof flags === 'number' && (flags & 0x40);  // GET_SIGNATURES
        send({ t: 'getPackageInfo', sig: sig, pkg: String(args[0]), flags: String(flags),
               signatures: interesting ? 'YES' : 'no', stack: stack(10) });
        return r;
      };
    });
  } catch (e) { send({ t: 'err', where: 'getPackageInfo', msg: String(e) }); }

  // who hashes certs
  try {
    var MD = Java.use('java.security.MessageDigest');
    MD.getInstance.overload('java.lang.String').implementation = function (alg) {
      return MD.getInstance(alg);
    };
    MD.digest.overload().implementation = function () {
      send({ t: 'MessageDigest.digest', alg: this.getAlgorithm(), stack: stack(14) });
      return this.digest();
    };
  } catch (e) { send({ t: 'err', where: 'MessageDigest', msg: String(e) }); }

  try {
    var Sys = Java.use('java.lang.System');
    Sys.loadLibrary.overload('java.lang.String').implementation = function (n) {
      send({ t: 'loadLibrary', name: n, stack: stack(8) });
      return Sys.loadLibrary(n);
    };
    Sys.load.overload('java.lang.String').implementation = function (p) {
      send({ t: 'System.load', path: p, stack: stack(8) });
      return Sys.load(p);
    };
  } catch (e) { send({ t: 'err', where: 'System', msg: String(e) }); }

  try {
    var ClassLoader = Java.use('java.lang.ClassLoader');
    ClassLoader.loadClass.overload('java.lang.String').implementation = function (n) {
      if (n.indexOf('dtGga4') >= 0 || n.indexOf('qCXAY9') >= 0) send({ t: 'loadClass', name: n });
      return this.loadClass(n);
    };
  } catch (e) {}
});

// native side: every dynamic lib open, with the requester
['android_dlopen_ext', 'dlopen'].forEach(function (fn) {
  var p = Module.findExportByName(null, fn);
  if (!p) return;
  Interceptor.attach(p, {
    onEnter: function (a) {
      try { this.path = a[0].readCString(); } catch (e) { this.path = '?'; }
      this.ret = Thread.backtrace(this.context, Backtracer.ACCURATE).slice(0, 6)
        .map(function (x) { return DebugSymbol.fromAddress(x).toString(); }).join('\n');
    },
    onLeave: function () {
      send({ t: fn, path: this.path, from: this.ret });
    }
  });
});

send({ t: 'ready' });
