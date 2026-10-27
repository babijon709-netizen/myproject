// tools/probe_loader.js — watch libloader.so's own syscall layer at runtime.
//   frida -U -f com.catsbit.oxidesurvivalisland -l tools/probe_loader.js --no-pause
//
// libloader.so has (at least) 60 hand-written `mov x8,#N; svc #0; ret` thunks, so hooking libc
// (kill, ptrace, open, ...) from Frida shows nothing — these calls never touch libc. This script
// attaches to the thunks themselves, by RVA, and prints every direct syscall with its args and
// the caller inside the lib. That is the honest way to see what the armor actually does.
//
// RVAs are from this exact build (libloader.so md5 check below). Nothing is patched.

const LOADER = 'libloader.so';
const MD5 = null;  // fill: file md5 of libloader.so if you want a hard guard

// rva -> arm64 syscall number, scanned from `mov x8,#imm ; svc #0` pairs at 0x8c000..0xc98000
const THUNKS = {
0x64fab4: 139 ,
0x667694: 63 ,
0x6676a4: 222 ,
0x6ae2d8: 43 ,
0x6ae2f0: 113 ,
0x6ae308: 167 ,
0x6ae320: 205 ,
0x6ae338: 175 ,
0x6ae350: 216 ,
0x6ae368: 226 ,
0x6ae380: 222 ,
0x6ae398: 215 ,
0x6ae3b0: 101 ,
0x6ae3c8: 169 ,
0x6ae3e0: 61 ,
0x6ae3f8: 56 ,
0x6ae410: 35 ,
0x6ae428: 57 ,
0x6ae440: 38 ,
0x6ae458: 221 ,
0x6ae470: 62 ,
0x6ae488: 59 ,
0x6ae4a0: 63 ,
0x6ae4bc: 63 ,
0x6ae4c8: 67 ,
0x6ae4e0: 79 ,
0x6ae4f8: 24 ,
0x6ae510: 78 ,
0x6ae528: 260 ,
0x6ae540: 25 ,
0x6ae558: 29 ,
0x6ae570: 198 ,
0x6ae588: 208 ,
0x6ae5a0: 209 ,
0x6ae5b8: 203 ,
0x6ae5d0: 220 ,
0x6ae5e8: 178 ,
0x6ae600: 94 ,
0x6ae618: 131 ,
0x6ae630: 172 ,
0x6ae648: 26 ,
0x6ae660: 27 ,
0x6ae678: 28 ,
0x6ae690: 64 ,
0x6ae6a8: 270 ,
0x6ae6c0: 173 ,
0x6ae6d8: 117 ,
0x6ae6f0: 160 ,
0x6ae708: 204 ,
0x6ae720: 211 ,
0x6ae738: 179 ,
0x6ae750: 129 ,
0x6ae768: 48 ,
0x6ae780: 132 ,
0x6ae798: 98 ,
0x6ae7b0: 34 ,
0x6ae7c8: 34 ,
0x6ae7d4: 278 ,
0x6ae7ec: 232 ,
0x6ae834: 73
};

const SYS = {
  24:'sched_yield', 25:'mremap', 26:'msync', 27:'mincore', 28:'madvise', 29:'shmget',
  34:'mkdirat', 35:'mknodat', 38:'renameat', 43:'statfs', 48:'faccessat', 56:'openat', 57:'close',
  59:'pipe2', 62:'lseek', 63:'read', 64:'write', 67:'pread64', 73:'ppoll', 78:'readlinkat',
  79:'newfstatat', 94:'exit_group', 98:'futex', 101:'nanosleep', 113:'clock_gettime',
  117:'ptrace', 129:'kill', 131:'tgkill', 132:'sigaltstack', 139:'rt_sigreturn',
  160:'uname', 167:'prctl', 169:'gettimeofday', 172:'getpid', 173:'getppid', 174:'getuid',
  175:'geteuid', 178:'gettid', 179:'sysinfo', 198:'socket', 200:'bind', 201:'listen',
  203:'accept', 204:'getsockname', 205:'getpeername', 206:'sendto', 207:'recvfrom',
  208:'setsockopt', 209:'getsockopt', 211:'sendmsg', 212:'recvmsg', 215:'munmap', 216:'mremap',
  220:'clone', 221:'execve', 222:'mmap', 226:'mprotect', 232:'epoll_wait', 233:'madvise',
  260:'wait4', 261:'prlimit64', 269:'process_vm_readv', 270:'process_vm_writev', 278:'getrandom'
};

const HOT = { 117:'PTRACE', 129:'KILL', 131:'TGKILL', 270:'VM_WRITEV', 269:'VM_READV',
              221:'EXECVE', 173:'GETPPID', 260:'WAIT4', 167:'PRCTL' };

function name(n) { return SYS[n] || ('sys' + n); }

function onLoad() {
  const m = Process.findModuleByName(LOADER);
  if (!m) { send({ t: 'waiting', for: LOADER }); return false; }

  send({ t: 'module', name: m.name, base: m.base.toString(), size: m.size, path: m.path });

  let hooked = 0;
  for (const rva in THUNKS) {
    const n = THUNKS[rva];
    const addr = m.base.add(ptr(rva));
    try {
      Interceptor.attach(addr, {
        onEnter: function () {
          const c = this.context;
          this.rec = { t: 'syscall', n: n, name: name(n),
                       hot: HOT[n] || null,
                       x0: c.x0.toString(), x1: c.x1.toString(), x2: c.x2.toString(),
                       x3: c.x3.toString(),
                       tid: Process.getCurrentThreadId() };
          if (HOT[n]) {
            this.rec.stack = Thread.backtrace(c, Backtracer.ACCURATE).slice(0, 8)
              .map(a => { const s = DebugSymbol.fromAddress(a); return s.name ? (s.name + '+' + (a.sub(s.address))) : a.toString(); });
          }
        },
        onLeave: function () { send(this.rec); }
      });
      hooked++;
    } catch (e) { send({ t: 'err', rva: rva, msg: String(e) }); }
  }
  send({ t: 'hooked', count: hooked });

  // and every module that gets mapped afterwards, in case the armor loads more pieces
  const dl = Module.findExportByName(null, 'android_dlopen_ext');
  if (dl) {
    Interceptor.attach(dl, {
      onEnter: function (a) { try { this.p = a[0].readCString(); } catch (e) { this.p = '?'; } },
      onLeave: function () { send({ t: 'dlopen', path: this.p }); }
    });
  }
  return true;
}

if (!onLoad()) {
  const iv = setInterval(() => { if (onLoad()) clearInterval(iv); }, 200);
}
