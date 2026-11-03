/* nova-bridge: makes a Novation MiniNova on USB a shared MIDI port on the MPC.
 *
 * Why: the MPC app opens every hardware MIDI port exclusively, so once the kernel drives the MiniNova nothing else (an
 * editor plugin) can reach it. This daemon drives it from user space instead and offers it as an ALSA *sequencer* port,
 * which the MPC shares: MIDI tracks play and record it, and the Patchy editor exchanges patches with it at the same time.
 *
 * USB: interface 0 of 1235:001e carries plain MIDI bytes over its bulk/interrupt endpoints (Linux's own quirk table
 * describes it so: QUIRK_MIDI_RAW_BYTES). Sequencer: client "MiniNova", port "MIDI", readable and writable by anyone.
 * The port exists from start to stop, plugged in or not, so the MPC never loses it; while the MiniNova is unplugged
 * what comes in is dropped. libasound is dlopen()ed (no build-time ALSA library needed).
 *
 * Usage: nova-bridge [-v] [--log FILE]      (installed as a systemd service by install.sh) */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VERSION "0.1.0"
#define VID "1235"
#define PID "001e"

/* ---- libasound ---- */
#define ALSA_FNS(X) \
    X(snd_seq_open) X(snd_seq_close) X(snd_seq_set_client_name) X(snd_seq_client_id) X(snd_seq_create_simple_port) \
    X(snd_seq_event_output_direct) X(snd_seq_event_input) X(snd_seq_event_input_pending) \
    X(snd_seq_poll_descriptors_count) X(snd_seq_poll_descriptors) X(snd_strerror) \
    X(snd_midi_event_new) X(snd_midi_event_free) X(snd_midi_event_encode_byte) X(snd_midi_event_decode) \
    X(snd_midi_event_no_status) X(snd_midi_event_reset_encode) X(snd_midi_event_reset_decode)
static struct {
#define DECL(fn) __typeof__(&fn) fn;
    ALSA_FNS(DECL)
#undef DECL
} A;

static int alsa_load(void) {
    void *lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "libasound.so.2: %s\n", dlerror()); return 0; }
#define LOAD(fn) if (!(A.fn = (__typeof__(&fn))dlsym(lib, #fn))) { fprintf(stderr, "libasound lacks %s\n", #fn); return 0; }
    ALSA_FNS(LOAD)
#undef LOAD
    return 1;
}

/* ---- log ---- */
static FILE *g_log;
static const char *g_log_path;
static int g_verbose;
static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    FILE *f = g_log ? g_log : stderr;
    if (g_log && g_log_path && ftell(g_log) > 256 * 1024) {   /* keep it small: start over */
        g_log = freopen(g_log_path, "w", g_log);
        f = g_log ? g_log : stderr;
    }
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    fprintf(f, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}

/* ---- state ---- */
static volatile sig_atomic_t g_quit;
static snd_seq_t *g_seq;
static int g_port;
static pthread_mutex_t g_usb_mx = PTHREAD_MUTEX_INITIALIZER;   /* guards g_ufd / endpoints for the writer */
static int g_ufd = -1, g_in, g_out, g_in_max = 64, g_out_max = 64;
static volatile int g_usb_ok;
static long g_rx, g_tx;

static int read_line(const char *path, char *buf, int n) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = fgets(buf, n, f) != NULL;
    fclose(f);
    if (ok) buf[strcspn(buf, "\r\n")] = 0;
    return ok;
}

