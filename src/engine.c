/* Nova Editor: an editor and librarian for the Novation MiniNova, as an MPC plugin.
 *
 * Link: the nova-bridge daemon shares the MiniNova as the sequencer port "MiniNova MIDI" (client "MiniNova", port
 * "MIDI"). This plugin opens its own sequencer client, listens to that port and sends to it directly.
 * Editing: every control maps to a MiniNova CC, NRPN or packed NRPN switch (tools/spec.py -> nova_map.h). A control
 * sends only when its value changes; the newest value of each parameter wins (a fast knob sweep never queues up).
 * Following: CC/NRPN from the synth move the controls; a program change (with the bank's CC32) updates the current
 * patch and pages the library to it. Patch dumps (526-byte SysEx, Novation 00 20 29, type 01 01 09 04, bank, slot)
 * fill the library, which is kept on the SD card. Any other SysEx is saved to Captures/ for later study.
 * Librarian: banks A-C as last dumped, plus Imported (.syx files); a row tap calls the patch up on the synth (bank
 * select + program change); WRITE (tap twice) puts the chosen patch into a slot; EXPORT writes all banks to a .syx.
 * Project recall: the current patch and the parameters edited since it was called up are saved with the project and
 * sent again when it loads.
 * Other synths: on linking, a Universal Identity Request names whatever answered (src/devices.h). The MiniNova is
 * the one profile verified against hardware, so only it gets knob editing and writing to a chosen slot. Any other
 * synth is handled as a librarian: its dumps are captured, listed by name where a name can be found, exported, and
 * sent back exactly as they came. Nothing infers which byte picks the slot, so an unknown synth can never have the
 * wrong patch overwritten.
 * libasound is dlopen()ed; third_party/alsa supplies types and macros only. */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "params.h"
#include "engine.h"
#include "nova_map.h"
#include "nova_tap.h"
#include "devices.h"

/* ---- libasound ---- */
#define ALSA_FNS(X) \
    X(snd_seq_open) X(snd_seq_close) X(snd_seq_set_client_name) X(snd_seq_client_id) X(snd_seq_create_simple_port) \
    X(snd_seq_client_info_malloc) X(snd_seq_client_info_free) X(snd_seq_client_info_set_client) \
    X(snd_seq_query_next_client) X(snd_seq_client_info_get_client) X(snd_seq_client_info_get_name) \
    X(snd_seq_port_info_malloc) X(snd_seq_port_info_free) X(snd_seq_port_info_set_client) X(snd_seq_port_info_set_port) \
    X(snd_seq_query_next_port) X(snd_seq_port_info_get_port) X(snd_seq_port_info_get_name) \
    X(snd_seq_connect_from) X(snd_seq_event_output_direct) X(snd_seq_event_input) X(snd_seq_event_input_pending) \
    X(snd_seq_poll_descriptors_count) X(snd_seq_poll_descriptors) X(snd_strerror)
static struct {
#define DECL(fn) __typeof__(&fn) fn;
    ALSA_FNS(DECL)
#undef DECL
} A;
static int alsa_ok = -1;
static pthread_mutex_t alsa_mx = PTHREAD_MUTEX_INITIALIZER;
static int alsa_load(void) {
    pthread_mutex_lock(&alsa_mx);
    if (alsa_ok < 0) {
        void *lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
        alsa_ok = lib != NULL;
#define LOAD(fn) if (alsa_ok && !(A.fn = (__typeof__(&fn))dlsym(lib, #fn))) alsa_ok = 0;
        ALSA_FNS(LOAD)
#undef LOAD
    }
    pthread_mutex_unlock(&alsa_mx);
    return alsa_ok;
}

/* ---- constants ---- */
#define PATCH_LEN 526
#define ROWS 16
#define MAX_IMPORT 128
#define HOME "/sdcard/Nova Editor"
static const unsigned char HDR[11] = {0xf0, 0x00, 0x20, 0x29, 0x03, 0x01, 0x7f, 0x01, 0x01, 0x09, 0x04};
static const char *BANKS = "ABC";
#define LIB_IMPORTED 3
#define LIB_DEVICE 4              /* the fifth bank: patches captured from another synth */
#define MAX_GEN 128
#define GEN_LEN 4096              /* a single patch dump; anything larger is kept in Captures only */

typedef struct { unsigned char d[PATCH_LEN]; int ok; } patch_t;
typedef struct { unsigned char d[GEN_LEN]; int len; } gpatch_t;

typedef struct {
    pthread_mutex_t mx;
    pthread_cond_t wake;
    pthread_t rd, wr;
    int rd_on, wr_on, quit;
    unsigned rev;

    /* values */
    unsigned char raw[NREAL];        /* MiniNova raw values */
    unsigned char dirty[NREAL];      /* waiting to be sent */
    unsigned char orig[NREAL];       /* the loaded patch's values: what "modified" is measured against (patch_modified) */
    unsigned char echo_raw[NREAL];   /* values as last reported to the host, for the echo guard */
    unsigned char echo_used[NREAL];  /* that parameter's echo has arrived: a further write is the user's */
    long echo_ms;                    /* when they were reported; an echo arrives within ECHO_MS */
    unsigned rev_sent;               /* the display revision the host has already collected */
    int edit_r;                      /* the parameter the user is editing, exempt from the guard while they hold it */
    long edit_ms;
    int ui[NCTL];                    /* editor-only controls: selectors, library, setup */
    unsigned char changed[NCTL];     /* controls the engine moved (reported to the host) */
    int chan;                        /* 0..15 */
    int nrpn_msb, nrpn_lsb;          /* NRPN state for what the synth sends */

    /* link */
    snd_seq_t *seq;
    int me, port, nova_client, nova_port, linked;
    long next_scan_ms;

    /* outgoing: notes ring, one SysEx at a time, program change */
    unsigned char notes[256][3];
    int nh, nt;
    unsigned char sx_out[GEN_LEN];
    int sx_out_pending, sx_out_len;
    int pc_pending, pc_bank, pc_slot;   /* bank 0..2, slot 0..127 */
    int recall_pending;              /* re-send the project's unsaved edits once the recalled patch has loaded */
    long recall_at_ms;

    /* library */
    patch_t lib[3][128];
    patch_t imp[MAX_IMPORT];
    int nimp;
    int cur_bank, cur_slot;          /* the synth's current patch (-1 unknown) */
    int sel_src;                     /* selected entry: 0..383 synth (bank*128+slot), 1000+i imported, -1 none */
    int lib_dirty[3];
    long last_dump_ms;
    int dumps_in;
    long write_armed_ms;

    /* whatever synth is on the other end */
    nova_dev_t dev;                  /* the active profile; the MiniNova until something else answers */
    dev_learn_t learn;
    unsigned char id_mfr[3];
    int id_mfr_len, id_family, id_member, have_id;
    char id_name[48];                /* what the display calls it: a profile name, or maker + model bytes */
    int id_req_pending;
    gpatch_t gen[MAX_GEN];
    int ngen;

    long nav_armed_ms;               /* leaving a modified patch: armed by the first tap (nav_blocked) */
    int nav_bank, nav_slot;
    int rx_bank;                     /* bank from the synth's CC32 / CC0 */

    /* incoming SysEx reassembly */
    unsigned char sx[4096];
    int sx_len;

    char disp[96], link[96];
    long disp_ms;                    /* when disp was set: shown on the display for MSG_MS, then the patch again */
    int disp_shown, receive_armed, linked_rep;
    char tap_key[24];                /* the last button tapped and when (tap_once) */
    long tap_ms;

    /* audio: the MiniNova's input, from the nova-audio addin's tap of MPC's inputs */
    nova_tap_t *tap;
    uint64_t rpos;
    int rsync;
} st_t;

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static int ctl_index(const char *key) {
    for (int i = 0; i < NCTL; i++) if (!strcmp(NCTL_T[i].key, key)) return i;
    return -1;
}
/* editor-only control indices, looked up once */
static int C_LINKED, C_AUDIO, C_CHAN, C_VIEW, C_LIB_BANK, C_LIB_PAGE, C_TGT_BANK, C_TGT_SLOT, C_ROW1;
static void init_indices(void) {
    C_CHAN = ctl_index("chan"); C_AUDIO = ctl_index("audio_in"); C_LINKED = ctl_index("linked"); C_VIEW = ctl_index("view"); C_LIB_BANK = ctl_index("lib_bank");
    C_LIB_PAGE = ctl_index("lib_page"); C_TGT_BANK = ctl_index("tgt_bank"); C_TGT_SLOT = ctl_index("tgt_slot");
    C_ROW1 = ctl_index("row_1");
}
static int param_index(const char *key) {
    for (int i = 0; i < NPARAMS; i++) if (!strcmp(PARAMS[i].key, key)) return i;
    return -1;
}

