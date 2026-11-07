/* SPDX-License-Identifier: GPL-3.0-or-later
 * dev-control: resident dev daemon for PS5 homebrew iteration.
 *
 * Listens on TCP 9098, one command per connection, one line each:
 *   PING                 -> "OK:PONG\n"
 *   LIST                 -> "proc pid=.. comm=.. title=.. stat=.. nthr=..\n"*
 *                           then "OK:LIST n=..\n"
 *   KILL:ACTIVE          -> kill the running big app (see below)
 *   KILL:PPSAxxxxx       -> kill the app with that title id
 *   KILL:<pid>           -> SIGKILL that pid
 *   KILL:<comm|title>    -> SIGKILL every process whose comm or title matches
 *   SHOT                 -> "OK:SHOT <png|jpg> <bytes>\n" + raw file bytes, or
 *                           "ERR:NO_SCREENSHOT\n" (see screenshot note)
 *
 * Kill strategy (two stages, best effort, never crashes on a missing symbol):
 *   Stage A: load libSceSystemService.sprx and dlsym the LncUtil /
 *     SystemService kill path, so a foreground app is asked to exit
 *     through the system instead of being shot mid-frame. Symbol names
 *     were checked against the SDK v0.44 libSceSystemService.so stub:
 *     sceLncUtilGetAppIdOfRunningBigApp, sceSystemServiceGetAppIdOfRunningBigApp,
 *     sceLncUtilGetAppId, sceLncUtilKillApp, sceSystemServiceKillApp.
 *     There is no separate libSceLncUtil.sprx stub in this SDK, and no
 *     sceSystemServiceGetAppIdOfBigApp symbol, so neither is used.
 *   Stage B: reuse the sysctl process enumeration from process-snapshot /
 *     process-watch (13.60 kinfo_proc offsets: pid 72, ppid 76, stat 388,
 *     comm 447, nthr 596). There is no struct kinfo_proc in this SDK's
 *     headers, so records are parsed by offset, exactly like the other
 *     payloads. Matches are SIGKILLed, except the daemon itself and
 *     Shell/SysCore processes, which are refused (killing the shell
 *     bricks the session).
 *
 * Screenshot note (honest limitation): this SDK (v0.44) ships no
 * screenshot library and no validated live-framebuffer source, and this
 * repository contains no framebuffer capture path. SHOT therefore does
 * NOT read a framebuffer and does NOT invent a raw file: it streams the
 * newest system screenshot file found under /user/av_contents/photo/
 * (searched recursively: real screenshots sit at
 * photo/NPXS40087/<TITLE_ID>/<3-hex>/<stamp>.jpg, verified on console
 * 2026-11-07). Take one with the Create button first. If no .png/.jpg
 * is found it returns ERR:NO_SCREENSHOT. A live framebuffer capture
 * needs a validated capture source first and is explicitly out of scope
 * for this payload.
 */
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEV_CTL_PORT 9098
#define MAX_BYTES (4u * 1024u * 1024u)
#define MAX_PROCS 512
#define PID_OFFSET 72u
#define PPID_OFFSET 76u
#define STAT_OFFSET 388u
#define COMM_OFFSET 447u
#define NTHR_OFFSET 596u
#define PROC_ONLY 8
#define MAX_SHOT_BYTES (UINT64_C(64) << 20)
#define SHOT_DIR "/user/av_contents/photo"

struct app_info {
    uint32_t app_id;
    uint64_t unknown1;
    uint32_t app_type;
    char title_id[10];
    unsigned char spare[236];
};

/* Declared by hand, like process-snapshot does for sceKernelGetAppInfo:
 * the SDK ships these as libkernel stubs with no public header. */
int sceKernelLoadStartModule(const char *name, size_t argc, const void *argv,
                             uint32_t flags, void *opt, int *res);
int sceKernelDlsym(int handle, const char *symbol, void **addr);
int sceKernelGetAppInfo(pid_t pid, struct app_info *info);

struct ent {
    uint32_t pid;
    uint32_t ppid;
    uint32_t nthr;
    uint8_t stat;
    char comm[21];
    char title[11];
};

