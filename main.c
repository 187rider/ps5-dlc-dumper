/* ps5-dlc-dumper — copy decrypted, mounted DLC straight to USB.
 *
 * Runs as a PS5 payload via ps5-payload-elfldr (port 9021).
 *
 * It looks in two places for DLC that the running game has mounted:
 *   1. /mnt/sandbox/pfsmnt/<CONTENTID>-ac     <- the canonical decrypted DLC mount
 *   2. /mnt/sandbox/<TITLEID>_000/addcont*    <- the in-sandbox addcont mount points
 * and recursively copies whatever it finds to /mnt/usb0/PS5_DLC_DUMP/.
 *
 * The game MUST be running with its DLC mounted (kstuff loaded before launch),
 * otherwise there is nothing decrypted to read and this will find zero sources.
 *
 * Optional argv[1] = case-insensitive substring filter, e.g. FALLOUT4DLC00003
 *
 * Dump content you own, from your own console.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__has_include)
#  if __has_include(<ps5/klog.h>)
#    include <ps5/klog.h>
#    define HAVE_KLOG 1
#  endif
#endif

/* Overridable at build time so the logic can be tested off-console:
 *   make CFLAGS="-DPFSMNT=\\"/tmp/t/pfsmnt\\" ..." */
#ifndef PFSMNT
#define PFSMNT      "/mnt/sandbox/pfsmnt"
#endif
#ifndef SANDBOX
#define SANDBOX     "/mnt/sandbox"
#endif
#ifndef USBBASE
#define USBBASE     "/mnt/usb"
#endif
#define OUTDIRNAME  "PS5_DLC_DUMP"
#define COPYBUF     (4 * 1024 * 1024)
#define MAXDEPTH    32
#define MAXPATH     1024

/* ---- PS5 toast notification (weak: harmless if the symbol is absent) ---- */
typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

static FILE *g_log = NULL;

static void LOG(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    printf("%s\n", buf);
    fflush(stdout);
#ifdef HAVE_KLOG
    klog_printf("[dlc_dump] %s\n", buf);
#endif
    if (g_log) { fprintf(g_log, "%s\n", buf); fflush(g_log); }
}

#if defined(SIM) || (defined(__APPLE__) && !defined(__PROSPERO__))
static void notify(const char *fmt, ...) {
    (void)fmt;
}
#else
extern int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int)
    __attribute__((weak));

static void notify(const char *fmt, ...) {
    notify_request_t req;
    va_list ap;

    if (sceKernelSendNotificationRequest == NULL) return;

    memset(&req, 0, sizeof(req));
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, ap);
    va_end(ap);

    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}
#endif

/* Provided by web.c (included near the bottom of this file). */
void web_progress(const char *path, uint64_t files, uint64_t dirs,
                  uint64_t bytes, uint64_t errors);
int  web_serve(void);

/* ------------------------------ helpers ------------------------------ */