#define MSG_MS 2500
/* The host writes back ("echoes") each value the engine reports, at reduced precision, and the wrapper's settle()
 * then resolves a value that landed between two steps to the step below or above -- one step off what the engine
 * reported. Taking that as an edit is what used to mark untouched patches modified, and only for patches whose
 * values happen not to survive the round trip (measured: 97% of oscillator waveforms, 4.5% of all values at 7-bit).
 * The engine knows exactly when the host will be told: the wrapper re-reads every parameter whenever "display_rev"
 * changes, which only this engine bumps. So each bump arms the guard, and for ECHO_MS afterwards the FIRST host write
 * to a given parameter is that parameter's echo: if it lands within one step of what was reported it is dropped, and
 * either way it spends that parameter's one echo, so the next write to it is the user's. A parameter the user has just
 * edited is armed as already spent, so turning a knob on is never mistaken for an echo however fast it is turned.
 * Comparison is in raw MiniNova units (whole numbers 0..127), never in the host's floats, so the host's precision loss
 * cannot reach the modified state at all. */
#define ECHO_MS 250
#define ECHO_STEP 1
static long now_ms(void);
static void set_text(st_t *s, char *dst, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_text(st_t *s, char *dst, const char *fmt, ...) {
    char t[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (dst == s->disp) { s->disp_ms = now_ms(); s->disp_shown = 1; }
    if (strcmp(t, dst)) { strcpy(dst, t); s->rev++; }
    else if (dst == s->disp) s->rev++;
}

static void patch_name(const unsigned char *d, char *out) {   /* 16 characters at 15..30, trailing spaces trimmed */
    int n = 0;
    for (int i = 15; i < 31; i++) {               /* the name ends at a zero byte; what follows is leftover */
        unsigned char c = d[i];
        if (!c) break;
        out[n++] = (c >= 32 && c < 127) ? (char)c : ' ';
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
}

/* ---- control <-> raw value ---- */
static int ctl_real(st_t *s, int c) {     /* the real parameter a control edits now, or -1 */
    const nctl_t *t = &NCTL_T[c];
    if (!t->nreal) return -1;
    int u = t->sel >= 0 ? s->ui[t->sel] : 0;
    if (u < 0 || u >= t->nreal) return -1;
    return t->real[u];
}
static int ctl_get(st_t *s, int c) {      /* the value the control shows */
    const nctl_t *t = &NCTL_T[c];
    if (!t->nreal) return s->ui[c];
    int r = ctl_real(s, c);
    if (r < 0) return 0;
    int v = s->raw[r];
    if (t->disp == 3) return v >= 124 ? v - 128 : v;   /* octave: 124..127 = -4..-1 */
    return v - t->off;
}
/* Is the current patch genuinely different from the one that was loaded? Compared as data, not as a flag, so a
 * parameter put back to its original value clears the modified state by itself. */
static int patch_modified(const st_t *s) { return memcmp(s->raw, s->orig, NREAL) != 0; }
static int patch_edits(const st_t *s) {
    int n = 0;
    for (int r = 0; r < NREAL; r++) n += s->raw[r] != s->orig[r];
    return n;
}
/* The loaded patch is now what the editor holds: nothing is modified until the user changes something. Called
 * whenever a patch is loaded, the synth reports a value of its own, or a project is restored. */
static void snapshot(st_t *s) { memcpy(s->orig, s->raw, NREAL); }
/* Arm the echo guard: the host is about to be told these values. */
static void arm_echo(st_t *s) {
    long t = now_ms();
    memcpy(s->echo_raw, s->raw, NREAL);
    memset(s->echo_used, 0, NREAL);
    s->echo_ms = t;
    /* not a parameter the user is working on: their next move on it is an edit, however fast they turn it */
    if (s->edit_r >= 0 && t - s->edit_ms < 400) s->echo_used[s->edit_r] = 1;
}

static void mark_controls_of(st_t *s, int r) {   /* the synth moved real parameter r: report the controls showing it */
    for (int c = 0; c < NCTL; c++)
        if (ctl_real(s, c) == r) { s->changed[c] = 1; }
    s->orig[r] = s->raw[r];   /* the synth telling us its value is the patch's state, never an edit of ours */
    s->echo_raw[r] = s->raw[r];
    s->echo_used[r] = 0;
    s->echo_ms = now_ms();
}

/* ---- sending ---- */
static void ev_send(st_t *s, snd_seq_event_t *ev) {
    if (!s->linked) return;
    snd_seq_ev_set_source(ev, s->port);
    snd_seq_ev_set_dest(ev, s->nova_client, s->nova_port);
    snd_seq_ev_set_direct(ev);
    A.snd_seq_event_output_direct(s->seq, ev);
}
static void send_cc(st_t *s, int cc, int v) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_controller(&ev, s->chan, cc, v);
    ev_send(s, &ev);
}
static void send_real(st_t *s, int r) {
    const nreal_t *t = &NREAL_T[r];
    int v = s->raw[r];
    if (t->kind == 0) { send_cc(s, t->a, v); return; }
    if (t->kind == 2) v = v ? t->hi : t->lo;          /* a packed switch sends its own code */
    send_cc(s, 99, t->a); send_cc(s, 98, t->b); send_cc(s, 6, v);
}
static void send_sysex(st_t *s, unsigned char *d, int n) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_sysex(&ev, n, d);
    ev_send(s, &ev);
}

/* the writer thread: notes first (low latency), then a program change, a SysEx write, then parameters (paced) */
static void *writer(void *arg) {
    st_t *s = arg;
    pthread_mutex_lock(&s->mx);
    while (!s->quit) {
        int did = 0;
        while (s->nt != s->nh) {
            unsigned char *m = s->notes[s->nt];
            snd_seq_event_t ev;
            snd_seq_ev_clear(&ev);
            int st = m[0] & 0xf0;
            if (st == 0x90) snd_seq_ev_set_noteon(&ev, s->chan, m[1], m[2]);
            else if (st == 0x80) snd_seq_ev_set_noteoff(&ev, s->chan, m[1], m[2]);
            else if (st == 0xb0) snd_seq_ev_set_controller(&ev, s->chan, m[1], m[2]);
            else if (st == 0xe0) snd_seq_ev_set_pitchbend(&ev, s->chan, (m[1] | (m[2] << 7)) - 8192);
            else if (st == 0xd0) snd_seq_ev_set_chanpress(&ev, s->chan, m[1]);
            if (st == 0x90 || st == 0x80 || st == 0xb0 || st == 0xe0 || st == 0xd0) ev_send(s, &ev);
            s->nt = (s->nt + 1) % 256;
            did = 1;
        }
        if (s->pc_pending && s->linked) {
            s->pc_pending = 0;
            send_cc(s, 32, s->pc_bank + 1);
            snd_seq_event_t ev;
            snd_seq_ev_clear(&ev);
            snd_seq_ev_set_pgmchange(&ev, s->chan, s->pc_slot);
            ev_send(s, &ev);
            did = 1;
        }
        if (s->id_req_pending && s->linked) {   /* "who are you?" -- every MIDI device answers this one */
            s->id_req_pending = 0;
            unsigned char q[6] = {0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7};
            send_sysex(s, q, sizeof q);
            did = 1;
        }
        if (s->sx_out_pending && s->linked) {
            s->sx_out_pending = 0;
            unsigned char d[GEN_LEN];
            int dn = s->sx_out_len > 0 && s->sx_out_len <= GEN_LEN ? s->sx_out_len : PATCH_LEN;
            memcpy(d, s->sx_out, (size_t)dn);
            send_sysex(s, d, dn);
            pthread_mutex_unlock(&s->mx);
            usleep(250000);                     /* let the synth store it */
            pthread_mutex_lock(&s->mx);
            did = 1;
        }
        if (s->recall_pending && s->linked && now_ms() >= s->recall_at_ms) {
            s->recall_pending = 0;
            for (int r = 0; r < NREAL; r++) if (s->raw[r] != s->orig[r]) s->dirty[r] = 1;
        }
        int sent = 0;
        for (int r = 0; r < NREAL && sent < 8; r++) {
            if (!s->dirty[r]) continue;
            s->dirty[r] = 0;
            if (!s->linked) continue;
            send_real(s, r);
            sent++;
        }
        if (sent) {                              /* a millisecond per parameter: the synth keeps up with sweeps */
            pthread_mutex_unlock(&s->mx);
            usleep(1000 * sent);
            pthread_mutex_lock(&s->mx);
            continue;
        }
        if (!did) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 20 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&s->wake, &s->mx, &ts);
        }
    }
    pthread_mutex_unlock(&s->mx);
    return NULL;
}

