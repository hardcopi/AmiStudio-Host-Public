#!/usr/bin/env python3
"""Stand-in for a service backend such as asb-chromed: `fake_backend.py -port N -token T [-idle 0]`.
Speaks the ASBC v2 handshake (token checked), then echoes everything back. Writes its pid to $FAKE_PIDFILE; the Host's "asbhost client KEY" command goes to $FAKE_CMDFILE."""
import os, socket, sys, threading
args = sys.argv[1:]; port = int(args[args.index("-port") + 1]); token = args[args.index("-token") + 1] if "-token" in args else ""
if os.environ.get("FAKE_PIDFILE"): open(os.environ["FAKE_PIDFILE"], "a").write("%d\n" % os.getpid())
if os.environ.get("FAKE_FAIL"): sys.exit(3)
def rd(c, n):
    b = b""
    while len(b) < n:
        k = c.recv(n - len(b))
        if not k: raise EOF
        b += k
    return b
class EOF(Exception): pass
def serve(c):
    try:
        h = rd(c, 7)
        if h[:4] != b"ASBC": return
        tok = rd(c, h[6]).decode() if h[6] else ""
        ok = (not token) or tok == token
        c.sendall(b"ASBC" + h[4:6] + bytes([0 if ok else 1]))
        print("fake_backend: client %s" % ("ok" if ok else "bad token"), flush=True)
        if not ok: return
        # Host 0.1.6+ first sends ASBC CMD "asbhost client KEY" (who the client is); swallow it, record it in $FAKE_CMDFILE
        c.settimeout(0.5)
        try: pk = c.recv(20, socket.MSG_PEEK | socket.MSG_WAITALL)
        except OSError: pk = b""
        c.settimeout(None)
        if len(pk) >= 20 and pk[4] == 18 and pk[5:20] == b"asbhost client ":
            n = int.from_bytes(pk[:4], "little"); cmd = rd(c, 4 + n)[5:].decode()
            if os.environ.get("FAKE_CMDFILE"): open(os.environ["FAKE_CMDFILE"], "a").write(cmd + "\n")
        while True:
            b = c.recv(65536)
            if not b: return
            c.sendall(b)
    except (EOF, OSError): pass
    finally: c.close()
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", port)); s.listen(8)
print("fake_backend: listening on %d" % port, flush=True)
while True:
    c, _ = s.accept(); threading.Thread(target=serve, args=(c,), daemon=True).start()
