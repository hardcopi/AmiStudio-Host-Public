#!/usr/bin/env python3
"""App-mode (AmiDiscord) end-to-end tests against the real engine (asb-chromed + Chromium) with a local fake app
(test/app_page.py) and, when online, the real discord.com login page (render only: private profile, private ports,
never logs in). Run from run_tests.py with ASH_TEST_BROWSER=1, or alone:
    python3 test/app_tests.py [ENGINE_SH]
ENGINE_SH defaults to build/engine/stage/engine/engine.sh (tools/fetch-engine.sh)."""
import os, re, socket, struct, subprocess, sys, tempfile, time, urllib.parse, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import app_page

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OPS = {"HELLO": 1, "NAV": 2, "BACK": 3, "RELOAD": 5, "MOUSE": 7, "KEY": 9, "RESIZE": 10, "TABNEW": 15, "TABSEL": 17, "CMD": 18, "FMT": 19,
       "ACK": 20, "CAPS": 22, "FOCUS": 23, "NOTIFYACT": 24, "FILEPUT": 25}
FRAME, META, STATUS, TABS = 32, 33, 35, 37
NOTIFY, NOTIFYCLOSE, UNREAD, OPENURL, FILEREQ, FILEDONE, DOWNLOAD, APPINFO = 45, 46, 47, 48, 49, 50, 51, 52

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p
def rd(s, n):
    b = b""
    while len(b) < n:
        k = s.recv(n - len(b))
        if not k: raise EOFError
        b += k
    return b
def s16(p, o):
    n = struct.unpack_from("<H", p, o)[0]; return p[o + 2:o + 2 + n].decode("utf-8", "replace"), o + 2 + n

class Client:
    """ASH v1 + ASBC v2 client, enough for the tests"""
    def __init__(self, port, service="amidiscord", client="AmiDiscord-test", token="", timeout=60):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.s.sendall(b"ASH1" + struct.pack("<H", 1) + bytes([len(service)]) + service.encode() + struct.pack("<H", 2)
                       + bytes([len(token)]) + token.encode() + bytes([len(client)]) + client.encode())
        h = rd(self.s, 10); self.status = h[6]; self.msg = rd(self.s, h[9]).decode() if h[9] else ""
        self.msgs = []; self.title = ""; self.url = ""; self.fbytes = 0; self.frames = 0
    def send(self, op, p=b""):
        if isinstance(op, str): op = OPS[op]
        self.s.sendall(struct.pack("<I", len(p) + 1) + bytes([op]) + p)
    def hello(self, w=640, h=400, pf=1, flags=0):
        self.send("HELLO", struct.pack("<HH", w, h)); self.send("FMT", bytes([pf, flags]) + b"\0\0")
    def caps(self, bits): self.send("CAPS", struct.pack("<I", bits))
    def nav(self, url): self.send("NAV", url.encode())
    def cmd(self, c): self.send("CMD", c.encode())
    def click(self, x, y):
        self.send("MOUSE", struct.pack("<HHBB", x, y, 0, 0)); self.send("MOUSE", struct.pack("<HHBB", x, y, 1, 1)); self.send("MOUSE", struct.pack("<HHBB", x, y, 0, 2))
    def pump(self, secs, until=None):
        """read for up to secs; until(op, payload) -> True stops early. Returns list of (op, payload) read."""
        got = []; end = time.time() + secs
        while time.time() < end:
            self.s.settimeout(max(0.05, end - time.time()))
            try:
                n = struct.unpack("<I", rd(self.s, 4))[0]; op = rd(self.s, 1)[0]; p = rd(self.s, n - 1)
            except (socket.timeout, TimeoutError): break
            except (EOFError, OSError): self.closed = True; break
            got.append((op, p)); self.msgs.append((op, p))
            if op == FRAME: self.frames += 1; self.fbytes += n + 4
            if op == META:
                self.url, o = s16(p, 1); self.title, o = s16(p, o)
            if until and until(op, p): break
        return got
    def wait_title(self, pat, secs=30):
        self.pump(secs, lambda op, p: op == META and re.search(pat, self.title) is not None)
        return re.search(pat, self.title) is not None
    def openurls(self): return [s16(p, 0)[0] + "|%d" % p[-1] for op, p in self.msgs if op == OPENURL]
    def close(self):
        try: self.s.close()
        except OSError: pass

def host_conf(exec_line, extra=""):
    return "service amidiscord\nproto 2\nexec %s\nhandshake asbc2\nready_timeout 40\nmax_clients 1\nlog amidiscord.log\n%s" % (exec_line, extra)
def start_host(tmp, conf, port, args=(), env=None):
    cd = os.path.join(tmp, "conf"); os.makedirs(cd, exist_ok=True)
    open(os.path.join(cd, "host.conf"), "w").write(conf)
    e = dict(os.environ, AMISTUDIO_HOST_DIR=cd); e.update(env or {})
    p = subprocess.Popen([os.path.join(ROOT, "build", "amistudio-hostd"), "--port", str(port), "--no-update-check", "--no-bundled-browser", "-v"] + list(args),
                         stdout=open(os.path.join(tmp, "hostd.out"), "w"), stderr=subprocess.STDOUT, env=e)
    for _ in range(100):
        try: socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
        except OSError: time.sleep(0.05)
    return p, cd
def stop_host(p):
    if p.poll() is None: p.terminate()
    try: p.wait(30)
    except subprocess.TimeoutExpired: p.kill()

def chrome_targets(log):
    """page targets of the engine's Chrome (its private debug port is in the engine log)"""
    import json, urllib.request
    m = re.findall(r"ws://127\.0\.0\.1:(\d+)/devtools/page/", open(log).read())
    if not m: return None
    try: return [t for t in json.load(urllib.request.urlopen("http://127.0.0.1:%s/json/list" % m[-1], timeout=5)) if t.get("type") == "page"]
    except OSError: return None
def own_procs(marker):
    out = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit(): continue
        try: cmd = open("/proc/%s/cmdline" % pid, "rb").read().replace(b"\0", b" ").decode(errors="replace")
        except OSError: continue
        if marker in cmd: out.append((int(pid), cmd))
    return out