/* ---- library files ---- */
static void mkdirs(void) {
    mkdir(HOME, 0755);
    mkdir(HOME "/Library", 0755);
    mkdir(HOME "/Import", 0755);
    mkdir(HOME "/Export", 0755);
    mkdir(HOME "/Captures", 0755);
}
/* every patch dump in a file -> cb(patch bytes); returns how many */
static int read_syx(const char *path, void (*cb)(st_t *, const unsigned char *), st_t *s) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static unsigned char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    int count = 0;
    for (size_t i = 0; i + PATCH_LEN <= n; i++) {
        if (buf[i] != 0xf0) continue;
        if (!memcmp(buf + i, HDR, sizeof HDR) && buf[i + PATCH_LEN - 1] == 0xf7) {
            cb(s, buf + i);
            count++;
            i += PATCH_LEN - 1;
        }
    }
    return count;
}
static void lib_store(st_t *s, const unsigned char *d) {
    int b = d[11] - 1, slot = d[12];
    if (b < 0 || b > 2 || slot > 127) return;
    memcpy(s->lib[b][slot].d, d, PATCH_LEN);
    s->lib[b][slot].ok = 1;
}
static void imp_store(st_t *s, const unsigned char *d) {
    if (s->nimp >= MAX_IMPORT) return;
    memcpy(s->imp[s->nimp].d, d, PATCH_LEN);
    s->imp[s->nimp].ok = 1;
    s->nimp++;
}
static void lib_load(st_t *s) {
    char p[256];
    for (int b = 0; b < 3; b++) {
        snprintf(p, sizeof p, HOME "/Library/Bank %c.syx", BANKS[b]);
        read_syx(p, lib_store, s);
    }
}
static void lib_save_bank(st_t *s, int b) {
    char p[256], tmp[260];
    snprintf(p, sizeof p, HOME "/Library/Bank %c.syx", BANKS[b]);
    snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    for (int i = 0; i < 128; i++) if (s->lib[b][i].ok) fwrite(s->lib[b][i].d, 1, PATCH_LEN, f);
    if (fclose(f) == 0) rename(tmp, p);
}
static int name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void imports_scan(st_t *s) {
    s->nimp = 0;
    DIR *d = opendir(HOME "/Import");
    if (!d) return;
    char *names[256];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 256) {
        size_t L = strlen(e->d_name);
        if (L > 4 && !strcasecmp(e->d_name + L - 4, ".syx")) names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, name_cmp);
    for (int i = 0; i < n; i++) {
        char p[512];
        snprintf(p, sizeof p, HOME "/Import/%s", names[i]);
        read_syx(p, imp_store, s);
        free(names[i]);
    }
}
static void export_all(st_t *s) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    /* Whichever bank is showing: the MiniNova's library, or the patches captured from another synth. */
    int device = s->ui[C_LIB_BANK] == LIB_DEVICE && s->ngen;
    char who[48];
    snprintf(who, sizeof who, "%s", device ? (s->have_id ? s->id_name : "Synth") : "MiniNova");
    for (char *q = who; *q; q++) if (*q == '/' || *q == '\\' || *q == ':') *q = ' ';
    char p[320];
    snprintf(p, sizeof p, HOME "/Export/%s %04d-%02d-%02d %02d%02d%02d.syx", who, tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    FILE *f = fopen(p, "wb");
    int n = 0;
    if (f) {
        if (device) {
            for (int i = 0; i < s->ngen; i++) { fwrite(s->gen[i].d, 1, (size_t)s->gen[i].len, f); n++; }
        } else {
            for (int b = 0; b < 3; b++)
                for (int i = 0; i < 128; i++)
                    if (s->lib[b][i].ok) { fwrite(s->lib[b][i].d, 1, PATCH_LEN, f); n++; }
        }
        fclose(f);
    }
    if (f && n) set_text(s, s->disp, "Exported %d patches to SD: Nova Editor/Export", n);
    else set_text(s, s->disp, n ? "Export failed: can't write to the SD card" : "Nothing to export yet: dump the synth's patches first");
}
static void save_capture(const unsigned char *d, int n) {
    static int k;
    char p[256];
    snprintf(p, sizeof p, HOME "/Captures/capture %ld-%d.syx", (long)time(NULL), k++);
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(d, 1, n, f); fclose(f); }
}

/* ---- whatever synth is on the other end ----------------------------------------------------------------------
 * The MiniNova is the only profile checked against hardware, so it is the only one that gets knob editing and
 * writing into a chosen slot. For anything else the plugin is a librarian: capture, list, export, send back. */
static int is_nova(const st_t *s) { return s->dev.verified && s->dev.dump_len == PATCH_LEN; }

static void gen_store(st_t *s, const unsigned char *d, int n) {
    if (n < 4 || n > GEN_LEN) return;
    for (int i = 0; i < s->ngen; i++)                     /* the same patch dumped twice replaces itself */
        if (s->gen[i].len == n && !memcmp(s->gen[i].d, d, (size_t)n)) return;
    if (s->ngen >= MAX_GEN) return;
    memcpy(s->gen[s->ngen].d, d, (size_t)n);
    s->gen[s->ngen].len = n;
    s->ngen++;
}
static void gen_name(st_t *s, int i, char *out, int outlen) {
    out[0] = 0;
    if (i < 0 || i >= s->ngen) return;
    if (s->dev.name_off >= 0 && s->dev.dump_len == s->gen[i].len)
        dev_patch_name(&s->dev, s->gen[i].d, out, outlen);
    if (!out[0]) snprintf(out, (size_t)outlen, "%d bytes", s->gen[i].len);
}

/* F0 7E <dev> 06 02 <manufacturer 1 or 3 bytes> <family lo hi> <member lo hi> <version...> F7 */
static int identity_reply(st_t *s, const unsigned char *d, int n) {
    if (n < 12 || d[1] != 0x7e || d[3] != 0x06 || d[4] != 0x02) return 0;
    int at = 5, len = d[5] == 0x00 ? 3 : 1;
    if (at + len + 4 > n) return 0;
    memcpy(s->id_mfr, d + at, (size_t)len);
    s->id_mfr_len = len;
    at += len;
    s->id_family = d[at] | (d[at + 1] << 7);
    s->id_member = d[at + 2] | (d[at + 3] << 7);
    s->have_id = 1;

    const char *maker = dev_maker(s->id_mfr, len);
    if (len == DEV_MININOVA.mfr_len && !memcmp(s->id_mfr, DEV_MININOVA.mfr, (size_t)len) && s->id_family == 0x03) {
        s->dev = DEV_MININOVA;                            /* a Nova: the verified profile, full editing */
        snprintf(s->id_name, sizeof s->id_name, "%s", s->dev.name);
    } else {
        memset(&s->learn, 0, sizeof s->learn);            /* a different synth: learn its dumps from scratch */
        dev_learned_profile(&s->learn, "synth", &s->dev);
        s->dev.dump_len = 0;
        if (maker) snprintf(s->id_name, sizeof s->id_name, "%s %d.%d", maker, s->id_family, s->id_member);
        else if (len == 1) snprintf(s->id_name, sizeof s->id_name, "Synth %02X %d.%d", s->id_mfr[0], s->id_family, s->id_member);
        else snprintf(s->id_name, sizeof s->id_name, "Synth %02X%02X%02X %d.%d", s->id_mfr[0], s->id_mfr[1],
                      s->id_mfr[2], s->id_family, s->id_member);
        s->dev.name = s->id_name;
        if (s->ui[C_LIB_BANK] < LIB_DEVICE) {             /* its patches go in their own bank */
            s->ui[C_LIB_BANK] = LIB_DEVICE;
            s->changed[C_LIB_BANK] = 1;
            s->ui[C_LIB_PAGE] = 1;
            s->changed[C_LIB_PAGE] = 1;
        }
    }
    set_text(s, s->disp, "%s answered. %s", s->id_name,
             is_nova(s) ? "Editing and writing available" : "Librarian only: receive, export and send back");
    s->rev++;
    return 1;
}