static int is_dir(const char *p) {
    struct stat st;
    if (stat(p, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

static int joinpath(char *out, size_t n, const char *a, const char *b) {
    int r = snprintf(out, n, "%s/%s", a, b);
    return (r > 0 && (size_t)r < n) ? 0 : -1;
}

static int mkdir_p(const char *path) {
    char tmp[MAXPATH];
    size_t len;

    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) return -1;
    len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int str_endswith(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    return (ls >= lf) && (strcmp(s + ls - lf, suf) == 0);
}

static int str_startswith(const char *s, const char *pre) {
    return strncmp(s, pre, strlen(pre)) == 0;
}

static int str_icontains(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return 1;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

/* ------------------------------ counters ------------------------------ */

typedef struct {
    uint64_t files;
    uint64_t dirs;
    uint64_t bytes;
    uint64_t errors;
} stats_t;

/* ------------------------------ copying ------------------------------ */

static int copy_file(const char *src, const char *dst, void *buf, stats_t *st) {
    int fs = -1, fd = -1, rc = -1;
    ssize_t n;
    uint64_t total = 0, nextmark = 256ull * 1024 * 1024;

    web_progress(src, st->files, st->dirs, st->bytes, st->errors);

    if ((fs = open(src, O_RDONLY, 0)) < 0) {
        LOG("  ! open src failed (%s): %s", strerror(errno), src);
        st->errors++;
        return -1;
    }
    if ((fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0777)) < 0) {
        LOG("  ! open dst failed (%s): %s", strerror(errno), dst);
        close(fs);
        st->errors++;
        return -1;
    }

    for (;;) {
        n = read(fs, buf, COPYBUF);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG("  ! read failed (%s): %s", strerror(errno), src);
            st->errors++;
            goto out;
        }
        {
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = write(fd, (char *)buf + off, (size_t)(n - off));
                if (w < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EFBIG || errno == EINVAL) {
                        LOG("  ! write failed (%s) - file >4GB on FAT32? Reformat USB "
                            "as exFAT: %s", strerror(errno), dst);
                    } else if (errno == ENOSPC) {
                        LOG("  ! USB IS FULL while writing: %s", dst);
                    } else {
                        LOG("  ! write failed (%s): %s", strerror(errno), dst);
                    }
                    st->errors++;
                    goto out;
                }
                off += w;
            }
        }
        total += (uint64_t)n;
        if (total >= nextmark) {
            LOG("    ... %llu MiB", (unsigned long long)(total / (1024 * 1024)));
            nextmark += 256ull * 1024 * 1024;
            web_progress(src, st->files, st->dirs, st->bytes + total, st->errors);
        }
    }

    rc = 0;
    st->files++;
    st->bytes += total;

out:
    if (fd >= 0) close(fd);
    if (fs >= 0) close(fs);
    if (rc < 0 && fd >= 0) unlink(dst);
    return rc;
}

static int copy_tree(const char *src, const char *dst, int depth,
                     void *buf, stats_t *st) {
    DIR *d;
    struct dirent *e;
    char sp[MAXPATH], dp[MAXPATH];

    if (depth > MAXDEPTH) {
        LOG("  ! max depth reached, skipping: %s", src);
        st->errors++;
        return -1;
    }
    if (mkdir_p(dst) != 0) {
        LOG("  ! mkdir failed (%s): %s", strerror(errno), dst);
        st->errors++;
        return -1;
    }
    st->dirs++;

    if (!(d = opendir(src))) {
        LOG("  ! opendir failed (%s): %s", strerror(errno), src);
        st->errors++;
        return -1;
    }

    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (joinpath(sp, sizeof(sp), src, e->d_name) != 0) continue;
        if (joinpath(dp, sizeof(dp), dst, e->d_name) != 0) continue;

        if (is_dir(sp)) {
            copy_tree(sp, dp, depth + 1, buf, st);
        } else {
            copy_file(sp, dp, buf, st);
        }
    }
    closedir(d);
    return 0;
}

/* ------------------------------ discovery ------------------------------ */

static char g_usb[64];

static int find_usb(void) {
    char probe[128];
    for (int i = 0; i < 8; i++) {
        char candidate[64];
        snprintf(candidate, sizeof(candidate), "%s%d", USBBASE, i);
        if (!is_dir(candidate)) continue;
        /* verify writability */
        snprintf(probe, sizeof(probe), "%s/.dlcdump_wtest", candidate);
        int fd = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0777);
        if (fd >= 0) {
            close(fd);
            unlink(probe);
            snprintf(g_usb, sizeof(g_usb), "%s", candidate);
            return 0;
        }
    }
    g_usb[0] = '\0';
    return -1;
}

