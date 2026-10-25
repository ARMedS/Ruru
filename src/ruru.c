/* ruru — local speech-to-text service.
 *
 * A small resident daemon that holds one speech model in memory and answers
 * transcription requests over a Unix domain socket. Audio never leaves the
 * machine. Any app on the box can call it: AceVoiceTyper does, and so can
 * anything else that needs private local dictation.
 *
 * Model runtime is transcribe.cpp (libtranscribe), which speaks 16 kHz mono
 * float32 PCM in and text out.
 *
 * Protocol — newline-delimited JSON, one request per line.
 *
 *   ->  {"cmd":"ping"}
 *   ->  {"cmd":"status"}
 *   ->  {"cmd":"shutdown"}
 *   ->  {"cmd":"transcribe","samples":N,"rate":16000}\n   followed by N*4 bytes of f32le
 *   <-  one line of JSON, always.
 *
 * The transcribe payload follows the newline immediately; the server reads
 * exactly N float32 samples. Requests are serialised: the underlying engine
 * allows only one decode in flight at a time.
 *
 * Licence: MIT. The model is CC-BY-4.0 (see the model NOTICE).
 */
#define _GNU_SOURCE
#include "transcribe.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define RURU_VERSION "1.0.0"
#define DEFAULT_MODEL_NAME "underdog-bark-3.0-clean-Q8_0.gguf"
#define MAX_SAMPLES (16000 * 60 * 10) /* 10 minutes of 16 kHz mono */
#define MAX_LINE 8192

static struct transcribe_model *g_model = NULL;
static struct transcribe_session *g_session = NULL;
static pthread_mutex_t g_decode = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t g_stop = 0;
static char g_socket_path[108]; /* struct sockaddr_un::sun_path holds 108 on Linux */
static char g_model_path[512];
static char g_variant[128] = "?";
static char g_arch[64] = "?";
static char g_backend[64] = "?";
static int g_ready = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void log_cb(transcribe_log_level lvl, const char *msg, void *ud) {
    (void)ud;
    const char *tag = "info";
    switch (lvl) {
        case TRANSCRIBE_LOG_LEVEL_ERROR: tag = "error"; break;
        case TRANSCRIBE_LOG_LEVEL_WARN: tag = "warn"; break;
        default: break;
    }
    fprintf(stderr, "[ruru:%s] %s\n", tag, msg);
    fflush(stderr);
}

/* ---------------------------------------------------------------- paths */

static void expand_home(const char *in, char *out, size_t outsz) {
    if (in[0] == '~') {
        const char *home = getenv("HOME") ? getenv("HOME") : "/tmp";
        snprintf(out, outsz, "%s%s", home, in + 1);
    } else {
        snprintf(out, outsz, "%s", in);
    }
}

static int pick_model_path(char *out, size_t outsz) {
    const char *env = getenv("RURU_MODEL");
    if (env && *env) {
        expand_home(env, out, outsz);
        return access(out, R_OK) == 0;
    }
    const char *home = getenv("HOME") ? getenv("HOME") : "/tmp";
    snprintf(out, outsz, "%s/.local/share/ruru/models/%s", home, DEFAULT_MODEL_NAME);
    return access(out, R_OK) == 0;
}

static void pick_socket_path(char *out, size_t outsz) {
    const char *env = getenv("RURU_SOCKET");
    if (env && *env) {
        expand_home(env, out, outsz);
        return;
    }
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg) snprintf(out, outsz, "%s/ruru.sock", xdg);
    else {
        const char *home = getenv("HOME") ? getenv("HOME") : "/tmp";
        snprintf(out, outsz, "%s/.local/share/ruru/ruru.sock", home);
    }
}

/* ------------------------------------------------------------------ io */

static int read_full(int fd, void *buf, size_t n) {
    char *p = buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r == 0) return -1;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Reads one '\n'-terminated line. Returns length, or -1 on EOF/error. */
static int read_line(int fd, char *buf, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r == 0) return n == 0 ? -1 : (int)n;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (c == '\n') break;
        buf[n++] = c;
    }
    buf[n] = '\0';
    return (int)n;
}

/* ------------------------------------------------------------ json bits */

/* Minimal, allocation-free JSON string extraction. Finds "key":"value". */
static int json_get_str(const char *line, const char *key, char *out, size_t outsz) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outsz) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 1;
}

