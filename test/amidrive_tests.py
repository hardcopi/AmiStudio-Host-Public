"""amidrive (Host 0.1.8) tests, run from run_tests.py: a `local` folder behind an rclone `alias` remote stands in for
Google Drive (the token check, Drive quota and the real sign-in need Rich's account and are not tested here).
Needs /dev/fuse, fusermount3 and the pinned rclone (build/rclone/stage from tools/fetch-rclone.sh, or ASH_RCLONE).
Every Host here gets its own data folder whose AmiStudio-drawer mount point has a space in its name.
At the end: no FUSE mount, rclone process or pulseaudio daemon from these tests is left, and nothing new appeared in
~ (incl. ~/.config/rclone, ~/.cache/rclone) or /tmp outside the test folder."""
import os, re, shutil, signal, socket, stat, struct, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOSTD = os.environ.get("ASH_HOSTD") or os.path.join(ROOT, "build", "amistudio-hostd")
CLI = os.path.join(ROOT, "build", "ash-cli")
RCLONE = os.environ.get("ASH_RCLONE") or os.path.join(ROOT, "build", "rclone", "stage", "engine", "rclone", "rclone")
HOME = os.path.expanduser("~")
ST = dict(unavailable=0, signed_out=1, signing_in=2, ready=3, mounting=4, mounted=5, flushing=6, unmounting=7, error=8)
F_FUSE, F_RCLONE, F_PENDING, F_LOWDISK, F_EXPIRED, F_NOTEMPTY, F_COPIED = 1, 2, 4, 8, 16, 32, 64

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p
def rd(s, n):
    b = b""
    while len(b) < n:
        k = s.recv(n - len(b))
        if not k: raise EOFError("closed")
        b += k
    return b
def mounted(path):
    for l in open("/proc/self/mountinfo"):
        mp = l.split(" ")[4]
        mp = re.sub(r"\\([0-7]{3})", lambda m: chr(int(m.group(1), 8)), mp)
        if mp == path: return True
    return False
def mountinfo_raw(path):
    enc = path.replace("\\", "\\134").replace(" ", "\\040")
    return [l for l in open("/proc/self/mountinfo") if l.split(" ")[4] == enc]
def pids_matching(word):
    out = []
    for p in os.listdir("/proc"):
        if not p.isdigit(): continue
        try: cmd = open("/proc/%s/cmdline" % p, "rb").read().split(b"\0")
        except OSError: continue
        if any(word.encode() in c for c in cmd): out.append(int(p))
    return out
def wait_for(fn, t=20, step=0.1):
    end = time.time() + t
    while time.time() < end:
        r = fn()
        if r: return r
        time.sleep(step)
    return fn()

class AD:
    """an amidrive v1 session"""
    def __init__(self, port, client="pytest-amidrive"):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=30)
        self.s.sendall(b"ASH1" + struct.pack("<H", 1) + b"\x08amidrive" + struct.pack("<H", 1) + b"\x00" + bytes([len(client)]) + client.encode())
        h = rd(self.s, 10); self.status = h[6]; self.msg = rd(self.s, h[9]).decode() if h[9] else ""
        self.frames = []; self.progress = []
    def send(self, op, payload=b""): self.s.sendall(struct.pack("<I", len(payload) + 1) + bytes([op]) + payload)
    def recv(self, timeout=60):
        self.s.settimeout(timeout); n = struct.unpack("<I", rd(self.s, 4))[0]; op = rd(self.s, 1)[0]; return op, rd(self.s, n - 1)
    @staticmethod
    def state(p):
        o = 0; d = {}
        d["state"], d["flags"] = p[0], p[1]; o = 2
        d["queued"], d["inprog"], d["kb"], d["open"], d["errored"] = struct.unpack("<5I", p[o:o + 20]); o += 20
        d["used"], d["total"] = struct.unpack("<2Q", p[o:o + 16]); o += 16
        for k in ("account", "mount", "nonce", "message"):
            l = p[o]; d[k] = p[o + 1:o + 1 + l].decode("utf-8", "replace"); o += 1 + l
        return d
    @staticmethod
    def result(p):
        l = p[2]; d = {"op": p[0], "status": p[1], "message": p[3:3 + l].decode("utf-8", "replace"), "paths": []}
        o = 3 + l
        if d["status"] == 1 and len(p) >= o + 2:
            n = struct.unpack("<H", p[o:o + 2])[0]; o += 2
            for _ in range(n): pl = p[o]; d["paths"].append(p[o + 1:o + 1 + pl].decode("utf-8", "replace")); o += 1 + pl
        return d
    def status_(self):
        self.send(1)
        while True:
            op, p = self.recv()
            if op == 16: return self.state(p)
            self.frames.append((op, p))
    def req(self, op, arg=None, timeout=120):
        """send a request, return (RESULT dict, the STATE that follows it or None)"""
        self.send(op, b"" if arg is None else bytes([arg]))
        res = None; end = time.time() + timeout
        while time.time() < end:
            fop, p = self.recv(max(1, end - time.time()))
            if fop == 19: self.progress.append(struct.unpack("<II", p)); continue
            if fop == 18 and p[0] == op and res is None:
                res = self.result(p)
                if op == 4: return res, None
                continue
            if fop == 16 and res is not None: return res, self.state(p)
            self.frames.append((fop, p))
        raise TimeoutError("no RESULT for op %d" % op)
    def close(self):
        try: self.s.close()
        except OSError: pass

class AHost:
    def __init__(self, base, name, extra="", fusermount=None, data=None, conf_only=False, env=None, signed_in=True, remote=None, prefix=None, exe=None):
        self.base = base; self.port = free_port(); self.prefix = prefix or []; self.exe = exe or HOSTD
        self.confdir = os.path.join(base, name + "-conf"); os.makedirs(self.confdir, exist_ok=True)
        self.data = data or os.path.join(base, name + "-data")
        self.drawer = os.path.join(base, name + " AmiStudio")          # a space in the drawer path too
        self.mp = os.path.join(self.drawer, "Google Drive")
        self.remote = remote or os.path.join(base, name + "-remote")
        self.ad = os.path.join(self.data, "amidrive")
        for d in (self.drawer, self.remote): os.makedirs(d, exist_ok=True)
        if signed_in and not os.path.exists(os.path.join(self.ad, "rclone.conf")):
            os.makedirs(self.ad, exist_ok=True); os.chmod(self.data, 0o700); os.chmod(self.ad, 0o700)
            open(os.path.join(self.ad, "rclone.conf"), "w").write("[gdrive]\ntype = alias\nremote = %s\n" % self.remote)
            os.chmod(os.path.join(self.ad, "rclone.conf"), 0o600)
            open(os.path.join(self.ad, "drawer.path"), "w").write(os.path.realpath(self.ad) + "\n")
        conf = ("port %d\namidrive_rclone %s\namidrive_mountpoint \"%s\"\namidrive_retry 2\namidrive_stop_timeout 20\n"
                "amidrive_flags --vfs-write-back 1h --dir-cache-time 1s --poll-interval 0\n" % (self.port, RCLONE, self.mp))
        if fusermount: conf += "amidrive_fusermount %s\n" % fusermount
        conf += extra
        open(os.path.join(self.confdir, "host.conf"), "w").write(conf)
        self.env = dict(os.environ, AMISTUDIO_HOST_DIR=self.confdir); self.env.update(env or {})
        self.log = os.path.join(self.confdir, "hostd.out"); self.p = None
    def start(self, wait=True, umask=-1):
        self.p = subprocess.Popen(self.prefix + [self.exe, "--data-dir", self.data, "--no-update-check", "-v"], stdout=open(self.log, "a"), stderr=subprocess.STDOUT, env=self.env, umask=umask)
        if wait:
            for _ in range(100):
                try: socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close(); return True
                except OSError: time.sleep(0.05)
            return False
        return True
    def out(self): return open(self.log, errors="replace").read()
    def adlog(self):
        p = os.path.join(self.ad, "amidrive.log"); return open(p, errors="replace").read() if os.path.exists(p) else ""
    def cli(self, *a, timeout=120): return subprocess.run([HOSTD, "amidrive"] + list(a), capture_output=True, text=True, env=self.env, timeout=timeout)
    def stop(self, sig=signal.SIGTERM, t=60):
        if self.p and self.p.poll() is None:
            self.p.send_signal(sig)
            try: return self.p.wait(t)
            except subprocess.TimeoutExpired: self.p.kill(); return self.p.wait()
        return self.p.returncode if self.p else None
    def rclone_pids(self): return [p for p in pids_matching(self.mp) if b"rclone" in open("/proc/%d/cmdline" % p, "rb").read()] if True else []