def m1_tests(check, skip, tmp, engine_sh):
    """M1: app mode with a local fake app; AmiSurf mode of the same engine unchanged"""
    home = os.path.join(tmp, "apphome"); os.makedirs(home, exist_ok=True)
    env = {"HOME": home, "XDG_DATA_HOME": os.path.join(home, "data")}
    pp = free_port(); srv = app_page.serve(pp); base = "http://127.0.0.1:%d" % pp
    ex = "env ASB_APP=amidiscord ASB_APP_URL=%s/app ASB_APP_ALLOW=127.0.0.1:%d ASB_APP_TITLE=AmiDiscord sh %s -port {port} -token {token} -cdp {port2} -idle 0" % (base, pp, engine_sh)
    port = free_port(); h, cd = start_host(os.path.join(tmp, "m1"), host_conf(ex), port, env=env)
    log = os.path.join(cd, "amidiscord.log"); state = os.path.join(home, "data", "amistudio-host", "amidiscord")
    try:
        c = Client(port); check(c.status == 0, "app mode: amidiscord service starts (ASB_APP)", c.msg)
        c.hello(); c.caps(8)   # OPENURL (M4: only with CAPS)
        first = c.pump(3)
        check(c.wait_title("^Fake App$") and c.url == base + "/app", "app mode: start URL loads by itself (ASB_APP_URL)", (c.title, c.url))
        check(not any(op == TABS for op, p in c.msgs), "app mode: no TABS message")
        check(any(op == META and s16(p, 1 + 2 + struct.unpack_from("<H", p, 1)[0])[0] == "AmiDiscord" for op, p in first) or "app mode 'amidiscord'" in open(log).read(),
              "app mode: fallback title until the page has one (ASB_APP_TITLE)")
        def click_case(x, y, want, label):
            n0 = len(c.openurls()); c.click(x, y); c.pump(4, lambda op, p: op == OPENURL)
            new = c.openurls()[n0:]; c.pump(0.5)
            check(new == [want] and c.url == base + "/app", label, (new, c.url))
        click_case(50, 30, "https://example.org/ext-link|0", "app mode: external link cancelled in Chrome, sent as OPENURL (reason 0), page stays")
        click_case(50, 90, "https://example.com/blank-link|1", "app mode: external target=_blank -> OPENURL (reason 1), no new tab")
        click_case(50, 210, "https://example.net/window-open|1", "app mode: window.open(external) -> OPENURL (reason 1)")
        click_case(50, 270, "https://example.net/later|1", "app mode: window.open('') then location=external -> OPENURL (reason 1)")
        c.click(50, 150); ok = c.wait_title("^Fake App 2$", 8)
        check(ok and c.url == base + "/app2", "app mode: internal target=_blank opens in the same page", (c.title, c.url))
        c.nav(base + "/app"); c.wait_title("^Fake App$", 10)
        c.nav("https://example.org/typed"); c.pump(2, lambda op, p: op == OPENURL)
        check(c.openurls()[-1:] == ["https://example.org/typed|2"] and c.url == base + "/app", "app mode: client NAV to a host not on ASB_APP_ALLOW refused (OPENURL reason 2)", c.openurls()[-1:])
        c.nav("asb://bookmarks"); c.send("TABNEW"); c.cmd("newtab https://example.org/"); c.cmd("page settings"); c.cmd("bookmark"); c.pump(2)
        tg = chrome_targets(log)
        check(tg is not None and len(tg) == 1 and c.url == base + "/app" and not any(op == TABS for op, p in c.msgs),
              "app mode: TABNEW, newtab, asb:// pages, bookmark ignored; Chrome has exactly one page (popups closed)", (tg and [t["url"] for t in tg], c.url))
        check("app mode: no asb:// pages" in open(log).read(), "app mode: no asb:// pages server")
        # password manager and history off
        c.nav(base + "/login"); c.wait_title("^Fake Login$", 10); c.pump(1); c.click(100, 90); c.pump(0.5)
        for ch in "secret": c.send("KEY", struct.pack("<BHBH", 2, 0, 0, 1) + ch.encode())
        c.send("KEY", struct.pack("<BHBH", 0, 13, 0, 0)); c.wait_title("^submitted$", 5)
        c.pump(2); cfg = os.path.join(state, "config")
        check(c.title == "submitted" and os.path.isdir(cfg) and not any(f in os.listdir(cfg) for f in ("key.bin", "passwords.dat", "history.tsv", "bookmarks.tsv")),
              "app mode: no password store, no history/bookmarks written (login form submitted)", (c.title, os.listdir(cfg) if os.path.isdir(cfg) else None))
        check(os.path.isdir(os.path.join(state, "profile")) and os.path.isdir(os.path.join(state, "downloads")),
              "app mode: own data folder ~/.local/share/amistudio-host/amidiscord (profile, config, downloads)", state)
        pr = [cmd for pid, cmd in own_procs(os.path.join(state, "profile")) if "--user-data-dir=" in cmd and "--type=" not in cmd]
        check(pr and "--mute-audio" in pr[0], "app mode: Chrome runs muted (Discord's own sounds off)", pr[:1])
        # reduced motion
        def measure(secs):
            c.pump(1.5); f0, b0 = c.frames, c.fbytes; c.pump(secs); return c.frames - f0, c.fbytes - b0
        c.nav(base + "/cssanim"); c.wait_title("^rm=", 10); f_css, b_css = measure(4)
        check(c.title == "rm=true" and f_css <= 2, "reduced motion: the page sees prefers-reduced-motion: reduce; CSS animation stops (%d frames in 4 s)" % f_css, c.title)
        c.nav(base + "/anim"); c.wait_title("^rm=", 10); f_on, b_on = measure(5)
        c.cmd("app reducedmotion 0"); c.nav(base + "/anim"); c.pump(1); c.wait_title("^rm=false", 10); f_off, b_off = measure(5)
        check(f_on <= 8 and b_off > 5 * max(b_on, 1), "reduced motion: animated page %d frames / %.1f MB in 5 s (off: %d frames / %.1f MB)" % (f_on, b_on / 1e6, f_off, b_off / 1e6))
        c.cmd("app reducedmotion 1"); c.cmd("app home"); c.wait_title("^Fake App$", 10)
        check(c.url == base + "/app", "app mode: cmd app home")
        c.close()
    finally:
        stop_host(h); srv.shutdown()
    time.sleep(1)
    check(not own_procs(os.path.join(state, "profile")), "app mode: engine and Chrome stop with the Host")
    # the same engine without ASB_APP: AmiSurf behaviour unchanged (tabs, asb:// pages)
    ex2 = "sh %s -port {port} -token {token} -cdp {port2} -idle 0" % engine_sh
    conf2 = "service browser\nproto 2\nexec %s\nhandshake asbc2\nready_timeout 40\nlog browser.log\n" % ex2
    port2 = free_port(); h2, cd2 = start_host(os.path.join(tmp, "m1b"), conf2, port2, env=env)
    try:
        c = Client(port2, "browser"); c.hello(); c.pump(3)
        check(any(op == TABS for op, p in c.msgs), "AmiSurf mode (no ASB_APP): TABS still sent")
        c.nav("asb://settings"); ok = c.wait_title("^Settings$", 15)
        check(ok, "AmiSurf mode: asb:// pages still work", c.title)
        c.close()
    finally: stop_host(h2)

def m2_tests(check, skip, tmp, d):
    """M2: per-service ASB_STATE + ASB_CONFIG via {state}, and a clean Chrome shutdown so a login (cookies,
    localStorage) survives Host restarts and engine idle exits; cmd app logout forgets it"""
    home = os.path.join(tmp, "m2home"); os.makedirs(home, exist_ok=True)
    data = os.path.join(tmp, "m2data")
    env = {"HOME": home}
    pp = free_port(); srv = app_page.serve(pp); base = "http://127.0.0.1:%d" % pp
    es = os.path.join(d, "engine", "engine.sh")
    ex = ("env ASB_APP=amidiscord ASB_APP_URL=%s/persist ASB_APP_ALLOW=127.0.0.1:%d ASB_STATE={state}/amidiscord "
          "ASB_CONFIG={state}/amidiscord/config sh \"%s\" -port {port} -token {token} -cdp {port2} -idle 0") % (base, pp, es)
    st = os.path.join(data, "amidiscord")
    port = free_port(); run = os.path.join(tmp, "m2")
    def session(fn, label):
        h, cd = start_host(run, host_conf(ex, "stop_timeout 20\n"), port, ["--data-dir", data], env=env)
        try:
            c = Client(port); ok = c.status == 0
            if ok: c.hello(); c.wait_title("^ls=", 30); fn(c); c.close()
            else: check(False, label, c.msg)
        finally:
            t0 = time.time(); stop_host(h); dt = time.time() - t0
        return cd, dt
    try:
        r = {}
        def s1(c):
            r["first"] = c.title; c.nav(base + "/persist?set=amiga42"); c.wait_title("ls=amiga42", 10); r["set"] = c.title
        cd, dt = session(s1, "M2: service starts")
        log = open(os.path.join(cd, "amidiscord.log")).read()
        check(r.get("first") == "ls=- ck=-" and r.get("set") == "ls=amiga42 ck=amiga42", "M2: fresh profile, the fake login is stored", r)
        check(os.path.isdir(os.path.join(st, "profile")) and os.path.isdir(os.path.join(st, "config")) and
              "state %s, config %s/config" % (st, st) in log,
              "M2: {state} placeholder: ASB_STATE and ASB_CONFIG are the service's own (--data-dir)", st)
        check("Chrome closed cleanly" in log, "M2: Host stop -> engine closes Chrome with Browser.close (%.1f s)" % dt,
              [l for l in log.splitlines() if "app:" in l][-3:])
        check(not own_procs(os.path.join(st, "profile")), "M2: no Chrome left after the Host stops")
        def s2(c): r["again"] = c.title
        session(s2, "M2: restart")
        check(r.get("again") == "ls=amiga42 ck=amiga42", "M2: login (cookie + localStorage) survives a Host restart", r.get("again"))
        # the engine exits by itself (SIGTERM to asb-chromed alone, as an idle exit would) and is restarted on demand
        h, cd = start_host(run, host_conf(ex), port, ["--data-dir", data], env=env)
        try:
            c = Client(port); c.hello(); c.wait_title("^ls=", 30); c.close()
            eng = [pid for pid, cmd in own_procs("-cdp") if "asb-chromed" in cmd and os.path.join(d, "engine") in cmd]
            for pid in eng: os.kill(pid, 15)
            for _ in range(100):
                if not own_procs(os.path.join(st, "profile")): break
                time.sleep(0.2)
            check(eng and not own_procs(os.path.join(st, "profile")), "M2: SIGTERM to the engine alone stops its Chrome", eng)
            time.sleep(1)
            c = Client(port); ok = c.status == 0
            if ok: c.hello(); c.wait_title("^ls=", 30)
            check(ok and c.title == "ls=amiga42 ck=amiga42", "M2: login survives an engine restart (restarted on demand)", (c.status, c.title))
            c.cmd("app logout"); c.pump(1); c.wait_title("^ls=", 10); c.pump(1)
            c.nav(base + "/persist"); c.pump(1); c.wait_title("^ls=", 10)
            check(c.title == "ls=- ck=-", "M2: cmd app logout clears cookies and storage of the app's origins", c.title)
            c.close()
        finally: stop_host(h)
        check(not os.path.exists(os.path.join(home, ".config", "amisurf")) and not os.path.exists(os.path.join(home, ".local", "share", "amistudio-host", "browser")),
              "M2: AmiSurf's data folders untouched")
    finally: srv.shutdown()

