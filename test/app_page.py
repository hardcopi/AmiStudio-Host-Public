#!/usr/bin/env python3
"""Local fake web app for the app-mode (AmiDiscord) tests: `app_page.py PORT`. Pages:
  /app     links at fixed places (external, target=_blank external/internal, window.open external/internal)
  /anim    full-view animation: CSS one (stops with prefers-reduced-motion) + a JS one (never stops)
  /cssanim the CSS animation only; title says whether reduced motion is on
  /notify  fires Notifications (Notification API + ServiceWorker-free); see M3 tests
Nothing here talks to the internet."""
import http.server, sys, threading
PORT = 8765
BOX = "position:absolute;left:10px;width:300px;height:44px;display:block;background:#36c;color:#fff;font:20px sans-serif"
APP = """<!doctype html><html><head><title>Fake App</title><style>body{margin:0;background:#eee}</style></head><body>
<a id=ext href="https://example.org/ext-link" style="%(b)s;top:10px">external link</a>
<a id=blank href="https://example.com/blank-link" target=_blank style="%(b)s;top:70px">external _blank</a>
<a id=iblank href="/app2" target=_blank style="%(b)s;top:130px">internal _blank</a>
<div id=wo onclick="window.open('https://example.net/window-open')" style="%(b)s;top:190px">window.open external</div>
<div id=wo2 onclick="var w=window.open('');w.location='https://example.net/later'" style="%(b)s;top:250px">window.open blank then external</div>
<a id=int href="/app2" style="%(b)s;top:310px">internal link</a>
</body></html>""" % {"b": BOX}
APP2 = "<!doctype html><title>Fake App 2</title><p>second page</p>"
LOGIN = """<!doctype html><title>Fake Login</title><form action=/app2 onsubmit="document.title='submitted';return false">
<input name=u value=amiga style="position:absolute;left:10px;top:10px;width:300px;height:40px">
<input id=p name=p type=password style="position:absolute;left:10px;top:70px;width:300px;height:40px"><button style="position:absolute;left:10px;top:130px">Log in</button></form>"""
ANIM = """<!doctype html><html><head><title>anim</title><style>
html,body{margin:0;height:100%%}
#c{position:fixed;inset:0;animation:spin 0.4s linear infinite;background:linear-gradient(90deg,#f00,#00f)}
@keyframes spin{from{filter:hue-rotate(0deg)}to{filter:hue-rotate(360deg)}}
@media (prefers-reduced-motion: reduce){#c{animation:none}}
</style></head><body><div id=c></div>%s
<script>document.title='rm='+matchMedia('(prefers-reduced-motion: reduce)').matches</script></body></html>"""
JSANIM = """<div id=j style="position:fixed;inset:0;opacity:.5"></div><script>
var k=0;setInterval(function(){k=(k+37)%360;document.getElementById('j').style.background='hsl('+k+',80%,50%)'},150)</script>"""
# /persist?set=X stores X in localStorage and a 1-year cookie; /persist shows both in the title: "ls=X ck=X"
PERSIST = """<!doctype html><title>persist</title><script>
var m=location.search.match(/set=(\\w+)/);
if(m){localStorage.setItem('v',m[1]);document.cookie='v='+m[1]+';max-age=31536000;path=/';}
var c=(document.cookie.match(/(?:^|; )v=(\\w+)/)||[])[1];
document.title='ls='+(localStorage.getItem('v')||'-')+' ck='+(c||'-');</script>"""
# /notify: Notification API test page. Query: fire=K (K notifications 300 ms after load, tags t1..tK, an icon;
# clicking one sets the title "clicked I"), close=1 (the page closes the first one after 1.5 s), bg=1 (like Discord:
# a notification "bg N" each time the page becomes hidden or unfocused), t=TITLE and fav=NAME (set title / favicon
# after 300 ms), sw=1 (ServiceWorkerRegistration.prototype.showNotification), push=PATH (history.pushState to PATH
# after 1 s, like Discord going from /login to /channels/@me) with title push_t=TITLE, fav2=NAME (favicon after 2 s).
# Also served as /login/fake (a logged-out path). Without t= the title shows
# "st vis=<visibilityState> foc=<hasFocus> perm=<Notification.permission> hid=<hidden>".
NOTIFY = """<!doctype html><html><head><title>notify</title><link rel=icon href="/fav-plain.png"></head><body>
<script>
var q=new URLSearchParams(location.search), bgn=0, last=null;
function st(){ if(!q.get('t')) document.title='st vis='+document.visibilityState+' foc='+document.hasFocus()+' perm='+Notification.permission+' hid='+document.hidden;
  var away=document.hidden||!document.hasFocus();
  if(q.get('bg') && away && last===false){ bgn++; new Notification('bg '+bgn,{body:'while away',tag:'bg'+bgn}); }
  last=away; }
document.addEventListener('visibilitychange',st); window.addEventListener('focus',st); window.addEventListener('blur',st);
setInterval(st,250); st();
var ns=[];
setTimeout(function(){
  var k=+(q.get('fire')||0);
  for(var i=1;i<=k;i++)(function(i){ var n=new Notification('Alice (#general, Test Server)',{body:'hello '+i+' @amiga',tag:'t'+i,icon:'/icon.png',data:{url:'/channels/1/'+i}});
    n.onclick=function(){ document.title='clicked '+i; q.set('t','x'); }; ns.push(n); })(i);
  if(q.get('sw')) ServiceWorkerRegistration.prototype.showNotification.call({}, 'Bob', {body:'sw hello'});
  if(q.get('close')) setTimeout(function(){ ns[0].close(); },1500);
  if(q.get('t')) document.title=q.get('t');
  if(q.get('later')) setTimeout(function(){ document.title=q.get('later'); },2000);
  if(q.get('spin')){ var k=0, iv=setInterval(function(){ k++; document.title='('+k+') Spin'; if(k>=40) clearInterval(iv); },50); }
  if(q.get('ifr')){ var f=document.createElement('iframe'); f.src='http://localhost:'+location.port+'/notify?fire=1&t=iframe'; document.body.appendChild(f); }
  if(q.get('fav')) document.querySelector('link[rel~=icon]').href='/'+q.get('fav')+'.png';
  if(q.get('push')) setTimeout(function(){ history.pushState({},'',q.get('push')); if(q.get('push_t')) document.title=q.get('push_t'); },700);
  if(q.get('fav2')) setTimeout(function(){ document.querySelector('link[rel~=icon]').href='/'+q.get('fav2')+'.png'; },1700);
},300);
</script></body></html>"""
# /upload: a file input over the whole top 300x100 (accept=.txt,image/png, multiple); after a choice the title is
# "files=N a.txt:123,b.png:456"; /files: same without multiple. /dl: a link (10,10 300x44) to /dl/file.bin (attachment,
# 3000 bytes); /clip: a button (10,10 300x44) that calls navigator.clipboard.writeText("hello clip").
UPLOAD = """<!doctype html><title>upload</title><input id=f type=file accept=".txt,image/png" %s
style="position:absolute;left:0;top:0;width:300px;height:100px">
<script>document.getElementById('f').onchange=function(){ var a=[].map.call(this.files,function(f){ return f.name+':'+f.size; });
document.title='files='+this.files.length+' '+a.join(',');};</script>"""
DL = """<!doctype html><title>dl</title><a href="/dl/file.bin" style="%s;top:10px">download</a>""" % BOX
CLIP = """<!doctype html><title>clip</title><div onclick="navigator.clipboard.writeText('hello clip').then(function(){document.title='clip done'})" style="%s;top:10px">copy</div>""" % BOX
# /tabs (AmiSurf 0.85 engine, browser mode): a target=_blank link, a window.open() popup, a popup that closes itself,
# a download link; static page (no frames unless something changes)
TABS = """<!doctype html><title>tabs</title><a href="/size?blank" target="_blank" style="%s;top:10px">blank</a>
<div onclick="window.open('/popup')" style="%s;top:70px">popup</div>
<div onclick="window.open('/popclose')" style="%s;top:130px">popclose</div>
<a href="/dl/file.bin" style="%s;top:190px">download</a>""" % (BOX, BOX, BOX, BOX)
POPUP = """<!doctype html><title>popup</title><script>document.title='popup ok '+(window.opener?'opener':'none')</script>"""
POPCLOSE = """<!doctype html><title>closing</title><script>setTimeout(function(){window.close()},1500)</script>"""
# /size: the title shows the CSS viewport and device pixel ratio ("w=640 h=400 dpr=1"), updated on resize; a
# 100px-wide red box at 0,0 so the frame shows how big a CSS pixel is; a counter ticks every 200 ms (screencast frames;
# /size?still: no counter, a static page)
SIZE = """<!doctype html><html><head><title>size</title><style>html,body{margin:0;background:#fff}#b{position:absolute;left:0;top:0;width:100px;height:100px;background:#f00}</style></head>
<body><div id=b></div><div id=k style="position:absolute;left:120px;top:20px;font:16px sans-serif">0</div><script>function t(){ document.title='w='+innerWidth+' h='+innerHeight+' dpr='+devicePixelRatio; }
t(); addEventListener('resize',t); setInterval(t,250);
var k=0; if(!/still/.test(location.search)) setInterval(function(){ document.getElementById('k').textContent=++k; },200);</script></body></html>"""
DLDATA = bytes(range(256)) * 11 + bytes(184)
def png(w, h, rgb):
    import zlib, struct as st
    raw = b"".join(b"\0" + bytes(rgb) * w for _ in range(h))
    ch = lambda t, d: st.pack(">I", len(d)) + t + d + st.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", st.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + ch(b"IDAT", zlib.compress(raw)) + ch(b"IEND", b"")