def snapshot():
    """entries in the places a stray rclone would write (HOME is never moved here, so these are the real ones)"""
    snap = set()
    for d in [HOME, os.path.join(HOME, ".config"), os.path.join(HOME, ".cache"), os.path.join(HOME, ".local", "share"),
              os.path.join(HOME, ".config", "rclone"), os.path.join(HOME, ".cache", "rclone"), "/tmp"]:
        try:
            for e in os.listdir(d): snap.add(os.path.join(d, e))
        except OSError: pass
    return snap

def force_clean(hosts):
    for h in hosts:
        try: h.stop(signal.SIGKILL, 5)
        except Exception: pass
        for p in h.rclone_pids():
            try: os.kill(p, signal.SIGKILL)
            except OSError: pass
        for _ in range(3):
            if mounted(h.mp): subprocess.run(["umount", "-l", h.mp], capture_output=True)

def amidrive_tests(check, skip, tmp):
    if not os.path.exists(RCLONE): skip("amidrive tests", "no rclone: run tools/fetch-rclone.sh (or set ASH_RCLONE)"); return
    if not os.access("/dev/fuse", os.R_OK | os.W_OK) or not shutil.which("fusermount3"): skip("amidrive tests", "no FUSE (/dev/fuse + fusermount3)"); return
    base = os.path.join(tmp, "amidrive tests"); os.makedirs(base, exist_ok=True)
    before = snapshot(); hosts = []
    # fusermount3 that always fails: the umount fallback must do the work (this box also has no /etc/mtab)
    fake_fm = os.path.join(base, "fusermount-fails"); open(fake_fm, "w").write("#!/bin/sh\necho 'fake fusermount3: failing on purpose' >&2\nexit 1\n"); os.chmod(fake_fm, 0o755)
    try:
        _tests(check, skip, base, hosts, fake_fm)
    finally:
        force_clean(hosts)
    left_mounts = [h.mp for h in hosts if mounted(h.mp)]
    check(not left_mounts, "amidrive: no FUSE mount left behind", left_mounts)
    left_rc = [p for h in hosts for p in h.rclone_pids()] + pids_matching(os.path.join(base, ""))
    check(not left_rc, "amidrive: no rclone process left behind", left_rc)
    after = snapshot()
    new = sorted(e for e in after - before if not e.startswith(tmp) and not re.search(r"/tmp/(ash-test-|apptest-|tmp|pytest|\.X|systemd|sand|playwright)", e))
    rc_new = [e for e in new if "rclone" in e.lower() or "amidrive" in e.lower() or "amistudio" in e.lower()]
    check(not rc_new, "amidrive self-contained: nothing new in ~, ~/.config/rclone, ~/.cache or /tmp from rclone/the Host", rc_new)
    check(not os.listdir(os.path.join(HOME, ".config", "rclone")) if os.path.isdir(os.path.join(HOME, ".config", "rclone")) else True,
          "amidrive self-contained: ~/.config/rclone has no rclone.conf", os.listdir(os.path.join(HOME, ".config", "rclone")) if os.path.isdir(os.path.join(HOME, ".config", "rclone")) else "")

