/* Device identity and patch-dump profiles.
 *
 * Identity: every MIDI device answers a Universal Identity Request (F0 7E 7F 06 01 F7) with its manufacturer,
 * family and member. The manufacturer ids below are from the MIDI Association's published SysEx ID table, so the
 * maker is always named exactly; the family and member bytes are vendor-specific, so they are shown as they came
 * unless a profile recognises them.
 *
 * Profiles: a profile says how to read that synth's patch dumps -- how long one is, the fixed header that marks it,
 * and where the bank, slot and name sit. A profile that has been verified against the hardware allows writing a
 * patch into any slot. Anything else is handled generically: dumps are still captured, listed, exported and sent
 * back unchanged, but never rewritten to a different slot, because guessing which byte selects the slot could
 * overwrite the wrong patch. */
#pragma once
#include <string.h>

#define DEV_HDR_MAX 12

typedef struct {
    const char *name;                    /* what the display calls it */
    unsigned char mfr[3];
    int mfr_len;                         /* 1 or 3 */
    int family, member;                  /* from the identity reply; -1 matches anything */
    int dump_len;                        /* one patch dump, in bytes */
    int hdr_len;                         /* leading bytes that mark a patch dump */
    unsigned char hdr[DEV_HDR_MAX];
    int bank_off, bank_base;             /* byte holding the bank, and its value for the first bank */
    int slot_off, slot_base;
    int banks, slots;
    int name_off, name_len;              /* -1: no name field */
    int verified;                        /* 1: writing to a chosen slot is allowed */
} nova_dev_t;

/* Verified on hardware. The MiniNova and UltraNova share this patch format, but only the MiniNova has been
 * checked here, so the UltraNova is matched as the same family and left unverified. */
static const nova_dev_t DEV_MININOVA = {
    "Novation MiniNova", {0x00, 0x20, 0x29}, 3, -1, -1,
    526, 11, {0xf0, 0x00, 0x20, 0x29, 0x03, 0x01, 0x7f, 0x01, 0x01, 0x09, 0x04},
    11, 1, 12, 0, 3, 128, 15, 16, 1
};

/* Makers that have made synthesizers, from the published table. Anything else shows its id bytes. */
typedef struct { unsigned char id[3]; int len; const char *name; } dev_mfr_t;
static const dev_mfr_t DEV_MFR[] = {
    {{0x01}, 1, "Sequential"},            {{0x04}, 1, "Moog"},
    {{0x06}, 1, "Lexicon"},               {{0x07}, 1, "Kurzweil"},
    {{0x08}, 1, "Fender"},                {{0x0f}, 1, "Ensoniq"},
    {{0x10}, 1, "Oberheim"},              {{0x18}, 1, "E-mu"},
    {{0x1c}, 1, "Eventide"},              {{0x21}, 1, "Siel"},
    {{0x24}, 1, "Hohner"},                {{0x26}, 1, "Ketron"},
    {{0x29}, 1, "PPG"},                   {{0x2f}, 1, "Elka"},
    {{0x30}, 1, "Dynacord"},              {{0x33}, 1, "Clavia Nord"},
    {{0x35}, 1, "GeneralMusic"},          {{0x3a}, 1, "Steinberg"},
    {{0x3b}, 1, "Wersi"},                 {{0x3e}, 1, "Waldorf"},
    {{0x3f}, 1, "Quasimidi"},             {{0x40}, 1, "Kawai"},
    {{0x41}, 1, "Roland"},                {{0x42}, 1, "Korg"},
    {{0x43}, 1, "Yamaha"},                {{0x44}, 1, "Casio"},
    {{0x47}, 1, "Akai"},                  {{0x48}, 1, "JVC"},
    {{0x4c}, 1, "Sony"},                  {{0x52}, 1, "Zoom"},
    {{0x55}, 1, "Suzuki"},
    {{0x00, 0x00, 0x0e}, 3, "Alesis"},    {{0x00, 0x20, 0x29}, 3, "Novation"},
    {{0x00, 0x20, 0x32}, 3, "Behringer"}, {{0x00, 0x20, 0x33}, 3, "Access"},
    {{0x00, 0x20, 0x3c}, 3, "Elektron"},  {{0x00, 0x20, 0x6b}, 3, "Arturia"},
    {{0x00, 0x20, 0x76}, 3, "Teenage Engineering"},
};

static const char *dev_maker(const unsigned char *id, int len) {
    for (unsigned i = 0; i < sizeof DEV_MFR / sizeof *DEV_MFR; i++)
        if (DEV_MFR[i].len == len && !memcmp(DEV_MFR[i].id, id, len)) return DEV_MFR[i].name;
    return NULL;
}

/* Does this SysEx message look like one of the profile's patch dumps? */
static int dev_is_patch(const nova_dev_t *d, const unsigned char *m, int n) {
    return d->dump_len && n == d->dump_len && d->hdr_len && !memcmp(m, d->hdr, (size_t)d->hdr_len);
}