def log_wait(log, pat, secs=8, start=0):
    """wait for regex pat in the engine log after offset start; returns the match or None"""
    t = time.time() + secs
    while True:
        m = re.search(pat, open(log, errors="replace").read()[start:])
        if m or time.time() > t: return m
        time.sleep(0.1)
def log_len(log): return len(open(log, errors="replace").read())

def m3_tests(check, skip, tmp, engine_sh):
    """M3: notifications (Notification API shim), unread state from title/favicon, focus/visibility; local fake page"""
    home = os.path.join(tmp, "m3home"); os.makedirs(home, exist_ok=True)
    env = {"HOME": home, "XDG_DATA_HOME": os.path.join(home, "data")}
    pp = free_port(); srv = app_page.serve(pp); base = "http://127.0.0.1:%d" % pp
    ex = "env ASB_APP=amidiscord ASB_APP_URL=%s/notify ASB_APP_ALLOW=127.0.0.1:%d ASB_APP_REDUCEDMOTION=0 sh %s -port {port} -token {token} -cdp {port2} -idle 0" % (base, pp, engine_sh)
    port = free_port(); h, cd = start_host(os.path.join(tmp, "m3"), host_conf(ex), port, env=env)
    log = os.path.join(cd, "amidiscord.log")
    try:
        c = Client(port); check(c.status == 0, "M3: amidiscord service starts", c.msg)
        c.hello()
        ok = c.wait_title("^st ", 30)
        check(ok and "perm=granted" in c.title, "M3: Notification.permission is granted up front for the app's origin", c.title)
        c.wait_title("vis=visible.*foc=true", 5)
        check("vis=visible" in c.title and "foc=true" in c.title, "M3: with a client the page is focused and visible", c.title)
        c.cmd("app focus 0 1"); c.wait_title("foc=false", 5)
        check("foc=false" in c.title and "vis=visible" in c.title, "M3: focus 0 1 -> document.hasFocus() false, still visible", c.title)
        c.cmd("app focus 0 0"); c.wait_title("vis=hidden", 5)
        check("vis=hidden" in c.title and "hid=true" in c.title, "M3: focus 0 0 -> visibilityState hidden, document.hidden", c.title)
        c.cmd("app focus 1 1"); c.wait_title("vis=visible.*foc=true", 5)
        check("vis=visible" in c.title and "foc=true" in c.title, "M3: focus 1 1 -> focused and visible again (focus/visibilitychange events)", c.title)
        # notifications
        L = log_len(log); c.nav(base + "/notify?fire=2&close=1"); c.wait_title("^st ", 10)
        m = log_wait(log, r"notification (\d+) kind (\d+) \"Alice \(#general, Test Server\)\" tag \"t1\" in #general, Test Server icon 32x32", 8, L)
        check(m and m.group(2) == "1", "M3: new Notification() reported (kind mention, hint '#general, Test Server', 32x32 icon)", m and m.group(0))
        m2 = log_wait(log, r"notification (\d+) kind \d+ \"Alice[^\n]*tag \"t2\"", 4, L)
        check(m2 is not None, "M3: a second notification (tag t2)")
        cl = log_wait(log, r"notification (\d+) closed by the page", 5, L)
        check(cl and m and cl.group(1) == m.group(1), "M3: Notification.close() from the page reported", cl and cl.group(0))
        if m2:
            c.cmd("app notifyact %s 0" % re.search(r"notification (\d+)", m2.group(0)).group(1)); ok = c.wait_title("^clicked 2$", 5)
            check(ok, "M3: a client click runs the page's onclick (Discord would open the channel)", c.title)
        L = log_len(log); c.nav(base + "/notify?sw=1"); c.wait_title("^st ", 10)
        check(log_wait(log, r"notification \d+ kind 2 \"Bob\"", 6, L) is not None, "M3: ServiceWorkerRegistration.showNotification covered too (kind DM)")
        # notifications keep coming while the window is hidden; no frames then
        c.nav(base + "/notify?bg=1"); c.wait_title("^st ", 10); c.pump(1.5)
        L = log_len(log); c.cmd("app focus 0 0"); c.pump(1)
        f0 = c.frames; c.nav(base + "/anim"); c.pump(2); c.nav(base + "/notify?bg=1"); c.pump(2)
        hid_frames = c.frames - f0
        c.cmd("app focus 0 1"); c.pump(0.5); c.cmd("app focus 0 0"); c.pump(1)
        bg = log_wait(log, r"notification \d+ kind 2 \"bg \d+\"", 6, L)
        check(hid_frames == 0, "M3: hidden (iconified) -> no frames, even for an animated page (%d)" % hid_frames)
        check(bg is not None, "M3: notifications keep coming while hidden (the page notifies when away, like Discord)", bg and bg.group(0))
        f0 = c.frames; c.cmd("app focus 1 1"); c.pump(3, lambda op, p: op == FRAME)
        check(c.frames > f0, "M3: visible again -> screencast restarts with a full frame")
        # unread state from the title and the favicon
        def unread(t, fav, want, label):
            L = log_len(log); q = "/notify?t=" + urllib.parse.quote(t) + ("&fav=" + fav if fav else "")
            c.nav(base + q); m = log_wait(log, r"unread (\d+) mentions (\d+) flags (\d+)", 6, L)
            got = m and tuple(int(x) for x in m.groups())
            check(got == want, label, (t, fav, m and m.group(0)))
        unread("(3) Fake Discord", None, (3, 3, 0), "M3: title '(3) ...' -> unread 3")
        unread("Fake Discord", None, (0, 0, 0), "M3: plain title -> unread 0")
        unread("\u2022 Fake Discord", None, (0, 0, 1), "M3: title '• ...' -> unread without mention (flag 1)")
        unread("Fake Discord", "fav-badge", (0, 0, 2), "M3: favicon swapped -> badge (flag 2)")
        unread("(12) \u2022 Fake", None, (12, 12, 1), "M3: '(12) • ...' -> count 12 and the dot")
        # 0.1.5: logged-out pages (Discord's /login swaps its favicon too) never report unread state
        def unread_all(q, secs):
            L = log_len(log); c.nav(base + q); c.pump(secs)
            return [tuple(int(x) for x in m) for m in re.findall(r"unread (\d+) mentions (\d+) flags (\d+)", open(log, errors="replace").read()[L:])]
        unread("Fake Discord", None, (0, 0, 0), "M3: (reset to no unread)")
        u = unread_all("/login/fake?t=Discord&fav=fav-badge", 2.5)
        check(all(x == (0, 0, 0) for x in u), "M3: logged out (/login): favicon swap is not a badge (no flag 2)", u)
        u = unread_all("/login/fake?t=(3)%20Discord", 2.5)
        check(all(x == (0, 0, 0) for x in u), "M3: logged out (/login): no unread count either", u)
        u = unread_all("/login/fake?t=Discord&fav=fav-badge&push=/channels/%40me&push_t=Friends&fav2=fav-mention", 4)
        check(u and all(x[2] & 2 == 0 for x in u[:-1]) and u[-1] == (0, 0, 2),
              "M3: /login -> /channels (pushState): no false badge after login; a later favicon swap is a badge", u)
        unread("Fake Discord", None, (0, 0, 0), "M3: (back to no unread)")
        # origins not on the allowlist (an iframe from localhost) keep the real Notification
        L = log_len(log); c.nav(base + "/notify?ifr=1"); c.wait_title("^st ", 10); c.pump(3)
        txt = open(log, errors="replace").read()[L:]
        check(not re.search(r"notification \d+ kind", txt), "M3: the shim is only active on allowlisted origins (localhost iframe ignored)")
        # no client: unfocused + hidden, so the app notifies
        c.close(); time.sleep(1)
        check("focus 0 visible 0" in open(log, errors="replace").read()[-3000:], "M3: no client -> page unfocused and hidden")
        c = Client(port); c.hello(); ok = c.wait_title("vis=visible.*foc=true", 8); c.pump(3, lambda op, p: op == FRAME)
        check(ok and c.frames > 0, "M3: a new client starts focused and visible, frames again", c.title)
        c.close()
    finally:
        stop_host(h)
    # AmiSurf mode: no shim, no granted permission
    ex2 = "sh %s -port {port} -token {token} -cdp {port2} -idle 0" % engine_sh
    port2 = free_port()
    h2, cd2 = start_host(os.path.join(tmp, "m3b"), "service browser\nproto 2\nexec %s\nhandshake asbc2\nready_timeout 40\nlog browser.log\n" % ex2, port2, env=env)
    try:
        c = Client(port2, "browser"); c.hello(); c.nav(base + "/notify"); c.wait_title("^st ", 20)
        check(c.title.startswith("st ") and "perm=granted" not in c.title, "M3: AmiSurf mode (no ASB_APP): no notification shim, permission not granted", c.title)
        c.close()
    finally: stop_host(h2); srv.shutdown()