IMAGES = {"/icon.png": png(32, 32, (200, 30, 40)), "/fav-plain.png": png(16, 16, (88, 101, 242)), "/fav-badge.png": png(16, 16, (237, 66, 69)),
          "/fav-mention.png": png(16, 16, (250, 160, 20))}
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        p = self.path.split("?")[0]
        body = {"/app": APP, "/app2": APP2, "/login": LOGIN, "/anim": ANIM % JSANIM, "/cssanim": ANIM % "", "/persist": PERSIST, "/notify": NOTIFY, "/login/fake": NOTIFY,
                "/upload": UPLOAD % "multiple", "/files": UPLOAD % "", "/dl": DL, "/clip": CLIP, "/size": SIZE, "/tabs": TABS, "/popup": POPUP, "/popclose": POPCLOSE}.get(p)
        if p == "/dl/file.bin":
            self.send_response(200); self.send_header("Content-Type", "application/octet-stream"); self.send_header("Content-Disposition", 'attachment; filename="file.bin"')
            self.send_header("Content-Length", str(len(DLDATA))); self.end_headers(); self.wfile.write(DLDATA); return
        if p in IMAGES:
            b = IMAGES[p]; self.send_response(200); self.send_header("Content-Type", "image/png"); self.send_header("Content-Length", str(len(b)))
            self.end_headers(); self.wfile.write(b); return
        if body is None and p.startswith("/channels/"): body = NOTIFY
        if body is None and hasattr(self.server, "extra") and p in self.server.extra: body = self.server.extra[p]
        if body is None: self.send_response(404); self.end_headers(); return
        b = body.encode(); self.send_response(200); self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
def serve(port, extra=None):
    s = http.server.ThreadingHTTPServer(("127.0.0.1", port), H); s.extra = extra or {}
    threading.Thread(target=s.serve_forever, daemon=True).start(); return s
if __name__ == "__main__":
    if len(sys.argv) > 1: PORT = int(sys.argv[1])
    serve(PORT); print("app_page on", PORT); threading.Event().wait()
