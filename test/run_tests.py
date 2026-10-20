#!/usr/bin/env python3
"""AmiStudio Host integration tests: `make test` (or python3 test/run_tests.py).
Starts build/amistudio-hostd on spare ports with a throw-away config dir and talks ASH v1 to it with an independent
Python client: handshake errors, hostinfo, pairing + tokens + revoke, LAN auth, ban, discovery, fetch (local HTTPS
server with a test CA, plus GitHub when online), the child-process service plugin (fake ASBC backend: on-demand
start, restart after a crash, compat port, busy), shutdown and (0.1.8) amidrive against an rclone alias remote
(test/amidrive_tests.py; needs tools/fetch-rclone.sh and FUSE). With ASH_TEST_BROWSER=1 it also mounts the real
asb-chromed from ../amistudio-browser/build/host (read only) and checks that a page frame arrives."""
import hashlib, http.server, os, re, shutil, signal, socket, ssl, struct, subprocess, sys, tempfile, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "test"))
HOSTD = os.environ.get("ASH_HOSTD") or os.path.join(ROOT, "build", "amistudio-hostd")
CLI = os.path.join(ROOT, "build", "ash-cli")
HOSTVER = re.search(r'HOST_VERSION "([^"]+)"', open(os.path.join(ROOT, "src", "host.h")).read()).group(1)
OK = FAIL = SKIP = 0
def check(cond, name, extra=""):
    global OK, FAIL
    if cond: OK += 1; print("ok   " + name)
    else: FAIL += 1; print("FAIL " + name + ("  -- " + str(extra) if extra else ""))
def skip(name, why): global SKIP; SKIP += 1; print("skip %s (%s)" % (name, why))

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p
def lan_ip():
    for cand in subprocess.run(["hostname", "-I"], capture_output=True, text=True).stdout.split():
        if not cand.startswith("127.") and ":" not in cand: return cand
    return None

# ---------- ASH client ----------
def rd(s, n):
    b = b""
    while len(b) < n:
        k = s.recv(n - len(b))
        if not k: raise EOFError("closed after %d of %d" % (len(b), n))
        b += k
    return b
def hs(addr, port, service, ver=1, token="", client="pytest", raw=None, timeout=10):
    s = socket.create_connection((addr, port), timeout=timeout)
    if addr != "127.0.0.1": pass
    pkt = raw if raw is not None else (b"ASH1" + struct.pack("<H", 1) + bytes([len(service)]) + service.encode() + struct.pack("<H", ver)
                                        + bytes([len(token)]) + token.encode() + bytes([len(client)]) + client.encode())
    s.sendall(pkt)
    try: h = rd(s, 10)
    except (EOFError, ConnectionResetError): s.close(); return None, None, None, None
    assert h[:4] == b"ASH1", h
    st, sv, ml = h[6], struct.unpack("<H", h[7:9])[0], h[9]
    msg = rd(s, ml).decode() if ml else ""
    return s, st, sv, msg
def lan_conn(ip, port):
    """connect from the LAN address so the Host sees a non-loopback peer"""
    s = socket.socket(); s.bind((ip, 0)); s.settimeout(10); s.connect((ip, port)); return s
def hs_from(ip, port, service, ver=1, token=""):
    s = lan_conn(ip, port)
    s.sendall(b"ASH1" + struct.pack("<H", 1) + bytes([len(service)]) + service.encode() + struct.pack("<H", ver) + bytes([len(token)]) + token.encode() + b"\x06pytest")
    try: h = rd(s, 10)
    except (EOFError, ConnectionResetError): s.close(); return None, None, ""
    msg = rd(s, h[9]).decode() if h[9] else ""
    return s, h[6], msg
def send_frame(s, op, payload=b""): s.sendall(struct.pack("<I", len(payload) + 1) + bytes([op]) + payload)
def recv_frame(s):
    n = struct.unpack("<I", rd(s, 4))[0]; op = rd(s, 1)[0]; return op, rd(s, n - 1)
def hostinfo(addr, port, token="", sock=None):
    s, st, _, msg = (hs(addr, port, "hostinfo", 1, token) if sock is None else sock)
    if st != 0: return None
    op, p = recv_frame(s); s.close()
    return dict((l.split(" ", 1) + [""])[:2] for l in p.decode().splitlines() if l and not l.startswith("service ")), \
           [l.split(" ", 1)[1] for l in p.decode().splitlines() if l.startswith("service ")]
def ctl(sock, op, payload=b""):
    """control service: send QUIT_WHEN_UNUSED (1) / QUIT_CANCEL (2) / QUIT_STATUS (3), return QUIT_STATE (state, others, grace)"""
    send_frame(sock, op, payload); rop, p = recv_frame(sock); assert rop == 16 and len(p) == 5, (rop, p)
    return (p[0],) + struct.unpack("<HH", p[1:5])
def pair(sock, code, name="pytest"):
    send_frame(sock, 1, ("%s\n%s" % (code, name)).encode()); op, p = recv_frame(sock); sock.close(); return op, p.decode()
def fetch(sock, url, size=None, sha=None):
    req = url + ("\n%s" % size if size is not None else "") + ("\n%s" % sha if sha else "")
    send_frame(sock, 1, req.encode()); data = b""; head = None
    while True:
        op, p = recv_frame(sock)
        if op == 2: head = struct.unpack("<IH", p)
        elif op == 3: data += p
        elif op == 4: return "END", head, data, p[:64].decode(), struct.unpack("<I", p[64:68])[0]
        elif op == 5: return "ERR", head, data, p.decode(), None

# ---------- Host process ----------
class Host:
    def __init__(self, args, confdir, conf=None, env=None, upd=False):
        self.confdir = confdir; os.makedirs(confdir, exist_ok=True)
        if conf is not None: open(os.path.join(confdir, "host.conf"), "w").write(conf)
        self.log = os.path.join(confdir, "hostd.out")
        e = dict(os.environ, AMISTUDIO_HOST_DIR=confdir); e.update(env or {})
        self.p = subprocess.Popen([HOSTD] + args + ([] if upd else ["--no-update-check"]) + ["-v"], stdout=open(self.log, "w"), stderr=subprocess.STDOUT, env=e)
        self.env = e
    def wait_port(self, port, addr="127.0.0.1"):
        for _ in range(100):
            try: socket.create_connection((addr, port), timeout=0.2).close(); return True
            except OSError: time.sleep(0.05)
        return False
    def out(self): return open(self.log).read()
    def cli(self, *a): return subprocess.run([HOSTD] + list(a), capture_output=True, text=True, env=self.env)
    def stop(self):
        if self.p.poll() is None: self.p.send_signal(signal.SIGTERM)
        try: return self.p.wait(10)
        except subprocess.TimeoutExpired: self.p.kill(); return None