/* Copy every *-ac mount from /mnt/sandbox/pfsmnt. Returns count handled. */
static int scan_pfsmnt(const char *outroot, const char *filter,
                       void *buf, stats_t *st) {
    DIR *d;
    struct dirent *e;
    char sp[MAXPATH], dp[MAXPATH];
    int found = 0;

    if (!(d = opendir(PFSMNT))) {
        LOG("Cannot open %s (%s) - is a game running?", PFSMNT, strerror(errno));
        return 0;
    }

    LOG("Scanning %s ...", PFSMNT);
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        LOG("  seen: %s", e->d_name);

        if (!str_endswith(e->d_name, "-ac")) continue;      /* DLC only */
        if (str_endswith(e->d_name, "-nest")) continue;     /* PFS internals */
        if (str_endswith(e->d_name, "-union")) continue;
        if (filter && !str_icontains(e->d_name, filter)) continue;

        if (joinpath(sp, sizeof(sp), PFSMNT, e->d_name) != 0) continue;
        if (joinpath(dp, sizeof(dp), outroot, e->d_name) != 0) continue;
        if (!is_dir(sp)) continue;

        LOG("");
        LOG("==> DLC: %s", e->d_name);
        LOG("    -> %s", dp);
        copy_tree(sp, dp, 0, buf, st);
        found++;
    }
    closedir(d);
    return found;
}

/* Fallback: copy <TITLEID>_000/addcont* mount points from live sandboxes. */
static int scan_sandbox_addcont(const char *outroot, const char *filter,
                                void *buf, stats_t *st) {
    DIR *d, *sd;
    struct dirent *e, *se;
    char sbox[MAXPATH], sp[MAXPATH], dp[MAXPATH], label[MAXPATH];
    int found = 0;

    if (!(d = opendir(SANDBOX))) return 0;

    LOG("Scanning %s for addcont mount points ...", SANDBOX);
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        /* Skip Sony system apps; we only care about PPSA/CUSA game sandboxes. */
        if (!str_startswith(e->d_name, "PPSA") && !str_startswith(e->d_name, "CUSA"))
            continue;
        if (joinpath(sbox, sizeof(sbox), SANDBOX, e->d_name) != 0) continue;
        if (!is_dir(sbox)) continue;

        LOG("  game sandbox: %s", e->d_name);
        if (!(sd = opendir(sbox))) continue;

        while ((se = readdir(sd))) {
            if (!str_startswith(se->d_name, "addcont")) continue;
            if (joinpath(sp, sizeof(sp), sbox, se->d_name) != 0) continue;
            if (!is_dir(sp)) continue;
            if (filter && !str_icontains(se->d_name, filter) &&
                !str_icontains(e->d_name, filter)) continue;

            snprintf(label, sizeof(label), "%s_%s", e->d_name, se->d_name);
            if (joinpath(dp, sizeof(dp), outroot, label) != 0) continue;

            LOG("");
            LOG("==> addcont: %s/%s", e->d_name, se->d_name);
            LOG("    -> %s", dp);
            copy_tree(sp, dp, 0, buf, st);
            found++;
        }
        closedir(sd);
    }
    closedir(d);
    return found;
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

#ifdef SIM
static const char *const ADDCONT_ROOTS[] = {
    "/tmp/ps5sim/user/addcont",
    "/tmp/ps5sim/ext1/user/addcont",
    NULL
};
static const char *const APPMETA_ROOTS[] = {
    "/tmp/ps5sim/user/appmeta/addcont",
    NULL
};
#else
static const char *const ADDCONT_ROOTS[] = {
    "/user/addcont",
    "/mnt/ext0/user/addcont",
    "/mnt/ext1/user/addcont",
    "/mnt/ext2/user/addcont",
    "/mnt/ext3/user/addcont",
    "/mnt/ext4/user/addcont",
    "/mnt/ext5/user/addcont",
    "/mnt/ext6/user/addcont",
    "/mnt/ext7/user/addcont",
    NULL
};

static const char *const APPMETA_ROOTS[] = {
    "/user/appmeta/addcont",
    "/mnt/ext0/user/appmeta/addcont",
    "/mnt/ext1/user/appmeta/addcont",
    "/mnt/ext2/user/appmeta/addcont",
    "/mnt/ext3/user/appmeta/addcont",
    NULL
};
#endif