def parse_notify(p):
    id_, kind = struct.unpack_from("<IB", p, 0); o = 5; f = []
    for _ in range(5): v, o = s16(p, o); f.append(v)
    icon = None
    if p[o]: w, h = struct.unpack_from("<HH", p, o + 1); icon = (w, h, p[o + 5:o + 5 + w * h * 3])
    return dict(id=id_, kind=kind, title=f[0], body=f[1], tag=f[2], hint=f[3], url=f[4], icon=icon)
def fileput(c, req, flags, name, size, data):
    nb = name.encode()
    c.send("FILEPUT", struct.pack("<IBH", req, flags, len(nb)) + nb + struct.pack("<I", size) + data)
def put_file(c, req, name, data, last):
    for k in range(0, max(len(data), 1), 65536):
        ch = data[k:k + 65536]; end = k + 65536 >= len(data)
        fileput(c, req, 1 if (end and last) else 0, name if k == 0 else "", len(data), ch)

def m4_tests(check, skip, tmp, engine_sh):
    """M4: the capability-gated ASBC messages (CAPS/APPINFO, NOTIFY, NOTIFYCLOSE, NOTIFYACT, UNREAD, FOCUS, OPENURL,
    FILEREQ/FILEPUT/FILEDONE, DOWNLOAD, CLIP); without CAPS nothing new is sent"""
    home = os.path.join(tmp, "m4home"); os.makedirs(home, exist_ok=True)
    env = {"HOME": home, "XDG_DATA_HOME": os.path.join(home, "data")}
    pp = free_port(); srv = app_page.serve(pp); base = "http://127.0.0.1:%d" % pp
    ex = ("env ASB_APP=amidiscord ASB_APP_URL=%s/notify ASB_APP_ALLOW=127.0.0.1:%d ASB_APP_MAXUPLOAD=1 sh %s "
          "-port {port} -token {token} -cdp {port2} -idle 0") % (base, pp, engine_sh)
    port = free_port(); h, cd = start_host(os.path.join(tmp, "m4"), host_conf(ex), port, env=env)
    st = os.path.join(home, "data", "amistudio-host", "amidiscord")
    new_ops = (NOTIFY, NOTIFYCLOSE, UNREAD, OPENURL, FILEREQ, FILEDONE, DOWNLOAD, APPINFO)
    try:
        # no CAPS: an old client (or a plain viewer) sees nothing new
        c = Client(port); c.hello(); c.wait_title("^st ", 30)
        c.nav(base + "/notify?fire=1&t=(2)%20X"); c.pump(3); c.nav("https://example.org/x"); c.pump(1.5)
        check(not any(op in new_ops for op, p in c.msgs), "M4: without CAPS no new message types (no APPINFO/NOTIFY/UNREAD/OPENURL)",
              sorted(set(op for op, p in c.msgs)))
        check(any(op == STATUS and b"example.org/x" in p for op, p in c.msgs), "M4: without CAPS_OPENURL an external URL is shown as STATUS")
        c.cmd("app focus 1 1"); c.send("FOCUS", bytes([0, 0])); c.pump(1)
        c.nav(base + "/notify"); c.wait_title("^st ", 10); c.pump(0.6)
        check("vis=visible" in c.title, "M4: FOCUS is ignored without CAPS_FOCUS", c.title)
        c.nav(base + "/notify?t=(2)%20X"); c.wait_title(r"^\(2\) X$", 10); c.pump(1); c.close(); time.sleep(0.5)
        # with CAPS
        c = Client(port); c.hello(); c.caps(127)
        got = c.pump(3, lambda op, p: op == APPINFO)
        ai = [p for op, p in got if op == APPINFO]
        if ai:
            aid, o = s16(ai[0], 0); aurl, o = s16(ai[0], o); acaps = struct.unpack_from("<I", ai[0], o)[0]
        check(ai and aid == "amidiscord" and aurl == base + "/notify" and acaps == 63, "M4: CAPS -> APPINFO (app id, start URL, host caps 63; PASTEIMG not offered)",
              ai and (aid, aurl, acaps))
        amax = struct.unpack_from("<I", ai[0], o + 4)[0] if ai and len(ai[0]) >= o + 8 else None
        check(amax == 1024 * 1024 and len(ai[0]) == o + 8, "0.1.5: APPINFO ends with u32 max upload bytes (ASB_APP_MAXUPLOAD=1 -> 1048576)", amax)
        c.pump(1.5)
        ur = [struct.unpack("<HHB", p) for op, p in c.msgs if op == UNREAD]
        check(ur[:1] == [(2, 2, 0)], "M4: the current unread state right after CAPS (UNREAD 2)", ur)
        # notifications
        c.nav(base + "/notify?fire=2&close=1"); c.wait_title("^st ", 10)
        c.pump(4, lambda op, p: op == NOTIFYCLOSE)
        ns = [parse_notify(p) for op, p in c.msgs if op == NOTIFY]
        n1 = [n for n in ns if n["tag"] == "t1"]; n2 = [n for n in ns if n["tag"] == "t2"]
        ok = n1 and n1[0]["kind"] == 1 and n1[0]["title"] == "Alice (#general, Test Server)" and n1[0]["body"] == "hello 1 @amiga" \
            and n1[0]["hint"] == "#general, Test Server" and n1[0]["url"] == "/channels/1/1" and n1[0]["icon"] and n1[0]["icon"][:2] == (32, 32) \
            and n1[0]["icon"][2][:3] == bytes([200, 30, 40])
        check(ok, "M4: NOTIFY: id, kind mention, title, body, tag, hint, url, 32x32 RGB icon", n1 and dict(n1[0], icon=n1[0]["icon"] and n1[0]["icon"][:2]))
        cl = [struct.unpack("<I", p)[0] for op, p in c.msgs if op == NOTIFYCLOSE]
        check(n1 and cl[:1] == [n1[0]["id"]], "M4: NOTIFYCLOSE when the page closes it", cl)
        if n2:
            c.send("NOTIFYACT", struct.pack("<IB", n2[0]["id"], 0)); ok = c.wait_title("^clicked 2$", 5)
            check(ok, "M4: NOTIFYACT click runs the page's onclick", c.title)
        else: check(False, "M4: NOTIFYACT click runs the page's onclick", "no second NOTIFY")
        # FOCUS and notifications while hidden
        c.nav(base + "/notify?bg=1"); c.wait_title("^st ", 10); c.pump(1)
        c.send("FOCUS", bytes([0, 0])); f0 = c.frames; n0 = sum(1 for op, p in c.msgs if op == NOTIFY)
        c.pump(4, lambda op, p: op == NOTIFY)
        check(any(op == NOTIFY and parse_notify(p)["title"].startswith("bg ") for op, p in c.msgs[-50:]) and c.frames == f0,
              "M4: FOCUS 0 0 -> no frames, NOTIFY still arrives", (c.frames - f0))
        c.send("FOCUS", bytes([1, 1])); c.pump(3, lambda op, p: op == FRAME)
        check(c.frames > f0, "M4: FOCUS 1 1 -> frames again")
        # UNREAD: change debounce (<= 2/s) and the last value arrives
        u0 = sum(1 for op, p in c.msgs if op == UNREAD)
        c.nav(base + "/notify?spin=1&t=(1)%20Spin"); c.pump(4)
        us = [struct.unpack("<HHB", p) for op, p in c.msgs if op == UNREAD][u0:]
        check(us and us[-1][0] == 40 and len(us) <= 10, "M4: UNREAD debounced (%d messages for 40 title changes in 2 s), last = 40" % len(us), us)
        # OPENURL with the cap
        c.nav("https://example.org/capped"); c.pump(2, lambda op, p: op == OPENURL)
        check(c.openurls()[-1:] == ["https://example.org/capped|2"], "M4: with CAPS_OPENURL external URLs come as OPENURL", c.openurls())
        u1023 = "https://example.org/" + "a" * (1023 - 20)
        c.nav(u1023); c.pump(2, lambda op, p: op == OPENURL)
        check(c.openurls()[-1:] == [u1023 + "|2"], "0.1.5: OPENURL url of 1023 bytes (the limit) is sent whole", len(c.openurls()[-1]) - 2 if c.openurls() else None)
        n0 = len(c.openurls()); c.nav(u1023 + "b"); c.pump(2, lambda op, p: op == STATUS and b"too long" in p)
        check(len(c.openurls()) == n0 and any(op == STATUS and b"Link too long" in p and b"1024 bytes" in p for op, p in c.msgs[-20:]),
              "0.1.5: OPENURL url over 1023 bytes is not sent (never cut); STATUS 'Link too long' instead", c.openurls()[n0:])
        # clipboard writes from the page
        c.nav(base + "/clip"); c.wait_title("^clip$", 10); c.pump(0.5); c.click(50, 30)
        c.pump(3, lambda op, p: op == 36)
        check(any(op == 36 and p == b"hello clip" for op, p in c.msgs), "M4: navigator.clipboard.writeText -> CLIP")
        # downloads
        c.nav(base + "/dl"); c.wait_title("^dl$", 10); c.pump(0.5); c.click(50, 30)
        c.pump(8, lambda op, p: op == DOWNLOAD and p[0] != 0)
        ds = []
        for op, p in c.msgs:
            if op == DOWNLOAD: nm, o = s16(p, 1); sz = struct.unpack_from("<I", p, o)[0]; hp, o = s16(p, o + 4); ds.append((p[0], nm, sz, hp))
        done = [x for x in ds if x[0] == 1]
        check(ds[:1] and ds[0][0] == 0 and done and done[0][1] == "file.bin" and done[0][2] == 3000 and done[0][3] == os.path.join(st, "downloads", "file.bin")
              and open(done[0][3], "rb").read() == app_page.DLDATA, "M4: DOWNLOAD started/done with name, size and the exact Host path", ds)
        # uploads
        c.nav(base + "/upload"); c.wait_title("^upload$", 10); c.pump(0.5); c.click(100, 50)
        got = c.pump(5, lambda op, p: op == FILEREQ); fr = [p for op, p in got if op == FILEREQ]
        if fr: req = struct.unpack_from("<I", fr[0], 0)[0]; acc, _ = s16(fr[0], 5)
        check(fr and fr[0][4] == 1 and acc == ".txt,image/png", "M4: file chooser -> FILEREQ (multiple, accept)", fr and (fr[0][4], acc))
        if fr:
            a = os.urandom(100000); put_file(c, req, "a.txt", a, False); put_file(c, req, "b.png", b"\x89PNG" * 10, True)
            got = c.pump(5, lambda op, p: op == FILEDONE); fd = [p for op, p in got if op == FILEDONE]
            ok = c.wait_title("^files=", 5)
            check(fd and fd[0][4] == 0 and c.title == "files=2 a.txt:100000,b.png:40", "M4: FILEPUT (2 files, 64 KB chunks) -> FILEDONE 0, the page has them",
                  (fd and fd[0][4:], c.title))
            ups = os.listdir(os.path.join(st, "uploads"))
            check(len(ups) == 2 and all(n.startswith("%d-" % req) for n in ups), "M4: uploads kept in {state}/uploads", ups)
        c.nav(base + "/files"); c.wait_title("^upload$", 10); c.pump(0.5); c.click(100, 50)
        got = c.pump(5, lambda op, p: op == FILEREQ); fr = [p for op, p in got if op == FILEREQ]
        if fr:
            req = struct.unpack_from("<I", fr[0], 0)[0]; check(fr[0][4] == 0, "M4: single-file input -> FILEREQ flags 0")
            fileput(c, req, 0, "big.bin", 2 * 1024 * 1024, b"x" * 1000)
            got = c.pump(4, lambda op, p: op == FILEDONE); fd = [p for op, p in got if op == FILEDONE]
            check(fd and fd[0][4] == 1, "M4: over ASB_APP_MAXUPLOAD -> FILEDONE 1 (too big)", fd and (fd[0][4], s16(fd[0], 5)[0]))
            c.click(100, 50); got = c.pump(5, lambda op, p: op == FILEREQ); fr = [p for op, p in got if op == FILEREQ]
            if fr:
                req = struct.unpack_from("<I", fr[0], 0)[0]; fileput(c, req, 2, "", 0, b"")
                got = c.pump(4, lambda op, p: op == FILEDONE); fd = [p for op, p in got if op == FILEDONE]
                check(fd and fd[0][4] == 2 and s16(fd[0], 5)[0] == "cancelled", "M4: FILEPUT cancel -> FILEDONE 2", fd and fd[0][4:])
            else: check(False, "M4: FILEPUT cancel -> FILEDONE 2", "no FILEREQ")
            def filereq():
                c.click(100, 50); got = c.pump(5, lambda op, p: op == FILEREQ); fr = [p for op, p in got if op == FILEREQ]
                return struct.unpack_from("<I", fr[0], 0)[0] if fr else None
            def filedone():
                got = c.pump(4, lambda op, p: op == FILEDONE); fd = [p for op, p in got if op == FILEDONE]
                return fd and (struct.unpack_from("<I", fd[0], 0)[0], fd[0][4], s16(fd[0], 5)[0])
            req = filereq()
            if req is not None:
                c.send("FILEPUT", struct.pack("<IB", req, 2)); fd = filedone()
                check(fd == (req, 2, "cancelled"), "0.1.5: short FILEPUT cancel (u32 req + u8 flags=2, 5 bytes) -> FILEDONE 2", fd)
            else: check(False, "0.1.5: short FILEPUT cancel", "no FILEREQ")
            req = filereq()
            if req is not None:
                n_up = len(os.listdir(os.path.join(st, "uploads")))
                fileput(c, req, 0, "part.bin", 5000, b"y" * 1000); c.pump(0.5)
                c.send("FILEPUT", struct.pack("<IB", req, 2)); fd = filedone()
                check(fd == (req, 2, "cancelled") and len(os.listdir(os.path.join(st, "uploads"))) == n_up,
                      "0.1.5: short cancel in the middle of a file -> FILEDONE 2, partial upload removed", (fd, os.listdir(os.path.join(st, "uploads"))))
            req = filereq()
            if req is not None:
                c.send("FILEPUT", struct.pack("<IBH", req, 0, 3) + b"ab"); fd = filedone()
                check(fd and fd[0] == req and fd[1] == 2 and "too short" in fd[2], "0.1.5: truncated FILEPUT header (not a cancel) -> FILEDONE 2, no hang", fd)
            req = filereq()
            if req is not None:
                put_file(c, req, "ok.txt", b"after cancel", True); fd = filedone(); ok = c.wait_title("^files=", 5)
                check(fd and fd[1] == 0 and c.title == "files=1 ok.txt:12", "0.1.5: a normal upload still works after the cancels", (fd, c.title))
        else: check(False, "M4: single-file input -> FILEREQ flags 0", "no FILEREQ")
        # disconnected: the latest UNREAD is sent on reconnect
        c.nav(base + "/notify?t=(5)%20X&later=(7)%20X"); c.pump(1); c.close(); time.sleep(3.5)
        c = Client(port); c.hello(); c.caps(2); c.pump(2, lambda op, p: op == UNREAD)
        ur = [struct.unpack("<HHB", p) for op, p in c.msgs if op == UNREAD]
        check(ur[:1] == [(7, 7, 0)], "M4: reconnect -> the latest UNREAD (changed while disconnected)", ur)
        check(not any(op == NOTIFY for op, p in c.msgs), "M4: only the caps asked for (CAPS 2 = UNREAD only)")
        c.close()
    finally: stop_host(h)
    # AmiSurf mode: CAPS ignored
    ex2 = "sh %s -port {port} -token {token} -cdp {port2} -idle 0" % engine_sh
    port2 = free_port()
    h2, cd2 = start_host(os.path.join(tmp, "m4b"), "service browser\nproto 2\nexec %s\nhandshake asbc2\nready_timeout 40\nlog browser.log\n" % ex2, port2, env=env)
    try:
        c = Client(port2, "browser"); c.hello(); c.caps(127); c.pump(3)
        c.nav(base + "/notify?fire=1&t=(3)%20X"); c.pump(3)
        check(any(op == TABS for op, p in c.msgs) and not any(op in new_ops for op, p in c.msgs), "M4: AmiSurf mode: CAPS ignored, no new messages, TABS as before",
              sorted(set(op for op, p in c.msgs)))
        c.close()
    finally: stop_host(h2); srv.shutdown()