# ---------- local HTTPS server with a test CA ----------
def make_ca(d):
    def run(*a): subprocess.run(a, check=True, capture_output=True)
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/CN=AmiStudio Test CA", "-keyout", d + "/ca.key", "-out", d + "/ca.pem",
        "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    run("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=localhost", "-keyout", d + "/srv.key", "-out", d + "/srv.csr")
    open(d + "/ext", "w").write("subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\nextendedKeyUsage=serverAuth\n")
    run("openssl", "x509", "-req", "-in", d + "/srv.csr", "-CA", d + "/ca.pem", "-CAkey", d + "/ca.key", "-CAcreateserial", "-days", "2", "-out", d + "/srv.pem", "-extfile", d + "/ext")
    # a second, untrusted self-signed cert
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost", "-keyout", d + "/bad.key", "-out", d + "/bad.pem")
BLOB = os.urandom(300 * 1024 + 123)
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_GET(self):
        try: self.route()
        except (ConnectionError, ssl.SSLError, BrokenPipeError): pass        # the Host aborted on purpose (size checks)
    def route(self):
        if self.path == "/blob.lha":
            self.send_response(200); self.send_header("Content-Length", str(len(BLOB))); self.end_headers(); self.wfile.write(BLOB)
        elif self.path == "/chunked":
            self.send_response(200); self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
            for i in range(0, len(BLOB), 7000): c = BLOB[i:i+7000]; self.wfile.write(b"%x\r\n" % len(c) + c + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        elif self.path == "/manifest.txt":
            m = b"format 1\nupdated 2026-10-17\n\napp amisurf\nver 9.0\nfile AmiSurf-9.0.lha\n\ncomponent host\nname AmiStudio Host\nver 9.9\nplatform linux-x86_64\n"
            self.send_response(200); self.send_header("Content-Length", str(len(m))); self.end_headers(); self.wfile.write(m)
        elif self.path == "/redir":
            self.send_response(302); self.send_header("Location", "/blob.lha"); self.send_header("Content-Length", "0"); self.end_headers()
        elif self.path == "/redir-evil":
            self.send_response(302); self.send_header("Location", "https://evil.example/x"); self.send_header("Content-Length", "0"); self.end_headers()
        else:
            self.send_response(404); self.send_header("Content-Length", "3"); self.end_headers(); self.wfile.write(b"no\n")
def https_server(d, cert, key):
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(d + "/" + cert, d + "/" + key)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv

def main():
    if not os.path.exists(HOSTD): sys.exit("build first: make")
    tmp = tempfile.mkdtemp(prefix="ash-test-")
    import pulse_guard
    psrv = pulse_guard.setup(tmp)
    print("PulseAudio for the test engines: %s" % (psrv or "none found (libpulse autospawn off; any daemon the engine starts is stopped at the end)"))
    ip = lan_ip()
    make_ca(tmp)
    srv = https_server(tmp, "srv.pem", "srv.key"); sport = srv.server_address[1]
    bad = https_server(tmp, "bad.pem", "bad.key"); bport = bad.server_address[1]
    port, cport = free_port(), free_port()
    pidfile = os.path.join(tmp, "fake.pids")
    conf = """# test config
name testhost
lan yes
fetch_allow localhost github.com raw.githubusercontent.com objects.githubusercontent.com release-assets.githubusercontent.com
fetch_max_mb 1
cafile %s/ca.pem

service echo
proto 2
exec %s %s/test/fake_backend.py -port {port} -token {token} -idle 0
handshake asbc2
compat_port %d
ready_timeout 10

service ranged
proto 2 3
exec %s %s/test/fake_backend.py -port {port}
handshake none
max_clients 1
""" % (tmp, sys.executable, ROOT, cport, sys.executable, ROOT)
    h = Host(["--port", str(port)], os.path.join(tmp, "conf"), conf, env={"FAKE_PIDFILE": pidfile})
    try:
        check(h.wait_port(port), "host listens on its port")
        out = h.out()
        m = re.search(r"pairing code (\d{6})", out)
        check(m is not None, "LAN mode with nothing paired logs a pairing code")
        check(os.stat(h.confdir).st_mode & 0o777 == 0o700, "config dir is 0700")
        # --- handshake ---
        s, st, sv, msg = hs("127.0.0.1", port, "nosuch"); check(st == 2, "unknown service -> status 2", msg)
        s, st, sv, msg = hs("127.0.0.1", port, "fetch", 9); check(st == 4 and sv == 1, "service version too new -> 4 + highest version", (st, sv))
        s, st, sv, msg = hs("127.0.0.1", port, "ranged", 1); check(st == 3 and sv == 2, "service version too old -> 3 + lowest version", (st, sv))
        s, st, sv, msg = hs("127.0.0.1", port, "", raw=b"GET / HTTP/1.0\r\n\r\n"); check(st is None, "non-ASH client is just closed")
        s, st, sv, msg = hs("127.0.0.1", port, "", raw=b"ASH1" + struct.pack("<H", 2) + b"\x08hostinfo\x01\x00\x00\x00"); check(st == 4, "ASH version 2 -> too new", st)
        # --- hostinfo ---
        info, svcs = hostinfo("127.0.0.1", port)
        check(info["host"] == HOSTVER and info["name"] == "testhost" and info["paired"] == "1" and info["lan"] == "1", "hostinfo over loopback (paired 1, name, lan)", info)
        check("echo 2 idle" in svcs and "fetch 1 up" in svcs and "ranged 3 idle" in svcs, "hostinfo lists services with state", svcs)
        r = subprocess.run([CLI, "-p", str(port), "info"], capture_output=True, text=True); check("service echo 2" in r.stdout, "C client library (ash-cli info) works")
        r = h.cli("status", "127.0.0.1:%d" % port); check("name testhost" in r.stdout, "amistudio-hostd status")
        # --- fetch (loopback, no token needed) ---
        good = hashlib.sha256(BLOB).hexdigest()
        r = fetch(hs("127.0.0.1", port, "fetch")[0], "https://localhost:%d/blob.lha" % sport, len(BLOB), good)
        check(r[0] == "END" and r[2] == BLOB and r[3] == good and r[4] == len(BLOB) and r[1] == (len(BLOB), 200), "fetch: body, HEAD size, END sha256 match", r[0:2] + r[3:])
        s = hs("127.0.0.1", port, "fetch")[0]
        r1 = fetch(s, "https://localhost:%d/chunked" % sport); r2 = fetch(s, "https://localhost:%d/redir" % sport); s.close()
        check(r1[0] == "END" and r1[2] == BLOB and r1[1][0] == 0xFFFFFFFF, "fetch: chunked body, size unknown in HEAD")
        check(r2[0] == "END" and r2[2] == BLOB, "fetch: redirect followed, two requests on one connection")
        r = fetch(hs("127.0.0.1", port, "fetch")[0], "https://localhost:%d/blob.lha" % sport, len(BLOB) + 1); check(r[0] == "ERR" and "size" in r[3] and not r[2], "fetch: size mismatch refused before any data", r[3])
        r = fetch(hs("127.0.0.1", port, "fetch")[0], "https://localhost:%d/blob.lha" % sport, len(BLOB), "0" * 64); check(r[0] == "ERR" and "SHA-256" in r[3], "fetch: SHA-256 mismatch -> ERR", r[3])
        r = fetch(hs("127.0.0.1", port, "fetch")[0], "https://localhost:%d/nothere" % sport); check(r[0] == "ERR" and "404" in r[3], "fetch: HTTP 404 -> ERR", r[3])
        r = fetch(hs("127.0.0.1", port, "fetch")[0], "https://localhost:%d/redir-evil" % sport); check(r[0] == "ERR" and "evil.example" in r[3], "fetch: redirect to a host not on fetch_allow refused", r[3])
        r = fetch(hs("127.0.0.1", port, "fetch")[0], "https://example.com/"); check(r[0] == "ERR" and "fetch list" in r[3], "fetch: host not on fetch_allow refused", r[3])
        r = fetch(hs("127.0.0.1", port, "fetch")[0], "http://localhost:%d/blob.lha" % sport); check(r[0] == "ERR" and "https" in r[3], "fetch: plain http refused", r[3])
        r = fetch(hs("127.0.0.1", port, "fetch")[0], "https://localhost:%d/blob.lha" % bport); check(r[0] == "ERR" and "TLS" in r[3], "fetch: untrusted certificate refused", r[3])
        r = fetch(hs("127.0.0.1", port, "fetch")[0], "https://localhost:%d/blob.lha\r\nX: y" % sport); check(r[0] == "ERR", "fetch: header injection in URL refused", r[3])
        # online: GitHub raw + a release asset through the 302 to release-assets.githubusercontent.com
        try:
            socket.create_connection(("raw.githubusercontent.com", 443), timeout=5).close(); online = True
        except OSError: online = False
        if online:
            r = fetch(hs("127.0.0.1", port, "fetch")[0], "https://raw.githubusercontent.com/hardcopi/AmiStudio-Releases/main/manifest.txt")
            check(r[0] == "END" and r[2].startswith(b"# AmiStudio manifest") and b"format 1" in r[2], "fetch: live AmiStudio-Releases manifest.txt from GitHub", r[3])
            url = "https://github.com/jens-maus/yam/releases/download/2.9p1/YAM29p1-AmigaOS3.lha"
            r = fetch(hs("127.0.0.1", port, "fetch")[0], url)
            if r[0] == "END": check(len(r[2]) > 100000 and r[2][2:7] == b"-lh5-" or r[2][2:5] == b"-lh", "fetch: real GitHub release asset via 302 redirect (YAM 2.9p1 .lha, %d bytes)" % len(r[2]))
            elif "larger" in r[3]: check(True, "fetch: GitHub release redirect followed; fetch_max_mb limit enforced (%s)" % r[3])
            else: check(False, "fetch: GitHub release asset", r[3])
        else: skip("fetch: live GitHub", "offline")
        # --- service plugin: child backend ---
        s, st, sv, msg = hs("127.0.0.1", port, "echo", 2)
        check(st == 0, "echo service: backend started on demand, ASH OK", msg)
        data = os.urandom(1024 * 1024)
        def pump():
            s.sendall(data)
        t = threading.Thread(target=pump); t.start()
        got = rd(s, len(data)); t.join(); s.close()
        check(got == data, "echo service: 1 MB passes through unchanged (Host did the ASBC handshake with the internal token)")
        info, svcs = hostinfo("127.0.0.1", port); check("echo 2 up" in svcs, "hostinfo shows the backend up", svcs)
        pids = [int(x) for x in open(pidfile).read().split()]
        os.kill(pids[-1], signal.SIGKILL)
        def zombie(pid):
            try: return "zombie" in open("/proc/%d/status" % pid).read().lower()
            except OSError: return False
        def gone(pid): return not os.path.exists("/proc/%d" % pid)
        end = time.time() + 4
        while time.time() < end and not gone(pids[-1]): time.sleep(0.1)
        check(gone(pids[-1]), "reaper: killed backend is reaped within seconds, no zombie, before any new connection", "zombie" if zombie(pids[-1]) else "still there")
        check(re.search(r"echo: backend exited signal 9 after \d+ s; it starts again for the next client", h.out()) is not None, "reaper: backend exit logged with signal")
        info, svcs = hostinfo("127.0.0.1", port); check("echo 2 idle exited signal 9" in svcs, "reaper: hostinfo shows 'idle exited signal 9' (restarts on demand)", svcs)
        check(len(open(pidfile).read().split()) == 1, "reaper does not restart an on-demand backend by itself")
        s, st, sv, msg = hs("127.0.0.1", port, "echo", 2); s and s.sendall(b"again")
        check(st == 0 and rd(s, 5) == b"again", "echo service: backend restarted after it died", msg); s.close()
        check(len(open(pidfile).read().split()) == 2, "exactly one restart")
        # compat port: plain ASBC from loopback
        c = socket.create_connection(("127.0.0.1", cport), timeout=10); c.sendall(b"ASBC\x02\x00\x04abcd"); a = rd(c, 7); c.sendall(b"ping")
        check(a == b"ASBC\x02\x00\x00" and rd(c, 4) == b"ping", "compat port 9223-style: loopback ASBC client works (token swapped for the internal one)"); c.close()
        # busy
        s1, st1, _, _ = hs("127.0.0.1", port, "ranged", 3, client="amiga-A"); s2, st2, _, m2 = hs("127.0.0.1", port, "ranged", 2, client="amiga-B")
        check(st1 == 0 and st2 == 5, "max_clients 1: a different client gets status 5 (busy)", (st1, st2, m2)); s1.close()
        time.sleep(0.2); s3, st3, _, _ = hs("127.0.0.1", port, "ranged", 2); check(st3 == 0, "busy clears when the first client leaves"); s3.close()
        # 0.1.4: a client that dropped without closing (Amiga reset, cable pulled) keeps its slot; when the same client
        # reconnects, its stale session is kicked and the new one takes over
        time.sleep(0.2)
        def raw_asbc(x): x.sendall(b"ASBC\x03\x00\x00"); return rd(x, 7)   # "ranged" is handshake none: talk to fake_backend ourselves
        s1, st1, _, _ = hs("127.0.0.1", port, "ranged", 3, client="AmiDiscord/0.1 68040"); raw_asbc(s1); s1.sendall(b"one"); e1 = rd(s1, 3)
        s2, st2, _, m2 = hs("127.0.0.1", port, "ranged", 3, client="AmiDiscord/0.1 68040")
        s1.settimeout(3)
        try: dead = s1.recv(10) == b""
        except OSError: dead = True
        s2 and raw_asbc(s2) and s2.sendall(b"two")
        check(e1 == b"one" and st2 == 0 and rd(s2, 3) == b"two", "max_clients 1: the same client reconnecting takes over the slot (status 0, works)", (st2, m2))
        check(dead, "the stale session of that client is closed by the Host")
        check("ranged reconnect replaces its stale session" in h.out(), "takeover is logged")
        s4, st4, _, _ = hs("127.0.0.1", port, "ranged", 3, client="amiga-B"); check(st4 == 5, "a different client still gets busy while the new session runs", st4)
        s5, st5, _, _ = hs("127.0.0.1", port, "ranged", 3, client="AmiDiscord/0.1 68040"); s6, st6, _, _ = hs("127.0.0.1", port, "ranged", 3, client="AmiDiscord/0.1 68040")
        s6 and raw_asbc(s6) and s6.sendall(b"3rd")
        check(st5 == 0 and st6 == 0 and rd(s6, 3) == b"3rd", "back-to-back reconnects of the same client each take over")
        for x in (s1, s2, s5, s6):
            try: x and x.close()
            except OSError: pass
        # --- LAN clients ---
        if not ip: skip("LAN tests", "no LAN address")
        else:
            s, st, msg = hs_from(ip, port, "fetch"); check(st == 1, "LAN: fetch without a token -> 1 (pair first)", msg)
            s, st, msg = hs_from(ip, port, "echo", 2); check(st == 1, "LAN: backend service without a token -> 1")
            s, st, msg = hs_from(ip, port, "hostinfo"); op, p = recv_frame(s); check(st == 0 and "paired 0" in p.decode(), "LAN: hostinfo works unpaired and says paired 0")
            s, st, msg = hs_from(ip, port, "pair"); op, p = pair(s, "000000" if m.group(1) != "000000" else "111111"); check(op == 3 and "Wrong code" in p, "LAN: wrong pairing code refused", p)
            r = h.cli("pair", "--minutes", "5"); code = re.search(r"(\d{3}) (\d{3})", r.stdout); code = code.group(1) + code.group(2)
            check(r.returncode == 0, "CLI 'pair' prints a new 6-digit code")
            s, st, msg = hs_from(ip, port, "pair"); op, tok = pair(s, code[:3] + " " + code[3:], "A4000 test")
            check(op == 2 and re.match(r"^[0-9a-f]{64}$", tok), "LAN: correct code -> 64-hex token", tok)
            s, st, msg = hs_from(ip, port, "pair"); op, p = pair(s, code); check(op == 3, "pairing code works only once", p)
            tf = open(os.path.join(h.confdir, "tokens")).read()
            check(tok not in tf and hashlib.sha256(tok.encode()).hexdigest() in tf and "A4000 test" in tf, "tokens file keeps only the SHA-256 of the token")
            check(os.stat(os.path.join(h.confdir, "tokens")).st_mode & 0o777 == 0o600, "tokens file is 0600")
            s, st, msg = hs_from(ip, port, "fetch", 1, tok); check(st == 0, "LAN: fetch with the paired token -> OK", msg)
            r = fetch(s, "https://localhost:%d/blob.lha" % sport, len(BLOB), good); check(r[0] == "END" and r[2] == BLOB, "LAN: fetch through the Host works")
            s, st, msg = hs_from(ip, port, "hostinfo", 1, tok); op, p = recv_frame(s); check("paired 1" in p.decode(), "LAN: hostinfo with token says paired 1")
            s, st, msg = hs_from(ip, port, "echo", 2, tok); s.sendall(b"lan!"); check(st == 0 and rd(s, 4) == b"lan!", "LAN: backend service with token"); s.close()
            s, st, msg = hs_from(ip, port, "control"); check(st == 1, "LAN: control (quit-when-unused) without a token -> 1", msg)
            s, st, msg = hs_from(ip, port, "control", 1, tok); st2 = ctl(s, 3) if st == 0 else None
            check(st == 0 and st2 == (0, 0, 10), "LAN: control with the paired token -> QUIT_STATUS answers not armed", (st, st2)); s and s.close()
            c = lan_conn(ip, cport); c.sendall(b"ASBC\x02\x00\x40" + tok.encode()); a = rd(c, 7); c.sendall(b"pong")
            check(a[6] == 0 and rd(c, 4) == b"pong", "LAN: compat port accepts the paired Host token as TOKEN="); c.close()
            c = lan_conn(ip, cport); c.sendall(b"ASBC\x02\x00\x04nope"); a = rd(c, 7); check(a[6] == 1, "LAN: compat port refuses a wrong token"); c.close()
            # discovery
            u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.bind((ip, 0)); u.settimeout(3); u.sendto(b"ASH?", (ip, port))
            try: d, _ = u.recvfrom(600); d = d.decode()
            except socket.timeout: d = ""
            check(d.startswith("ASH!") and "name=testhost" in d and "port=%d" % port in d and "pair=1" in d, "LAN discovery: ASH? -> ASH! name/ver/port/pair", d)
            r = subprocess.run([CLI, "-p", str(port), "discover", ip], capture_output=True, text=True); check("name=testhost" in r.stdout, "C client discovery (ash-cli discover)")
            r = h.cli("tokens"); check("A4000 test" in r.stdout, "CLI 'tokens' lists the Amiga")
            row = [l for l in r.stdout.splitlines() if "A4000 test" in l]
            check("last used" in r.stdout and row and re.search(r"\d{4}-\d\d-\d\d \d\d:\d\d +\d{4}-\d\d-\d\d \d\d:\d\d", row[0]), "CLI 'tokens' shows paired and last-used times", r.stdout)
            def lines(): return [l for l in open(os.path.join(h.confdir, "tokens")).read().splitlines() if l and not l.startswith("#")]
            def newcode(): r = h.cli("pair"); m2 = re.search(r"(\d{3}) (\d{3})", r.stdout); return m2.group(1) + m2.group(2)
            def pair_raw(payload):
                s, st, msg = hs_from(ip, port, "pair"); send_frame(s, 1, payload.encode()); op, p = recv_frame(s); s.close(); return op, p.decode()
            # re-pair the same Amiga (same name, same address, no client id): the old token is replaced
            op, tok2 = pair(hs_from(ip, port, "pair")[0], newcode(), "A4000 test")
            hashes = [l.split()[0] for l in lines()]
            check(op == 2 and len([l for l in lines() if "A4000 test" in l]) == 1 and hashlib.sha256(tok2.encode()).hexdigest() in hashes
                  and hashlib.sha256(tok.encode()).hexdigest() not in hashes, "re-pair with the same name replaces the old token (one entry)", lines())
            check("replaces " + hashlib.sha256(tok.encode()).hexdigest()[:8] in h.out(), "re-pair logs which token it replaced")
            s, st, msg = hs_from(ip, port, "fetch", 1, tok2); check(st == 0, "the new token works after re-pairing", msg); s.close()
            # client id: same id under a new name replaces; a different name without an id is a different Amiga
            op, t3 = pair_raw(newcode() + "\nA1200 one\nCID-1200")
            op, t4 = pair_raw(newcode() + "\nA1200 renamed\nCID-1200")
            op5, t5 = pair_raw(newcode() + "\nA500 other")
            L = lines(); hashes = [l.split()[0] for l in L]
            check(len([l for l in L if "c=CID-1200" in l]) == 1 and "A1200 renamed" in "".join(L) and hashlib.sha256(t3.encode()).hexdigest() not in hashes
                  and hashlib.sha256(t4.encode()).hexdigest() in hashes, "re-pair with the same client id replaces the old token even under a new name", L)
            check(op5 == 2 and len(L) == 3, "a different Amiga (other name, no id) is added, not replaced", L)
            r = h.cli("tokens"); check("[client id CID-1200]" in r.stdout and "A500 other" in r.stdout, "CLI 'tokens' shows the client id", r.stdout)
            # takeover by token: the same paired Amiga reconnecting from the LAN replaces its stale session
            time.sleep(0.3)
            a1, sa1, _ = hs_from(ip, port, "ranged", 3, tok2); a2, sa2, ma2 = hs_from(ip, port, "ranged", 3, tok2)
            a1.settimeout(3)
            try: gone1 = a1.recv(10) == b""
            except OSError: gone1 = True
            check(sa1 == 0 and sa2 == 0 and gone1, "LAN: the same token reconnecting takes over the busy slot", (sa1, sa2, ma2))
            b1, sb1, _ = hs_from(ip, port, "ranged", 3, t5); check(sb1 == 5, "LAN: another paired Amiga (other token) still gets busy", sb1)
            for x in (a1, a2, b1):
                try: x and x.close()
                except OSError: pass
            tok = tok2
            r = h.cli("revoke", hashlib.sha256(t5.encode()).hexdigest()[:8]); check(r.returncode == 0 and len(lines()) == 2, "revoke still works on the new format")
            r = h.cli("revoke", hashlib.sha256(tok.encode()).hexdigest()[:8]); check(r.returncode == 0, "CLI 'revoke ID'")
            s, st, msg = hs_from(ip, port, "fetch", 1, tok); check(st == 1, "revoked token is refused at once", msg)
            # ban: 5 failures in 60 s
            for i in range(4): hs_from(ip, port, "fetch", 1, "f" * 64)
            s, st, msg = hs_from(ip, port, "hostinfo"); check(st == 6, "5 failed handshakes in 60 s -> refused (status 6) for a while", (st, msg))
            s, st, sv, msg = hs("127.0.0.1", port, "hostinfo"); check(st == 0, "the ban is per address (loopback still fine)")
        # --- shutdown stops backends ---
        pids = [int(x) for x in open(pidfile).read().split()]
        rc = h.stop(); time.sleep(0.3)
        alive = [p for p in pids if os.path.exists("/proc/%d" % p) and "zombie" not in open("/proc/%d/status" % p).read().lower()]
        check(rc == 0 and not alive, "SIGTERM: Host exits 0 and stops its backends", (rc, alive))
    finally:
        h.stop()
    # --- backend that fails to start ---
    port2 = free_port()
    h2 = Host(["--port", str(port2)], os.path.join(tmp, "conf2"), "service broken\nproto 1\nexec %s %s/test/fake_backend.py -port {port}\nready_timeout 3\n" % (sys.executable, ROOT), env={"FAKE_FAIL": "1"})
    try:
        h2.wait_port(port2)
        s, st, sv, msg = hs("127.0.0.1", port2, "broken", 1); check(st == 6 and "exited" in msg, "backend that dies at start -> status 6 with a reason", msg)
        info, svcs = hostinfo("127.0.0.1", port2); check(any(x.startswith("broken 1 down") for x in svcs), "hostinfo shows the backend down", svcs)
        check("LAN" not in h2.out() and "127.0.0.1:%d" % port2 in h2.out(), "default listens on 127.0.0.1 only")
        if ip:
            try: lan_conn(ip, port2).close(); check(False, "loopback-only Host is not reachable from the LAN address")
            except OSError: check(True, "loopback-only Host is not reachable from the LAN address")
    finally: h2.stop()
    # --- the Host's own update check reads a "component host" block ---
    port6 = free_port()
    h6 = Host(["--port", str(port6)], os.path.join(tmp, "conf6"), "fetch_allow localhost\ncafile %s/ca.pem\nmanifest https://localhost:%d/manifest.txt\n" % (tmp, sport), upd=True)
    try:
        h6.wait_port(port6); end = time.time() + 15; info = {}
        while time.time() < end and "update" not in info: time.sleep(0.5); info = hostinfo("127.0.0.1", port6)[0] or {}
        check(info.get("update") == "host 9.9" and "AmiStudio Host 9.9 is available" in h6.out(), "update check: 'component host' block in the manifest -> hostinfo 'update host 9.9'", (info.get("update"), h6.out()[-300:]))
    finally: h6.stop()
    # --- `status` against a port held by something that is not a Host: no hang, clear message ---
    mute = socket.socket(); mute.bind(("127.0.0.1", 0)); mute.listen(4); mport = mute.getsockname()[1]
    t0 = time.time()
    try: r = subprocess.run([HOSTD, "status", "127.0.0.1:%d" % mport], capture_output=True, text=True, timeout=15); rc, err = r.returncode, r.stderr
    except subprocess.TimeoutExpired: rc, err = None, "hung"
    dt = time.time() - t0
    check(rc == 1 and dt < 5 and "not an AmiStudio Host" in err and "within 2" in err, "status: silent program on the port -> gives up after ~2 s with a clear message (%.1f s)" % dt, err)
    def talker():
        c, _ = mute2.accept(); c.recv(100); c.sendall(b"HTTP/1.0 400 Bad Request\r\n\r\n"); c.close()
    mute2 = socket.socket(); mute2.bind(("127.0.0.1", 0)); mute2.listen(4); threading.Thread(target=talker, daemon=True).start()
    r = subprocess.run([HOSTD, "status", "127.0.0.1:%d" % mute2.getsockname()[1]], capture_output=True, text=True, timeout=15)
    check(r.returncode == 1 and "not an AmiStudio Host (it answered something else)" in r.stderr, "status: other program answering -> 'not an AmiStudio Host'", r.stderr)
    r = subprocess.run([HOSTD, "status", "127.0.0.1:%d" % free_port()], capture_output=True, text=True, timeout=15)
    check(r.returncode == 1 and "nothing listening" in r.stderr, "status: free port -> 'nothing listening'", r.stderr)
    mute.close(); mute2.close()
    # --- idle shutdown: any open or new client connection is activity; the engine idles separately ---
    port7 = free_port()
    conf7 = "service echo\nproto 2\nexec %s %s/test/fake_backend.py -port {port} -token {token}\nhandshake asbc2\nready_timeout 10\n" % (sys.executable, ROOT)
    h7 = Host(["--port", str(port7), "--exit-idle", "3s"], os.path.join(tmp, "conf7"), conf7)
    try:
        h7.wait_port(port7)
        s7, st7, _, _ = hs("127.0.0.1", port7, "echo", 2)             # an open session (AmiSurf)
        time.sleep(5); s7.sendall(b"hi"); alive1 = h7.p.poll() is None and rd(s7, 2) == b"hi"; s7.close()
        for i in range(5): time.sleep(1); hostinfo("127.0.0.1", port7)   # launcher-style status checks
        alive2 = h7.p.poll() is None
        t0 = time.time()
        try: rc7 = h7.p.wait(10)
        except subprocess.TimeoutExpired: rc7 = None
        dt = time.time() - t0
        check(alive1, "idle shutdown: an open client session keeps the Host up past exit_idle")
        check(alive2, "idle shutdown: hostinfo checks (launcher keepalives) keep the Host up")
        check(rc7 == 0 and 2 <= dt <= 6 and "stopping (idle shutdown)" in h7.out(), "idle shutdown: Host exits ~3 s after the last client, exit 0 (%.1f s)" % dt, h7.out()[-300:])
    finally: h7.stop()
    # --- 0.1.6 control service: quit-when-unused (the launcher closes; the Host must not hang around) ---
    def ctl_host(name, args=(), env=None):
        pt = free_port(); pf = os.path.join(tmp, name + ".pids"); cf = os.path.join(tmp, name + ".cmds")
        cv = "service echo\nproto 2\nexec %s %s/test/fake_backend.py -port {port} -token {token}\nhandshake asbc2\nready_timeout 10\nstop_timeout 5\n" % (sys.executable, ROOT)
        hx = Host(["--port", str(pt)] + list(args), os.path.join(tmp, name), cv, env=dict({"FAKE_PIDFILE": pf, "FAKE_CMDFILE": cf}, **(env or {}))); hx.wait_port(pt)
        return hx, pt, pf, cf
    def waitexit(hx, secs):
        t0 = time.time()
        try: return hx.p.wait(secs), time.time() - t0
        except subprocess.TimeoutExpired: return None, time.time() - t0
    def gone(pf):
        pids = [int(x) for x in open(pf).read().split()] if os.path.exists(pf) else []
        return pids and not any(os.path.exists("/proc/%d" % x) for x in pids)
    hx, pt, pf, cf = ctl_host("ctl1")
    try:
        info, svcs = hostinfo("127.0.0.1", pt)
        check(any(x.startswith("control 1 ") for x in svcs), "control: hostinfo lists 'service control 1' (how a launcher tells a 0.1.6+ Host)", svcs)
        s, st, sv, _ = hs("127.0.0.1", pt, "control"); r = ctl(s, 3); check(st == 0 and r == (0, 0, 10), "control: QUIT_STATUS -> not armed, 0 other clients, grace 10", r)
        r = ctl(s, 1); rc, dt = waitexit(hx, 8)
        check(r == (2, 0, 10), "control: QUIT_WHEN_UNUSED with nothing else connected -> STOPPING", r)
        check(rc == 0 and dt < 3 and "quit-when-unused: no other client: stopping" in hx.out(), "control: ... and the Host exits 0 right away (%.1f s)" % dt, hx.out()[-400:])
    finally: hx.stop()
    hx, pt, pf, cf = ctl_host("ctl2")
    try:
        e1, st1, _, _ = hs("127.0.0.1", pt, "echo", 2, client="AmiSurf-test"); e1.sendall(b"x1"); ok1 = rd(e1, 2) == b"x1"
        keys = open(cf).read().split("\n") if os.path.exists(cf) else []
        check(ok1 and re.match(r"^asbhost client l[0-9a-f]{16}$", keys[0] if keys else ""), "asbc2 proxy: the Host tells the backend who the client is first (asbhost client KEY), then passes bytes through", keys)
        s, st, sv, _ = hs("127.0.0.1", pt, "control"); r = ctl(s, 1, struct.pack("<H", 2))
        check(r == (1, 1, 2), "control: QUIT_WHEN_UNUSED (grace 2 s) with AmiSurf connected -> ARMED, 1 other client", r)
        hostinfo("127.0.0.1", pt); r = ctl(s, 3); check(r == (1, 1, 2), "control: hostinfo/control connections do not count as clients", r)
        s.close(); time.sleep(3); check(hx.p.poll() is None, "control: armed Host keeps running while a client is connected")
        e1.close(); time.sleep(0.8)
        e2, st2, _, _ = hs("127.0.0.1", pt, "echo", 2, client="AmiSurf-test"); time.sleep(2.5)
        check(st2 == 0 and hx.p.poll() is None, "control: a reconnect within the grace period keeps the Host up")
        e2.sendall(b"x2"); ok2 = rd(e2, 2) == b"x2"; t0 = time.time(); e2.close()
        rc, dt = waitexit(hx, 10)
        check(ok2 and rc == 0 and 1.5 <= dt <= 4 and "the last client left 2 s ago: stopping" in hx.out(), "control: Host exits 0 ~2 s after the last client (%.1f s, exit_idle off)" % dt, hx.out()[-400:])
        check(gone(pf), "control: the backend (engine) is stopped with the Host")
        k2 = open(cf).read().split()
        check(len(set(x for x in k2 if x.startswith("l"))) == 1, "asbc2 proxy: the same loopback client gets the same key on reconnect", k2)
    finally: hx.stop()
    hx, pt, pf, cf = ctl_host("ctl3")
    try:
        e1, _, _, _ = hs("127.0.0.1", pt, "echo", 2); s, st, sv, _ = hs("127.0.0.1", pt, "control")
        r1 = ctl(s, 1); r2 = ctl(s, 2); e1.close(); time.sleep(12.5)
        check(r1 == (1, 1, 10) and r2[0] == 0 and hx.p.poll() is None, "control: no grace given -> 10 s; QUIT_CANCEL disarms (Host still up 12 s after the last client)", (r1, r2))
        s.close()
    finally: hx.stop()
    hx, pt, pf, cf = ctl_host("ctl4", ["--no-remote-quit"])
    try:
        s, st, sv, _ = hs("127.0.0.1", pt, "control"); r = ctl(s, 1); time.sleep(1.5)
        check(r[0] == 0 and hx.p.poll() is None and "refused (allow_quit no" in hx.out(), "control: --no-remote-quit (allow_quit no) -> never armed", r)
        s.close()
    finally: hx.stop()
    # --- tokens file written by Host 0.1 (no u=/c= fields) still loads ---
    d4 = os.path.join(tmp, "conf4"); os.makedirs(d4)
    open(os.path.join(d4, "tokens"), "w").write("# old\n%s abcd1234 1760000000 Old A3000 @10.0.0.5\n" % ("a" * 64))
    r = subprocess.run([HOSTD, "tokens"], capture_output=True, text=True, env=dict(os.environ, AMISTUDIO_HOST_DIR=d4))
    check("abcd1234" in r.stdout and "never" in r.stdout and "Old A3000 @10.0.0.5" in r.stdout, "0.1 tokens file still lists (last used: never)", r.stdout)
    # --- a "start boot" backend is restarted by the reaper without a client ---
    port5 = free_port(); pf5 = os.path.join(tmp, "boot.pids")
    h5 = Host(["--port", str(port5)], os.path.join(tmp, "conf5"), "service bootsvc\nproto 1\nexec %s %s/test/fake_backend.py -port {port}\nstart boot\nready_timeout 10\n" % (sys.executable, ROOT), env={"FAKE_PIDFILE": pf5})
    try:
        h5.wait_port(port5)
        end = time.time() + 10
        while time.time() < end and not (os.path.exists(pf5) and open(pf5).read().strip()): time.sleep(0.1)
        p0 = int(open(pf5).read().split()[0]); time.sleep(5.5)      # past the 5 s quick-crash back-off
        os.kill(p0, signal.SIGTERM)
        end = time.time() + 10
        while time.time() < end and len(open(pf5).read().split()) < 2: time.sleep(0.2)
        time.sleep(0.5)
        info, svcs = hostinfo("127.0.0.1", port5)
        check(len(open(pf5).read().split()) == 2 and not os.path.exists("/proc/%d" % p0) and "bootsvc 1 up" in svcs, "reaper restarts a 'start boot' backend by itself", (open(pf5).read().split(), svcs))
    finally: h5.stop()
    # --- optional: real asb-chromed ---
    UCHROME = next(iter(sorted(__import__("glob").glob(os.path.join(ROOT, "build", "engine", "uc-*", "chrome")))), "")
    # AmiSurf's own build if there is one, else the Host's patched work tree (tools/engine-work.sh build)
    asb = os.environ.get("ASH_ASB_CHROMED") or next((x for x in (os.path.join(ROOT, "..", "amistudio-browser", "build", "host", "asb-chromed"),
                                                               os.path.join(ROOT, "build", "engine", "work", "build", "host", "asb-chromed")) if os.path.exists(x)), "")
    if os.environ.get("ASH_TEST_BROWSER") and os.path.exists(asb):
        exe = os.path.join(tmp, "asb-chromed"); shutil.copy(asb, exe)
        port3, cp3 = free_port(), free_port()
        h3 = Host(["--port", str(port3)], os.path.join(tmp, "conf3"),
                  "service browser\nproto 2\nexec %s -port {port} -token {token} -cdp %d -idle 0 -profile %s/prof\nhandshake asbc2\nready_timeout 40\n" % (exe, free_port(), tmp),
                  env=dict({"ASB_HEADLESS": "1", "ASB_CONFIG": os.path.join(tmp, "asbcfg"), "ASB_DOWNLOADS": os.path.join(tmp, "asbdl")},   # private: never the user's AmiSurf config
                           **({"ASB_CHROME": UCHROME} if os.path.exists(UCHROME) else {})))
        try:
            h3.wait_port(port3)
            t0 = time.time(); s, st, sv, msg = hs("127.0.0.1", port3, "browser", 2, timeout=60)
            check(st == 0, "browser service: real asb-chromed started by the Host (%.1f s)" % (time.time() - t0), msg)
            if st == 0:
                send_frame(s, 1, struct.pack("<HH", 640, 400))          # ASBC HELLO 640x400
                send_frame(s, 2, b"data:text/html,<h1>AmiStudio Host</h1>")
                seen = set(); s.settimeout(30); end = time.time() + 30
                try:
                    while time.time() < end and not (32 in seen):
                        op, p = recv_frame(s); seen.add(op)
                except (socket.timeout, EOFError): pass
                check(32 in seen or 43 in seen, "browser service: ASBC frames arrive through the Host (ops %s)" % sorted(seen))
                s.close()
                bookmarks_check(port3, os.path.join(tmp, "asbcfg"), "browser service")
                r = subprocess.run(["pgrep", "-xf", "%s -port .*" % re.escape(exe)], capture_output=True, text=True); bp = [int(x) for x in r.stdout.split()]
                if bp:
                    os.kill(bp[0], signal.SIGTERM); end = time.time() + 10
                    while time.time() < end and os.path.exists("/proc/%d" % bp[0]): time.sleep(0.2)
                    info, svcs = hostinfo("127.0.0.1", port3)
                    check(not os.path.exists("/proc/%d" % bp[0]) and any(x.startswith("browser 2 idle exited") for x in svcs), "browser service: asb-chromed that quits is reaped (no zombie) and shown idle", svcs)
                    s, st, sv, msg = hs("127.0.0.1", port3, "browser", 2, timeout=60); check(st == 0, "browser service: restarted on demand after it quit", msg); s and s.close()
                else: check(False, "browser service: find asb-chromed pid")
        finally:
            h3.stop()
            r = subprocess.run(["pgrep", "-f", exe], capture_output=True); check(r.returncode != 0, "browser service: asb-chromed stopped with the Host")
    else: skip("real asb-chromed", "set ASH_TEST_BROWSER=1 (needs ../amistudio-browser/build/host/asb-chromed or tools/engine-work.sh init+build, + Chrome)")
    # --- 0.1.10: the static release binary links none of glibc's NSS (Host 0.1.9 crashed on Fedora 44 / Nobara) ---
    st = os.path.join(ROOT, "build", "static", "amistudio-hostd")
    if os.path.exists(st): static_nss_check(st, "static binary")
    else: skip("static binary: no NSS", "build/static/amistudio-hostd not built (make tarball)")
    # --- optional: the release tarball with the bundled engine (asb-chromed + Chromium), end to end ---
    tb = os.environ.get("ASH_TEST_TARBALL") or os.path.join(ROOT, "dist", "amistudio-host-%s-linux-x86_64.tar.gz" % HOSTVER)
    if os.environ.get("ASH_TEST_BROWSER") and os.path.exists(tb): bundle_tests(tmp, tb, HOSTVER)
    else: skip("bundled engine from the release tarball", "set ASH_TEST_BROWSER=1 and build %s (make tarball)" % os.path.basename(tb))
    # --- 0.1.8 amidrive (Google Drive through rclone; a local folder behind an alias remote stands in for Drive) ---
    import amidrive_tests
    amidrive_tests.amidrive_tests(check, skip, tmp)
    srv.shutdown(); bad.shutdown()
    left = pulse_guard.ours()
    if os.environ.get("ASH_TEST_BROWSER"):
        check(not left or not psrv, "no PulseAudio daemon started under the test HOME (the user's own server was passed through)", left)
    n = pulse_guard.reap()
    if n: print("stopped %d PulseAudio daemon(s) started under %s" % (n, tmp))
    check(not pulse_guard.ours(), "no PulseAudio daemon left behind under the test folder", pulse_guard.ours())
    print("%s: %d ok, %d failed, %d skipped" % ("all passed" if not FAIL else "FAILED", OK, FAIL, SKIP))
    if not FAIL: shutil.rmtree(tmp, ignore_errors=True)
    else: print("logs kept in", tmp)
    sys.exit(1 if FAIL else 0)

NSS_SYMS = ("getaddrinfo", "gethostbyname", "gethostbyname2", "gethostbyname_r", "gethostbyname2_r", "gethostbyaddr", "getnameinfo",
            "getservbyname", "getservbyport", "getpwnam", "getpwuid", "getpwnam_r", "getpwuid_r", "getgrnam", "getgrgid",
            "__nss_module_get_function", "__nss_database_lookup2", "__nss_lookup_function")
def static_nss_check(exe, label):
    """0.1.10: a statically linked glibc program that calls getaddrinfo/gethostbyname/getpw* dlopens the system's NSS
    modules and their libc.so.6; a newer glibc there crashes it (Host 0.1.9 on Fedora 44: SIGFPE in __libc_early_init via
    libnss_myhostname, at the amidrive sign-in check). The static Host wraps them all to src/nss_off.c, so neither the
    symbols (unstripped build) nor glibc's NSS loader strings ("libnss_%s.so", "nsswitch.conf"; stripped tarball
    binary too) may be in it."""
    data = open(exe, "rb").read()
    strs = [x for x in (b"libnss_", b"nsswitch.conf") if x in data]
    syms = []
    if shutil.which("nm"):
        r = subprocess.run(["nm", exe], capture_output=True, text=True)
        if r.returncode == 0:
            for l in r.stdout.splitlines():
                p = l.split()
                if len(p) >= 2 and p[-1] in NSS_SYMS: syms.append(p[-1])
    check(not strs and not syms, "%s: no glibc NSS lookups linked (%s)" % (label, os.path.basename(exe)), (strs, syms))
def bundle_procs(d):
    """pids whose command line mentions the folder d (our own engine/Chromium only, never other testers')"""
    out = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit() or int(pid) == os.getpid(): continue
        try:
            cmd = open("/proc/%s/cmdline" % pid, "rb").read().replace(b"\0", b" ").decode(errors="replace")
            st = open("/proc/%s/status" % pid).read()
        except OSError: continue
        if d in cmd and "zombie" not in st.lower(): out.append(int(pid))
    return out
def bookmarks_check(port, cfgdir, label):
    """0.1.9 engine (AmiSurf 0.87): the client's "bookmarks" CMD gets ASBC_BMARKS (45) = "url\ttitle\tfolder" lines
    in bookmarks.tsv order; the 4th (folder) column is optional"""
    os.makedirs(cfgdir, exist_ok=True)
    with open(os.path.join(cfgdir, "bookmarks.tsv"), "w") as f:
        f.write("1760000000\thttps://example.org/a\tAlpha\tNews\n1760000001\thttps://example.org/b\tBeta\n")
    s, st, sv, msg = hs("127.0.0.1", port, "browser", 2, timeout=60)
    if st != 0: check(False, "%s: bookmarks command (session)" % label, msg); return
    send_frame(s, 1, struct.pack("<HH", 640, 400)); send_frame(s, 18, b"bookmarks")
    got = None; s.settimeout(30); end = time.time() + 30
    try:
        while time.time() < end and got is None:
            op, pl = recv_frame(s)
            if op == 45: got = pl.decode("utf-8", "replace")
    except (socket.timeout, EOFError, OSError): pass
    s.close()
    check(got == "https://example.org/a\tAlpha\tNews\nhttps://example.org/b\tBeta\t\n",
          "%s: \"bookmarks\" command -> ASBC_BMARKS with the list and the folder column" % label, got)
def browser_page(port, url, timeout=60):
    s, st, sv, msg = hs("127.0.0.1", port, "browser", 2, timeout=timeout)
    if st != 0: return st, msg, set()
    send_frame(s, 1, struct.pack("<HH", 640, 400)); send_frame(s, 2, url.encode())
    seen = set(); s.settimeout(30); end = time.time() + 30
    try:
        while time.time() < end and 32 not in seen: op, p = recv_frame(s); seen.add(op)
    except (socket.timeout, EOFError, OSError): pass
    s.close(); return st, msg, seen
def wait_port(port):
    for _ in range(200):
        try: socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); return True
        except OSError: time.sleep(0.05)
    return False