/* Extract Content ID from ac.json (e.g. EP3969-PPSA01769_00-DLC0000000000020) */
static int extract_content_id_from_ac_json(const char *json_path, char *out_cid, size_t outsz) {
    int fd = open(json_path, O_RDONLY, 0);
    if (fd < 0) return -1;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    const char *p = strstr(buf, "ac/");
    int adv = 3;
    if (!p) {
        p = strstr(buf, "ac\\/");
        adv = 4;
    }
    if (p) {
        p += adv;
        size_t len = 0;
        while (p[len] && p[len] != '/' && p[len] != '\\' && p[len] != '"' && len + 1 < outsz) {
            out_cid[len] = p[len];
            len++;
        }
        out_cid[len] = '\0';
        if (len >= 16) return 0;
    }

    /* Fallback: search for standard Content ID pattern like EPxxxx- or UPxxxx- */
    for (ssize_t i = 0; i < n - 16; i++) {
        if ((buf[i] == 'E' || buf[i] == 'U' || buf[i] == 'J' || buf[i] == 'H') &&
            buf[i+1] == 'P' && isdigit((unsigned char)buf[i+2]) &&
            isdigit((unsigned char)buf[i+3]) && isdigit((unsigned char)buf[i+4]) &&
            buf[i+5] == '-') {
            size_t len = 0;
            const char *s = buf + i;
            while (s[len] && (isalnum((unsigned char)s[len]) || s[len] == '-' || s[len] == '_') &&
                   len + 1 < outsz) {
                out_cid[len] = s[len];
                len++;
            }
            out_cid[len] = '\0';
            if (len >= 16) return 0;
        }
    }
    return -1;
}

/* Check if a DLC is already mounted decrypted in /mnt/sandbox/pfsmnt */
static int is_mounted_in_pfsmnt(const char *cid, const char *dlc_dir) {
    DIR *d = opendir(PFSMNT);
    if (!d) return 0;
    struct dirent *e;
    int mounted = 0;
    while ((e = readdir(d))) {
        if (!str_endswith(e->d_name, "-ac")) continue;
        if (cid && cid[0] && str_icontains(e->d_name, cid)) {
            mounted = 1;
            break;
        }
        if (dlc_dir && dlc_dir[0] && str_icontains(e->d_name, dlc_dir)) {
            mounted = 1;
            break;
        }
    }
    closedir(d);
    return mounted;
}

/* Base title ID like "PPSA01769" from running game, or empty string */
static int get_running_title_prefix(char *out, size_t outsz) {
    char title[64];
    if (outsz == 0) return 0;
    out[0] = '\0';
    if (!game_running(title, sizeof(title))) return 0;
    size_t i = 0;
    while (title[i] && title[i] != '_' && i + 1 < outsz) {
        out[i] = title[i];
        i++;
    }
    out[i] = '\0';
    return 1;
}

/* Copy installed addcont packages (e.g. Hitman 3 unlock/license PKGs) from disk.
 * Returns count of DLC directories dumped. */