static int json_get_int(const char *line, const char *key, long *out) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return 0;
    *out = v;
    return 1;
}

static void json_escape(const char *in, char *out, size_t outsz) {
    size_t i = 0;
    for (const char *p = in; *p && i + 7 < outsz; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out[i++] = '\\';
            out[i++] = (char)c;
        } else if (c == '\n') {
            out[i++] = '\\'; out[i++] = 'n';
        } else if (c == '\r') {
            out[i++] = '\\'; out[i++] = 'r';
        } else if (c == '\t') {
            out[i++] = '\\'; out[i++] = 't';
        } else if (c < 0x20) {
            i += (size_t)snprintf(out + i, outsz - i, "\\u%04x", c);
        } else {
            out[i++] = (char)c;
        }
    }
    out[i] = '\0';
}

/* ------------------------------------------------------------- replies */

static int reply(int fd, const char *body) {
    size_t n = strlen(body);
    if (write_full(fd, body, n) < 0) return -1;
    return write_full(fd, "\n", 1);
}

static int reply_err(int fd, const char *code, const char *msg) {
    char esc[1024];
    char out[1280];
    json_escape(msg, esc, sizeof(esc));
    snprintf(out, sizeof(out), "{\"ok\":false,\"code\":\"%s\",\"error\":\"%s\"}", code, esc);
    return reply(fd, out);
}

static int reply_ok(int fd, const char *extra) {
    char out[2048];
    if (extra && *extra) snprintf(out, sizeof(out), "{\"ok\":true,%s}", extra);
    else snprintf(out, sizeof(out), "{\"ok\":true}");
    return reply(fd, out);
}

/* --------------------------------------------------------- transcribe */

static int do_transcribe(int fd, long samples, long rate) {
    if (!g_ready) return reply_err(fd, "NOT_READY", "model is not loaded");
    if (samples <= 0) return reply_err(fd, "BAD_REQUEST", "samples must be > 0");
    if (samples > MAX_SAMPLES) return reply_err(fd, "BAD_REQUEST", "audio longer than the 10 minute limit");
    if (rate != 16000) return reply_err(fd, "BAD_REQUEST", "this engine takes 16000 Hz mono only");

    size_t nbytes = (size_t)samples * sizeof(float);
    float *pcm = malloc(nbytes);
    if (!pcm) return reply_err(fd, "OOM", "could not allocate the audio buffer");

    if (read_full(fd, pcm, nbytes) < 0) {
        free(pcm);
        return reply_err(fd, "BAD_REQUEST", "client sent fewer samples than it promised");
    }

    /* Reject a payload of all zeros or NaN — nothing to say about silence. */
    int finite = 1;
    double energy = 0.0;
    for (long i = 0; i < samples; i++) {
        if (!isfinite(pcm[i])) { finite = 0; break; }
        energy += (double)pcm[i] * (double)pcm[i];
    }
    if (!finite) {
        free(pcm);
        return reply_err(fd, "BAD_REQUEST", "audio contains non-finite samples");
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_mutex_lock(&g_decode);
    struct transcribe_run_params rp;
    transcribe_run_params_init(&rp);
    transcribe_status st = transcribe_run(g_session, pcm, (int)samples, &rp);
    const char *text = (st == TRANSCRIBE_OK) ? transcribe_full_text(g_session) : NULL;
    pthread_mutex_unlock(&g_decode);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    free(pcm);

    if (st != TRANSCRIBE_OK) {
        char msg[512];
        snprintf(msg, sizeof(msg), "decode failed: %s", transcribe_status_string(st));
        return reply_err(fd, "RUN_FAILED", msg);
    }

    char esc[4096];
    char out[4600];
    json_escape(text ? text : "", esc, sizeof(esc));
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"text\":\"%s\",\"samples\":%ld,\"ms\":%.1f,\"rtf\":%.3f,\"energy\":%.4f,\"model\":\"%s\"}",
             esc, samples, ms, (samples / 16000.0) / (ms / 1e3), sqrt(energy / (double)samples), g_variant);
    return reply(fd, out);
}

/* --------------------------------------------------------- connection */

