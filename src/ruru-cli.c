/* ruru-cli — talk to a running ruru service.
 *
 *   ruru-cli status
 *   ruru-cli ping
 *   ruru-cli stop
 *   ruru-cli wav FILE.wav         transcribe a 16 kHz mono 16-bit WAV
 *   ruru-cli pcm FILE.f32         transcribe raw 16 kHz mono float32
 *
 * Prints the transcript to stdout and the JSON envelope to stderr, so
 *   ruru-cli wav foo.wav
 * can be used directly in a pipeline.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static char g_sock[108]; /* struct sockaddr_un::sun_path holds 108 on Linux */

static void pick_socket(void) {
    const char *e = getenv("RURU_SOCKET");
    if (e && *e) { snprintf(g_sock, sizeof(g_sock), "%s", e); return; }
    e = getenv("XDG_RUNTIME_DIR");
    if (e && *e) snprintf(g_sock, sizeof(g_sock), "%s/ruru.sock", e);
    else snprintf(g_sock, sizeof(g_sock), "%s/.local/share/ruru/ruru.sock", getenv("HOME") ? getenv("HOME") : "/tmp");
}

static int connect_ruru(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", g_sock);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        fprintf(stderr, "ruru-cli: cannot reach %s (%s)\n", g_sock, strerror(errno));
        fprintf(stderr, "ruru-cli: start it with: systemctl --user start ruru\n");
        close(fd);
        return -1;
    }
    return fd;
}

static int write_full(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int read_line(int fd, char *buf, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r == 0) return n ? (int)n : -1;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (c == '\n') break;
        buf[n++] = c;
    }
    buf[n] = '\0';
    return (int)n;
}

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
        if (*p == '\\' && p[1] == 'n') { out[i++] = '\n'; p += 2; continue; }
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 1;
}

/* --- WAV (16-bit PCM) reader ------------------------------------------ */

static float *read_wav(const char *path, long *n_out) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); return NULL; }
    fclose(f);

    long pos = 12, data_off = -1, data_sz = 0;
    int bits = 16, ch = 1, rate = 16000;
    while (pos + 8 <= sz) {
        unsigned char *h = buf + pos;
        int cksz = h[4] | (h[5] << 8) | (h[6] << 16) | (h[7] << 24);
        if (!memcmp(h, "fmt ", 4)) {
            ch = h[10] | (h[11] << 8);
            rate = h[12] | (h[13] << 8) | (h[14] << 16) | (h[15] << 24);
            bits = h[22] | (h[23] << 8);
        } else if (!memcmp(h, "data", 4)) {
            data_off = pos + 8; data_sz = cksz;
        }
        pos += 8 + cksz + (cksz & 1);
    }
    if (data_off < 0 || bits != 16) {
        fprintf(stderr, "ruru-cli: unsupported wav (need 16-bit PCM)\n");
        free(buf);
        return NULL;
    }
    if (rate != 16000) {
        fprintf(stderr, "ruru-cli: warning: %d Hz — ruru takes 16000 Hz mono only\n", rate);
    }
    long n = data_sz / 2 / ch;
    float *out = malloc((size_t)n * sizeof(float));
    const short *s16 = (const short *)(buf + data_off);
    for (long i = 0; i < n; i++) {
        int acc = 0;
        for (int c = 0; c < ch; c++) acc += s16[i * ch + c];
        out[i] = (float)(acc / ch) / 32768.0f;
    }
    free(buf);
    *n_out = n;
    return out;
}

static float *read_f32(const char *path, long *n_out) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    long n = sz / (long)sizeof(float);
    float *out = malloc((size_t)n * sizeof(float));
    if (!out || fread(out, sizeof(float), (size_t)n, f) != (size_t)n) { fclose(f); free(out); return NULL; }
    fclose(f);
    *n_out = n;
    return out;
}

/* --------------------------------------------------------------------- */

static int do_simple(const char *cmd) {
    int fd = connect_ruru();
    if (fd < 0) return 1;
    char line[256];
    snprintf(line, sizeof(line), "{\"cmd\":\"%s\"}\n", cmd);
    if (write_full(fd, line, strlen(line)) < 0) { close(fd); return 1; }
    char resp[8192];
    int n = read_line(fd, resp, sizeof(resp));
    close(fd);
    if (n <= 0) { fprintf(stderr, "ruru-cli: no reply\n"); return 1; }
    /* The envelope goes to stdout so it can be piped; --quiet would hide it. */
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int do_audio(const char *path, int is_wav) {
    long n = 0;
    float *pcm = is_wav ? read_wav(path, &n) : read_f32(path, &n);
    if (!pcm) return 1;
    if (n <= 0) { fprintf(stderr, "ruru-cli: empty audio\n"); free(pcm); return 1; }

    int fd = connect_ruru();
    if (fd < 0) { free(pcm); return 1; }

    char hdr[128];
    snprintf(hdr, sizeof(hdr), "{\"cmd\":\"transcribe\",\"samples\":%ld,\"rate\":16000}\n", n);
    if (write_full(fd, hdr, strlen(hdr)) < 0 ||
        write_full(fd, pcm, (size_t)n * sizeof(float)) < 0) {
        fprintf(stderr, "ruru-cli: send failed\n");
        free(pcm); close(fd);
        return 1;
    }
    free(pcm);

    char *resp = malloc(1 << 20);
    if (!resp) { close(fd); return 1; }
    int r = read_line(fd, resp, 1 << 20);
    close(fd);
    if (r <= 0) { fprintf(stderr, "ruru-cli: no reply\n"); free(resp); return 1; }

    char text[65536];
    if (json_get_str(resp, "text", text, sizeof(text))) printf("%s\n", text);
    fprintf(stderr, "%s\n", resp);
    int ok = strstr(resp, "\"ok\":true") != NULL;
    free(resp);
    return ok ? 0 : 1;
}

static void usage(const char *a0) {
    fprintf(stderr,
            "ruru-cli — client for the local ruru speech service\n\n"
            "  %s ping\n"
            "  %s status\n"
            "  %s stop\n"
            "  %s wav FILE.wav     (16 kHz mono 16-bit)\n"
            "  %s pcm FILE.f32     (raw 16 kHz mono float32)\n"
            "\n"
            "socket: $RURU_SOCKET or $XDG_RUNTIME_DIR/ruru.sock\n",
            a0, a0, a0, a0, a0);
}

int main(int argc, char **argv) {
    pick_socket();
    if (argc < 2) { usage(argv[0]); return 2; }
    if (!strcmp(argv[1], "ping")) return do_simple("ping");
    if (!strcmp(argv[1], "status")) return do_simple("status");
    if (!strcmp(argv[1], "stop") || !strcmp(argv[1], "shutdown")) return do_simple("shutdown");
    if (!strcmp(argv[1], "wav") && argc >= 3) return do_audio(argv[2], 1);
    if (!strcmp(argv[1], "pcm") && argc >= 3) return do_audio(argv[2], 0);
    usage(argv[0]);
    return 2;
}