/* ---- the library view ---- */
static const patch_t *entry(st_t *s, int src) {
    if (src >= 2000) return NULL;   /* another synth's patches are variable-length: s->gen, not patch_t */
    if (src >= 1000) return (src - 1000 < s->nimp) ? &s->imp[src - 1000] : NULL;
    if (src >= 0 && src < 384) return &s->lib[src / 128][src % 128];
    return NULL;
}
static int row_src(st_t *s, int row) {   /* row 0..15 on the current page -> entry, or -1 */
    int bank = s->ui[C_LIB_BANK], idx = (s->ui[C_LIB_PAGE] - 1) * ROWS + row;
    if (bank == LIB_DEVICE) return idx < s->ngen ? 2000 + idx : -1;
    if (bank == LIB_IMPORTED) return idx < s->nimp ? 1000 + idx : -1;
    return idx < 128 ? bank * 128 + idx : -1;
}
static int lib_pages(st_t *s) {
    int count = s->ui[C_LIB_BANK] == LIB_DEVICE ? s->ngen : s->ui[C_LIB_BANK] == LIB_IMPORTED ? s->nimp : 128;
    int pg = (count + ROWS - 1) / ROWS;
    return pg < 1 ? 1 : pg;
}
static void show_patch(st_t *s, const char *prefix) {
    const patch_t *p = s->cur_bank >= 0 && s->cur_slot >= 0 ? &s->lib[s->cur_bank][s->cur_slot] : NULL;
    char name[20] = "";
    if (p && p->ok) patch_name(p->d, name);
    (void)prefix; (void)name;
    s->disp_ms = 0;                  /* a patch change ends any message */
    s->rev++;
}
static void page_to(st_t *s, int bank, int slot) {   /* follow the synth: show the page holding this patch */
    int pg = slot / ROWS + 1;
    if (s->ui[C_LIB_BANK] != bank) { s->ui[C_LIB_BANK] = bank; s->changed[C_LIB_BANK] = 1; }
    if (s->ui[C_LIB_PAGE] != pg) { s->ui[C_LIB_PAGE] = pg; s->changed[C_LIB_PAGE] = 1; }
    s->rev++;
}
/* Leaving the current patch loses any unsaved edits (they live in the synth's edit buffer, not in a slot), so a
 * genuinely modified patch asks once before it goes. An unmodified one just changes, with no prompt at all. */
static int nav_blocked(st_t *s, int bank, int slot) {
    long t = now_ms();
    if (!patch_modified(s)) return 0;
    if (s->nav_armed_ms && t - s->nav_armed_ms < 5000 && s->nav_bank == bank && s->nav_slot == slot) {
        s->nav_armed_ms = 0;     /* confirmed: go, and lose the edits */
        return 0;
    }
    s->nav_armed_ms = t;
    s->nav_bank = bank; s->nav_slot = slot;
    set_text(s, s->disp, "%d UNSAVED EDIT%s: WRITE FIRST, OR TAP AGAIN TO DISCARD", patch_edits(s),
             patch_edits(s) == 1 ? "" : "S");
    return 1;
}

static void call_up(st_t *s, int bank, int slot) {   /* the synth goes to this patch; edits made here start over */
    s->cur_bank = bank; s->cur_slot = slot;
    s->pc_bank = bank; s->pc_slot = slot; s->pc_pending = 1;
    snapshot(s);                     /* a freshly loaded patch starts unmodified */
    arm_echo(s);
    s->sel_src = bank * 128 + slot;
    show_patch(s, "");
    pthread_cond_signal(&s->wake);
}

/* ---- incoming ---- */
static void got_sysex(st_t *s, const unsigned char *d, int n) {
    /* A MiniNova patch is always recognised as one, whatever profile is active, so plugging the Nova back in after
     * another synth just works. Its bank and slot bytes come from the profile, not from guesswork. */
    if (dev_is_patch(&DEV_MININOVA, d, n) && d[DEV_MININOVA.bank_off] >= 1 && d[DEV_MININOVA.bank_off] <= 3 &&
        d[DEV_MININOVA.slot_off] < 128) {
        lib_store(s, d);
        s->lib_dirty[d[11] - 1] = 1;
        s->last_dump_ms = now_ms();
        s->dumps_in++;
        arm_echo(s);             /* receiving patches refreshes the screen; the echoes are not edits */
        set_text(s, s->disp, "RECEIVING  %c %03d", BANKS[d[11] - 1], d[12] + 1);
        s->receive_armed = 0;
        return;
    }
    if (identity_reply(s, d, n)) return;
    save_capture(d, n);   /* keep every message, whether or not it is understood */
    /* The MiniNova's other SysEx (globals, and so on) is not a patch, and its patches took the branch above. Any
     * other maker's bytes mean another synth is talking, whether or not it ever answered the identity request. */
    int novation = n > 4 && d[1] == 0x00 && d[2] == 0x20 && d[3] == 0x29;
    if (is_nova(s) && novation) {
        set_text(s, s->disp, "Saved a %d-byte message from the synth to Captures", n);
        return;
    }
    /* Another synth, or one that never answered the identity request: repeated messages of one shape are its
     * patches. Their names are read off the data; nothing guesses which byte picks a slot. */
    if (n >= 16 && n <= GEN_LEN) {
        int before = s->ngen;
        gen_store(s, d, n);
        if (dev_learn(&s->learn, d, n)) {
            const char *keep = s->have_id ? s->id_name : "Unknown synth";
            dev_learned_profile(&s->learn, keep, &s->dev);   /* learned, so never verified: no writing to slots */
        }
        s->last_dump_ms = now_ms();
        if (s->ngen != before) s->dumps_in++;
        s->receive_armed = 0;
        if (s->ui[C_LIB_BANK] != LIB_DEVICE) {
            s->ui[C_LIB_BANK] = LIB_DEVICE; s->changed[C_LIB_BANK] = 1;
            s->ui[C_LIB_PAGE] = 1; s->changed[C_LIB_PAGE] = 1;
        }
        set_text(s, s->disp, "RECEIVING  %d patch%s from %s", s->ngen, s->ngen == 1 ? "" : "es",
                 s->have_id ? s->id_name : "the synth");
        s->rev++;
        return;
    }
    set_text(s, s->disp, "Saved a %d-byte message from the synth to Captures", n);
}
static void got_cc(st_t *s, int cc, int v) {
    if (cc == 32) { if (v >= 1 && v <= 3) s->rx_bank = v - 1; return; }
    if (cc == 0) return;
    if (cc == 99) { s->nrpn_msb = v; return; }
    if (cc == 98) { s->nrpn_lsb = v; return; }
    if (cc == 6) {
        for (int r = 0; r < NREAL; r++) {
            const nreal_t *t = &NREAL_T[r];
            if (t->kind == 1 && t->a == s->nrpn_msb && t->b == s->nrpn_lsb) {
                if (s->raw[r] != v) { s->raw[r] = (unsigned char)v; mark_controls_of(s, r); s->rev++; }
                return;
            }
            if (t->kind == 2 && t->a == s->nrpn_msb && t->b == s->nrpn_lsb && (v == t->lo || v == t->hi)) {
                int on = v == t->hi;
                if (s->raw[r] != on) { s->raw[r] = (unsigned char)on; mark_controls_of(s, r); s->rev++; }
                return;
            }
        }
        return;
    }
    for (int r = 0; r < NREAL; r++)
        if (NREAL_T[r].kind == 0 && NREAL_T[r].a == cc) {
            if (s->raw[r] != v) { s->raw[r] = (unsigned char)v; mark_controls_of(s, r); s->rev++; }
            return;
        }
}
static void handle_in(st_t *s, snd_seq_event_t *ev) {
    switch (ev->type) {
    case SND_SEQ_EVENT_CONTROLLER: got_cc(s, ev->data.control.param, ev->data.control.value); break;
    case SND_SEQ_EVENT_PGMCHANGE: {
        int slot = ev->data.control.value & 127, bank = s->rx_bank >= 0 ? s->rx_bank : (s->cur_bank >= 0 ? s->cur_bank : 0);
        s->cur_bank = bank; s->cur_slot = slot;
        s->sel_src = bank * 128 + slot;
        snapshot(s);                 /* the synth changed patch: a fresh slate, nothing modified */
        arm_echo(s);
        page_to(s, bank, slot);
        show_patch(s, "");
        break;
    }
    case SND_SEQ_EVENT_SYSEX: {
        const unsigned char *b = ev->data.ext.ptr;
        for (unsigned i = 0; i < ev->data.ext.len; i++) {
            if (b[i] == 0xf0) s->sx_len = 0;
            if (s->sx_len < (int)sizeof s->sx) s->sx[s->sx_len++] = b[i];
            if (b[i] == 0xf7) { got_sysex(s, s->sx, s->sx_len); s->sx_len = 0; }
        }
        break;
    }
    default: break;
    }
}

