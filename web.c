/* web.c — resident web UI for ps5-dlc-dumper.
 *
 * Solves the ordering deadlock:
 *   The Homebrew Launcher runs homebrew by hijacking a "big app" process, and a
 *   PS5 runs one big app at a time — so launching a payload that way CLOSES the
 *   running game, unmounting the DLC we came for.
 *
 * Fix: launch this first (nothing running), it goes resident and serves a page.
 * Then start the game, enter the DLC, and press Dump in a browser.
 *
 * Included by main.c — not a standalone translation unit.
 */

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>

#define WEB_PORT_FIRST 8082    /* 8080 = websrv, 8081 = ps5-app-dumper */
#define WEB_PORT_TRIES 8

typedef struct {
    int      running;
    int      finished;
    int      sources;
    uint64_t files, dirs, bytes, errors;
    char     current[320];
    char     message[256];
    char     filter[64];
} webstate_t;

static webstate_t      g_ws;
static pthread_mutex_t g_wslock = PTHREAD_MUTEX_INITIALIZER;

void web_progress(const char *path, uint64_t files, uint64_t dirs,
                  uint64_t bytes, uint64_t errors) {
    pthread_mutex_lock(&g_wslock);
    if (path) snprintf(g_ws.current, sizeof(g_ws.current), "%s", path);
    g_ws.files = files; g_ws.dirs = dirs; g_ws.bytes = bytes; g_ws.errors = errors;
    pthread_mutex_unlock(&g_wslock);
}

/* ------------------------------ tiny helpers ------------------------------ */

static int get_local_ip(char *out, size_t n) {
    struct sockaddr_in a, l;
    socklen_t ll = sizeof(l);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons(53);
    a.sin_addr.s_addr = inet_addr("8.8.8.8");
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) { close(s); return -1; }
    if (getsockname(s, (struct sockaddr *)&l, &ll) != 0)   { close(s); return -1; }
    close(s);
    return inet_ntop(AF_INET, &l.sin_addr, out, (socklen_t)n) ? 0 : -1;
}

static void send_all(int fd, const char *b, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, b + off, n - off);
        if (w <= 0) { if (errno == EINTR) continue; return; }
        off += (size_t)w;
    }
}

static void send_response(int fd, const char *status, const char *ctype,
                          const char *body) {
    char hdr[256];
    int  n = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 %s\r\nContent-Type: %s\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n"
                      "Cache-Control: no-store\r\n\r\n",
                      status, ctype, strlen(body));
    send_all(fd, hdr, (size_t)n);
    send_all(fd, body, strlen(body));
}

/* Scan pfsmnt and emit a JSON array of dumpable DLC folder names. */
static void json_mounts(char *out, size_t n) {
    DIR *d;
    struct dirent *e;
    size_t used = 0;
    int first = 1;

    used += (size_t)snprintf(out + used, n - used, "[");
    if ((d = opendir(PFSMNT))) {
        while ((e = readdir(d)) && used < n - 128) {
            if (!str_endswith(e->d_name, "-ac"))    continue;
            if (str_endswith(e->d_name, "-nest"))   continue;
            if (str_endswith(e->d_name, "-union"))  continue;
            used += (size_t)snprintf(out + used, n - used, "%s\"%s\"",
                                     first ? "" : ",", e->d_name);
            first = 0;
        }
        closedir(d);
    }
    snprintf(out + used, n - used, "]");
}