def _tests(check, skip, base, hosts, fake_fm):
    # ---------- A: boot, no automount, CONNECT 1, the space in the path, marker, icon ----------
    a = AHost(base, "a", fusermount=fake_fm); hosts.append(a)
    os.makedirs(a.ad, exist_ok=True); icon = b"\xe3\x10fake icon bytes from the AmiDrive release"
    open(os.path.join(a.ad, "Google Drive.info"), "wb").write(icon)
    open(os.path.join(a.remote, "hello.txt"), "w").write("hello from drive\n")
    check(a.start(), "amidrive: Host starts with the amidrive service")
    out = subprocess.run([CLI, "-p", str(a.port), "info"], capture_output=True, text=True).stdout
    check(re.search(r"^service amidrive 1 up$", out, re.M), "amidrive: hostinfo shows 'service amidrive 1 up'", out)
    time.sleep(1.5)
    check(not mounted(a.mp) and not os.path.exists(a.mp), "amidrive: nothing mounts at Host boot (automount waits for CONNECT 1)")
    s = AD(a.port); check(s.status == 0, "amidrive: loopback handshake ok", s.msg)
    st = s.status_(); check(st["state"] == ST["ready"] and st["flags"] & (F_FUSE | F_RCLONE) == F_FUSE | F_RCLONE and st["mount"] == a.mp,
                             "amidrive: STATUS -> ready, fuse + rclone ok, mount path", st)
    r, st = s.req(5, 1)
    check(r["status"] == 0 and st["state"] == ST["mounted"] and mounted(a.mp), "amidrive: CONNECT 1 (AmiStudio start) mounts", (r, st))
    check(mountinfo_raw(a.mp) and "\\040" in mountinfo_raw(a.mp)[0], "amidrive: mount point with a space (\\040 in mountinfo)", mountinfo_raw(a.mp))
    marker = open(os.path.join(a.ad, "Drive.mount")).read() if os.path.exists(os.path.join(a.ad, "Drive.mount")) else ""
    check(marker == "amidrive\t1\t%s\t-\t%s\n" % (st["nonce"], a.mp) and re.fullmatch(r"[0-9a-f]{16}", st["nonce"] or ""), "amidrive: Drive.mount = 'amidrive<TAB>1<TAB><nonce><TAB><account><TAB><linux path>' (path last)", marker)
    check(open(a.mp + ".info", "rb").read() == icon if os.path.exists(a.mp + ".info") else False, "amidrive: Google Drive.info moved next to the mount, byte for byte")
    check(open(os.path.join(a.mp, "hello.txt")).read() == "hello from drive\n", "amidrive: remote files readable through the mount")
    rcpids = a.rclone_pids(); cmd = open("/proc/%d/cmdline" % rcpids[0], "rb").read().split(b"\0") if rcpids else []
    check(b"--drive-use-trash=true" in cmd and cmd.index(b"--drive-use-trash=true") > cmd.index(b"--vfs-write-back") and a.mp.encode() in cmd and b"--allow-other" not in cmd,
          "amidrive: rclone argv: mount path as one argument, --drive-use-trash=true last of the flags, no --allow-other", [c.decode() for c in cmd])
    check(not os.path.exists(os.path.join(a.confdir, "busy")), "amidrive: amidrive_keeps_host no -> no CONFDIR/busy while mounted")
    # ---------- pending uploads, FLUSH ----------
    open(os.path.join(a.mp, "new file.txt"), "w").write("written on the Amiga\n")
    st = wait_for(lambda: (lambda x: x if x["flags"] & F_PENDING else None)(s.status_()), 10)
    check(st and st["queued"] >= 1 and not os.path.exists(os.path.join(a.remote, "new file.txt")), "amidrive: a write shows as a pending upload (flag 4, queued)", st)
    w0 = AD(a.port); w0.send(2, b"\x01"); op0, _ = w0.recv(5)
    open(os.path.join(a.mp, "second.txt"), "w").write("second\n"); wait_for(lambda: s.status_()["queued"] >= 2, 10)
    s.progress = []; r, st = s.req(7)
    check(r["status"] == 0 and os.path.exists(os.path.join(a.remote, "new file.txt")) and not st["flags"] & F_PENDING and s.progress and s.progress[-1] == (0, 0),
          "amidrive: FLUSH uploads now (PROGRESS ends at 0 files), stays mounted", (r, st, s.progress))
    got = []
    try:
        while True:
            op, p = w0.recv(1); got.append(op)
            if op == 18: break
    except (socket.timeout, TimeoutError, OSError, EOFError): pass
    check(op0 == 16 and 19 in got and 18 not in got, "amidrive: a watcher gets the PROGRESS (and STATE), not the requester's RESULT", (op0, got))
    w0.close()
    # ---------- open files refuse UNMOUNT 0, with a long path cut keeping the end ----------
    deep = a.mp
    for i in range(6): deep = os.path.join(deep, "a very long folder name number %d with words" % i)
    os.makedirs(deep); fh = open(os.path.join(deep, "open file.txt"), "w"); fh.write("x"); fh.flush()
    r, st = s.req(6, 0)
    rel = os.path.relpath(os.path.join(deep, "open file.txt"), a.mp)
    check(r["status"] == 1 and len(r["paths"]) == 1 and r["paths"][0].startswith("\u2026") and rel.endswith(r["paths"][0][1:]) and len(r["paths"][0].encode()) <= 255 and mounted(a.mp),
          "amidrive: UNMOUNT 0 refused with files open (path list, cut to 255 bytes keeping the end)", r)
    many = []
    for i in range(25):
        f = open(os.path.join(deep, ("open file %02d " % i) + "x" * 200), "w"); f.write("y"); f.flush(); many.append(f)
    s.send(6, b"\x00"); flen = 0
    while True:
        op, p = s.recv(30)
        if op == 18: flen = len(p) + 5; rr = AD.result(p); break
    check(rr["status"] == 1 and len(rr["paths"]) == 20 and flen < 8000 and "26 files are open" in rr["message"], "amidrive: 26 open files -> 20 paths in the list, frame under 8 KB", (rr["message"], len(rr["paths"]), flen))
    for f in many: f.close()
    out = a.cli("unmount")
    check(out.returncode == 1 and "open " in out.stdout, "amidrive CLI: unmount refused with files open (exit 1)", out.stdout + out.stderr)
    fh.close()
    st = wait_for(lambda: (lambda x: x if x["open"] == 0 else None)(s.status_()), 5); check(st, "amidrive: STATE open-files back to 0 after the close", st)
    # ---------- the umount fallback (fusermount fails) and folder/icon cleanup ----------
    r, st = s.req(6, 0)
    check(r["status"] == 0 and not mounted(a.mp) and not os.path.exists(a.mp) and not os.path.exists(a.mp + ".info") and os.path.exists(os.path.join(a.ad, "Google Drive.info"))
          and not os.path.exists(os.path.join(a.ad, "Drive.mount")) and st["state"] == ST["ready"],
          "amidrive: UNMOUNT 0 with failing fusermount3 -> umount fallback; folder removed, icon parked, Drive.mount gone", (r, st))
    check("trying umount" in a.adlog(), "amidrive: the fusermount3 failure and fallback are logged")
    check(not a.rclone_pids(), "amidrive: rclone exits after the unmount")
    # ---------- non-empty mount point refused ----------
    os.makedirs(a.mp); open(os.path.join(a.mp, "stray.txt"), "w").write("x")
    r, st = s.req(5, 0)
    check(r["status"] == 7 and st["flags"] & F_NOTEMPTY and "has 1 file in it" in r["message"] and not mounted(a.mp), "amidrive: non-empty mount point refused (RESULT 7, flag 32)", (r, st))
    check(not a.rclone_pids(), "amidrive: no rclone started for a non-empty mount point")
    shutil.rmtree(a.mp)
    # ---------- unmount now (2) leaves pending in cache/, CONNECT then uploads ----------
    r, st = s.req(5, 0); check(r["status"] == 0 and st["state"] == ST["mounted"] and not st["flags"] & F_NOTEMPTY, "amidrive: CONNECT after the folder was emptied", (r, st))
    open(os.path.join(a.mp, "later.txt"), "w").write("upload me later\n")
    wait_for(lambda: s.status_()["flags"] & F_PENDING, 10)
    r, st = s.req(6, 2)
    check(r["status"] == 0 and not mounted(a.mp) and st["flags"] & F_PENDING and st["queued"] >= 1 and not os.path.exists(os.path.join(a.remote, "later.txt")),
          "amidrive: UNMOUNT 2 (now): pending upload stays in cache/ (flag 4 while unmounted)", (r, st))
    r, st = s.req(5, 0); r2, st2 = s.req(7)
    check(r["status"] == 0 and r2["status"] == 0 and open(os.path.join(a.remote, "later.txt")).read() == "upload me later\n", "amidrive: next CONNECT + FLUSH uploads what was left in cache/", (r, r2))
    # ---------- unmount later (1) with a file open: flush, lazy unmount ----------
    fh = open(os.path.join(a.mp, "hello.txt")); fh.read()
    open(os.path.join(a.mp, "late2.txt"), "w").write("flushed before the lazy unmount\n")
    r, st = s.req(6, 1)
    check(r["status"] == 0 and not mounted(a.mp) and os.path.exists(os.path.join(a.remote, "late2.txt")), "amidrive: UNMOUNT 1 flushes then unmounts lazily with a file open", (r, st))
    r2, _ = s.req(5, 0)
    check(r2["status"] == 4 and a.rclone_pids(), "amidrive: rclone keeps serving the open file; CONNECT waits (busy) until it is closed", r2)
    fh.close()
    check(wait_for(lambda: not a.rclone_pids(), 15), "amidrive: closing the last open file ends that rclone")
    # ---------- CLI ----------
    out = a.cli("connect"); check(out.returncode == 0 and "state mounted" in out.stdout, "amidrive CLI: connect", out.stdout + out.stderr)
    out = a.cli("status"); check(out.returncode == 0 and "state mounted" in out.stdout and "mount %s" % a.mp in out.stdout, "amidrive CLI: status", out.stdout)
    out = subprocess.run([CLI, "-p", str(a.port), "amidrive", "unmount", "--force"], capture_output=True, text=True, timeout=60)
    check(out.returncode == 0 and not mounted(a.mp), "ash-cli amidrive unmount --force", out.stdout + out.stderr)
    out = a.cli("bogus"); check(out.returncode == 10, "amidrive CLI: unknown command -> usage (10)", out.returncode)
    # ---------- WATCH ----------
    w = AD(a.port); w.send(2, b"\x01"); op, p = w.recv(5)
    check(op == 16 and AD.state(p)["state"] == ST["ready"], "amidrive: WATCH 1 -> the current STATE at once (no RESULT)", op)
    s.req(5, 0); seen = []
    end = time.time() + 5
    while time.time() < end:
        try: op, p = w.recv(1)
        except (socket.timeout, TimeoutError): continue
        if op == 16: seen.append(AD.state(p)["state"])
        if ST["mounted"] in seen: break
    check(ST["mounted"] in seen, "amidrive: WATCH pushes the new state on change", seen)
    # ---------- rclone killed: lazy unmount, error state, one retry while a session is open ----------
    pid = a.rclone_pids()[0]; os.kill(pid, signal.SIGKILL)
    ok = wait_for(lambda: (lambda pp: pp and pp[0] != pid and mounted(a.mp))(a.rclone_pids()), 25, 0.25)
    check(ok and "rclone exited" in a.adlog() and "retrying the mount once" in a.adlog(), "amidrive: rclone killed -> lazy unmount, one automatic retry", a.adlog()[-600:])
    w.close()
    # ---------- Host killed (-9) with rclone running: a restart clears the leftover via the pidfile ----------
    old = a.rclone_pids(); a.stop(signal.SIGKILL)
    check(old and all(os.path.exists("/proc/%d" % p) for p in old) and mounted(a.mp), "amidrive: Host killed -9 leaves its rclone and the mount (as after a crash)")
    s.close()
    a.start()
    ok = wait_for(lambda: not mounted(a.mp) and not any(os.path.exists("/proc/%d/cmdline" % p) and b"rclone" in open("/proc/%d/cmdline" % p, "rb").read() for p in old), 15)
    check(ok and not os.path.exists(os.path.join(a.ad, "Drive.mount")) and not os.path.exists(a.mp) and "leftover rclone" in a.adlog(),
          "amidrive: Host restart kills the leftover rclone (pidfile + cmdline check) and clears the stale mount", a.adlog()[-500:])
    check(not mounted(a.mp), "amidrive: and still no automount at boot")
    s = AD(a.port); r, st = s.req(5, 1); check(r["status"] == 0 and mounted(a.mp), "amidrive: CONNECT after the restart", r)
    # ---------- logout-style SIGTERM to rclone and the Host together: pending stays in cache/, uploaded next time ----------
    open(os.path.join(a.mp, "at logout.txt"), "w").write("pending at logout\n")
    wait_for(lambda: s.status_()["flags"] & F_PENDING, 10)
    rp = a.rclone_pids(); s.close()
    for p in rp: os.kill(p, signal.SIGTERM)
    a.p.send_signal(signal.SIGTERM)
    try: a.p.wait(30)
    except subprocess.TimeoutExpired: pass
    wait_for(lambda: not a.rclone_pids(), 15)
    metas = [os.path.join(dp, f) for dp, _, fs in os.walk(os.path.join(a.ad, "cache")) for f in fs if "vfsMeta" in dp]
    dirty = [m for m in metas if '"Dirty": true' in open(m).read() or '"Dirty":true' in open(m).read()]
    check(a.p.poll() is not None and not mounted(a.mp) and (dirty or os.path.exists(os.path.join(a.remote, "at logout.txt"))),
          "amidrive: SIGTERM to rclone + Host together: Host exits, mount gone, the file is in cache/ (or already uploaded)", (dirty, a.p.poll()))
    a.start(); s = AD(a.port); r, _ = s.req(5, 0); r2, _ = s.req(7)
    check(r["status"] == 0 and os.path.exists(os.path.join(a.remote, "at logout.txt")), "amidrive: ... and uploaded after the next CONNECT", (r, r2))
    # ---------- STOPHOST (launcher quit): flush + unmount before the Host exits ----------
    open(os.path.join(a.mp, "before quit.txt"), "w").write("flushed by the Host stop\n")
    wait_for(lambda: s.status_()["flags"] & F_PENDING, 10)
    s.close(); time.sleep(0.3)
    c = socket.create_connection(("127.0.0.1", a.port), timeout=30)
    c.sendall(b"ASH1" + struct.pack("<H", 1) + b"\x07control" + struct.pack("<H", 1) + b"\x00\x08launcher")
    h = rd(c, 10); rd(c, h[9])
    c.sendall(struct.pack("<I", 1) + b"\x01"); n = struct.unpack("<I", rd(c, 4))[0]; q = rd(c, n)
    rc = None
    try: rc = a.p.wait(40)
    except subprocess.TimeoutExpired: pass
    log = a.out()
    check(q[0] == 16 and q[1] == 2, "amidrive: STOPHOST with only the launcher's control session -> stopping now", q)
    check(rc == 0 and not mounted(a.mp) and os.path.exists(os.path.join(a.remote, "before quit.txt")) and not os.path.exists(os.path.join(a.ad, "Drive.mount"))
          and not os.path.exists(a.mp) and os.path.exists(os.path.join(a.ad, "Google Drive.info")) and not a.rclone_pids(),
          "amidrive: Host stop flushes pending uploads and unmounts before it exits (stop-on-quit)", (rc, log[-500:]))
    check("stop: unmounted" in a.adlog(), "amidrive: the stop is logged")
    c.close()
    # ---------- sign-in: URL only, cancel; the client secret never in argv or a log ----------
    secret = "test-secret-" + os.urandom(6).hex()
    # the OAuth client comes from host-defaults.conf next to the binary (as in the release tarball)
    defaults = os.path.join(os.path.dirname(HOSTD), "host-defaults.conf")
    had = os.path.exists(defaults)
    if not had: open(defaults, "w").write('amidrive_client_id test-client.apps.googleusercontent.com\namidrive_client_secret "%s"\n' % secret)
    b = AHost(base, "b", extra="amidrive_auth_timeout 30\namidrive_auth_check no\n", signed_in=False); hosts.append(b)   # a made-up client: Google would refuse it
    try: b.start()
    finally:
        time.sleep(0.5)
        if not had: os.remove(defaults)
    s = AD(b.port)
    st = s.status_(); check(st["state"] == ST["signed_out"], "amidrive: no rclone.conf -> signed out", st)
    r, st = s.req(5, 1); check(r["status"] == 3 and r["message"] == "" and not mounted(b.mp), "amidrive: CONNECT 1 while signed out -> RESULT 3 with no message, nothing mounted", r)
    r, st = s.req(5, 0); check(r["status"] == 3, "amidrive: CONNECT (manual) while signed out -> RESULT 3", r)
    if socket.socket().connect_ex(("127.0.0.1", 53682)) == 0:
        skip("amidrive sign-in URL test", "port 53682 is in use")
    else:
        s.send(3, b"\x01"); r = {"status": 0}
        url = None; end = time.time() + 20
        while time.time() < end and not url:
            op, p = s.recv(20)
            if op == 17 and p[0] == 0: url = p[2:2 + p[1]].decode()
            if op == 18: r = AD.result(p)
        apids = pids_matching("authorize")
        argv_ok = all(secret.encode() not in open("/proc/%d/cmdline" % p, "rb").read() for p in apids if os.path.exists("/proc/%d/cmdline" % p))
        check(r["status"] == 0 and url and url.startswith("http://127.0.0.1:53682/auth"), "amidrive: AUTH_START 1 -> AUTH 0 with the local sign-in URL", (r, url))
        check(apids and argv_ok, "amidrive: rclone authorize runs without the client secret on its command line", apids)
        try:
            import urllib.request
            loc = None
            class NoRedir(urllib.request.HTTPRedirectHandler):
                def redirect_request(self, req, fp, code, msg, hdrs, newurl): nonlocal loc; loc = newurl; return None
            try: urllib.request.build_opener(NoRedir).open(url, timeout=5)
            except Exception: pass
            check(loc and "client_id=test-client.apps.googleusercontent.com" in loc and secret not in loc, "amidrive: the client ID from host-defaults.conf reaches Google's sign-in URL (secret does not)", loc)
        except Exception as e: check(False, "amidrive: sign-in redirect", e)
        s.send(4); res4 = res3 = None; step = None; end = time.time() + 15
        while time.time() < end and (step is None or res3 is None or res4 is None):
            op, p = s.recv(15)
            if op == 18 and p[0] == 4: res4 = AD.result(p)
            if op == 18 and p[0] == 3: res3 = AD.result(p)
            if op == 17 and p[0] == 3: step = p[2:2 + p[1]].decode()
        check(step and "cancel" in step and res3 and res3["status"] == 2 and res4 and res4["status"] == 0,
              "amidrive: AUTH_CANCEL -> AUTH 3, RESULT op 3 failed, RESULT op 4 ok", (step, res3, res4))
        check(wait_for(lambda: not [p for p in pids_matching("authorize") if p in apids], 5), "amidrive: the cancelled rclone authorize is gone")
    s.close(); b.stop()
    leaks = [f for f in (b.out(), b.adlog(), a.out(), a.adlog()) if secret in f]
    check(not leaks, "amidrive: the client secret is in no log")
    check(not os.path.exists(os.path.join(b.ad, "rclone.conf")), "amidrive: a cancelled sign-in writes no rclone.conf")
    # ---------- sign-in check against Google (a local HTTPS stand-in) and AUTH_START 2 (URL for AmiSurf) ----------
    signin_check_tests(check, skip, base, hosts)
    nss_tests(check, skip, base, hosts)
    # ---------- a second Host on the same data folder leaves the first one's Drive alone (amidrive.lock) ----------
    a.start(); s = AD(a.port); r, _ = s.req(5, 0)
    a2 = AHost(base, "a2", data=a.data, remote=a.remote); a2.mp = a.mp; hosts.append(a2)
    open(os.path.join(a2.confdir, "host.conf"), "a").write('amidrive_mountpoint "%s"\n' % a.mp)
    a2.start(); s2 = AD(a2.port); time.sleep(0.5); r2, _ = s2.req(5, 0)
    check(r["status"] == 0 and mounted(a.mp) and r2["status"] == 4 and "another" in r2["message"] and "leaving its mount alone" in a.adlog(),
          "amidrive: a second Host on the same data folder neither clears nor mounts the first Host's Drive", (r, r2))
    s2.close(); a2.stop(); check(mounted(a.mp), "amidrive: ... and its stop leaves the mount alone")
    s.close(); a.stop(); check(not mounted(a.mp), "amidrive: the owning Host unmounts on stop")
    # ---------- copied and moved drawer ----------
    c1 = AHost(base, "c"); hosts.append(c1)
    c1.start(); s = AD(c1.port); s.req(5, 0)
    ship = open(os.path.join(ROOT, "dist", "amidrive", "Google Drive.info"), "rb").read()
    check(os.path.exists(c1.mp + ".info") and open(c1.mp + ".info", "rb").read() == ship, "amidrive: first mount places the drawer icon shipped with the Host (services/amidrive), byte for byte")
    open(os.path.join(c1.mp, "c.txt"), "w").write("pending in the original\n"); wait_for(lambda: s.status_()["flags"] & F_PENDING, 10)
    s.req(6, 2); s.close(); c1.stop()
    cp_data = os.path.join(base, "c-copy-data"); shutil.copytree(c1.data, cp_data, symlinks=True)
    c2 = AHost(base, "c2", data=cp_data, remote=c1.remote); hosts.append(c2)
    c2.start(); s = AD(c2.port); r, st = s.req(5, 1)
    check(r["status"] == 8 and st["flags"] & F_COPIED and "copied" in r["message"] and not mounted(c2.mp) and not c2.rclone_pids(),
          "amidrive: copied drawer -> RESULT 8, flag 64, the token is not used (no rclone)", (r, st))
    aside = [e for e in os.listdir(c2.ad) if e.startswith("cache.copied-")]
    check(aside and not os.path.exists(os.path.join(c2.ad, "cache")) and os.path.isdir(os.path.join(c1.ad, "cache")), "amidrive: the copy's cache/ is set aside as cache.copied-<date> (never uploaded)", os.listdir(c2.ad))
    r, st = s.req(5, 0); check(r["status"] == 8, "amidrive: ... and stays refused until a new sign-in", r)
    s.close(); c2.stop()
    check(not os.path.exists(os.path.join(c1.remote, "c.txt")), "amidrive: nothing of the copy was uploaded")
    mv_data = os.path.join(base, "c-moved-data"); os.rename(c1.data, mv_data)
    c3 = AHost(base, "c3", data=mv_data, remote=c1.remote); hosts.append(c3)
    c3.start(); s = AD(c3.port); r, st = s.req(5, 1)
    check(r["status"] == 8 and "moved" in r["message"] and os.path.isdir(os.path.join(c3.ad, "cache")) and not c3.rclone_pids(), "amidrive: moved drawer -> RESULT 8, cache/ kept", r)
    s.close(); c3.stop()
    # a new sign-in (stand-in: drawer.path rewritten the way the sign-in does) and the moved cache uploads
    open(os.path.join(c3.ad, "drawer.path"), "w").write(os.path.realpath(c3.ad) + "\n")
    c3.start(); s = AD(c3.port); r, _ = s.req(5, 0); r2, _ = s.req(7)
    check(r["status"] == 0 and os.path.exists(os.path.join(c1.remote, "c.txt")), "amidrive: after re-sign-in the moved drawer's pending upload goes out", (r, r2))
    # ---------- SIGNOUT ----------
    open(os.path.join(c3.mp, "keep.txt"), "w").write("pending\n"); wait_for(lambda: s.status_()["flags"] & F_PENDING, 10)
    r, _ = s.req(6, 2); r, st = s.req(8, 0)
    check(r["status"] == 0 and not os.path.exists(os.path.join(c3.ad, "rclone.conf")) and not os.path.exists(os.path.join(c3.ad, "drawer.path")) and not os.path.exists(os.path.join(c3.ad, "cache"))
          and st["state"] == ST["signed_out"], "amidrive: SIGNOUT after UNMOUNT 2 deletes the remote, drawer.path and cache/", (r, st))
    s.close(); c3.stop()
    d = AHost(base, "d"); hosts.append(d); d.start(); s = AD(d.port); s.req(5, 0)
    open(os.path.join(d.mp, "p.txt"), "w").write("p\n"); wait_for(lambda: s.status_()["flags"] & F_PENDING, 10)
    r, st = s.req(8, 0)
    check(r["status"] == 0 and os.path.exists(os.path.join(d.remote, "p.txt")) and not os.path.exists(os.path.join(d.ad, "rclone.conf")), "amidrive: SIGNOUT flushes, unmounts, then signs out", r)
    s.close(); d.stop()
    # ---------- amidrive_keeps_host yes: busy() keeps the Host up, CONFDIR/busy for run-host.sh ----------
    k = AHost(base, "k", extra="amidrive_keeps_host yes\n"); hosts.append(k); k.start()
    s = AD(k.port); s.req(5, 0); s.close()
    check(wait_for(lambda: os.path.exists(os.path.join(k.confdir, "busy")), 3), "amidrive_keeps_host yes: CONFDIR/busy while mounted")
    c = socket.create_connection(("127.0.0.1", k.port), timeout=30)
    c.sendall(b"ASH1" + struct.pack("<H", 1) + b"\x07control" + struct.pack("<H", 1) + b"\x00\x08launcher"); h = rd(c, 10); rd(c, h[9])
    c.sendall(struct.pack("<I", 3) + b"\x01" + struct.pack("<H", 1)); n = struct.unpack("<I", rd(c, 4))[0]; q = rd(c, n)
    time.sleep(2.5)
    check(q[1] == 1 and k.p.poll() is None and mounted(k.mp), "amidrive_keeps_host yes: quit-when-unused stays armed while mounted", q)
    s = AD(k.port); s.req(6, 0); s.close()
    rc = None
    try: rc = k.p.wait(15)
    except subprocess.TimeoutExpired: pass
    check(rc == 0 and not os.path.exists(os.path.join(k.confdir, "busy")), "amidrive_keeps_host yes: after the unmount busy ends and the armed quit stops the Host", rc)
    c.close()
    # ---------- the mount point is created 0755 (even with umask 077), and a failed mount removes it again ----------
    fake_rc = os.path.join(base, "rclone-that-fails"); open(fake_rc, "w").write("#!/bin/sh\necho 'ERROR : fake rclone: mount failed on purpose' >&2\nsleep 2\nexit 1\n"); os.chmod(fake_rc, 0o755)
    m = AHost(base, "m", extra="amidrive_rclone %s\n" % fake_rc); hosts.append(m); m.start(umask=0o077)
    s = AD(m.port); s.send(5, b"\x00")
    mode = wait_for(lambda: os.path.isdir(m.mp) and stat.S_IMODE(os.stat(m.mp).st_mode), 5, 0.02)
    s2 = AD(m.port); rb, _ = s2.req(7); s2.close()
    check(rb["status"] == 4 and "being mounted" in rb["message"], "amidrive: a FLUSH from another client while a CONNECT runs -> RESULT 4 busy (not queued)", rb)
    check(mode == 0o755, "amidrive: mount point folder created with mode 0755 (Host umask 077)", oct(mode or 0))
    while True:
        op, p = s.recv(60)
        if op == 18: res = AD.result(p); break
    st = s.status_()
    check(res["status"] == 2 and st["state"] == ST["error"] and not os.path.exists(m.mp), "amidrive: a failed mount -> RESULT 2, state 8, empty folder removed", (res, st))
    s.close(); m.stop()
    # ---------- expired token (invalid_grant) -> flag 16, RESULT 6, no retry ----------
    e = AHost(base, "e", signed_in=False); hosts.append(e)
    os.makedirs(e.ad, exist_ok=True)
    open(os.path.join(e.ad, "rclone.conf"), "w").write('[gdrive]\ntype = drive\nscope = drive\ntoken = {"access_token":"x","token_type":"Bearer","refresh_token":"1//not-a-real-token","expiry":"2001-01-01T00:00:00Z"}\n')
    os.chmod(os.path.join(e.ad, "rclone.conf"), 0o600)
    open(os.path.join(e.ad, "drawer.path"), "w").write(os.path.realpath(e.ad) + "\n")
    try: socket.create_connection(("oauth2.googleapis.com", 443), timeout=5).close(); online = True
    except OSError: online = False
    if not online: skip("amidrive expired token", "offline")
    else:
        e.start(); s = AD(e.port); r, st = s.req(5, 0, timeout=60)
        if r["status"] == 0:   # rclone mounted before it needed the token: the monitor finds invalid_grant in rclone.log
            os.listdir(e.mp) if mounted(e.mp) else None
            st = wait_for(lambda: (lambda x: x if x["flags"] & F_EXPIRED else None)(s.status_()), 20)
        check(st and st["flags"] & F_EXPIRED, "amidrive: invalid_grant -> flag 16 (sign-in expired)", (r, st))
        s.req(6, 2)
        r, st = s.req(5, 1); check(r["status"] == 6 and not e.rclone_pids(), "amidrive: CONNECT with an expired sign-in -> RESULT 6, rclone not started", r)
        s.close(); e.stop()