/* find the bridge's port and connect to it */
/* Is the MiniNova on the USB bus? (The bridge's port stays up while it's unplugged.) */
static int nova_on_usb(void) {
    DIR *d = opendir("/sys/bus/usb/devices");
    if (!d) return 1;   /* can't tell: don't claim it's gone */
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.' || strchr(e->d_name, ':')) continue;
        char p[300], v[8] = "", q[8] = "";
        snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/idVendor", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (!fgets(v, sizeof v, f)) v[0] = 0;
        fclose(f);
        if (strncmp(v, "1235", 4)) continue;
        snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/idProduct", e->d_name);
        if ((f = fopen(p, "r"))) { if (!fgets(q, sizeof q, f)) q[0] = 0; fclose(f); }
        found = !strncmp(q, "001e", 4);
    }
    closedir(d);
    return found;
}

static void link_scan(st_t *s) {
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t *pi;
    int found = 0, c = -1, p = -1;
    if (A.snd_seq_client_info_malloc(&ci) < 0) return;
    if (A.snd_seq_port_info_malloc(&pi) < 0) { A.snd_seq_client_info_free(ci); return; }
    /* The bridge shares the MiniNova as the client "MiniNova"; that is what this was built for, so it wins. Any
     * other synth reachable as a sequencer client is taken as a fallback, which is what makes other synths work. */
    int alt_c = -1, alt_p = -1, bridge = 0;
    char alt_name[64] = "";
    A.snd_seq_client_info_set_client(ci, -1);
    while (!found && A.snd_seq_query_next_client(s->seq, ci) >= 0) {
        const char *cname = A.snd_seq_client_info_get_name(ci);
        int cid = A.snd_seq_client_info_get_client(ci);
        int is_bridge = !strcmp(cname, "MiniNova");
        if (cid == s->me || cid == 0) continue;                      /* ourselves, and ALSA's own System client */
        if (!is_bridge && (!strcmp(cname, "Midi Through") || strstr(cname, "MPC"))) continue;
        A.snd_seq_port_info_set_client(pi, cid);
        A.snd_seq_port_info_set_port(pi, -1);
        while (A.snd_seq_query_next_port(s->seq, pi) >= 0) {
            const char *pname = A.snd_seq_port_info_get_name(pi);
            if (is_bridge && !strcmp(pname, "MIDI")) {
                c = cid; p = A.snd_seq_port_info_get_port(pi); found = bridge = 1;
                break;
            }
            if (!is_bridge && alt_c < 0) {
                alt_c = cid; alt_p = A.snd_seq_port_info_get_port(pi);
                snprintf(alt_name, sizeof alt_name, "%s", cname);
            }
        }
    }
    A.snd_seq_port_info_free(pi);
    A.snd_seq_client_info_free(ci);
    if (!found && alt_c >= 0) { c = alt_c; p = alt_p; found = 1; }
    if (!found) { s->linked = 0; set_text(s, s->link, "Bridge not found: is nova-bridge installed?"); return; }
    /* The bridge's port stays up while the MiniNova is unplugged, so the USB bus is what says it is really there.
     * That check is about the MiniNova only: another synth arrives on its own port, which disappears with it. */
    if (bridge && !nova_on_usb()) { s->linked = 0; set_text(s, s->link, "MiniNova not connected"); return; }
    int r = A.snd_seq_connect_from(s->seq, s->port, c, p);
    if (r < 0 && r != -EBUSY) { s->linked = 0; set_text(s, s->link, "Can't listen to the bridge: %s", A.snd_strerror(r)); return; }
    s->nova_client = c; s->nova_port = p;
    if (!s->linked) { s->linked = 1; s->id_req_pending = 1; pthread_cond_signal(&s->wake); }
    char want[80];
    snprintf(want, sizeof want, "Linked to %s", bridge ? "MiniNova" : alt_name[0] ? alt_name : "a synth");
    if (strcmp(s->link, want)) {
        set_text(s, s->link, "%s", want);
        if (!s->tap && s->ui[C_AUDIO]) set_text(s, s->disp, "No audio: install the nova-audio addin (or set AUDIO IN off)");
        else if (s->cur_bank < 0) set_text(s, s->disp, "%s linked. Pick a patch, or dump the synth to fill the library",
                                           bridge ? "MiniNova" : alt_name[0] ? alt_name : "Synth");
    }
}

static void *reader(void *arg) {
    st_t *s = arg;
    int nfd = A.snd_seq_poll_descriptors_count(s->seq, POLLIN);
    if (nfd < 1 || nfd > 4) nfd = 1;
    struct pollfd pfd[4];
    A.snd_seq_poll_descriptors(s->seq, pfd, nfd, POLLIN);
    while (!s->quit) {
        int pr = poll(pfd, nfd, 200);
        pthread_mutex_lock(&s->mx);
        if (pr > 0) {
            snd_seq_event_t *ev = NULL;
            int guard = 0;
            while (guard++ < 512 && A.snd_seq_event_input(s->seq, &ev) >= 0 && ev) {
                if (ev->source.client == s->nova_client) handle_in(s, ev);
                ev = NULL;
                if (A.snd_seq_event_input_pending(s->seq, 0) <= 0) break;
            }
        }
        long t = now_ms();
        if (t >= s->next_scan_ms) { link_scan(s); s->next_scan_ms = t + 700; }
        /* a dump finished: save the banks it filled */
        if (s->dumps_in && t - s->last_dump_ms > 1500) {
            for (int b = 0; b < 3; b++) if (s->lib_dirty[b]) { lib_save_bank(s, b); s->lib_dirty[b] = 0; }
            set_text(s, s->disp, "RECEIVED  %d patch%s", s->dumps_in, s->dumps_in == 1 ? "" : "es");
            s->dumps_in = 0;
            s->rev++;
        }
        if (s->write_armed_ms && t - s->write_armed_ms > 5000) { s->write_armed_ms = 0; show_patch(s, ""); }
        if (s->disp_shown && t - s->disp_ms >= MSG_MS) { s->disp_shown = 0; s->rev++; }
        if (s->linked != s->linked_rep) { s->linked_rep = s->linked; s->changed[C_LINKED] = 1; s->rev++; }
        pthread_mutex_unlock(&s->mx);
    }
    return NULL;
}