/* Is any PPSA/CUSA game sandbox alive right now? */
static int game_running(char *title, size_t tn) {
    DIR *d;
    struct dirent *e;
    int found = 0;
    if (!(d = opendir(SANDBOX))) return 0;
    while ((e = readdir(d))) {
        if (str_startswith(e->d_name, "PPSA") || str_startswith(e->d_name, "CUSA")) {
            if (title) snprintf(title, tn, "%s", e->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

/* ------------------------------ dump thread ------------------------------ */

int  run_dump(const char *filter, stats_t *st);   /* provided by main.c */

static void *dump_thread(void *arg) {
    char   filter[64];
    stats_t st;
    int     sources;

    pthread_mutex_lock(&g_wslock);
    snprintf(filter, sizeof(filter), "%s", g_ws.filter);
    g_ws.running  = 1;
    g_ws.finished = 0;
    g_ws.files = g_ws.dirs = g_ws.bytes = g_ws.errors = 0;
    g_ws.sources = 0;
    snprintf(g_ws.message, sizeof(g_ws.message), "Dumping...");
    pthread_mutex_unlock(&g_wslock);

    memset(&st, 0, sizeof(st));
    sources = run_dump(filter[0] ? filter : NULL, &st);

    pthread_mutex_lock(&g_wslock);
    g_ws.running  = 0;
    g_ws.finished = 1;
    g_ws.sources  = sources;
    g_ws.files = st.files; g_ws.dirs = st.dirs;
    g_ws.bytes = st.bytes; g_ws.errors = st.errors;
    g_ws.current[0] = '\0';
    if (sources < 0)
        snprintf(g_ws.message, sizeof(g_ws.message),
                 "No writable USB found. Plug in an exFAT drive and retry.");
    else if (sources == 0)
        snprintf(g_ws.message, sizeof(g_ws.message),
                 "Nothing dumped - no mounted DLC found. Is the game running "
                 "with kstuff, and did you enter the DLC content?");
    else
        snprintf(g_ws.message, sizeof(g_ws.message),
                 "Done: %d source(s), %llu files, %llu errors",
                 sources, (unsigned long long)st.files,
                 (unsigned long long)st.errors);
    pthread_mutex_unlock(&g_wslock);

    (void)arg;
    return NULL;
}

static int start_dump(const char *filter) {
    pthread_t t;
    pthread_mutex_lock(&g_wslock);
    if (g_ws.running) { pthread_mutex_unlock(&g_wslock); return -1; }
    snprintf(g_ws.filter, sizeof(g_ws.filter), "%s", filter ? filter : "");
    pthread_mutex_unlock(&g_wslock);

    if (pthread_create(&t, NULL, dump_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}

/* ------------------------------ the page ------------------------------ */

static const char *PAGE =
"<!DOCTYPE html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>PS5 DLC Dumper</title><style>"
"*{box-sizing:border-box}body{margin:0;padding:24px;background:#0d1117;color:#e6edf3;"
"font:15px/1.5 -apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif}"
".w{max-width:760px;margin:0 auto}h1{font-size:22px;margin:0 0 4px}"
".sub{color:#8b949e;font-size:13px;margin-bottom:20px}"
".c{background:#161b22;border:1px solid #30363d;border-radius:10px;padding:16px;margin-bottom:14px}"
".r{display:flex;justify-content:space-between;padding:6px 0;border-bottom:1px solid #21262d}"
".r:last-child{border:0}.k{color:#8b949e}.v{font-family:ui-monospace,Menlo,monospace;font-size:13px}"
"button{background:#238636;color:#fff;border:0;border-radius:7px;padding:10px 18px;"
"font-size:14px;font-weight:600;cursor:pointer;margin:4px 6px 4px 0}"
"button:hover{background:#2ea043}button:disabled{background:#30363d;color:#8b949e;cursor:not-allowed}"
"button.s{background:#1f6feb;padding:7px 13px;font-size:13px;font-weight:500}"
"button.s:hover{background:#388bfd}"
".ok{color:#3fb950}.bad{color:#f85149}.warn{color:#d29922}"
".m{font-family:ui-monospace,Menlo,monospace;font-size:12px;color:#8b949e;"
"word-break:break-all;margin-top:10px;min-height:16px}"
"ul{list-style:none;padding:0;margin:0}li{padding:8px 0;border-bottom:1px solid #21262d;"
"display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap}"
"li:last-child{border:0}.n{font-family:ui-monospace,Menlo,monospace;font-size:12px}"
".e{color:#8b949e;font-size:13px;padding:6px 0}"
"</style></head><body><div class=w>"
"<h1>PS5 DLC Dumper</h1>"
"<div class=sub>Copies decrypted, mounted DLC to USB. Dump content you own.</div>"
"<div class=c id=st></div>"
"<div class=c><b>Mounted DLC</b><div id=ls></div></div>"
"<div class=c><button id=all onclick='dump(\"\")'>Dump all mounted DLC</button>"
"<div class=m id=msg></div></div>"
"<div class=c style='font-size:13px;color:#8b949e'>"
"<b style='color:#e6edf3'>If nothing is listed:</b><br>"
"1. kstuff must be loaded <i>before</i> the game launches<br>"
"2. the game must be running now (PS button to home is fine, don't close it)<br>"
"3. enter the DLC content in-game once - many titles mount add-ons lazily"
"</div></div><script>"
"function esc(s){return s.replace(/[&<>\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c]))}"
"function gb(b){return (b/1073741824).toFixed(2)+' GiB'}"
"async function dump(f){"
"  document.getElementById('msg').textContent='Starting...';"
"  await fetch('/dump'+(f?('?f='+encodeURIComponent(f)):''));tick();}"
"async function tick(){"
" try{const r=await fetch('/status');const s=await r.json();"
"  document.getElementById('st').innerHTML="
"   '<div class=r><span class=k>USB</span><span class=v>'+(s.usb?esc(s.usb):'<span class=bad>not found</span>')+'</span></div>'+"
"   '<div class=r><span class=k>Game running</span><span class=v>'+(s.game?('<span class=ok>'+esc(s.title)+'</span>'):'<span class=warn>no</span>')+'</span></div>'+"
"   '<div class=r><span class=k>State</span><span class=v>'+(s.running?'<span class=warn>dumping</span>':'idle')+'</span></div>'+"
"   '<div class=r><span class=k>Files</span><span class=v>'+s.files+'</span></div>'+"
"   '<div class=r><span class=k>Copied</span><span class=v>'+gb(s.bytes)+'</span></div>'+"
"   '<div class=r><span class=k>Errors</span><span class=v class='+(s.errors?'bad':'ok')+'>'+s.errors+'</span></div>';"
"  const L=document.getElementById('ls');"
"  if(!s.mounts.length){L.innerHTML='<div class=e>None mounted yet.</div>';}"
"  else{L.innerHTML='<ul>'+s.mounts.map(m=>'<li><span class=n>'+esc(m)+'</span>'+"
"   '<button class=s '+(s.running?'disabled':'')+' onclick=\"dump(\\''+esc(m)+'\\')\">Dump</button></li>').join('')+'</ul>';}"
"  document.getElementById('all').disabled=s.running||!s.mounts.length;"
"  document.getElementById('msg').textContent=s.running?(s.current||'working...'):(s.message||'');"
" }catch(e){document.getElementById('msg').textContent='lost connection to payload';}"
"}tick();setInterval(tick,1500);"
"</script></body></html>";

/* ------------------------------ server ------------------------------ */

static void handle(int fd) {
    char req[2048], path[512], body[8192], mounts[4096], title[64];
    ssize_t n;
    char *sp, *q;

    n = read(fd, req, sizeof(req) - 1);
    if (n <= 0) return;
    req[n] = '\0';

    if (strncmp(req, "GET ", 4) != 0) {
        send_response(fd, "405 Method Not Allowed", "text/plain", "no");
        return;
    }
    sp = req + 4;
    q = strchr(sp, ' ');
    if (!q) return;
    *q = '\0';
    snprintf(path, sizeof(path), "%s", sp);

    if (!strcmp(path, "/") || !strncmp(path, "/index", 6)) {
        send_response(fd, "200 OK", "text/html; charset=utf-8", PAGE);
        return;
    }

    if (!strncmp(path, "/dump", 5)) {
        char filter[64] = "";
        char *f = strstr(path, "?f=");
        if (f) {
            f += 3;
            /* keep it boring: alphanumerics, dash, underscore only */
            size_t i = 0;
            for (; f[i] && i < sizeof(filter) - 1; i++) {
                char c = f[i];
                if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) break;
                filter[i] = c;
            }
            filter[i] = '\0';
        }
        if (start_dump(filter[0] ? filter : NULL) == 0)
            send_response(fd, "200 OK", "application/json", "{\"ok\":1}");
        else
            send_response(fd, "409 Conflict", "application/json",
                          "{\"ok\":0,\"error\":\"busy\"}");
        return;
    }

    if (!strcmp(path, "/status")) {
        int  gr;
        char cur[320], msg[256];
        uint64_t files, dirs, bytes, errors;
        int running, finished, sources;

        json_mounts(mounts, sizeof(mounts));
        title[0] = '\0';
        gr = game_running(title, sizeof(title));

        pthread_mutex_lock(&g_wslock);
        running = g_ws.running; finished = g_ws.finished; sources = g_ws.sources;
        files = g_ws.files; dirs = g_ws.dirs; bytes = g_ws.bytes; errors = g_ws.errors;
        snprintf(cur, sizeof(cur), "%s", g_ws.current);
        snprintf(msg, sizeof(msg), "%s", g_ws.message);
        pthread_mutex_unlock(&g_wslock);

        snprintf(body, sizeof(body),
                 "{\"usb\":\"%s\",\"game\":%d,\"title\":\"%s\",\"running\":%d,"
                 "\"finished\":%d,\"sources\":%d,\"files\":%llu,\"dirs\":%llu,"
                 "\"bytes\":%llu,\"errors\":%llu,\"current\":\"%s\","
                 "\"message\":\"%s\",\"mounts\":%s}",
                 g_usb, gr, title, running, finished, sources,
                 (unsigned long long)files, (unsigned long long)dirs,
                 (unsigned long long)bytes, (unsigned long long)errors,
                 cur, msg, mounts);
        send_response(fd, "200 OK", "application/json", body);
        return;
    }

    send_response(fd, "404 Not Found", "text/plain", "not found");
}

int web_serve(void) {
    struct sockaddr_in a;
    int srv = -1, port = 0, one = 1;

    for (int i = 0; i < WEB_PORT_TRIES; i++) {
        port = WEB_PORT_FIRST + i;
        if ((srv = socket(AF_INET, SOCK_STREAM, 0)) < 0) continue;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        memset(&a, 0, sizeof(a));
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port        = htons((uint16_t)port);
        if (bind(srv, (struct sockaddr *)&a, sizeof(a)) == 0 &&
            listen(srv, 8) == 0)
            break;
        close(srv);
        srv = -1;
    }
    if (srv < 0) {
        LOG("FATAL: could not bind any port in %d..%d",
            WEB_PORT_FIRST, WEB_PORT_FIRST + WEB_PORT_TRIES - 1);
        notify("DLC Dump: could not start web UI");
        return -1;
    }

    {
        char ip[64] = "";
        if (get_local_ip(ip, sizeof(ip)) == 0) {
            LOG("Web UI: http://%s:%d", ip, port);
            notify("DLC Dumper ready\nhttp://%s:%d", ip, port);
        } else {
            LOG("Web UI listening on port %d", port);
            notify("DLC Dumper ready on port %d", port);
        }
        LOG("Leave this running, start the game, enter the DLC, then press Dump.");
    }

    pthread_mutex_lock(&g_wslock);
    snprintf(g_ws.message, sizeof(g_ws.message),
             "Idle. Start the game and enter the DLC content, then press Dump.");
    pthread_mutex_unlock(&g_wslock);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        handle(fd);
        close(fd);
    }
    close(srv);
    return 0;
}