static int scan_installed_addcont(const char *outroot, const char *filter,
                                  void *buf, stats_t *st) {
    char active_title[32] = "";
    int has_running = get_running_title_prefix(active_title, sizeof(active_title));
    int found = 0;

    LOG("Scanning installed addcont directories ...");
    if (has_running) {
        LOG("  active game: %s", active_title);
    }

    for (int r = 0; ADDCONT_ROOTS[r]; r++) {
        const char *root = ADDCONT_ROOTS[r];
        DIR *rd = opendir(root);
        if (!rd) continue;

        struct dirent *te;
        while ((te = readdir(rd))) {
            if (!strcmp(te->d_name, ".") || !strcmp(te->d_name, "..")) continue;
            if (!str_startswith(te->d_name, "PPSA") && !str_startswith(te->d_name, "CUSA"))
                continue;

            /* If a game is running and no filter is active, only scan DLCs for that game. */
            if (has_running && (!filter || !filter[0])) {
                if (strcmp(te->d_name, active_title) != 0) continue;
            }

            char tpath[MAXPATH];
            if (joinpath(tpath, sizeof(tpath), root, te->d_name) != 0) continue;
            if (!is_dir(tpath)) continue;

            DIR *td = opendir(tpath);
            if (!td) continue;

            struct dirent *de;
            while ((de = readdir(td))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;

                char dlc_src[MAXPATH];
                if (joinpath(dlc_src, sizeof(dlc_src), tpath, de->d_name) != 0) continue;
                if (!is_dir(dlc_src)) continue;

                /* Determine Content ID from ac.json if available */
                char json_path[MAXPATH], cid[128] = "";
                if (joinpath(json_path, sizeof(json_path), dlc_src, "ac.json") == 0) {
                    extract_content_id_from_ac_json(json_path, cid, sizeof(cid));
                }
                if (cid[0] == '\0') {
                    snprintf(cid, sizeof(cid), "%s_%s", te->d_name, de->d_name);
                }

                /* Skip if already mounted decrypted in pfsmnt (e.g. Fallout 4) */
                if (is_mounted_in_pfsmnt(cid, de->d_name)) {
                    LOG("  skip %s (already mounted in pfsmnt)", cid);
                    continue;
                }

                /* Check filter */
                if (filter && filter[0]) {
                    if (!str_icontains(cid, filter) &&
                        !str_icontains(de->d_name, filter) &&
                        !str_icontains(te->d_name, filter)) {
                        continue;
                    }
                }

                char dp[MAXPATH];
                if (joinpath(dp, sizeof(dp), outroot, cid) != 0) continue;

                LOG("");
                LOG("==> Installed DLC: %s (%s)", cid, dlc_src);
                LOG("    -> %s", dp);
                copy_tree(dlc_src, dp, 0, buf, st);

                /* Copy companion appmeta (icon0.png, param.json) if it exists */
                for (int m = 0; APPMETA_ROOTS[m]; m++) {
                    char meta_src[MAXPATH];
                    snprintf(meta_src, sizeof(meta_src), "%s/%s/%s",
                             APPMETA_ROOTS[m], te->d_name, de->d_name);
                    if (is_dir(meta_src)) {
                        char meta_dst[MAXPATH];
                        snprintf(meta_dst, sizeof(meta_dst), "%s/sce_sys", dp);
                        copy_tree(meta_src, meta_dst, 0, buf, st);

                        char icon_src[MAXPATH], icon_dst[MAXPATH];
                        snprintf(icon_src, sizeof(icon_src), "%s/icon0.png", meta_src);
                        snprintf(icon_dst, sizeof(icon_dst), "%s/icon0.png", dp);
                        struct stat ist;
                        if (stat(icon_src, &ist) == 0) {
                            copy_file(icon_src, icon_dst, buf, st);
                        }
                        break;
                    }
                }

                found++;
            }
            closedir(td);
        }
        closedir(rd);
    }
    return found;
}

/* ------------------------------ dump driver ------------------------------ */