def signin_check_tests(check, skip, base, hosts):
    """0.1.9: before rclone starts, the Host asks the authorization endpoint whether it accepts the
    OAuth client with rclone's redirect (Google answers "Error 400: redirect_uri_mismatch" for a client without it);
    AUTH_START 2 gives the URL for the Host's browser engine (AmiSurf) when there is a "browser" service, else it opens
    the desktop browser like 0. Google itself is replaced by a local HTTPS server (amidrive_auth_check_url + cafile)."""
    import http.server, ssl, threading, urllib.parse
    if socket.socket().connect_ex(("127.0.0.1", 53682)) == 0: skip("amidrive sign-in check", "port 53682 is in use"); return
    if not os.path.exists(os.path.join(base, "ca.pem")):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import run_tests; run_tests.make_ca(base)
    seen = []; mode = ["mismatch"]
    class G(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        def log_message(self, *a): pass
        def do_GET(self):
            seen.append(self.path)
            filler = b"<!-- " + b"x" * 70000 + b" -->"   # the marker comes after more than one read
            if mode[0] == "hang": time.sleep(12); return   # 0.1.10: a server that never answers
            status, chunked = 200, False
            if mode[0] == "mismatch": body = b"<html><head><title>Error 400 (OAuth2 Error)</title></head><body>" + filler + b"<p>Access blocked: This app's request is invalid</p><p>Error 400: redirect_uri_mismatch</p></body></html>"
            elif mode[0] == "weird":   # 0.1.10: odd pages must neither crash the scan nor count as a refusal
                body = (b"\x00\xff\xfeError 4\x00Error 40Error 4x0: abcError 400:Error 400: ABC_DEF Error 400: ab \xc3\xa9" + b"\xff" * 9000
                        + b"Error 499: " + b"\x01\x80" * 10000 + b"<p>Error 40")
            elif mode[0] == "chunked": status, chunked = 400, True; body = filler + b"<p>Error 400: invalid_client</p>"
            elif mode[0] == "empty": body = b""
            elif mode[0] == "redirect":
                self.send_response(302); self.send_header("Location", "/v3/signin/identifier?" + "a" * 3000); self.send_header("Content-Length", "0"); self.end_headers(); return
            else: body = b"<html><head><title>Sign in - Google Accounts</title></head><body>" + filler + b"<input type=email></body></html>"
            try:
                self.send_response(status); self.send_header("Content-Type", "text/html")
                if chunked:
                    self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
                    for i in range(0, len(body), 777): c = body[i:i + 777]; self.wfile.write(b"%x\r\n%s\r\n" % (len(c), c))
                    self.wfile.write(b"0\r\n\r\n")
                else: self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
            except (ConnectionError, ssl.SSLError, BrokenPipeError): pass   # the Host stops reading once it has the answer
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), G)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(os.path.join(base, "srv.pem"), os.path.join(base, "srv.key"))
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True); threading.Thread(target=srv.serve_forever, daemon=True).start()
    secret = "test-secret-" + os.urandom(6).hex(); cid = "test-client-%s.apps.googleusercontent.com" % os.urandom(3).hex()
    common = ('amidrive_client_id %s\namidrive_client_secret "%s"\namidrive_auth_timeout 30\ncafile %s/ca.pem\n'
              'amidrive_auth_check_url https://localhost:%d/o/oauth2/auth\n' % (cid, secret, base, srv.server_address[1]))
    browser = "\nservice browser\nproto 2\nexec %s %s/test/fake_backend.py -port {port} -token {token} -idle 0\nhandshake asbc2\nready_timeout 10\n" % (sys.executable, ROOT)
    def auth(s, method, want_url=True, t=30):
        """AUTH_START method -> (first AUTH step, its text, RESULT op 3 or None)"""
        s.send(3, bytes([method])); end = time.time() + t; step = text = None; res = None
        while time.time() < end:
            op, p = s.recv(max(1, end - time.time()))
            if op == 17 and step is None: step, text = p[0], p[2:2 + p[1]].decode("utf-8", "replace"); last_flags[0] = p[2 + p[1]] if len(p) > 2 + p[1] else None
            if op == 18 and p[0] == 3: res = AD.result(p); break
            if want_url and step == 0: break
        return step, text, res
    last_flags = [None]
    def next_auth(s, t=10):
        """the next AUTH frame -> (step, text, trailing flags or None), or None"""
        end = time.time() + t
        while time.time() < end:
            try: op, p = s.recv(max(0.2, end - time.time()))
            except (socket.timeout, TimeoutError): return None
            if op == 17: return p[0], p[2:2 + p[1]].decode("utf-8", "replace"), (p[2 + p[1]] if len(p) > 2 + p[1] else None)
        return None
    def cancel(s):
        s.send(4); end = time.time() + 15
        while time.time() < end:
            op, p = s.recv(15)
            if op == 18 and p[0] == 4: return AD.result(p)
    c = AHost(base, "c", extra=common + browser, signed_in=False); hosts.append(c)
    try:
        c.start(); s = AD(c.port)
        before = set(pids_matching("authorize"))
        step, text, res = auth(s, 1, want_url=False)
        check(step == 3 and text and "redirect_uri_mismatch" in text and res and res["status"] == 2 and res["message"] == text,
              "amidrive: sign-in check: Google's 'Error 400: redirect_uri_mismatch' -> AUTH 3 + RESULT failed, same text", (step, text, res))
        check(len(text.encode()) <= 160, "amidrive: the refusal text stays well under 255 bytes (%d)" % len(text.encode()), text)
        check(not (set(pids_matching("authorize")) - before), "amidrive: sign-in check: no rclone authorize (and no browser) when Google refuses the client")
        q = urllib.parse.parse_qs(urllib.parse.urlparse(seen[-1]).query) if seen else {}
        check(seen and q.get("client_id") == [cid] and q.get("redirect_uri") == ["http://127.0.0.1:53682/"] and q.get("scope") == ["https://www.googleapis.com/auth/drive"]
              and secret not in seen[-1], "amidrive: sign-in check asks with the client ID, rclone's redirect and the drive scope (never the secret)", seen[-1:])
        check("sign-in check: Google answers Error 400: redirect_uri_mismatch" in c.adlog(), "amidrive: sign-in check: the refusal is logged")
        st = s.status_(); check(st["state"] == ST["signed_out"], "amidrive: sign-in check: back to signed out after the refusal", st)
        # 0.1.10: other answers from the check (Rich's crash report): none may crash the Host; only a real
        # "Error 4xx: code" page refuses, everything else (odd bytes, empty, a redirect, silence) lets the sign-in go on
        for m, want in (("chunked", "invalid_client"), ("weird", None), ("empty", None), ("redirect", None), ("hang", None)):
            mode[0] = m; t0 = time.time(); before2 = set(pids_matching("authorize"))
            st_, tx_, rs_ = auth(s, 2, want_url=want is None, t=40)
            took = time.time() - t0; alive = c.p.poll() is None
            if want: ok_ = st_ == 3 and want in (tx_ or "") and rs_ and rs_["message"] == tx_
            else: ok_ = st_ == 0 and (tx_ or "").startswith("http://127.0.0.1:53682/auth")
            check(alive and ok_ and (m != "hang" or took < 15), "amidrive: sign-in check answer '%s' -> %s, Host alive (%.1f s)" % (m, "refused (%s)" % want if want else "sign-in goes on", took), (st_, tx_, rs_, alive))
            if want is None:
                cancel(s); ap = [p for p in pids_matching("authorize") if p not in before2]; wait_for(lambda: not [p for p in pids_matching("authorize") if p in ap], 5)
                while next_auth(s, 1): pass
        check("sign-in check: Google not reachable" in c.adlog(), "amidrive: a check that gets no answer is logged and the sign-in goes on")
        mode[0] = "ok"; n0 = len(seen)
        step, text, res = auth(s, 2)
        apids = [p for p in pids_matching("authorize") if p not in before]
        argv = b" ".join(open("/proc/%d/cmdline" % p, "rb").read().replace(b"\0", b" ") for p in apids if os.path.exists("/proc/%d" % p))
        check(len(seen) == n0 + 1 and step == 0 and text.startswith("http://127.0.0.1:53682/auth") and res is None,
              "amidrive: AUTH_START 2 with a browser engine on this Host -> sign-in check passes, AUTH 0 with the URL for AmiSurf", (step, text, res))
        check(last_flags[0] == 1, "amidrive: AUTH 0 flags = 1 (this Host has a browser engine)", last_flags)
        url0 = text
        s2 = AD(c.port); j = auth(s2, 2); jf = last_flags[0]
        check(j[0] == 0 and j[1] == url0 and jf == 1, "amidrive: a second AUTH_START 2 joins and gets the same URL (flags 1)", (j, jf))
        s3 = AD(c.port); j = auth(s3, 0)
        check(j[0] == 0 and j[1] == url0, "amidrive: an AUTH_START 0 joiner of a URL sign-in gets the URL too (no browser opened)", j)
        check(apids and b"--auth-no-open-browser" in argv and secret.encode() not in argv, "amidrive: AUTH_START 2: rclone authorize does not open the desktop browser", argv)
        check("sign-in started (URL for AmiSurf)" in c.adlog(), "amidrive: AUTH_START 2 is logged as the AmiSurf sign-in")
        r4 = cancel(s); check(r4 and r4["status"] == 0 and wait_for(lambda: not [p for p in pids_matching("authorize") if p in apids], 5), "amidrive: the AmiSurf sign-in can be cancelled", r4)
        ends = []
        for x in (s2, s3):
            end = time.time() + 10; got = None
            while time.time() < end and got is None:
                op, p = x.recv(10)
                if op == 18 and p[0] == 3: got = AD.result(p)
            ends.append(got and got["status"]); x.close()
        check(ends == [2, 2], "amidrive: the joiners get RESULT op 3 failed when it is cancelled", ends)
        s.close()
    finally: c.stop()
    # no browser engine on this Host: AUTH_START 2 still sends only the URL (any browser on this PC can open it, e.g. an
    # AmiSurf engine of another Host process); flags 0. A desktop browser that opens (fake xdg-open) for method 0: the
    # starter gets AUTH 1, a method-2 joiner gets AUTH 0 with the URL and never AUTH 1; no desktop browser -> flags 2.
    fake = os.path.join(base, "fake-xdg"); os.makedirs(fake, exist_ok=True)
    xo = os.path.join(fake, "xdg-open"); open(xo, "w").write("#!/bin/sh\nexit 0\n"); os.chmod(xo, 0o755)
    empty = os.path.join(base, "empty-path"); os.makedirs(empty, exist_ok=True)
    d = AHost(base, "d", extra=common + "amidrive_auth_check no\n", signed_in=False, env={"PATH": fake, "DISPLAY": ":99", "WAYLAND_DISPLAY": "", "BROWSER": ""}); hosts.append(d)
    try:
        d.start(); s = AD(d.port); n0 = len(seen); before = set(pids_matching("authorize"))
        step, text, res = auth(s, 2); fl = last_flags[0]
        apids = [p for p in pids_matching("authorize") if p not in before]
        argv = b" ".join(open("/proc/%d/cmdline" % p, "rb").read().replace(b"\0", b" ") for p in apids if os.path.exists("/proc/%d" % p))
        check(apids and b"--auth-no-open-browser" in argv and step == 0 and text.startswith("http://127.0.0.1:53682/auth") and fl == 0,
              "amidrive: AUTH_START 2 without a browser engine -> AUTH 0 with the URL, flags 0, no desktop browser", (argv, step, text, fl))
        check("this Host has no browser engine" in d.adlog(), "amidrive: ... logged")
        check(len(seen) == n0, "amidrive: amidrive_auth_check no -> no sign-in check request", seen[n0:])
        cancel(s); wait_for(lambda: not [p for p in pids_matching("authorize") if p in apids], 5)
        while next_auth(s, 1): pass
        before = set(pids_matching("authorize"))
        s.send(3, b"\x00"); fr = []; step = text = res = None; end = time.time() + 8
        while time.time() < end and step is None:
            try: op, p = s.recv(max(0.2, end - time.time()))
            except (socket.timeout, TimeoutError): break
            fr.append((op, p[:4]))
            if op == 17: step, text = p[0], p[2:2 + p[1]].decode("utf-8", "replace")
            if op == 18 and p[0] == 3: res = AD.result(p); break
        check(step == 1 and res is None, "amidrive: AUTH_START 0 with a desktop browser that opens -> AUTH 1", (step, text, res, d.adlog()[-400:]))
        s2 = AD(d.port); j = auth(s2, 2); jf = last_flags[0]
        check(j[0] == 0 and j[1].startswith("http://127.0.0.1:53682/auth") and jf == 0, "amidrive: an AUTH_START 2 joiner of a desktop sign-in gets AUTH 0 with the URL", (j, jf))
        check(next_auth(s2, 2.5) is None, "amidrive: ... and no AUTH 1")
        check(next_auth(s, 1.5) is None, "amidrive: the method 0 starter gets no AUTH 0")
        cancel(s); s2.close()
        apids = [p for p in pids_matching("authorize") if p not in before]; wait_for(lambda: not [p for p in pids_matching("authorize") if p in apids], 5)
        s.close()
    finally: d.stop()
    e = AHost(base, "e2", extra=common + "amidrive_auth_check no\n", signed_in=False, env={"PATH": empty, "DISPLAY": "", "WAYLAND_DISPLAY": "", "BROWSER": ""}); hosts.append(e)
    try:
        e.start(); s = AD(e.port); before = set(pids_matching("authorize"))
        step, text, res = auth(s, 0); fl = last_flags[0]
        check(step == 0 and text.startswith("http://127.0.0.1:53682/auth") and fl == 2, "amidrive: AUTH_START 0 and no desktop browser opens -> AUTH 0 with flags 2", (step, text, fl))
        cancel(s); apids = [p for p in pids_matching("authorize") if p not in before]; wait_for(lambda: not [p for p in pids_matching("authorize") if p in apids], 5); s.close()
    finally: e.stop()
    srv.shutdown()
    leaks = [f for f in (c.out(), c.adlog(), d.out(), d.adlog(), e.out(), e.adlog()) if secret in f]
    check(not leaks, "amidrive: sign-in check: the client secret is in no log")