/* The name inside a dump, trimmed. Falls back to "" when the profile has no name field. */
static void dev_patch_name(const nova_dev_t *d, const unsigned char *m, char *out, int outlen) {
    int k = 0;
    if (d->name_off >= 0)
        for (int i = d->name_off; i < d->name_off + d->name_len && i < d->dump_len && k < outlen - 1; i++) {
            unsigned char c = m[i];
            if (!c) break;
            out[k++] = (c >= 32 && c < 127) ? (char)c : ' ';
        }
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = 0;
}

/* ---- learning an unknown synth's dumps -------------------------------------------------------------------
 * Only what can be read straight off the data: the length, the header the dumps share, and a name field (a run of
 * printable characters at the same offset in every dump, differing between them). The bank and slot bytes are
 * deliberately NOT guessed: those decide where a write lands. */
#define DEV_SCAN 64                      /* a name field lives in the first bytes of a dump, not deep in its data */
#define DEV_NAME_MAX 24

typedef struct {
    int len;                             /* dump length being learned, 0 = nothing yet */
    int hdr_len;
    unsigned char hdr[DEV_HDR_MAX];
    unsigned char first[DEV_SCAN];       /* the first dump, to compare the rest against */
    unsigned char varies[DEV_SCAN];      /* this byte has differed between dumps */
    unsigned char unprint[DEV_SCAN];     /* this byte was not a printable character in some dump */
    int name_off, name_len;
    int count;
} dev_learn_t;

/* Where the name is: the window whose bytes change the most from patch to patch while staying readable in all of
 * them. Constant filler scores zero however printable it looks, and a window spanning name and filler scores worse
 * than the name alone, so the field that actually carries the name wins. */
static void dev_find_name(dev_learn_t *L) {
    int top = L->len < DEV_SCAN ? L->len : DEV_SCAN;
    int best_off = -1, best_len = 0;
    double best = 0;
    for (int off = 0; off < top; off++) {
        if (L->unprint[off]) continue;
        for (int len = 4; len <= DEV_NAME_MAX && off + len <= top; len++) {
            if (L->unprint[off + len - 1]) break;
            int v = 0;
            for (int i = off; i < off + len; i++) v += L->varies[i] != 0;
            if (!v) continue;
            double score = (double)v / len;
            if (score > best + 1e-9 || (score > best - 1e-9 && len > best_len)) {
                best = score; best_off = off; best_len = len;
            }
        }
    }
    if (best_off >= 0) {
        while (best_len > 1 && !L->varies[best_off + best_len - 1]) best_len--;   /* drop trailing padding */
        L->name_off = best_off;
        L->name_len = best_len;
    }
}

/* Feed each captured dump in; returns 1 once enough is known to treat this shape as the synth's patches. */
static int dev_learn(dev_learn_t *L, const unsigned char *m, int n) {
    if (n < 16 || n > 1 << 20) return 0;
    int top = n < DEV_SCAN ? n : DEV_SCAN;
    if (L->len != n) {                   /* a different shape: start again on it */
        memset(L, 0, sizeof *L);
        L->len = n;
        L->hdr_len = n < DEV_HDR_MAX ? n : DEV_HDR_MAX;
        memcpy(L->hdr, m, (size_t)L->hdr_len);
        memcpy(L->first, m, (size_t)top);
        L->name_off = -1;
        L->count = 1;
        for (int i = 0; i < top; i++) if (m[i] < 32 || m[i] >= 127) L->unprint[i] = 1;
        return 0;
    }
    int common = 0;
    while (common < L->hdr_len && m[common] == L->hdr[common]) common++;
    L->hdr_len = common;                 /* the header is what every dump agrees on */
    L->count++;
    for (int i = 0; i < top; i++) {
        if (m[i] != L->first[i]) L->varies[i] = 1;
        if (m[i] < 32 || m[i] >= 127) L->unprint[i] = 1;
    }
    for (int i = 0; i < L->hdr_len && i < top; i++) L->varies[i] = 0;   /* the header is not part of a name */
    dev_find_name(L);
    return L->hdr_len >= 3 && L->count >= 2;
}

/* Turn what was learned into a profile that can list and send, but not rewrite slots. */
static void dev_learned_profile(const dev_learn_t *L, const char *name, nova_dev_t *out) {
    memset(out, 0, sizeof *out);
    out->name = name;
    out->mfr_len = 0;
    out->family = out->member = -1;
    out->dump_len = L->len;
    out->hdr_len = L->hdr_len < DEV_HDR_MAX ? L->hdr_len : DEV_HDR_MAX;
    memcpy(out->hdr, L->hdr, (size_t)out->hdr_len);
    out->bank_off = out->slot_off = -1;
    out->banks = 1;
    out->slots = 128;
    out->name_off = L->name_off;
    out->name_len = L->name_len;
    out->verified = 0;
}