static uint32_t word32(const unsigned char *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void safe_string(char *out, size_t out_size,
                        const unsigned char *in, size_t in_size) {
    size_t i = 0;
    while (i < in_size && i + 1 < out_size && in[i]) {
        out[i] = in[i] >= 32 && in[i] < 127 ? (char)in[i] : '?';
        ++i;
    }
    out[i] = 0;
}

static int send_all(int fd, const void *data, size_t len) {
    const unsigned char *p = data;
    while (len) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int reply(int fd, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if ((size_t)n >= sizeof(buf))
        n = (int)sizeof(buf) - 1;
    return send_all(fd, buf, (size_t)n);
}

/* One sysctl query; caller frees the buffer. Returns 0 on success. */
static int query_procs(unsigned char **out, size_t *out_n) {
    int mib[3] = { CTL_KERN, KERN_PROC, PROC_ONLY };
    for (int attempt = 0; attempt < 3; ++attempt) {
        size_t need = 0;
        errno = 0;
        if (sysctl(mib, 3, NULL, &need, NULL, 0))
            return -1;
        if (!need || need > MAX_BYTES - 65536)
            return -1;
        size_t cap = need + 65536;
        unsigned char *p = calloc(1, cap);
        if (!p)
            return -1;
        size_t n = cap;
        errno = 0;
        int rc = sysctl(mib, 3, p, &n, NULL, 0);
        int error = errno;
        if (!rc && n <= cap) {
            *out = p;
            *out_n = n;
            return 0;
        }
        free(p);
        if (error != ENOMEM)
            return -1;
    }
    return -1;
}

static int collect_procs(struct ent *ents, int max, int *truncated) {
    unsigned char *p = NULL;
    size_t n = 0;
    if (query_procs(&p, &n))
        return -1;
    size_t at = 0;
    int count = 0;
    *truncated = 0;
    while (at + 4 <= n) {
        size_t stride = word32(p + at);
        if (stride < COMM_OFFSET + 20 || stride > n - at) {
            free(p);
            return -2;
        }
        if (count < max) {
            struct ent *e = &ents[count];
            e->pid = word32(p + at + PID_OFFSET);
            e->ppid = word32(p + at + PPID_OFFSET);
            e->nthr = word32(p + at + NTHR_OFFSET);
            e->stat = p[at + STAT_OFFSET];
            safe_string(e->comm, sizeof(e->comm), p + at + COMM_OFFSET, 20);
            struct app_info info;
            memset(&info, 0, sizeof(info));
            if (sceKernelGetAppInfo((pid_t)e->pid, &info) == 0)
                safe_string(e->title, sizeof(e->title),
                            (unsigned char *)info.title_id, 10);
            else
                e->title[0] = 0;
        } else {
            *truncated = 1;
        }
        at += stride;
        ++count;
    }
    free(p);
    return count;
}

static int is_protected(const char *comm) {
    return strstr(comm, "Shell") != NULL || strstr(comm, "SysCore") != NULL ||
           strstr(comm, "SceShell") != NULL;
}

static int is_all_digits(const char *s) {
    if (!*s)
        return 0;
    for (; *s; ++s)
        if (*s < '0' || *s > '9')
            return 0;
    return 1;
}

/* Stage A: system-level kill. Returns 1 if the app was killed,
 * 0 if the path exists but did not kill it, -1 if unavailable. */
static int kill_via_system(const char *target, int *app_id_out) {
    int handle = sceKernelLoadStartModule("libSceSystemService.sprx", 0, NULL,
                                          0, NULL, NULL);
    if (handle < 0)
        return -1;

    int (*get_running_lnc)(void) = NULL;
    int (*get_running_sys)(void) = NULL;
    int (*get_app_id)(const char *title_id) = NULL;
    int (*kill_app_lnc)(int app_id) = NULL;
    int (*kill_app_sys)(int app_id) = NULL;
    sceKernelDlsym(handle, "sceLncUtilGetAppIdOfRunningBigApp",
                   (void **)&get_running_lnc);
    sceKernelDlsym(handle, "sceSystemServiceGetAppIdOfRunningBigApp",
                   (void **)&get_running_sys);
    sceKernelDlsym(handle, "sceLncUtilGetAppId", (void **)&get_app_id);
    sceKernelDlsym(handle, "sceLncUtilKillApp", (void **)&kill_app_lnc);
    sceKernelDlsym(handle, "sceSystemServiceKillApp", (void **)&kill_app_sys);
    if (!kill_app_lnc && !kill_app_sys)
        return -1;

    int app_id = -1;
    if (strcmp(target, "ACTIVE") == 0) {
        if (get_running_lnc)
            app_id = get_running_lnc();
        if (app_id <= 0 && get_running_sys)
            app_id = get_running_sys();
    } else if (strncmp(target, "PPSA", 4) == 0 && get_app_id) {
        app_id = get_app_id(target);
    }
    if (app_id <= 0)
        return 0;
    *app_id_out = app_id;
    if (kill_app_lnc && kill_app_lnc(app_id) == 0)
        return 1;
    if (kill_app_sys && kill_app_sys(app_id) == 0)
        return 1;
    return 0;
}

/* Stage B: SIGKILL by pid / comm / title. Returns killed count,
 * -1 on query/layout failure, -2 if the target is protected. */
static int kill_via_sysctl(const char *target, uint32_t *first_pid) {
    struct ent ents[MAX_PROCS];
    int truncated = 0;
    int count = collect_procs(ents, MAX_PROCS, &truncated);
    if (count < 0)
        return -1;

    int numeric = is_all_digits(target);
    long want_pid = numeric ? strtol(target, NULL, 10) : -1;
    int killed = 0;
    for (int i = 0; i < count && i < MAX_PROCS; ++i) {
        int match = 0;
        if (numeric && (long)ents[i].pid == want_pid)
            match = 1;
        else if (!numeric && (strcmp(ents[i].comm, target) == 0 ||
                              (ents[i].title[0] &&
                               strcmp(ents[i].title, target) == 0)))
            match = 1;
        if (!match)
            continue;
        if ((pid_t)ents[i].pid == getpid())
            continue;
        if (is_protected(ents[i].comm))
            return -2;
        if (kill((pid_t)ents[i].pid, SIGKILL) == 0) {
            if (!killed)
                *first_pid = ents[i].pid;
            ++killed;
        }
    }
    (void)truncated;
    return killed;
}

static int handle_kill(int fd, const char *target) {
    if (!target[0])
        return reply(fd, "ERR:EMPTY_TARGET\n");

    /* ACTIVE / PPSAxxxx go through the system path first. */
    if (strcmp(target, "ACTIVE") == 0 || strncmp(target, "PPSA", 4) == 0) {
        int app_id = -1;
        int r = kill_via_system(target, &app_id);
        if (r == 1)
            return reply(fd, "OK:KILLED app=%d\n", app_id);
        /* Fall through to sysctl: ACTIVE matches eboot.bin, a title id
         * matches the title recorded per process. */
        uint32_t first_pid = 0;
        const char *fallback =
            strcmp(target, "ACTIVE") == 0 ? "eboot.bin" : target;
        int killed = kill_via_sysctl(fallback, &first_pid);
        if (killed > 0)
            return reply(fd, "OK:KILLED pid=%u count=%d\n", first_pid,
                         killed);
        if (killed == -2)
            return reply(fd, "ERR:REFUSED\n");
        return reply(fd, "ERR:NOT_FOUND\n");
    }

    uint32_t first_pid = 0;
    int killed = kill_via_sysctl(target, &first_pid);
    if (killed > 0)
        return reply(fd, "OK:KILLED pid=%u count=%d\n", first_pid, killed);
    if (killed == -2)
        return reply(fd, "ERR:REFUSED\n");
    return reply(fd, "ERR:NOT_FOUND\n");
}

static int handle_list(int fd) {
    struct ent ents[MAX_PROCS];
    int truncated = 0;
    int count = collect_procs(ents, MAX_PROCS, &truncated);
    if (count < 0)
        return reply(fd, "ERR:QUERY_FAILED\n");
    int shown = count < MAX_PROCS ? count : MAX_PROCS;
    for (int i = 0; i < shown; ++i) {
        if (reply(fd, "proc pid=%u ppid=%u stat=%u nthr=%u comm=%s title=%s\n",
                  ents[i].pid, ents[i].ppid, ents[i].stat, ents[i].nthr,
                  ents[i].comm, ents[i].title) < 0)
            return -1;
    }
    return reply(fd, "OK:LIST n=%d%s\n", count,
                 truncated ? " truncated" : "");
}

static int has_ext(const char *name, const char *ext) {
    size_t nl = strlen(name), el = strlen(ext);
    if (nl < el)
        return 0;
    const char *tail = name + nl - el;
    for (size_t i = 0; i < el; ++i) {
        char a = tail[i], b = ext[i];
        if (a >= 'A' && a <= 'Z')
            a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z')
            b += 'a' - 'A';
        if (a != b)
            return 0;
    }
    return 1;
}

/* Screenshots live two levels below SHOT_DIR on a real console:
 * /user/av_contents/photo/NPXS40087/<TITLE_ID>/<3-hex>/<stamp>.jpg
 * (verified on the base-model PS5 2026-11-07 via FTP). Scan recursively
 * so the top-level-only search does not miss every real screenshot. */
static char g_best[576];
static char g_best_fmt[8];
static time_t g_best_mtime;
static int g_scanned;

static void scan_shot_dir(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (g_scanned > 8192)
            break;
        ++g_scanned;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char path[576];
        int path_len = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (path_len < 0 || (size_t)path_len >= sizeof(path))
            continue;
        struct stat st;
        if (stat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth > 0)
                scan_shot_dir(path, depth - 1);
        } else if (S_ISREG(st.st_mode)) {
            const char *fmt = NULL;
            if (has_ext(e->d_name, ".png"))
                fmt = "png";
            else if (has_ext(e->d_name, ".jpg") ||
                     has_ext(e->d_name, ".jpeg"))
                fmt = "jpg";
            if (!fmt)
                continue;
            if (!g_best[0] || st.st_mtime > g_best_mtime) {
                snprintf(g_best, sizeof(g_best), "%s", path);
                snprintf(g_best_fmt, sizeof(g_best_fmt), "%s", fmt);
                g_best_mtime = st.st_mtime;
            }
        }
    }
    closedir(d);
}