NSS_TRAP_C = r"""
/* test NSS module "amitrap": in getent (the system's own lookup tool) it answers drive-check.amistudio.test = 127.0.0.1;
   loaded into amistudio-hostd itself it crashes the way a foreign glibc's libc.so.6 does on Fedora/Nobara (SIGFPE in
   __libc_early_init), so a Host that still loads NSS modules in-process dies. */
#include <nss.h>
#include <netdb.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
__attribute__((constructor)) static void trap(void) {
    char e[512]; ssize_t n = readlink("/proc/self/exe", e, sizeof e - 1); if (n <= 0) return; e[n] = 0;
    if (strstr(e, "amistudio-hostd")) {
        const char *m = getenv("AMITRAP_MARK"); if (m) { int f = open(m, O_WRONLY | O_CREAT, 0600); if (f >= 0) close(f); }
        raise(SIGFPE);
    }
}
static int ours(const char *n) { return !strcasecmp(n, "drive-check.amistudio.test"); }
enum nss_status _nss_amitrap_gethostbyname4_r(const char *name, struct gaih_addrtuple **pat, char *buf, size_t bl, int *errnop, int *herrnop, int32_t *ttlp) {
    if (!ours(name)) { *herrnop = HOST_NOT_FOUND; return NSS_STATUS_NOTFOUND; }
    if (bl < sizeof(struct gaih_addrtuple) + 64) { *errnop = ERANGE; *herrnop = NETDB_INTERNAL; return NSS_STATUS_TRYAGAIN; }
    struct gaih_addrtuple *t = (struct gaih_addrtuple *)buf; memset(t, 0, sizeof *t);
    char *nm = buf + sizeof *t; strcpy(nm, "drive-check.amistudio.test");
    t->name = nm; t->family = AF_INET; inet_pton(AF_INET, "127.0.0.1", t->addr); *pat = t; if (ttlp) *ttlp = 0;
    return NSS_STATUS_SUCCESS;
}
enum nss_status _nss_amitrap_gethostbyname2_r(const char *name, int af, struct hostent *h, char *buf, size_t bl, int *errnop, int *herrnop) {
    if (!ours(name) || af != AF_INET) { *herrnop = HOST_NOT_FOUND; return NSS_STATUS_NOTFOUND; }
    if (bl < 128) { *errnop = ERANGE; return NSS_STATUS_TRYAGAIN; }
    char **al = (char **)buf; char *a = buf + 32; char *nm = buf + 40; strcpy(nm, "drive-check.amistudio.test");
    inet_pton(AF_INET, "127.0.0.1", a); al[0] = a; al[1] = 0; char **ali = al + 2; ali[0] = 0;
    h->h_name = nm; h->h_aliases = ali; h->h_addrtype = AF_INET; h->h_length = 4; h->h_addr_list = al;
    return NSS_STATUS_SUCCESS;
}
enum nss_status _nss_amitrap_gethostbyname_r(const char *name, struct hostent *h, char *buf, size_t bl, int *errnop, int *herrnop) {
    return _nss_amitrap_gethostbyname2_r(name, AF_INET, h, buf, bl, errnop, herrnop);
}
"""