def dlpath_tests(check, skip, tmp, engine_sh):
    """0.1.5: one download path rule: <data>/<app id>/downloads, where <data> is the Host's {state} (--data-dir /
    data_dir, default ~/.local/share/amistudio-host), also when the exec line does not set ASB_STATE"""
    home = os.path.join(tmp, "dlhome"); os.makedirs(home, exist_ok=True); data = os.path.join(tmp, "dldata")
    env = {"HOME": home, "XDG_DATA_HOME": os.path.join(home, "xdg")}
    pp = free_port(); srv = app_page.serve(pp); base = "http://127.0.0.1:%d" % pp
    ex = ("env ASB_APP=amidiscord ASB_APP_URL=%s/dl ASB_APP_ALLOW=127.0.0.1:%d sh %s -port {port} -token {token} -cdp {port2} -idle 0") % (base, pp, engine_sh)
    for label, args, root in (("--data-dir", ["--data-dir", data], data), ("default", [], os.path.join(home, "xdg", "amistudio-host"))):
        port = free_port(); h, cd = start_host(os.path.join(tmp, "dl-" + label.strip("-")), host_conf(ex), port, args, env=env)
        try:
            c = Client(port); c.hello(); c.caps(32); c.wait_title("^dl$", 30); c.pump(0.5); c.click(50, 30)
            c.pump(8, lambda op, p: op == DOWNLOAD and p[0] == 1)
            done = []
            for op, p in c.msgs:
                if op == DOWNLOAD and p[0] == 1: nm, o = s16(p, 1); hp, o = s16(p, o + 4); done.append(hp)
            want = os.path.join(root, "amidiscord", "downloads", "file.bin")
            check(done[:1] == [want] and os.path.exists(want), "0.1.5: no ASB_STATE in exec, %s: DOWNLOAD path = <data>/amidiscord/downloads/NAME" % label, (done, want))
            c.close()
        finally: stop_host(h)
    srv.shutdown()