static int handle_shot(int fd) {
    g_best[0] = 0;
    g_best_fmt[0] = 0;
    g_best_mtime = 0;
    g_scanned = 0;
    scan_shot_dir(SHOT_DIR, 3);
    if (!g_best[0])
        return reply(fd, "ERR:NO_SCREENSHOT\n");
    const char *best = g_best;
    const char *best_fmt = g_best_fmt;

    int file = open(best, O_RDONLY | O_NOFOLLOW);
    if (file < 0)
        return reply(fd, "ERR:NO_SCREENSHOT\n");
    struct stat st;
    if (fstat(file, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size > MAX_SHOT_BYTES) {
        close(file);
        return reply(fd, "ERR:NO_SCREENSHOT\n");
    }
    if (reply(fd, "OK:SHOT %s %llu\n", best_fmt,
              (unsigned long long)st.st_size) < 0) {
        close(file);
        return -1;
    }
    unsigned char buf[65536];
    uint64_t sent = 0;
    uint64_t size = (uint64_t)st.st_size;
    while (sent < size) {
        size_t want = size - sent < sizeof(buf) ? (size_t)(size - sent)
                                                : sizeof(buf);
        ssize_t n = read(file, buf, want);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        if (send_all(fd, buf, (size_t)n) < 0)
            break;
        sent += (uint64_t)n;
    }
    close(file);
    return sent == size ? 0 : -1;
}

static int read_line(int fd, char *buf, size_t cap) {
    size_t len = 0;
    while (len + 1 < cap) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return len ? (int)len : -1;
        if (c == '\n')
            break;
        buf[len++] = c;
    }
    buf[len] = 0;
    return (int)len;
}