def nss_tests(check, skip, base, hosts):
    """0.1.10 regression (Host 0.1.9 crashed on Nobara at "Sign in..."): the sign-in check's lookup of a name that is not
    in /etc/hosts made the static release binary dlopen() the system's NSS modules (libnss_myhostname/resolve, with
    Fedora's own libc.so.6), which died with SIGFPE and took the Host down. Names must go to the system's getent, never
    to NSS modules inside the Host. bwrap gives the Host an nsswitch.conf with a trap module that resolves the check's
    host name in getent but crashes when it is loaded into amistudio-hostd. Run against the test build and, when it
    exists, the static release binary (build/static/amistudio-hostd)."""
    import http.server, ssl, threading
    need = [x for x in ("bwrap", "getent", "gcc", "openssl") if not shutil.which(x)]
    if need: skip("amidrive: no NSS modules in the Host", "needs " + " ".join(need)); return
    if subprocess.run(["bwrap", "--bind", "/", "/", "true"], capture_output=True).returncode: skip("amidrive: no NSS modules in the Host", "bwrap does not run here"); return
    if socket.socket().connect_ex(("127.0.0.1", 53682)) == 0: skip("amidrive: no NSS modules in the Host", "port 53682 is in use"); return
    d = os.path.join(base, "nss"); os.makedirs(d, exist_ok=True)
    open(os.path.join(d, "trap.c"), "w").write(NSS_TRAP_C)
    lib = os.path.join(d, "lib"); os.makedirs(lib, exist_ok=True)
    r = subprocess.run(["gcc", "-shared", "-fPIC", "-O2", "-o", os.path.join(lib, "libnss_amitrap.so.2"), os.path.join(d, "trap.c")], capture_output=True, text=True)
    if r.returncode: check(False, "amidrive: build the NSS trap module", r.stderr[-400:]); return
    nssconf = os.path.join(d, "nsswitch.conf"); open(nssconf, "w").write("passwd: files\ngroup: files\nhosts: files amitrap\n")
    # the module goes into the system's library folder (an overlay inside bwrap), so the system's getent and glibc's
    # in-process NSS loader would both find it, as with libnss_myhostname on Fedora (no LD_LIBRARY_PATH: the Host
    # strips that for getent)
    libdir = next((x for x in ("/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib") if os.path.exists(os.path.join(x, "libnss_files.so.2"))), None)
    if not libdir: skip("amidrive: no NSS modules in the Host", "no libnss_files.so.2 folder found"); return
    bw = ["bwrap", "--die-with-parent", "--bind", "/", "/", "--dev-bind", "/dev", "/dev", "--ro-bind", nssconf, "/etc/nsswitch.conf",
          "--overlay-src", libdir, "--overlay-src", lib, "--ro-overlay", libdir]
    g = subprocess.run(bw + ["getent", "ahosts", "drive-check.amistudio.test"], capture_output=True, text=True)
    if "127.0.0.1" not in g.stdout: skip("amidrive: no NSS modules in the Host", "the trap module does not load in getent here (bwrap overlay?) " + g.stderr[-200:]); return
    if not os.path.exists(os.path.join(base, "ca.pem")):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import run_tests; run_tests.make_ca(base)
    def run(*a): subprocess.run(a, check=True, capture_output=True)
    run("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=drive-check.amistudio.test", "-keyout", d + "/srv.key", "-out", d + "/srv.csr")
    open(d + "/ext", "w").write("subjectAltName=DNS:drive-check.amistudio.test\nbasicConstraints=CA:FALSE\nextendedKeyUsage=serverAuth\n")
    run("openssl", "x509", "-req", "-in", d + "/srv.csr", "-CA", base + "/ca.pem", "-CAkey", base + "/ca.key", "-CAcreateserial", "-days", "2", "-out", d + "/srv.pem", "-extfile", d + "/ext")
    class G(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        def log_message(self, *a): pass
        def do_GET(self):
            body = b"<html><body><p>Error 400: redirect_uri_mismatch</p></body></html>"
            try: self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
            except (ConnectionError, ssl.SSLError, BrokenPipeError): pass
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), G)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(d + "/srv.pem", d + "/srv.key")
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True); threading.Thread(target=srv.serve_forever, daemon=True).start()
    common = ('amidrive_client_id nss-client.apps.googleusercontent.com\namidrive_client_secret "nss-secret-%s"\namidrive_auth_timeout 30\ncafile %s/ca.pem\n'
              'amidrive_auth_check_url https://drive-check.amistudio.test:%d/o/oauth2/auth\n' % (os.urandom(4).hex(), base, srv.server_address[1]))
    exes = [("test build", HOSTD)]
    st = os.path.join(ROOT, "build", "static", "amistudio-hostd")
    if os.path.exists(st) and os.path.getmtime(st) >= os.path.getmtime(os.path.join(ROOT, "src", "plat_posix.c")): exes.append(("static release binary", st))
    else: skip("amidrive: no NSS modules in the static release binary", "build/static/amistudio-hostd missing or older than src/plat_posix.c (make tarball builds it)")
    for i, (label, exe) in enumerate(exes):
        mark = os.path.join(d, "trapped-%d" % i)
        h = AHost(base, "nss%d" % i, extra=common, signed_in=False, prefix=bw, exe=exe, env={"AMITRAP_MARK": mark}); hosts.append(h)
        try:
            h.start(); s = AD(h.port)
            s.send(3, b"\x02"); step = text = res = None; end = time.time() + 30
            try:
                while time.time() < end and res is None:
                    op, p = s.recv(max(1, end - time.time()))
                    if op == 17 and step is None: step, text = p[0], p[2:2 + p[1]].decode("utf-8", "replace")
                    if op == 18 and p[0] == 3: res = AD.result(p)
            except (EOFError, OSError, socket.timeout) as e: text = "connection lost: %r" % e
            time.sleep(0.3); alive = h.p.poll() is None
            check(alive and not os.path.exists(mark), "amidrive (%s): AUTH_START 2 with a check host that only the system's NSS knows: the Host does not load NSS modules and stays up" % label,
                  (h.p.returncode, os.path.exists(mark), text, h.out()[-300:]))
            check(step == 3 and "redirect_uri_mismatch" in (text or "") and res and res["message"] == text,
                  "amidrive (%s): ... the name is resolved through getent and the check's answer arrives (AUTH 3 + RESULT)" % label, (step, text, res))
            s.close()
        finally:   # SIGTERM the Host itself (bwrap does not pass it on), then bwrap ends with it
            for pid in pids_matching(h.data):
                try:
                    if open("/proc/%d/cmdline" % pid, "rb").read().split(b"\0")[0] == exe.encode(): os.kill(pid, signal.SIGTERM)
                except OSError: pass
            h.stop()
    srv.shutdown()

if __name__ == "__main__":   # python3 test/amidrive_tests.py: these tests alone
    import tempfile
    res = {"ok": 0, "fail": 0, "skip": 0}
    def check(c, name, extra=""):
        res["ok" if c else "fail"] += 1; print(("ok   " if c else "FAIL ") + name + ("" if c or extra == "" else "  -- " + str(extra)))
    def skip(name, why): res["skip"] += 1; print("skip %s (%s)" % (name, why))
    t = tempfile.mkdtemp(prefix="ash-test-")
    try: amidrive_tests(check, skip, t)
    finally: shutil.rmtree(t, ignore_errors=True)
    print("%s: %d ok, %d failed, %d skipped" % ("all passed" if not res["fail"] else "FAILED", res["ok"], res["fail"], res["skip"]))
    sys.exit(1 if res["fail"] else 0)