def frame_sizes(c, secs):
    """pump secs; Counter of full-frame sizes (pw, ph) seen"""
    import collections
    n0 = len(c.msgs); c.pump(secs)
    return collections.Counter(struct.unpack_from("<HH", p, 0) for op, p in c.msgs[n0:] if op == FRAME)
def red_width(c):
    """width in device pixels of /size's 100-CSS-px red box, from the newest raw (FMT 3, uncompressed) frame at 0,0"""
    for op, p in reversed(c.msgs):
        if op != FRAME: continue
        pw, ph, x, y, tw, th = struct.unpack_from("<HHHHHH", p, 0)
        if p[12] == 3 and p[14] == 0 and x == 0 and y == 0 and tw >= 200:
            px = p[15:]; n = 0
            while n < tw and px[n * 3] > 200 and px[n * 3 + 1] < 80: n += 1
            return n
    return None
def view_tests(check, skip, tmp, engine_sh):
    """0.1.6 view fixes, app mode and AmiSurf (browser) mode: RESIZE restarts the screencast at the new size; under zoom the
    frames are the view size with magnified content (no "re-applying viewport" loop); app mode: a new client starts at
    the default zoom, the same client reconnecting (takeover) keeps it"""
    home = os.path.join(tmp, "vhome"); os.makedirs(home, exist_ok=True)
    env = {"HOME": home, "XDG_DATA_HOME": os.path.join(home, "data"), "ASB_CONFIG": os.path.join(home, "asbcfg")}
    pp = free_port(); srv = app_page.serve(pp); base = "http://127.0.0.1:%d" % pp
    try:
        for mode in ("app", "browser"):
            tag = "view (%s mode)" % mode
            if mode == "app":
                ex = "env ASB_APP=amidiscord ASB_APP_URL=%s/size ASB_APP_ALLOW=127.0.0.1:%d sh %s -port {port} -token {token} -cdp {port2} -idle 0" % (base, pp, engine_sh)
                conf, svc = host_conf(ex), "amidiscord"
            else:
                ex = "sh %s -port {port} -token {token} -cdp {port2} -idle 0" % engine_sh
                conf, svc = "service browser\nproto 2\nexec %s\nhandshake asbc2\nready_timeout 40\nmax_clients 1\nlog browser.log\n" % ex, "browser"
            port = free_port(); h, cd = start_host(os.path.join(tmp, "v" + mode), conf, port, env=env)
            log = os.path.join(cd, svc + ".log")
            try:
                c = Client(port, svc, client="Viewer-A"); c.hello(640, 400, pf=1)
                if mode == "browser": c.nav(base + "/size")
                ok = c.wait_title("^w=640 h=400 dpr=1$", 40); fs = frame_sizes(c, 2)
                check(ok and set(fs) == {(640, 400)} and red_width(c) == 100, "%s: start 640x400, 1:1" % tag, (c.title, fs, red_width(c)))
                c.send("RESIZE", struct.pack("<HH", 1000, 700)); c.wait_title("w=1000 h=700", 8); fs = frame_sizes(c, 3)
                check(fs and set(fs) == {(1000, 700)}, "%s: RESIZE 640x400 -> 1000x700: every frame is 1000x700 (screencast restarted at the new size)" % tag, fs)
                c.send("FMT", bytes([1, 0]) + b"\0\0"); fs = frame_sizes(c, 2)
                check(fs and set(fs) == {(1000, 700)}, "%s: FMT re-sent after RESIZE still streams 1000x700" % tag, fs)
                L0 = log_len(log)
                c.cmd("zoom 200"); c.wait_title("dpr=2", 8); fs = frame_sizes(c, 4)
                check(c.title == "w=500 h=350 dpr=2" and fs and set(fs) == {(1000, 700)} and red_width(c) == 200,
                      "%s: zoom 200%%: page sees 500x350 CSS px, frames stay 1000x700 and are magnified (100 CSS px = 200 px)" % tag, (c.title, fs, red_width(c)))
                for z in (175, 125, 67):
                    c.cmd("zoom %d" % z); c.pump(1.5); fs = frame_sizes(c, 1.5)
                    check(fs and set(fs) == {(1000, 700)}, "%s: zoom %d%%: frames exactly the view size" % (tag, z), fs)
                c.cmd("zoom 200"); c.wait_title("dpr=2", 8); c.nav(base + "/size?still"); c.wait_title("dpr=2", 8); c.pump(2)
                n0 = c.frames; c.pump(4)
                check(c.frames - n0 <= 3, "%s: zoom 200%% on a static page: no frame loop (%d frames in 4 s)" % (tag, c.frames - n0))
                lg = open(log, errors="replace").read()[L0:]
                if mode == "app":
                    check("browser connection open" not in open(log, errors="replace").read(), "%s: AmiSurf 0.85's browser-wide connection (g_bcon) stays off in app mode" % tag)
                check("re-applying viewport" not in lg, "%s: no 'frame != view, re-applying viewport' log spam under zoom" % tag, lg.count("re-applying viewport"))
                if mode == "app":
                    c.close(); time.sleep(0.5)
                    c = Client(port, svc, client="Viewer-A"); c.hello(1000, 700, pf=1); c.pump(1); c.nav(base + "/size"); ok = c.wait_title("^w=", 15); c.pump(1)
                    check(c.status == 0 and c.title == "w=500 h=350 dpr=2" and log_wait(log, r"app: same client: zoom 200%"),
                          "%s: the same client reconnecting keeps its zoom (200%%)" % tag, c.title)
                    c2 = Client(port, svc, client="Viewer-A"); c2.hello(1000, 700, pf=1); c2.pump(1); c2.nav(base + "/size"); c2.wait_title("^w=", 15); c2.pump(1)
                    check(c2.status == 0 and c2.title == "w=500 h=350 dpr=2", "%s: a takeover by the same client keeps its zoom" % tag, c2.title)
                    c.close(); c2.close(); time.sleep(0.5)
                    c = Client(port, svc, client="Viewer-B"); c.hello(1000, 700, pf=1); c.pump(1); c.nav(base + "/size"); c.wait_title("^w=", 15); c.pump(1)
                    fs = frame_sizes(c, 1)
                    check(c.status == 0 and c.title == "w=1000 h=700 dpr=1" and log_wait(log, r"app: new client: zoom 100%"),
                          "%s: a different client starts at the default zoom (100%%), not the previous client's" % tag, c.title)
                    c.close()
                else:
                    c.close()
            finally: stop_host(h)
    finally: srv.shutdown()