/* Returns: -1 no writable USB, otherwise the number of sources dumped. */
int run_dump(const char *filter, stats_t *st) {
    char   outroot[128], logpath[160];
    void  *buf;
    int    found;
    time_t t0, t1;

    if (find_usb() != 0) {
        LOG("FATAL: no writable USB found at %s0..%s7.", USBBASE, USBBASE);
        LOG("       Plug in an exFAT-formatted drive and let the PS5 mount it.");
        return -1;
    }

    snprintf(outroot, sizeof(outroot), "%s/%s", g_usb, OUTDIRNAME);
    if (mkdir_p(outroot) != 0) {
        LOG("FATAL: cannot create %s (%s)", outroot, strerror(errno));
        return -1;
    }

    /* Reopen the log if it was never opened, OR if the file it points at has
     * vanished (USB replug, dump dir deleted). Without this the handle goes
     * stale and every later log line is silently written to a dead inode. */
    snprintf(logpath, sizeof(logpath), "%s/dump.log", outroot);
    if (g_log) {
        struct stat lst;
        if (stat(logpath, &lst) != 0) {
            fclose(g_log);
            g_log = NULL;
        }
    }
    if (!g_log) g_log = fopen(logpath, "a");

    if (!(buf = malloc(COPYBUF))) {
        LOG("FATAL: out of memory");
        return -1;
    }

    LOG("");
    LOG("==== dump run ====");
    LOG("USB    : %s", g_usb);
    LOG("Output : %s", outroot);
    if (filter) LOG("Filter : %s", filter);

    t0 = time(NULL);
    found = scan_pfsmnt(outroot, filter, buf, st);
    if (found == 0) {
        LOG("");
        LOG("No *-ac folders in pfsmnt. Trying sandbox addcont mount points ...");
        found += scan_sandbox_addcont(outroot, filter, buf, st);
    }
    LOG("");
    LOG("Scanning installed addcont (unlock / entitlement DLCs) ...");
    found += scan_installed_addcont(outroot, filter, buf, st);
    t1 = time(NULL);

    LOG("");
    LOG("======================================================");
    if (found == 0) {
        LOG("NOTHING DUMPED. No mounted DLC was found.");
        LOG("  - kstuff must be loaded BEFORE the game launches");
        LOG("  - the game must be running right now");
        LOG("  - enter the DLC content in-game (many titles mount lazily)");
    } else {
        LOG("Sources dumped : %d", found);
        LOG("Directories    : %llu", (unsigned long long)st->dirs);
        LOG("Files          : %llu", (unsigned long long)st->files);
        LOG("Bytes          : %llu (%.2f GiB)",
            (unsigned long long)st->bytes,
            (double)st->bytes / (1024.0 * 1024.0 * 1024.0));
        LOG("Errors         : %llu", (unsigned long long)st->errors);
        LOG("Elapsed        : %lld s", (long long)(t1 - t0));
    }
    LOG("======================================================");

    free(buf);
    return found;
}

#include "web.c"

/* ------------------------------ main ------------------------------ */

static void usage(void) {
    printf("ps5-dlc-dumper\n"
           "  (no args)          start the resident web UI (use this from the\n"
           "                     Homebrew Launcher, BEFORE starting the game)\n"
           "  --now [FILTER]     dump immediately; the game must ALREADY be\n"
           "                     running with its DLC mounted\n"
           "  FILTER             same as --now FILTER\n"
           "  --web              force the web UI\n");
}

int main(int argc, char **argv) {
    const char *filter   = NULL;
    int         headless = 0;
    int         web      = 0;
    stats_t     st;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--now") || !strcmp(argv[i], "-n")) {
            headless = 1;
        } else if (!strcmp(argv[i], "--web") || !strcmp(argv[i], "-w")) {
            web = 1;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage();
            return 0;
        } else if (argv[i][0]) {
            filter   = argv[i];
            headless = 1;   /* an explicit filter implies a one-shot dump */
        }
    }
    if (web) headless = 0;

    if (!headless) {
        /* Resident mode. Nothing needs to be running yet - that is the point. */
        LOG("==== ps5-dlc-dumper (web UI mode) ====");
        return web_serve();
    }

    /* One-shot mode: the game must already be running with DLC mounted. */
    memset(&st, 0, sizeof(st));
    LOG("==== ps5-dlc-dumper (one-shot mode) ====");
    notify("DLC Dump: started");

    int found = run_dump(filter, &st);

    if (found < 0) {
        notify("DLC Dump: no writable USB found");
    } else if (found == 0) {
        notify("DLC Dump: no mounted DLC found");
    } else {
        notify("DLC Dump: done - %llu files, %.2f GiB, %llu errors",
               (unsigned long long)st.files,
               (double)st.bytes / (1024.0 * 1024.0 * 1024.0),
               (unsigned long long)st.errors);
    }

    if (g_log) fclose(g_log);
    return (found > 0 && st.errors == 0) ? 0 : 1;
}
