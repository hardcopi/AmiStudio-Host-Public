"""Keep the test Chromium on the user's own PulseAudio and leave no daemon behind.

The tests start the engine with HOME pointing into a throw-away folder (and without XDG_RUNTIME_DIR). libpulse then
finds no server, and asb-chromed runs `pulseaudio --start` itself, so every run used to leave a daemon whose HOME
was a deleted /tmp/ash-test-* or /tmp/apptest-* folder. setup() does what the AmiStudio launcher's run-host.sh does
(0.1.8): it passes the real session's PULSE_SERVER and PULSE_COOKIE through, so no second daemon is needed, and it
turns libpulse's own autospawn off. reap() stops any pulseaudio whose HOME is under this run's test folder (only
those: a daemon from another run or the user's own is never touched); it runs at the end and at exit."""
import atexit, glob, os, signal, socket, subprocess, time

REAL_HOME = os.path.expanduser("~")     # read at import: the tests change HOME only in the child environments
_roots = []

def _alive(path):
    s = socket.socket(socket.AF_UNIX)
    try: s.settimeout(1); s.connect(path); return True
    except OSError: return False
    finally: s.close()

def real_server():
    """unix:<socket> of the user's running PulseAudio/PipeWire-pulse, or None"""
    cands = []
    rt = os.environ.get("XDG_RUNTIME_DIR")
    if rt: cands.append(os.path.join(rt, "pulse", "native"))
    cands += [os.path.join(os.path.realpath(l), "native") for l in sorted(glob.glob(os.path.join(REAL_HOME, ".config", "pulse", "*-runtime")))]
    for c in cands:
        if _alive(c): return "unix:" + c
    return None

def setup(tmp):
    """call once per run, right after the test folder is made; changes os.environ for all child processes"""
    _roots.append(os.path.realpath(tmp))
    if not _roots[1:]: atexit.register(reap)
    conf = os.path.join(tmp, "pulse-client.conf")
    with open(conf, "w") as f: f.write("autospawn = no\n")
    os.environ["PULSE_CLIENTCONFIG"] = conf
    if not os.environ.get("PULSE_SERVER"):
        srv = real_server()
        if srv: os.environ["PULSE_SERVER"] = srv
    cookie = os.path.join(REAL_HOME, ".config", "pulse", "cookie")
    if not os.environ.get("PULSE_COOKIE") and os.access(cookie, os.R_OK): os.environ["PULSE_COOKIE"] = cookie
    return os.environ.get("PULSE_SERVER")

def _home(pid):
    try:
        for kv in open("/proc/%d/environ" % pid, "rb").read().split(b"\0"):
            if kv.startswith(b"HOME="): return kv[5:].decode(errors="replace")
    except OSError: pass
    return None

def ours():
    """pids of pulseaudio daemons (this uid) whose HOME is under one of this run's test folders"""
    r = subprocess.run(["pgrep", "-u", str(os.getuid()), "-x", "pulseaudio"], capture_output=True, text=True)
    out = []
    for pid in map(int, r.stdout.split()):
        h = _home(pid)
        if h is None: continue
        hp = os.path.realpath(h) if os.path.exists(h) else os.path.normpath(h)
        if any(hp == t or hp.startswith(t + os.sep) or os.path.normpath(h).startswith(t + os.sep) for t in _roots): out.append(pid)
    return out

def reap():
    """stop this run's pulseaudio daemons; returns how many there were"""
    pids = ours()
    for pid in pids:
        try: os.kill(pid, signal.SIGTERM)
        except OSError: pass
    end = time.time() + 5
    while time.time() < end and any(os.path.exists("/proc/%d" % p) for p in pids): time.sleep(0.1)
    for pid in pids:
        if os.path.exists("/proc/%d" % pid):
            try: os.kill(pid, signal.SIGKILL)
            except OSError: pass
    return len(pids)