static void handle_client(int fd) {
    struct timeval timeout = { 30, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    char cmd[256];
    if (read_line(fd, cmd, sizeof(cmd)) < 0)
        return;
    size_t len = strlen(cmd);
    while (len && (cmd[len - 1] == '\r' || cmd[len - 1] == '\n' ||
                   cmd[len - 1] == ' ' || cmd[len - 1] == '\t'))
        cmd[--len] = 0;

    if (strcmp(cmd, "PING") == 0) {
        reply(fd, "OK:PONG\n");
    } else if (strcmp(cmd, "LIST") == 0) {
        handle_list(fd);
    } else if (strncmp(cmd, "KILL:", 5) == 0) {
        handle_kill(fd, cmd + 5);
    } else if (strcmp(cmd, "SHOT") == 0) {
        handle_shot(fd);
    } else {
        reply(fd, "ERR:UNKNOWN_CMD\n");
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct sigaction ignore = { .sa_handler = SIG_IGN };
    sigaction(SIGPIPE, &ignore, 0);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        return 1;
    }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(DEV_CTL_PORT);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(srv, 4) < 0) {
        perror("bind/listen");
        close(srv);
        return 1;
    }
    printf("dev-control: ready pid=%d port=%d\n", (int)getpid(), DEV_CTL_PORT);
    for (;;) {
        int client = accept(srv, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        handle_client(client);
        close(client);
    }
    close(srv);
    return 0;
}