def tabs_tests(check, skip, tmp, engine_sh):
    """AmiSurf 0.85 engine in browser mode (the Host's 'browser' service): tabs the page opens (target=_blank,
    window.open) show at once and come as TABS, window.open popups run (not blank), a popup that closes itself falls
    back to its opener, a fresh frame on tab switch, download status from the browser-wide CDP connection"""
    home = os.path.join(tmp, "thome"); os.makedirs(home, exist_ok=True); dl = os.path.join(tmp, "tdl"); os.makedirs(dl, exist_ok=True)
    env = {"HOME": home, "XDG_DATA_HOME": os.path.join(home, "data"), "ASB_CONFIG": os.path.join(home, "asbcfg"), "ASB_DOWNLOADS": dl}
    pp = free_port(); srv = app_page.serve(pp); base = "http://127.0.0.1:%d" % pp
    ex = "sh %s -port {port} -token {token} -cdp {port2} -idle 0" % engine_sh
    conf = "service browser\nproto 2\nexec %s\nhandshake asbc2\nready_timeout 40\nmax_clients 1\nlog browser.log\n" % ex
    port = free_port(); h, cd = start_host(os.path.join(tmp, "tabs"), conf, port, env=env); log = os.path.join(cd, "browser.log")
    def tabs(c):
        t = [p for op, p in c.msgs if op == TABS]; return (t[-1][0], t[-1][1]) if t else None
    def status(c, pat): return any(op == STATUS and re.search(pat, p.decode("utf-8", "replace")) for op, p in c.msgs)
    try:
        c = Client(port, "browser", client="Tabs-test"); c.hello(640, 400, pf=1); c.nav(base + "/tabs"); ok = c.wait_title("^tabs$", 40); c.pump(1.5)
        check(ok and log_wait(log, r"browser connection open"), "0.85 browser mode: browser-wide CDP connection open", c.title)
        c.click(50, 30); ok = c.wait_title("^w=640", 6); c.pump(1)
        check(ok and tabs(c) and tabs(c)[0] == 2 and tabs(c)[1] == 1 and status(c, "Opened in a new tab"),
              "0.85 browser mode: target=_blank link opens a tab that is shown at once (TABS 2 tabs, current 1)", (c.title, tabs(c)))
        c.send("TABSEL", bytes([0])); n0 = c.frames; ok = c.wait_title("^tabs$", 5); c.pump(2)
        check(ok and c.frames > n0, "0.85 browser mode: switching back to a static tab sends a fresh frame (%d)" % (c.frames - n0), c.title)
        c.click(50, 90); ok = c.wait_title("^popup ok", 6); c.pump(1)
        check(ok and c.title == "popup ok opener" and tabs(c) and tabs(c)[0] == 3, "0.85 browser mode: window.open() popup runs (not blank, has its opener) and is shown", (c.title, tabs(c)))
        c.send("TABSEL", bytes([0])); c.wait_title("^tabs$", 5); c.pump(1)
        c.click(50, 150); ok1 = c.wait_title("^closing$", 6); ok2 = c.wait_title("^tabs$", 8); c.pump(1)
        check(ok1 and ok2 and log_wait(log, r"shown tab closed by the page") and tabs(c) and tabs(c)[0] == 3,
              "0.85 browser mode: a popup that closes itself falls back to its opener", (c.title, tabs(c)))
        L0 = log_len(log); c.click(50, 210); c.pump(6, lambda op, p: op == STATUS and b"Downloaded file.bin" in p)
        f = os.path.join(dl, "file.bin"); lg = open(log, errors="replace").read()[L0:]
        check(status(c, r"^Downloading file\.bin to ") and status(c, r"^Downloaded file\.bin \(3 KB\) to ") and os.path.exists(f) and open(f, "rb").read() == app_page.DLDATA,
              "0.85 browser mode: download status (start, finished with the folder) and the file in ASB_DOWNLOADS", [p for op, p in c.msgs if op == STATUS][-4:])
        check(lg.count("download file.bin ->") == 1, "0.85 browser mode: one download event per download (browser + page connections)", lg.count("download file.bin ->"))
        c.close()
    finally:
        stop_host(h); srv.shutdown()

def discord_tests(check, skip, tmp, d):
    """the bundled 'amidiscord' service against the real discord.com login page: private HOME, ports and profile; never logs in"""
    try: socket.create_connection(("discord.com", 443), timeout=5).close()
    except OSError: skip("real discord.com login page", "offline"); return
    home = os.path.join(tmp, "dhome"); os.makedirs(home)
    env = dict(os.environ, HOME=home)
    for k in ("XDG_DATA_HOME", "ASB_STATE", "ASB_CONFIG", "DISPLAY", "AMISTUDIO_HOST_DIR"): env.pop(k, None)
    port = free_port(); cd = os.path.join(tmp, "dconf")
    p = subprocess.Popen([os.path.join(d, "amistudio-hostd"), "--port", str(port), "--confdir", cd, "--no-update-check", "-v"],
                         stdout=open(os.path.join(tmp, "d.out"), "w"), stderr=subprocess.STDOUT, env=env)
    try:
        for _ in range(100):
            try: socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
            except OSError: time.sleep(0.05)
        c = Client(port); check(c.status == 0, "bundled amidiscord service: starts from the release tarball", c.msg)
        c.hello(1024, 720); c.nav("https://discord.com/login")
        ok = c.wait_title("Discord", 60); c.pump(4)
        check(ok and "discord.com" in c.url and c.frames > 0, "real discord.com/login renders in app mode (title %r, %d frames)" % (c.title, c.frames), c.url)
        lg = open(os.path.join(cd, "amidiscord.log"), errors="replace").read()
        check("notification shim active on https://discord.com" in lg, "real discord.com: notification shim active (permission granted, no login)")
        f0, b0 = c.frames, c.fbytes; c.pump(10)
        check(c.fbytes - b0 < 30e6, "real discord.com/login with reduced motion: %d frames / %.1f MB in 10 s idle (prototype without it: a 2.2 MB frame every few hundred ms)" % (c.frames - f0, (c.fbytes - b0) / 1e6))
        c.close()
        st = os.path.join(home, ".local", "share", "amistudio-host", "amidiscord")
        check(os.path.isdir(os.path.join(st, "profile")) and os.path.isdir(os.path.join(st, "config")) and not os.path.exists(os.path.join(home, ".config", "amisurf")),
              "amidiscord data stays in its own folder: ASB_STATE and ASB_CONFIG = ~/.local/share/amistudio-host/amidiscord[/config] (AmiSurf's config untouched)")
    finally:
        p.terminate()
        try: p.wait(30)
        except subprocess.TimeoutExpired: p.kill()