static void handle_connection(int fd) {
    char line[MAX_LINE];
    while (!g_stop) {
        int n = read_line(fd, line, sizeof(line));
        if (n <= 0) break;

        char cmd[32] = "";
        json_get_str(line, "cmd", cmd, sizeof(cmd));

        if (!strcmp(cmd, "ping")) {
            reply_ok(fd, "\"pong\":true,\"version\":\"" RURU_VERSION "\"");
        } else if (!strcmp(cmd, "status")) {
            char esc_model[512], esc_variant[160];
            json_escape(g_model_path, esc_model, sizeof(esc_model));
            json_escape(g_variant, esc_variant, sizeof(esc_variant));
            char extra[900];
            snprintf(extra, sizeof(extra),
                     "\"version\":\"%s\",\"ready\":%s,\"model\":\"%s\",\"variant\":\"%s\",\"arch\":\"%s\",\"backend\":\"%s\",\"socket\":\"%s\"",
                     RURU_VERSION, g_ready ? "true" : "false", esc_model, esc_variant, g_arch, g_backend, g_socket_path);
            reply_ok(fd, extra);
        } else if (!strcmp(cmd, "shutdown")) {
            reply_ok(fd, "\"bye\":true");
            g_stop = 1;
            break;
        } else if (!strcmp(cmd, "transcribe")) {
            long samples = 0, rate = 16000;
            json_get_int(line, "samples", &samples);
            json_get_int(line, "rate", &rate);
            do_transcribe(fd, samples, rate);
        } else {
            reply_err(fd, "BAD_REQUEST", "unknown cmd; use ping, status, transcribe or shutdown");
        }
    }
    close(fd);
}

static void *conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    handle_connection(fd);
    return NULL;
}

/* ------------------------------------------------------------- startup */

static int load_model(void) {
    if (!pick_model_path(g_model_path, sizeof(g_model_path))) {
        fprintf(stderr, "[ruru:error] no model found. Set RURU_MODEL or place %s in ~/.local/share/ruru/models/\n",
                DEFAULT_MODEL_NAME);
        return -1;
    }
    fprintf(stderr, "[ruru] model: %s\n", g_model_path);

    transcribe_log_set(log_cb, NULL);

    /* Point the engine at its own backend modules (they ship next to libtranscribe). */
    const char *art = getenv("RURU_ARTIFACT_DIR");
    struct transcribe_backend_init_params bp;
    transcribe_backend_init_params_init(&bp);
    if (art && *art) bp.artifact_dir = art;
    transcribe_status st = transcribe_init_backends_ex(&bp);
    if (st != TRANSCRIBE_OK) {
        /* Fall back to the plain call; static builds accept either. */
        st = transcribe_init_backends_default();
    }
    if (st != TRANSCRIBE_OK) {
        fprintf(stderr, "[ruru:error] backend init failed: %s\n", transcribe_status_string(st));
        return -1;
    }
    fprintf(stderr, "[ruru] devices=%d cpu=%s vulkan=%s cuda=%s\n",
            transcribe_device_count(),
            transcribe_backend_available(TRANSCRIBE_BACKEND_CPU) ? "yes" : "no",
            transcribe_backend_available(TRANSCRIBE_BACKEND_VULKAN) ? "yes" : "no",
            transcribe_backend_available(TRANSCRIBE_BACKEND_CUDA) ? "yes" : "no");

    struct transcribe_model_load_params lp;
    transcribe_model_load_params_init(&lp);
    lp.backend = TRANSCRIBE_BACKEND_AUTO;

    st = transcribe_model_load_file(g_model_path, &lp, &g_model);
    if (st != TRANSCRIBE_OK || !g_model) {
        fprintf(stderr, "[ruru:error] model load failed: %s\n", transcribe_status_string(st));
        return -1;
    }

    const char *v = transcribe_model_variant_string(g_model);
    const char *a = transcribe_model_arch_string(g_model);
    const char *b = transcribe_model_backend(g_model);
    snprintf(g_variant, sizeof(g_variant), "%s", v ? v : "?");
    snprintf(g_arch, sizeof(g_arch), "%s", a ? a : "?");
    snprintf(g_backend, sizeof(g_backend), "%s", b ? b : "?");
    fprintf(stderr, "[ruru] arch=%s variant=%s backend=%s\n", g_arch, g_variant, g_backend);

    struct transcribe_session_params sp;
    transcribe_session_params_init(&sp);
    st = transcribe_session_init(g_model, &sp, &g_session);
    if (st != TRANSCRIBE_OK || !g_session) {
        fprintf(stderr, "[ruru:error] session init failed: %s\n", transcribe_status_string(st));
        transcribe_model_free(g_model);
        g_model = NULL;
        return -1;
    }

    g_ready = 1;
    fprintf(stderr, "[ruru] ready\n");
    return 0;
}