def bundle_tests(tmp, tb, ver):
    bt = os.path.join(tmp, "bundle"); os.makedirs(bt)
    subprocess.run(["tar", "-C", bt, "-xzf", tb], check=True)
    # the folder is named after the tarball (a test tarball such as ...-0.1.7-... may hold a Host still reporting 0.1.6)
    m = re.search(r"amistudio-host-([^/]+)-linux-x86_64\.tar\.gz$", tb)
    d = os.path.join(bt, "amistudio-host-%s-linux-x86_64" % (m.group(1) if m else ver))
    home = os.path.join(tmp, "home"); os.makedirs(home)
    env = dict(os.environ, HOME=home)
    for k in ("XDG_DATA_HOME", "XDG_CONFIG_HOME", "AMISTUDIO_HOST_DIR", "ASB_STATE", "DISPLAY"): env.pop(k, None)
    lic = os.path.join(d, "LICENSES")
    need = ["Chromium-LICENSE.txt", "chromium-credits.html", "ungoogled-chromium-LICENSE.txt", "portablelinux-LICENSE.txt", "OpenSSL-Apache-2.0.txt", "ENGINE-VERSION.txt", "AmiStudio-Host.txt"]
    check(all(os.path.exists(os.path.join(lic, f)) for f in need) and os.path.exists(os.path.join(d, "engine", "chromium", "LICENSE")), "tarball: Chromium licence, credits and notices in LICENSES/", os.listdir(lic) if os.path.isdir(lic) else None)
    check(os.access(os.path.join(d, "engine", "chromium", "chrome"), os.X_OK) and not os.path.exists(os.path.join(d, "engine", "chromium", "chromedriver"))
          and not any("widevine" in f.lower() for _, _, fs in os.walk(d) for f in fs), "tarball: Chromium executable, no chromedriver, no Widevine")
    static_nss_check(os.path.join(d, "amistudio-hostd"), "tarball")
    before = sorted(os.path.join(r, f) for r, _, fs in os.walk(d) for f in fs)
    conf = os.path.join(tmp, "bconf"); port = free_port()
    p = subprocess.Popen(["sh", os.path.join(d, "run-host.sh"), "--port", str(port), "--engine-idle", "1", "--confdir", conf, "--no-update-check", "-v"],
                         stdout=open(os.path.join(tmp, "bundle.out"), "w"), stderr=subprocess.STDOUT, env=env)
    try:
        wait_port(port)
        out = open(os.path.join(tmp, "bundle.out")).read()
        info, svcs = hostinfo("127.0.0.1", port) or ({}, [])
        check(info.get("host") == ver and "browser 2 idle" in svcs and "bundled browser engine" in out and "1 min idle" in out,
              "run-host.sh --port --engine-idle: bundled engine mounted as 'browser' with no host.conf", (svcs, out[-400:]))
        check("amidiscord 2 idle" in svcs and "bundled AmiDiscord engine" in out and "30 min after the last client" in out,
              "bundled engine also mounted as 'amidiscord' (app mode; Chrome stays 30 min after the last client)", (svcs, out[-400:]))
        t0 = time.time(); st, msg, seen = browser_page(port, "data:text/html,<h1>AmiStudio Host bundle</h1>")
        check(st == 0 and (32 in seen or 43 in seen), "bundled engine: AmiSurf session renders a page through the Host (%.1f s, ops %s)" % (time.time() - t0, sorted(seen)), msg)
        state = os.path.join(home, ".local", "share", "amistudio-host", "browser")
        check(os.path.isdir(os.path.join(state, "profile")) and os.listdir(os.path.join(state, "profile")) and os.path.isdir(os.path.join(state, "config")),
              "bundled engine: profile and config (bookmarks, history) in ~/.local/share/amistudio-host/browser", state)
        bookmarks_check(port, os.path.join(state, "config"), "bundled engine")
        after = sorted(os.path.join(r, f) for r, _, fs in os.walk(d) for f in fs)
        check(after == before, "bundled engine: nothing is written into the install folder", sorted(set(after) - set(before))[:5])
        exe = os.path.join(d, "engine", "asb-chromed")
        eng = [x for x in bundle_procs(exe) if open("/proc/%d/cmdline" % x, "rb").read().split(b"\0")[0].decode(errors="replace") == exe]
        if eng:
            os.kill(eng[0], signal.SIGTERM); end = time.time() + 10
            while time.time() < end and os.path.exists("/proc/%d" % eng[0]): time.sleep(0.2)
            time.sleep(1.2)
            info, svcs = hostinfo("127.0.0.1", port)
            check(not os.path.exists("/proc/%d" % eng[0]) and any(x.startswith("browser 2 idle exited") for x in svcs), "bundled engine: quit engine reaped and shown idle", svcs)
            st, msg, seen = browser_page(port, "data:text/html,<p>again</p>")
            check(st == 0 and (32 in seen or 43 in seen), "bundled engine: restarted on demand", msg)
        else: check(False, "bundled engine: find the asb-chromed process", bundle_procs(d))
        r = subprocess.run(["sh", os.path.join(d, "run-host.sh"), "--pair", "--confdir", conf], capture_output=True, text=True, env=env)
        check(re.search(r"Pairing code: \d{3} \d{3}", r.stdout) is not None, "run-host.sh --pair prints a code", r.stdout + r.stderr)
    finally:
        p.send_signal(signal.SIGTERM)
        try: rc = p.wait(20)
        except subprocess.TimeoutExpired: p.kill(); rc = None
    time.sleep(1)
    left = bundle_procs(d)
    check(rc == 0 and not left, "bundled engine: Host stop takes asb-chromed and Chromium with it", (rc, left))
    # AmiSurf AmiKit Host/data/{config,profile} copied straight into the Host's browser state works (same layout)
    amikit = os.path.join(tmp, "AmiSurf", "Host", "data"); os.makedirs(amikit)
    subprocess.run(["cp", "-a", os.path.join(state, "config"), os.path.join(state, "profile"), amikit], check=True)
    open(os.path.join(amikit, "config", "bookmarks.tsv"), "w").write("Moved\thttps://example.org/\n")
    home4 = os.path.join(tmp, "home4"); st4 = os.path.join(home4, ".local", "share", "amistudio-host", "browser"); os.makedirs(st4)
    subprocess.run(["cp", "-a", os.path.join(amikit, "config"), os.path.join(amikit, "profile"), st4], check=True)
    port4 = free_port(); env4 = dict(env, HOME=home4)
    p4 = subprocess.Popen([os.path.join(d, "amistudio-hostd"), "run", "--port", str(port4), "--confdir", os.path.join(tmp, "bconf4"), "--no-update-check"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env4)
    try:
        wait_port(port4); st, msg, seen = browser_page(port4, "data:text/html,<p>moved</p>")
        check(st == 0 and (32 in seen or 43 in seen) and open(os.path.join(st4, "config", "bookmarks.tsv")).read().startswith("Moved"),
              "AmiKit Host/data/{config,profile} copied to ~/.local/share/amistudio-host/browser works as is", msg)
    finally:
        p4.send_signal(signal.SIGTERM)
        try: p4.wait(20)
        except subprocess.TimeoutExpired: p4.kill()
    # --idle sets both: engine idle and Host idle shutdown
    port5 = free_port()
    p5 = subprocess.Popen(["sh", os.path.join(d, "run-host.sh"), "--port", str(port5), "--idle", "7", "--confdir", os.path.join(tmp, "bconf5"), "--no-update-check"], stdout=open(os.path.join(tmp, "b5.out"), "w"), stderr=subprocess.STDOUT, env=env)
    wait_port(port5); time.sleep(0.3); o5 = open(os.path.join(tmp, "b5.out")).read()
    check("7 min idle" in o5 and "stops after 420 s with no client" in o5, "run-host.sh --idle 7: engine idle 7 min and Host idle shutdown 7 min", o5[-300:])
    p5.send_signal(signal.SIGTERM); p5.wait(20)
    # --lan and --background
    port2 = free_port(); conf2 = os.path.join(tmp, "bconf2")
    r = subprocess.run(["sh", os.path.join(d, "run-host.sh"), "--background", "--lan", "--port", str(port2), "--confdir", conf2, "--no-discovery", "--no-update-check"], capture_output=True, text=True, env=env)
    ok = wait_port(port2)
    info = (hostinfo("127.0.0.1", port2) or ({}, []))[0] if ok else {}
    check(ok and "starting on port" in r.stdout and info.get("lan") == "1", "run-host.sh --background --lan: detached Host serving the LAN", (r.stdout, r.stderr, info))
    if shutil.which("nc"):
        r2 = subprocess.run(["sh", os.path.join(d, "run-host.sh"), "--background", "--port", str(port2), "--confdir", conf2], capture_output=True, text=True, env=env)
        check("already running" in r2.stdout, "run-host.sh --background twice: second one just exits", r2.stdout)
    try: os.kill(int(open(os.path.join(conf2, "hostd.pid")).read()), signal.SIGTERM)
    except (OSError, ValueError): pass
    # install-user.sh: copy to ~/.local/lib, link in ~/.local/bin; the link still finds the engine; data survives updates
    home2 = os.path.join(tmp, "home2"); os.makedirs(home2); env2 = dict(env, HOME=home2)
    st2 = os.path.join(home2, ".local", "share", "amistudio-host", "browser", "config"); os.makedirs(st2)
    open(os.path.join(st2, "bookmarks.tsv"), "w").write("AmiStudio\thttps://github.com/hardcopi\n")
    r = subprocess.run(["sh", os.path.join(d, "install-user.sh")], capture_output=True, text=True, env=env2)
    r = subprocess.run(["sh", os.path.join(d, "install-user.sh")], capture_output=True, text=True, env=env2)    # again = an update
    link = os.path.join(home2, ".local", "bin", "amistudio-hostd"); lib = os.path.join(home2, ".local", "lib", "amistudio-host")
    check(r.returncode == 0 and os.path.islink(link) and os.path.realpath(link) == os.path.join(lib, "amistudio-hostd") and os.path.isdir(os.path.join(lib, "engine", "chromium"))
          and os.path.exists(os.path.join(home2, ".config", "systemd", "user", "amistudio-host.service")), "install-user.sh: ~/.local/lib/amistudio-host + link + systemd unit", r.stdout + r.stderr)
    check(open(os.path.join(st2, "bookmarks.tsv")).read().startswith("AmiStudio"), "install-user.sh: browser data (bookmarks) kept across an update")
    port3 = free_port(); conf3 = os.path.join(tmp, "bconf3")
    p3 = subprocess.Popen([link, "run", "--port", str(port3), "--confdir", conf3, "--no-update-check"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env2)
    try:
        wait_port(port3)
        info, svcs = hostinfo("127.0.0.1", port3) or ({}, [])
        check("browser 2 idle" in svcs, "installed Host started through the ~/.local/bin link (as the systemd unit does) mounts the engine", svcs)
        st, msg, seen = browser_page(port3, "data:text/html,<p>installed</p>")
        check(st == 0 and (32 in seen or 43 in seen), "installed Host: page renders with the installed engine", msg)
    finally:
        p3.send_signal(signal.SIGTERM)
        try: p3.wait(20)
        except subprocess.TimeoutExpired: p3.kill()
    time.sleep(1); check(not bundle_procs(lib), "installed Host: engine stopped with it", bundle_procs(lib))
    if shutil.which("systemd-analyze"):
        u = os.path.join(home2, ".config", "systemd", "user", "amistudio-host.service")
        r = subprocess.run(["systemd-analyze", "verify", u], capture_output=True, text=True, env=env2)
        check("amistudio-host.service:" not in r.stderr, "systemd unit verifies (systemd-analyze)", r.stderr[-300:])
    else: skip("systemd unit verify", "no systemd-analyze")
    # AmiDiscord app mode (M1-M4) with a local fake app, then the real discord.com login page (no login)
    import app_tests
    app_tests.m1_tests(check, skip, tmp, os.path.join(d, "engine", "engine.sh"))
    if hasattr(app_tests, "m2_tests"): app_tests.m2_tests(check, skip, tmp, d)
    if hasattr(app_tests, "m3_tests"): app_tests.m3_tests(check, skip, tmp, os.path.join(d, "engine", "engine.sh"))
    if hasattr(app_tests, "m4_tests"): app_tests.m4_tests(check, skip, tmp, os.path.join(d, "engine", "engine.sh"))
    if hasattr(app_tests, "dlpath_tests"): app_tests.dlpath_tests(check, skip, tmp, os.path.join(d, "engine", "engine.sh"))
    if hasattr(app_tests, "view_tests"): app_tests.view_tests(check, skip, tmp, os.path.join(d, "engine", "engine.sh"))
    if hasattr(app_tests, "tabs_tests"): app_tests.tabs_tests(check, skip, tmp, os.path.join(d, "engine", "engine.sh"))
    if hasattr(app_tests, "selfcontained_tests"): app_tests.selfcontained_tests(check, skip, tmp, d)
    app_tests.discord_tests(check, skip, tmp, d)

if __name__ == "__main__": main()