/* ---- engine interface ---- */
static void *create(const char *data_dir) {
    (void)data_dir;
    int have_alsa = alsa_load();
    st_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    init_indices();
    pthread_mutex_init(&s->mx, NULL);
    pthread_cond_init(&s->wake, NULL);
    for (int r = 0; r < NREAL; r++) s->raw[r] = NREAL_T[r].dflt;
    for (int i = 0; i < NPARAMS; i++) {   /* editor-only controls start at their defaults */
        int c = ctl_index(PARAMS[i].key);
        if (c < 0 || NCTL_T[c].nreal) continue;
        const param_t *q = &PARAMS[i];   /* defaults are stored normalized (0..1) */
        s->ui[c] = q->nopts ? (int)(q->def * (q->nopts - 1) + 0.5f) : (int)(q->min + q->def * (q->max - q->min) + 0.5f);
    }
    snapshot(s);                     /* the defaults are the starting point, not an edit */
    s->chan = s->ui[C_CHAN] > 0 ? s->ui[C_CHAN] - 1 : 0;
    s->cur_bank = s->cur_slot = -1;
    s->edit_r = -1;
    s->sel_src = -1;
    s->rx_bank = -1;
    s->nova_client = -1;
    s->dev = DEV_MININOVA;           /* what this was built for, until a synth says otherwise */
    s->id_family = s->id_member = -1;
    strcpy(s->disp, "Nova Editor");
    strcpy(s->link, "Looking for the MiniNova...");
    nova_audio_tap_fn fn = (nova_audio_tap_fn)dlsym(RTLD_DEFAULT, "nova_audio_tap");
    if (fn) {
        nova_tap_t *t = fn();
        if (t && t->magic == NOVA_TAP_MAGIC && t->version == NOVA_TAP_VERSION) s->tap = t;
    }
    mkdirs();
    lib_load(s);
    imports_scan(s);
    if (!have_alsa) { set_text(s, s->link, "MIDI library missing (libasound)"); return s; }
    int r = A.snd_seq_open(&s->seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
    if (r < 0) { s->seq = NULL; set_text(s, s->link, "Can't open MIDI: %s", A.snd_strerror(r)); return s; }
    A.snd_seq_set_client_name(s->seq, "Nova Editor");
    s->me = A.snd_seq_client_id(s->seq);
    s->port = A.snd_seq_create_simple_port(s->seq, "Editor", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_WRITE,
                                           SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    link_scan(s);
    s->next_scan_ms = now_ms() + 2000;
    s->rd_on = pthread_create(&s->rd, NULL, reader, s) == 0;
    s->wr_on = pthread_create(&s->wr, NULL, writer, s) == 0;
    return s;
}

static void destroy(void *inst) {
    st_t *s = inst;
    pthread_mutex_lock(&s->mx);
    s->quit = 1;
    pthread_cond_signal(&s->wake);
    pthread_mutex_unlock(&s->mx);
    if (s->rd_on) pthread_join(s->rd, NULL);
    if (s->wr_on) pthread_join(s->wr, NULL);
    for (int b = 0; b < 3; b++) if (s->lib_dirty[b]) lib_save_bank(s, b);
    if (s->seq) A.snd_seq_close(s->seq);
    pthread_cond_destroy(&s->wake);
    pthread_mutex_destroy(&s->mx);
    free(s);
}

/* the track's notes play the MiniNova */
static void midi(void *inst, const uint8_t *m, int n) {
    st_t *s = inst;
    if (n < 2) return;
    pthread_mutex_lock(&s->mx);
    int nh = (s->nh + 1) % 256;
    if (nh != s->nt) {
        s->notes[s->nh][0] = m[0]; s->notes[s->nh][1] = m[1]; s->notes[s->nh][2] = n > 2 ? m[2] : 0;
        s->nh = nh;
        pthread_cond_signal(&s->wake);
    }
    pthread_mutex_unlock(&s->mx);
}

/* Numbers are read here rather than with atoi/sscanf: glibc redirects those to __isoc23_* symbols that only exist
 * from 2.38, and MPC OS ships older. Everything parsed here is this plugin's own text, so the rules are simple. */
static int nv_int(const char **pp, int *out) {       /* a decimal integer, after any spaces; 0 if there is none */
    const char *p = *pp;
    while (*p == ' ' || *p == '\t') p++;
    int sign = 1;
    if (*p == '-') { sign = -1; p++; } else if (*p == '+') p++;
    if (*p < '0' || *p > '9') return 0;
    long v = 0;
    while (*p >= '0' && *p <= '9') {
        if (v < 1000000000L) v = v * 10 + (*p - '0');
        p++;
    }
    *out = (int)(sign * v);
    *pp = p;
    return 1;
}
static int nv_atoi(const char *p) { int v = 0; nv_int(&p, &v); return v; }
static double nv_atof(const char *p) {               /* enough for the host's "0.75" and "1" */
    while (*p == ' ') p++;
    double sign = 1;
    if (*p == '-') { sign = -1; p++; } else if (*p == '+') p++;
    double v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    if (*p == '.') {
        p++;
        for (double scale = 0.1; *p >= '0' && *p <= '9'; p++, scale *= 0.1) v += (*p - '0') * scale;
    }
    return sign * v;
}
static int tapped(const char *v) { return nv_atof(v) > 0.5; }
/* MPC can deliver one tap as two "1" events with no release between (docs/NOTES.md, the plugin manager's buttons):
 * a repeat of the same button within 250 ms is the same tap. */
static int tap_once(st_t *s, const char *key, const char *v) {
    if (!tapped(v)) return 0;
    long t = now_ms();
    int dup = !strcmp(s->tap_key, key) && t - s->tap_ms < 250;
    snprintf(s->tap_key, sizeof s->tap_key, "%s", key);
    s->tap_ms = t;
    return !dup;
}

static void do_write(st_t *s) {
    /* Another synth's patch goes back exactly as it came. Which byte picks the slot is the synth's business, and
     * guessing it could overwrite the wrong patch, so the patch returns to where it came from and nowhere else. */
    if (s->sel_src >= 2000) {
        int i = s->sel_src - 2000;
        if (i >= s->ngen) { set_text(s, s->disp, "Pick a patch in the list first, then WRITE"); return; }
        char nm[40];
        gen_name(s, i, nm, sizeof nm);
        long t = now_ms();
        if (!s->write_armed_ms || t - s->write_armed_ms > 5000) {
            s->write_armed_ms = t;
            set_text(s, s->disp, "Tap WRITE again: send %s back to %s, into the slot it came from", nm,
                     s->have_id ? s->id_name : "the synth");
            return;
        }
        s->write_armed_ms = 0;
        memcpy(s->sx_out, s->gen[i].d, (size_t)s->gen[i].len);
        s->sx_out_len = s->gen[i].len;
        s->sx_out_pending = 1;
        set_text(s, s->disp, "Sent %s (%d bytes). The synth's memory protect must be off", nm, s->gen[i].len);
        s->rev++;
        pthread_cond_signal(&s->wake);
        return;
    }
    const patch_t *src = entry(s, s->sel_src);
    int tb = s->ui[C_TGT_BANK], ts = s->ui[C_TGT_SLOT] - 1;
    if (!src || !src->ok) { set_text(s, s->disp, "Pick a patch in the list first, then WRITE"); return; }
    char from[20], over[20] = "";
    patch_name(src->d, from);
    if (s->lib[tb][ts].ok) patch_name(s->lib[tb][ts].d, over);
    long t = now_ms();
    if (!s->write_armed_ms || t - s->write_armed_ms > 5000) {
        s->write_armed_ms = t;
        set_text(s, s->disp, "Tap WRITE again: %s -> %c%03d%s%s", from, BANKS[tb], ts + 1, over[0] ? " over " : "", over);
        return;
    }
    s->write_armed_ms = 0;
    memcpy(s->sx_out, src->d, PATCH_LEN);
    s->sx_out[11] = (unsigned char)(tb + 1);
    s->sx_out[12] = (unsigned char)ts;
    s->sx_out_len = PATCH_LEN;
    s->sx_out_pending = 1;
    lib_store(s, s->sx_out);
    s->lib_dirty[tb] = 1;
    lib_save_bank(s, tb);
    s->lib_dirty[tb] = 0;
    s->nav_armed_ms = 0;
    call_up(s, tb, ts);   /* hear it: the synth goes to the slot just written */
    set_text(s, s->disp, "Wrote %s to %c%03d (synth's Memory Protect must be Off)", from, BANKS[tb], ts + 1);
    s->rev++;
}

static void set_param(void *inst, const char *key, const char *val) {
    st_t *s = inst;
    if (!strcmp(key, "state")) {
        /* "ne1 <chan> <local> <bank> <slot> <sel ui values...> | r=v,r=v..." : saved by get_param("state") */
        pthread_mutex_lock(&s->mx);
        int ch = 1, lc = 1, b = -1, sl = -1, au = -1;
        int v2 = !strncmp(val, "ne2 ", 4), v1 = !strncmp(val, "ne1 ", 4);
        const char *q = val + 4;
        int ok = (v1 || v2) && nv_int(&q, &ch) && nv_int(&q, &lc) && nv_int(&q, &b) && nv_int(&q, &sl) &&
                 (!v2 || nv_int(&q, &au));
        if (ok) {
            if (au >= 0 && au <= 3) { s->ui[C_AUDIO] = au; s->changed[C_AUDIO] = 1; }
            s->ui[C_CHAN] = ch; s->chan = ch - 1;
            (void)lc;
            const char *p = strchr(val, '|');
            snapshot(s);             /* what is in the editor now is the patch; the saved edits below sit on top */
            while (p && *p) {
                int r = 0, v = 0;
                const char *e = p + 1;
                if (nv_int(&e, &r) && *e == '=' && (e++, nv_int(&e, &v)) && r >= 0 && r < NREAL) {
                    s->raw[r] = (unsigned char)v;   /* differs from orig, so the project reopens still modified */
                }
                p = strchr(p + 1, ',');
            }
            if (b >= 0 && b < 3 && sl >= 0 && sl < 128) {
                s->cur_bank = b; s->cur_slot = sl; s->sel_src = b * 128 + sl;
                s->pc_bank = b; s->pc_slot = sl; s->pc_pending = 1;
                page_to(s, b, sl);
                show_patch(s, "Recalled ");
            }
            arm_echo(s);             /* the restored values are about to be reported to the host */
            s->recall_pending = 1;
            s->recall_at_ms = now_ms() + 400;   /* after the synth has loaded the patch */
            for (int c = 0; c < NCTL; c++) s->changed[c] = 1;
            pthread_cond_signal(&s->wake);
            s->rev++;
        }
        pthread_mutex_unlock(&s->mx);
        return;
    }
    int c = ctl_index(key);
    if (c < 0) return;
    pthread_mutex_lock(&s->mx);
    const nctl_t *t = &NCTL_T[c];
    int v = nv_atoi(val);
    if (t->nreal) {
        int r = ctl_real(s, c);
        if (r >= 0) {
            const nreal_t *q = &NREAL_T[r];
            int raw = t->disp == 3 ? (v < 0 ? v + 128 : v) : v + t->off;
            int lo = q->kind == 2 ? 0 : q->lo, hi = q->kind == 2 ? 1 : q->hi;
            if (t->disp != 3) { if (raw < lo) raw = lo; if (raw > hi) raw = hi; }
            /* The host echoing back a value the engine just reported, one step off (see ECHO_MS): not an edit. */
            int in_window = !s->echo_used[r] && now_ms() - s->echo_ms < ECHO_MS;
            if (in_window) {
                s->echo_used[r] = 1;   /* spent, whether or not it moved: the next write to it is the user's */
                if (raw != s->raw[r] && raw != s->echo_raw[r] && abs(raw - (int)s->echo_raw[r]) <= ECHO_STEP)
                    raw = s->echo_raw[r];
            }
            if (raw != s->raw[r]) {
                s->raw[r] = (unsigned char)raw;
                s->dirty[r] = 1;
                s->edit_r = r;         /* exempt from the guard while the user is on it (arm_echo) */
                s->edit_ms = now_ms();
                pthread_cond_signal(&s->wake);
                /* the display says what moved: "OSC 2  SEMITONES  +7" */
                const char *unit = "";
                char ubuf[24] = "";
                if (t->sel >= 0) {
                    const char *sk = NCTL_T[t->sel].key;
                    unit = !strcmp(sk, "osc_sel") ? "OSC" : !strcmp(sk, "env_sel") ? "ENV" : "LFO";
                    if (!strcmp(sk, "env_sel") && s->ui[t->sel] == 0) snprintf(ubuf, sizeof ubuf, "AMP ENV  ");
                    else snprintf(ubuf, sizeof ubuf, "%s %d  ", unit, s->ui[t->sel] + 1);
                }
                int pi = param_index(key);
                const char *name = pi >= 0 ? PARAMS[pi].name : key;
                if (pi >= 0 && PARAMS[pi].nopts && v >= 0 && v < PARAMS[pi].nopts)
                    set_text(s, s->disp, "%s%s  %s", ubuf, name, PARAMS[pi].opts[v]);
                else set_text(s, s->disp, "%s%s  %d", ubuf, name, v);
            }
        }
    } else if (!strncmp(key, "row_", 4)) {
        if (tap_once(s, key, val)) {
            int src = row_src(s, nv_atoi(key + 4) - 1);
            const patch_t *p = entry(s, src);
            if (src >= 2000) {
                char nm[40];
                gen_name(s, src - 2000, nm, sizeof nm);
                s->sel_src = src;
                set_text(s, s->disp, "%s  (tap WRITE twice to send it back to the synth)", nm);
            }
            else if (src >= 0 && src < 1000) { if (!nav_blocked(s, src / 128, src % 128)) call_up(s, src / 128, src % 128); }
            else if (p && p->ok) {
                char nm[20];
                patch_name(p->d, nm);
                s->sel_src = src;
                set_text(s, s->disp, "Imported: %s  (choose a slot, then WRITE)", nm);
            }
            s->rev++;
        }
    } else if (!strcmp(key, "lib_page_prev") || !strcmp(key, "lib_page_next")) {
        if (tap_once(s, key, val)) {
            int pages = lib_pages(s);
            int pg = s->ui[C_LIB_PAGE] + (key[9] == 'n' ? 1 : -1);
            if (pg < 1) pg = pages;
            if (pg > pages) pg = 1;
            s->ui[C_LIB_PAGE] = pg; s->changed[C_LIB_PAGE] = 1; s->rev++;
        }
    } else if (!strcmp(key, "write")) {
        if (tap_once(s, key, val)) do_write(s);
    } else if (!strcmp(key, "receive")) {
        if (tap_once(s, key, val)) {
            s->receive_armed = 1;
            if (!s->linked) set_text(s, s->disp, "CONNECT THE SYNTH FIRST");
            else {
                s->id_req_pending = 1;        /* ask who is there, then wait for whatever it sends */
                pthread_cond_signal(&s->wake);
                if (is_nova(s)) set_text(s, s->disp, "READY: ON THE MININOVA, MENU > DUMP > ALL");
                else set_text(s, s->disp, "READY: dump patches from the synth (its SysEx or MIDI dump menu)");
            }
        }
    } else if (!strcmp(key, "export")) {
        if (tap_once(s, key, val)) export_all(s);
    } else if (!strcmp(key, "patch_prev") || !strcmp(key, "patch_next")) {
        if (tap_once(s, key, val)) {
            int b0 = s->cur_bank >= 0 ? s->cur_bank : 0, sl = s->cur_slot >= 0 ? s->cur_slot : 0;
            int i = (b0 * 128 + sl + (key[6] == 'n' ? 1 : 383)) % 384;
            if (nav_blocked(s, i / 128, i % 128)) { pthread_mutex_unlock(&s->mx); return; }
            call_up(s, i / 128, i % 128);
            page_to(s, i / 128, i % 128);
        }
    } else {
        int old = s->ui[c];
        s->ui[c] = v;
        if (c == C_CHAN) s->chan = v >= 1 && v <= 16 ? v - 1 : 0;
        if (c == C_LIB_BANK && v != old) {
            s->ui[C_LIB_PAGE] = 1; s->changed[C_LIB_PAGE] = 1;
            if (v == 3) {   /* Imported: read the SD card's Import folder now */
                imports_scan(s);
                set_text(s, s->disp, "Imported: %d patch%s in SD: Nova Editor/Import", s->nimp, s->nimp == 1 ? "" : "es");
            }
        }
        if (v != old) {   /* a selector: its proxies now show another unit */
            for (int k = 0; k < NCTL; k++) if (NCTL_T[k].sel == c) s->changed[k] = 1;
            arm_echo(s);   /* those controls are about to be reported: the echoes are not edits */
            s->rev++;
        }
    }
    pthread_mutex_unlock(&s->mx);
}

static int get_param(void *inst, const char *key, char *b, int n) {
    st_t *s = inst;
    int r = 0;
    pthread_mutex_lock(&s->mx);
    size_t L = strlen(key);
    if (!strcmp(key, "display_rev")) {
        /* The wrapper re-reads and reports every parameter whenever this value changes, and the host writes each of
         * them back: arm the echo guard here, so no refresh can be taken for an edit whatever caused it. */
        if (s->rev != s->rev_sent) { s->rev_sent = s->rev; arm_echo(s); }
        r = snprintf(b, n, "%u", s->rev);
    }
    else if (!strcmp(key, "lcd_info") || !strcmp(key, "lcd_name")) {
        static const char *CAT[] = {"", "ARP", "BASS", "BELL", "CLASSIC", "DRUM", "KEYBOARD", "LEAD", "MOVEMENT", "PAD",
                                    "POLY", "SFX", "STRING", "EXT INPUT", "VOC/TUNE"};
        static const char *GEN[] = {"", "CLASSIC", "D&B/BREAKS", "HOUSE", "INDUSTRIAL", "JAZZ", "R&B/HIP-HOP", "ROCK/POP",
                                    "TECHNO", "DUBSTEP"};
        const patch_t *p = s->cur_bank >= 0 && s->cur_slot >= 0 ? &s->lib[s->cur_bank][s->cur_slot] : NULL;
        int msg = s->disp_ms && now_ms() - s->disp_ms < MSG_MS;
        if (key[4] == 'i') {
            if (msg) {
                r = snprintf(b, n, "%s", s->disp);
                for (char *q = b; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;   /* the info line is all capitals */
            }
            else if (!s->tap && s->ui[C_AUDIO]) r = snprintf(b, n, "NO AUDIO: NOVA-AUDIO ADDIN NOT LOADED");
            else if (s->cur_bank < 0) r = snprintf(b, n, "%s", s->linked ? "PICK A PATCH" : "");
            else {
                const char *c = p && p->ok && p->d[31] < 15 ? CAT[p->d[31]] : "", *g = p && p->ok && p->d[32] < 10 ? GEN[p->d[32]] : "";
                r = snprintf(b, n, "%c %03d%s%s%s%s%s", BANKS[s->cur_bank], s->cur_slot + 1, *c ? "   " : "", c,
                             *g ? "  /  " : "", g, patch_modified(s) ? "   EDITED" : "");
            }
        } else {
            char nm[20] = "";
            if (p && p->ok) patch_name(p->d, nm);
            if (!s->linked) r = snprintf(b, n, "%s", !s->seq ? s->link : strstr(s->link, "Bridge") ? "NO BRIDGE" : "NOT CONNECTED");
            else if (s->cur_bank < 0) r = snprintf(b, n, "%s", s->have_id ? s->id_name : "MiniNova");
            else r = snprintf(b, n, "%s", nm[0] ? nm : "(tap RECEIVE)");
        }
        if (r == 0) { b[0] = ' '; b[1] = 0; r = 1; }
    }
    else if (!strcmp(key, "linked")) r = snprintf(b, n, "%d", s->linked);
    else if (!strcmp(key, "state")) {
        int w = snprintf(b, n, "ne2 %d %d %d %d %d |", s->ui[C_CHAN], 1, s->cur_bank, s->cur_slot, s->ui[C_AUDIO]);
        for (int i = 0; i < NREAL && w < n - 16; i++)
            if (s->raw[i] != s->orig[i]) w += snprintf(b + w, n - w, "%d=%d,", i, s->raw[i]);
        r = w;
    } else if (!strncmp(key, "row_", 4)) {
        int row = nv_atoi(key + 4) - 1, on = L > 3 && !strcmp(key + L - 3, "_on");
        int src = row_src(s, row);
        const patch_t *p = entry(s, src);
        if (on) r = snprintf(b, n, "%d", src >= 0 && src == s->sel_src);
        else if (src < 0 && s->ui[C_LIB_BANK] == LIB_DEVICE && row == 0 && !s->ngen)
            r = snprintf(b, n, "Empty: tap RECEIVE, then dump patches from the synth");
        else if (src < 0) r = snprintf(b, n, "%s", "");
        else if (src >= 2000) {
            char nm[40];
            gen_name(s, src - 2000, nm, sizeof nm);
            r = snprintf(b, n, "%3d  %s", src - 2000 + 1, nm);
        }
        else if (s->ui[C_LIB_BANK] == LIB_IMPORTED) {
            char nm[20] = "";
            if (p) patch_name(p->d, nm);
            r = snprintf(b, n, "%3d  %s", src - 1000 + 1, nm);
        } else if (p && p->ok) {
            char nm[20];
            patch_name(p->d, nm);
            r = snprintf(b, n, "%03d  %s", src % 128 + 1, nm);
        } else if (row == 0 && !s->lib[s->ui[C_LIB_BANK]][0].ok) {
            r = snprintf(b, n, "Empty: tap RECEIVE, then dump from the MiniNova");
        } else r = snprintf(b, n, "%03d", src % 128 + 1);
        if (r == 0) { b[0] = ' '; b[1] = 0; r = 1; }   /* an empty row still answers */
    } else if (!strcmp(key, "lib_page_display")) {
        int bank = s->ui[C_LIB_BANK];
        int count = bank == LIB_DEVICE ? s->ngen : bank == LIB_IMPORTED ? s->nimp : 128;
        int last = s->ui[C_LIB_PAGE] * ROWS;
        if (!count) r = snprintf(b, n, "%s", bank == LIB_DEVICE ? "No patches yet" : "No imports");
        else r = snprintf(b, n, "%d - %d", (s->ui[C_LIB_PAGE] - 1) * ROWS + 1, last < count ? last : count);
    } else if (L > 8 && !strcmp(key + L - 8, "_display")) {
        char base[64];
        snprintf(base, sizeof base, "%.*s", (int)(L - 8), key);
        int c = ctl_index(base);
        if (c >= 0 && NCTL_T[c].nreal) {
            int rr = ctl_real(s, c), v = ctl_get(s, c);
            int pi = param_index(base);
            if (rr < 0) r = snprintf(b, n, "--");
            else if (NCTL_T[c].disp == 1) r = s->raw[rr] >= 120 ? snprintf(b, n, "Free") : snprintf(b, n, "%d deg", s->raw[rr] * 3);
            else if (NCTL_T[c].disp == 2) r = snprintf(b, n, "%+d dB", v);
            else if (NCTL_T[c].disp == 3) r = snprintf(b, n, "%+d oct", v);
            else if (pi >= 0 && PARAMS[pi].nopts && v >= 0 && v < PARAMS[pi].nopts) r = snprintf(b, n, "%s", PARAMS[pi].opts[v]);
            else r = snprintf(b, n, "%d", v);
        }
    } else {
        int c = ctl_index(key);
        if (c >= 0) {
            if (!strcmp(key, "write") || !strcmp(key, "export") || !strcmp(key, "receive") || !strncmp(key, "lib_page_", 9) || !strncmp(key, "patch_", 6))
                r = snprintf(b, n, "0");
            else r = snprintf(b, n, "%d", ctl_get(s, c));
        }
    }
    pthread_mutex_unlock(&s->mx);
    return r;
}

/* controls the engine moved (the synth turned a knob, a selector switched units, the library followed the synth) */
int mpc_engine_changed(void *inst, char *key, int len) {
    st_t *s = inst;
    int found = 0;
    pthread_mutex_lock(&s->mx);
    for (int c = 0; c < NCTL && !found; c++)
        if (s->changed[c]) {
            s->changed[c] = 0;
            if (!strncmp(NCTL_T[c].key, "row_", 4)) continue;
            snprintf(key, len, "%s", NCTL_T[c].key);
            found = 1;
        }
    pthread_mutex_unlock(&s->mx);
    return found;
}

/* The MiniNova's audio: MPC's input frames as the addin tapped them, about two blocks behind the newest ones, so a
 * block is always there whichever of MPC's input read and this render runs first in a cycle. Same clock, no drift. */
static void render(void *inst, int16_t *out, int frames) {
    st_t *s = inst;
    memset(out, 0, sizeof(int16_t) * 2 * frames);
    nova_tap_t *t = s->tap;
    int mode = s->ui[C_AUDIO];
    if (!t || !mode || !atomic_load_explicit(&t->ok, memory_order_acquire) || !t->ch) { s->rsync = 0; return; }
    uint64_t w = atomic_load_explicit(&t->wpos, memory_order_acquire);
    uint64_t lag = (uint64_t)frames * 2;
    if (!s->rsync || w < s->rpos + (uint64_t)frames || w - s->rpos > lag + 1024) {
        s->rpos = w > lag ? w - lag : 0;
        s->rsync = 1;
        if (w < s->rpos + (uint64_t)frames) return;   /* not enough yet */
    }
    unsigned ch = t->ch;
    int l = mode == 3 && ch > 1 ? 1 : 0, r = mode == 1 && ch > 1 ? 1 : l;
    for (int i = 0; i < frames; i++) {
        const int32_t *f = t->ring[(s->rpos + (uint64_t)i) & (NOVA_TAP_FRAMES - 1)];
        out[2 * i] = (int16_t)(f[l] >> 16);
        out[2 * i + 1] = (int16_t)(f[r] >> 16);
    }
    s->rpos += (uint64_t)frames;
}

static const mpc_engine_t ENGINE = {create, destroy, midi, set_param, get_param, render, NULL, NULL};
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