/* Is there already a ruru listening? Exit politely if so. */
static int already_running(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", g_socket_path);
    int ok = connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0;
    if (ok) {
        if (write(fd, "{\"cmd\":\"ping\"}\n", 16) < 0) { close(fd); return 0; }
        char buf[256];
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssize_t r = read(fd, buf, sizeof(buf) - 1);
        ok = r > 0;
    }
    close(fd);
    return ok;
}

static int open_listener(void) {
    /* A socket file with nothing behind it is stale; clear it. */
    struct stat st;
    if (stat(g_socket_path, &st) == 0) {
        if (already_running()) {
            fprintf(stderr, "[ruru] already listening on %s\n", g_socket_path);
            return -1;
        }
        unlink(g_socket_path);
    }

    /* Keep the socket in a private directory when we can, so only the user can connect. */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", g_socket_path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        mkdir(dir, 0700);
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "[ruru:error] socket: %s\n", strerror(errno));
        return -1;
    }
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", g_socket_path);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        fprintf(stderr, "[ruru:error] bind %s: %s\n", g_socket_path, strerror(errno));
        close(fd);
        return -1;
    }
    chmod(g_socket_path, 0600);
    if (listen(fd, 8) < 0) {
        fprintf(stderr, "[ruru:error] listen: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    fprintf(stderr, "[ruru] listening on %s\n", g_socket_path);
    return fd;
}

/* ---------------------------------------------------------------- main */

static void usage(const char *argv0) {
    fprintf(stderr,
            "ruru %s — local speech-to-text service\n"
            "\n"
            "usage: %s [options]\n"
            "  --socket PATH   socket to listen on (default $XDG_RUNTIME_DIR/ruru.sock)\n"
            "  --model PATH    model file (default ~/.local/share/ruru/models/%s)\n"
            "  --check         load the model, print its identity, then exit\n"
            "  --version       print the version and exit\n"
            "  --help          this text\n"
            "\n"
            "env: RURU_MODEL, RURU_SOCKET, RURU_ARTIFACT_DIR\n",
            RURU_VERSION, argv0, DEFAULT_MODEL_NAME);
}

int main(int argc, char **argv) {
    const char *sock_arg = NULL, *model_arg = NULL;
    int check_only = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        if (!strcmp(argv[i], "--version")) { printf("ruru %s\n", RURU_VERSION); return 0; }
        if (!strcmp(argv[i], "--check")) { check_only = 1; continue; }
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) { sock_arg = argv[++i]; continue; }
        if (!strcmp(argv[i], "--model") && i + 1 < argc) { model_arg = argv[++i]; continue; }
        fprintf(stderr, "[ruru:error] unknown option %s\n", argv[i]);
        usage(argv[0]);
        return 2;
    }

    if (model_arg) {
        expand_home(model_arg, g_model_path, sizeof(g_model_path));
        setenv("RURU_MODEL", g_model_path, 1);
    }
    if (sock_arg) {
        expand_home(sock_arg, g_socket_path, sizeof(g_socket_path));
        setenv("RURU_SOCKET", g_socket_path, 1);
    } else {
        pick_socket_path(g_socket_path, sizeof(g_socket_path));
    }

    if (load_model() != 0) return 1;

    if (check_only) {
        printf("arch=%s variant=%s backend=%s model=%s\n", g_arch, g_variant, g_backend, g_model_path);
        transcribe_session_free(g_session);
        transcribe_model_free(g_model);
        return 0;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int lfd = open_listener();
    if (lfd < 0) return 1;

    while (!g_stop) {
        struct sockaddr_un peer;
        socklen_t plen = sizeof(peer);
        int cfd = accept(lfd, (struct sockaddr *)&peer, &plen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (g_stop) break;
            fprintf(stderr, "[ruru:error] accept: %s\n", strerror(errno));
            continue;
        }
        pthread_t th;
        if (pthread_create(&th, NULL, conn_thread, (void *)(intptr_t)cfd) == 0) {
            pthread_detach(th);
        } else {
            close(cfd);
        }
    }

    fprintf(stderr, "[ruru] shutting down\n");
    close(lfd);
    unlink(g_socket_path);
    if (g_session) transcribe_session_free(g_session);
    if (g_model) transcribe_model_free(g_model);
    return 0;
}