CHROMEY = re.compile(r"chrom|pulse|scoped_dir|\.pki|fontconfig|crashpad|amistudio|asb", re.I)
def new_files(top, t0, prune=()):
    out = []
    for dp, dns, fns in os.walk(top):
        dns[:] = [x for x in dns if os.path.join(dp, x) not in prune]
        for n in dns + fns:
            q = os.path.join(dp, n)
            try:
                if os.lstat(q).st_mtime > t0: out.append(q)
            except OSError: pass
    return out
def selfcontained_tests(check, skip, tmp, d):
    """0.1.8: a self-contained install (the AmiStudio launcher's Host/ drawer on AmiKit): HOME, XDG_*, TMPDIR,
    --confdir and --data-dir all under one folder -> the Host, both bundled services and their Chromium write nothing
    outside it (no ~/.config, ~/.local/share, ~/.cache, ~/.pki, ~/Downloads, no Chromium files in /tmp)"""
    root = os.path.join(tmp, "selfcontained"); home = os.path.join(root, "home"); tdir = os.path.join(root, "tmp")
    for x in (home, tdir): os.makedirs(x, exist_ok=True)
    real_home = os.path.realpath(os.path.expanduser("~"))
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=os.path.join(home, ".config"), XDG_DATA_HOME=os.path.join(home, ".local", "share"),
               XDG_STATE_HOME=os.path.join(home, ".local", "state"), XDG_CACHE_HOME=os.path.join(home, ".cache"), TMPDIR=tdir,
               AMISTUDIO_HOST_DIR=os.path.join(root, "hostd"), ASB_DOWNLOADS=os.path.join(root, "Downloads"))
    for k in ("XDG_RUNTIME_DIR", "ASB_STATE", "ASB_CONFIG", "DISPLAY"): env.pop(k, None)
    tmp_before = set(os.listdir("/tmp")); t0 = time.time() - 1
    pp = free_port(); srv = app_page.serve(pp); base = "http://127.0.0.1:%d" % pp
    port = free_port()
    p = subprocess.Popen([os.path.join(d, "amistudio-hostd"), "run", "--port", str(port), "--confdir", os.path.join(root, "hostd"), "--data-dir", os.path.join(root, "services"),
                          "--no-update-check", "--no-discovery", "--listen", "127.0.0.1", "-v"], stdout=open(os.path.join(tmp, "sc.out"), "w"), stderr=subprocess.STDOUT, env=env)
    try:
        for _ in range(100):
            try: socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
            except OSError: time.sleep(0.05)
        c = Client(port, "browser", client="AmiSurf-sc"); ok = c.status == 0
        if ok:
            c.hello(); c.pump(3); c.nav(base + "/dl"); ok = c.wait_title("^dl$", 60); c.pump(0.5); c.click(50, 30)
            c.pump(8, lambda op, p: op == STATUS and b"Downloaded" in p); c.close()
        check(ok, "self-contained: browser service (AmiSurf) works", c.msg)
        c = Client(port, "amidiscord", client="AmiDiscord-sc"); ok = c.status == 0
        if ok: c.hello(); c.caps(127); c.pump(10); c.close()
        check(ok, "self-contained: amidiscord service (AmiDiscord) starts", c.msg)
    finally:
        p.terminate()
        try: p.wait(30)
        except subprocess.TimeoutExpired: p.kill()
        srv.shutdown()
    time.sleep(1)
    sv = os.path.join(root, "services")
    check(os.path.isdir(os.path.join(sv, "browser", "profile")) and os.path.isdir(os.path.join(sv, "amidiscord", "profile")) and os.path.isfile(os.path.join(root, "hostd", "host-id")),
          "self-contained: profiles in <root>/services/<service>, Host id and logs in <root>/hostd")
    check(os.path.isfile(os.path.join(root, "Downloads", "file.bin")), "self-contained: AmiSurf's download in ASB_DOWNLOADS (<root>/Downloads)")
    leak = [q for q in new_files(real_home, t0, prune=(os.path.join(real_home, "agent-data"), os.path.join(real_home, ".cursor"))) if CHROMEY.search(q)]
    # with the user's own PulseAudio passed through (as the launcher's run-host.sh does), that daemon updates its own
    # state (default sink/source, volumes) in the real ~/.config/pulse: the user's daemon, not a Host file
    upa = os.path.join(real_home, ".config", "pulse") + os.sep
    own = [q for q in leak if q.startswith(upa)] if env.get("PULSE_SERVER") else []
    leak = [q for q in leak if q not in own]
    check(not leak, "self-contained: nothing Chromium/Host-like written to the real ~" + (" (the user's PulseAudio updated its own state: %s)" % ", ".join(sorted(os.path.basename(q) for q in own)) if own else ""), leak[:5])
    tleak = [n for n in set(os.listdir("/tmp")) - tmp_before if CHROMEY.search(n) and not n.startswith(("apptest-", "ash-test"))]
    check(not tleak, "self-contained: no Chromium files in /tmp (TMPDIR honoured)", tleak[:5])
    inside = [x for x in (os.path.join(home, ".config", "chromium"), os.path.join(home, ".cache", "fontconfig"), os.path.join(home, ".config", "pulse")) if os.path.exists(x)]
    check(any(os.listdir(tdir)) or inside, "self-contained: what Chromium keeps in HOME/TMPDIR landed in <root> (%s)" % ", ".join(os.path.relpath(x, root) for x in inside + [tdir]))

if __name__ == "__main__":
    OK = [0, 0]
    def check(c, name, extra=""):
        OK[0 if c else 1] += 1; print(("ok   " if c else "FAIL ") + name + ("" if c or not extra else "  -- " + str(extra)), flush=True)
    def skip(n, why): print("skip %s (%s)" % (n, why))
    tmp = tempfile.mkdtemp(prefix="apptest-")
    import pulse_guard
    pulse_guard.setup(tmp)
    eng = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "engine", "stage", "engine", "engine.sh")
    if os.environ.get("ONLY") is None: m1_tests(check, skip, tmp, eng)
    if os.environ.get("ONLY") in (None, "m2"): m2_tests(check, skip, tmp, os.path.dirname(os.path.dirname(eng)))
    if os.environ.get("ONLY") in (None, "m3"): m3_tests(check, skip, tmp, eng)
    if os.environ.get("ONLY") in (None, "m4"): m4_tests(check, skip, tmp, eng)
    if os.environ.get("ONLY") in (None, "m4", "dl"): dlpath_tests(check, skip, tmp, eng)
    if os.environ.get("ONLY") in (None, "view"): view_tests(check, skip, tmp, eng)
    if os.environ.get("ONLY") in (None, "tabs"): tabs_tests(check, skip, tmp, eng)
    if os.environ.get("ONLY") in (None, "sc"): selfcontained_tests(check, skip, tmp, os.path.dirname(os.path.dirname(eng)))
    n = pulse_guard.reap()
    check(not pulse_guard.ours(), "no PulseAudio daemon left behind under the test folder (%d stopped)" % n)
    print("%d ok, %d failed (logs in %s)" % (OK[0], OK[1], tmp))