/* find the MiniNova: sysfs name (e.g. "2-1.2.1"), bus/dev numbers, interface 0's endpoints, and a bound driver (if any) */
static int usb_find(char *name, int nlen, int *bus, int *dev, char *drv, int dlen) {
    DIR *d = opendir("/sys/bus/usb/devices");
    struct dirent *e;
    int found = 0;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' || strchr(e->d_name, ':')) continue;
        char p[400], v[16] = "", pr[16] = "";
        snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/idVendor", e->d_name);
        if (!read_line(p, v, sizeof v) || strcmp(v, VID)) continue;
        snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/idProduct", e->d_name);
        if (!read_line(p, pr, sizeof pr) || strcmp(pr, PID)) continue;
        snprintf(name, nlen, "%s", e->d_name);
        found = 1;
        break;
    }
    if (d) closedir(d);
    if (!found) return 0;
    char p[400], b[32];
    snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/busnum", name);
    *bus = read_line(p, b, sizeof b) ? atoi(b) : -1;
    snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/devnum", name);
    *dev = read_line(p, b, sizeof b) ? atoi(b) : -1;
    drv[0] = 0;
    snprintf(p, sizeof p, "/sys/bus/usb/devices/%s:1.0/driver", name);
    char tgt[256];
    ssize_t k = readlink(p, tgt, sizeof tgt - 1);
    if (k > 0) { tgt[k] = 0; const char *s = strrchr(tgt, '/'); snprintf(drv, dlen, "%s", s ? s + 1 : tgt); }
    /* endpoints of interface 0, alt 0 (first IN and OUT bulk/interrupt), from the raw descriptors */
    unsigned char desc[4096];
    snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/descriptors", name);
    int fd = open(p, O_RDONLY), n = fd >= 0 ? (int)read(fd, desc, sizeof desc) : -1;
    if (fd >= 0) close(fd);
    int cur_if = -1, cur_alt = -1;
    g_in = g_out = 0;
    for (int i = 0; n > 0 && i + 2 <= n; ) {
        int len = desc[i], type = desc[i + 1];
        if (len < 2 || i + len > n) break;
        if (type == 4 && len >= 9) { cur_if = desc[i + 2]; cur_alt = desc[i + 3]; }
        else if (type == 5 && len >= 7 && cur_if == 0 && cur_alt == 0) {
            int addr = desc[i + 2], attr = desc[i + 3] & 3, mps = desc[i + 4] | (desc[i + 5] << 8);
            if (attr == 2 || attr == 3) {
                if ((addr & 0x80) && !g_in) { g_in = addr; g_in_max = mps > 0 && mps <= 512 ? mps : 64; }
                if (!(addr & 0x80) && !g_out) { g_out = addr; g_out_max = mps > 0 && mps <= 512 ? mps : 64; }
            }
        }
        i += len;
    }
    return 1;
}

static int bulk(int fd, int ep, unsigned char *buf, int len, int timeout_ms) {
    struct usbdevfs_bulktransfer bt;
    memset(&bt, 0, sizeof bt);
    bt.ep = (unsigned)ep; bt.len = (unsigned)len; bt.timeout = (unsigned)timeout_ms; bt.data = buf;
    return ioctl(fd, USBDEVFS_BULK, &bt);
}

static void usb_close(void) {
    pthread_mutex_lock(&g_usb_mx);
    if (g_ufd >= 0) {
        unsigned int ifn = 0;
        ioctl(g_ufd, USBDEVFS_RELEASEINTERFACE, &ifn);
        close(g_ufd);
    }
    g_ufd = -1;
    g_usb_ok = 0;
    pthread_mutex_unlock(&g_usb_mx);
}

/* open and claim; returns 1 when linked. Logs only when the reason changes, so a missing synth doesn't fill the log. */
static int usb_open(void) {
    static char last[160];
    char why[160] = "", name[64], drv[64];
    int bus, dev;
    if (!usb_find(name, sizeof name, &bus, &dev, drv, sizeof drv)) snprintf(why, sizeof why, "MiniNova not on USB");
    else if (drv[0]) snprintf(why, sizeof why, "interface 0 is owned by driver \"%s\" (run install.sh again)", drv);
    else if (!g_in || !g_out) snprintf(why, sizeof why, "interface 0 has no bulk/interrupt endpoints");
    else {
        char path[64];
        snprintf(path, sizeof path, "/dev/bus/usb/%03d/%03d", bus, dev);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        unsigned int ifn = 0;
        if (fd < 0) snprintf(why, sizeof why, "open %s: %s", path, strerror(errno));
        else if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifn) < 0) { snprintf(why, sizeof why, "claim: %s", strerror(errno)); close(fd); }
        else {
            pthread_mutex_lock(&g_usb_mx);
            g_ufd = fd;
            g_usb_ok = 1;
            pthread_mutex_unlock(&g_usb_mx);
            logf_("linked: MiniNova %s (%s), in %02X out %02X", name, path, g_in, g_out);
            last[0] = 0;
            return 1;
        }
    }
    if (strcmp(why, last)) { logf_("waiting: %s", why); snprintf(last, sizeof last, "%s", why); }
    return 0;
}

/* USB -> sequencer */
static void *usb_reader(void *arg) {
    (void)arg;
    snd_midi_event_t *enc = NULL;
    A.snd_midi_event_new(4096, &enc);   /* room for a whole patch dump (526 bytes) and more */
    unsigned char buf[512];
    int errs = 0;
    while (!g_quit) {
        if (!g_usb_ok) {
            if (!usb_open()) { sleep(1); continue; }
            A.snd_midi_event_reset_encode(enc);
            errs = 0;
        }
        int r = bulk(g_ufd, g_in, buf, g_in_max, 200);
        if (r > 0) {
            errs = 0;
            g_rx += r;
            for (int i = 0; i < r; i++) {
                snd_seq_event_t ev;
                snd_seq_ev_clear(&ev);
                if (A.snd_midi_event_encode_byte(enc, buf[i], &ev) == 1) {   /* a complete message */
                    snd_seq_ev_set_source(&ev, g_port);
                    snd_seq_ev_set_subs(&ev);
                    snd_seq_ev_set_direct(&ev);
                    A.snd_seq_event_output_direct(g_seq, &ev);
                    if (g_verbose) logf_("synth -> type %d", ev.type);
                }
            }
        } else if (r < 0 && errno != ETIMEDOUT && errno != EAGAIN) {
            if (errno == ENODEV || errno == ENOENT || errno == ESHUTDOWN || ++errs > 20) {
                logf_("unlinked: %s", strerror(errno));
                usb_close();
            } else usleep(20000);
        }
    }
    A.snd_midi_event_free(enc);
    return NULL;
}

/* write bytes to the synth, in max-packet chunks */
static void usb_write(const unsigned char *b, long n) {
    pthread_mutex_lock(&g_usb_mx);
    if (g_ufd >= 0 && g_usb_ok) {
        for (long off = 0; off < n; ) {
            int c = (int)(n - off < g_out_max ? n - off : g_out_max);
            int r = bulk(g_ufd, g_out, (unsigned char *)b + off, c, 1000);
            if (r < 0) { logf_("write failed: %s", strerror(errno)); break; }
            off += r > 0 ? r : c;
        }
        g_tx += n;
    }
    pthread_mutex_unlock(&g_usb_mx);
}

static void on_signal(int s) { (void)s; g_quit = 1; }

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) g_log_path = argv[++i];
        else if (!strcmp(argv[i], "--version")) { puts("nova-bridge " VERSION); return 0; }
    }
    if (g_log_path) g_log = fopen(g_log_path, "a");
    if (!alsa_load()) return 1;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int r = A.snd_seq_open(&g_seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
    if (r < 0) { logf_("can't open the sequencer: %s", A.snd_strerror(r)); return 1; }
    A.snd_seq_set_client_name(g_seq, "MiniNova");
    g_port = A.snd_seq_create_simple_port(g_seq, "MIDI",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE |
        SND_SEQ_PORT_CAP_DUPLEX, SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE | SND_SEQ_PORT_TYPE_PORT);
    if (g_port < 0) { logf_("can't create the port: %s", A.snd_strerror(g_port)); return 1; }
    logf_("nova-bridge %s: sequencer port %d:%d \"MiniNova MIDI\"", VERSION, A.snd_seq_client_id(g_seq), g_port);

    pthread_t th;
    pthread_create(&th, NULL, usb_reader, NULL);

    /* sequencer -> USB */
    snd_midi_event_t *dec = NULL;
    A.snd_midi_event_new(256, &dec);
    A.snd_midi_event_no_status(dec, 1);   /* full status bytes: simplest for the synth */
    int nfd = A.snd_seq_poll_descriptors_count(g_seq, POLLIN);
    if (nfd < 1 || nfd > 4) nfd = 1;
    struct pollfd pfd[4];
    A.snd_seq_poll_descriptors(g_seq, pfd, nfd, POLLIN);
    while (!g_quit) {
        if (poll(pfd, nfd, 500) <= 0) continue;
        snd_seq_event_t *ev = NULL;
        while (!g_quit && A.snd_seq_event_input(g_seq, &ev) >= 0 && ev) {
            if (ev->type == SND_SEQ_EVENT_SYSEX) {
                usb_write(ev->data.ext.ptr, ev->data.ext.len);   /* SysEx goes through untouched */
            } else if (ev->type < SND_SEQ_EVENT_CLIENT_START) {   /* MIDI events only, not port announcements */
                unsigned char b[16];
                long n = A.snd_midi_event_decode(dec, b, sizeof b, ev);
                if (n > 0) usb_write(b, n);
            }
            ev = NULL;
            if (A.snd_seq_event_input_pending(g_seq, 0) <= 0) break;
        }
    }
    logf_("stopping (in %ld bytes, out %ld bytes)", g_rx, g_tx);
    pthread_join(th, NULL);
    usb_close();
    A.snd_midi_event_free(dec);
    A.snd_seq_close(g_seq);
    return 0;
}
