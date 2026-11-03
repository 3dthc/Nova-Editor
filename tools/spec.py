#!/usr/bin/env python3
"""Nova Editor's single source of truth: every MiniNova parameter the editor knows (its CC or NRPN address and raw range),
and how the editor's controls reach them. Writes vst/params.json (the VST parameters, in order) and src/nova_map.h
(the engine's tables). Run after any change:  python3 tools/spec.py

Addresses: Novation's "MiniNova Extended CC & NRPN Map" (version 0002), cross-checked with the pencilresearch/midi
MiniNova map (CC BY-SA 4.0, https://github.com/pencilresearch/midi). Where the two disagreed, Novation's chart wins.

Raw value <-> control value: a control shows `raw - off` (e.g. semitones raw 0..127 shown -64..63). Kinds:
  cc   one CC message;   nrpn  CC99 msb, CC98 lsb, CC6 value;
  sw   a switch packed into NRPN 0:122 (or 1:123): sends `lo` when off, `hi` when on.
Proxies: one set of controls per page edits the unit the page's selector picks (oscillator 1-3, filter 1-2, envelope
1-6, LFO 1-3). A unit without that parameter (envelope 1's delay) shows it greyed at 0 and sends nothing."""
import json, os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

SYNC = ["Off", "32nd T", "32nd", "16th T", "16th", "8th T", "16th D", "8th", "4th T", "8th D", "4th", "1+1/3", "4th D",
        "2nd", "2+2/3", "3 beats", "4 beats", "5+1/3", "6 beats", "8 beats", "10+2/3", "12 beats", "13+1/3", "16 beats",
        "18 beats", "18+2/3", "20 beats", "21+1/3", "24 beats", "28 beats", "30 beats", "32 beats", "36 beats",
        "42 beats", "48 beats", "64 beats"]
WAVES = ["Sine", "Triangle", "Sawtooth", "Saw 9:1PW", "Saw 8:2PW", "Saw 7:3PW", "Saw 6:4PW", "Saw 5:5PW", "Saw 4:6PW",
         "Saw 3:7PW", "Saw 2:8PW", "Saw 1:9PW", "Pulse", "Square", "Bass Camp", "Bass FM", "EP Dull", "EP Bell", "Clav",
         "Double Reed", "Retro", "String Mch 1", "String Mch 2", "Organ 1", "Organ 2", "Evil Organ", "Hi Stuff",
         "Bell FM 1", "Bell FM 2", "Dig Bell 1", "Dig Bell 2", "Dig Bell 3", "Dig Bell 4", "Digi Pad"] + \
        ["Wavetable %d" % i for i in range(1, 37)] + ["Audio In L", "Audio In R"]
FILTERS = ["LP6", "LP12", "LP18", "LP24", "BP6-6", "BP12-12", "BP6-12", "BP12-6", "BP6-18", "BP18-6", "HP6", "HP12",
           "HP18", "HP24"]
DRIVES = ["Diode", "Valve", "Clipper", "X-Over", "Rectify", "Bits Down", "Rate Down"]
LFO_WAVES = ["Sine", "Triangle", "Sawtooth", "Square", "Random S/H", "Time S/H", "Piano Env", "Seq 1", "Seq 2", "Seq 3",
             "Seq 4", "Seq 5", "Seq 6", "Seq 7", "Alt 1", "Alt 2", "Alt 3", "Alt 4", "Alt 5", "Alt 6", "Alt 7", "Alt 8",
             "Chromatic", "Chrom 16", "Major", "Major 7", "Minor 7", "Min Arp 1", "Min Arp 2", "Diminished",
             "Dec Minor", "Minor 3rd", "Pedal", "4ths", "4ths x12", "1625 Maj", "1625 Min", "2511"]
FX_TYPES = ["Bypass", "EQ", "Comp 1", "Comp 2", "Dist 1", "Dist 2", "Delay 1", "Delay 2", "Reverb 1", "Reverb 2",
            "Chorus 1", "Chorus 2", "Chorus 3", "Chorus 4", "Gator"]
ARP_RATES = ["32nd T", "32nd", "16th T", "16th", "8th T", "16th D", "8th", "4th T", "8th D", "4th", "1+1/3", "4th D",
             "2nd", "2+2/3", "3 beats", "4 beats", "5+1/3", "6 beats", "8 beats"]
OFFON = ["Off", "On"]

REAL = []          # (key, kind, a, b, lo, hi, default_raw)
def real(key, kind, a, b=0, lo=0, hi=127, dflt=None):
    REAL.append((key, kind, a, b, lo, hi, (0 if kind == "sw" else lo) if dflt is None else dflt))   # a switch stores 0/1

# ---- oscillators: CC addresses per oscillator (wave, interp, pw, sync, hard, dens, ddet, semi, cents, bend)
OSC_CC = {1: [19, 20, 21, 22, 23, 24, 25, 26, 27, 28],
          2: [29, 30, 31, 33, 34, 35, 36, 37, 39, 40],
          3: [41, 42, 43, 44, 45, 46, 47, 48, 49, 50]}
OSC_P = [("wave", 0, 71, 2), ("interp", 0, 127, 0), ("pw", 0, 127, 64), ("sync", 0, 127, 0), ("hard", 0, 127, 0),
         ("dens", 0, 127, 0), ("ddet", 0, 127, 0), ("semi", 0, 127, 64), ("cents", 0, 127, 64), ("bend", 52, 76, 76)]
for n in (1, 2, 3):
    for (p, lo, hi, d), cc in zip(OSC_P, OSC_CC[n]):
        real("osc%d_%s" % (n, p), "cc", cc, lo=lo, hi=hi, dflt=d)
real("drift", "cc", 16); real("phase", "cc", 17, hi=120); real("vib_spd", "cc", 76); real("vib_dep", "cc", 77)
# ---- mixer and voice
for i, k in enumerate(["lvl1", "lvl2", "lvl3", "rm13", "rm23", "noise"]):
    real(k, "cc", 51 + i, dflt=127 if k == "lvl1" else 0)
real("noise_col", "cc", 57, hi=3); real("pre_fx", "cc", 58, lo=52, hi=82, dflt=64); real("post_fx", "cc", 59, lo=52, hi=82, dflt=64)
real("poly", "cc", 3, hi=4, dflt=2); real("unison", "cc", 14, hi=4); real("uni_det", "cc", 15)
real("porta", "cc", 5); real("porta_mode", "cc", 12, hi=1); real("preglide", "cc", 9, lo=52, hi=76, dflt=64)
real("octave", "cc", 13, lo=0, hi=127, dflt=0)          # wrapped: 124..127 = -4..-1, 0..4 = 0..+4
# ---- filters (type, freq, res, drive, drive type, track, q norm, env2->freq)
FLT_CC = {1: [68, 74, 71, 63, 65, 69, 78, 79], 2: [82, 83, 85, 80, 81, 84, 86, 87]}
FLT_P = [("type", 0, 13, 3), ("freq", 0, 127, 127), ("res", 0, 127, 0), ("drive", 0, 127, 0), ("dtype", 0, 6, 0),
         ("track", 0, 127, 127), ("qn", 0, 127, 64), ("env", 0, 127, 64)]
for n in (1, 2):
    for (p, lo, hi, d), cc in zip(FLT_P, FLT_CC[n]):
        real("flt%d_%s" % (n, p), "cc", cc, lo=lo, hi=hi, dflt=d)
real("f_routing", "cc", 60, hi=5, dflt=1); real("f_balance", "cc", 61, dflt=64)
real("f_freqlink", "sw", 0, 122, lo=42, hi=43); real("f_reslink", "sw", 0, 122, lo=44, hi=45)
# ---- envelopes: env1 on CCs, env2-6 on NRPN 0:x
ENV_FIELDS = ["delay", "att", "dec", "sus", "rel", "vel", "srate", "stime", "rep", "atrk", "dtrk", "ltrk", "aslope", "dslope"]
CENTER_ENV = {"vel", "srate", "atrk", "dtrk", "ltrk"}
env_addr = {1: {"att": 73, "dec": 75, "sus": 70, "rel": 72, "vel": 108, "srate": 109, "stime": 110, "rep": 111,
                "atrk": 112, "dtrk": 113, "ltrk": 114, "aslope": 115, "dslope": 116}}
env2 = ["vel", "att", "dec", "sus", "rel", "srate", "stime", "rep", "atrk", "dtrk", "ltrk", "aslope", "dslope"]
env36 = ["delay", "att", "dec", "sus", "rel", "srate", "stime", "rep", "atrk", "dtrk", "ltrk", "aslope", "dslope"]
for n in range(1, 7):
    for f in ENV_FIELDS:
        d = 64 if f in CENTER_ENV else {"att": 2, "dec": 90, "sus": 127, "rel": 40}.get(f, 0)
        if n == 1:
            if f in env_addr[1]: real("env1_" + f, "cc", env_addr[1][f], dflt=d)
        elif n == 2:
            if f in env2: real("env2_" + f, "nrpn", 0, env2.index(f), dflt=d)
        else:
            if f in env36: real("env%d_%s" % (n, f), "nrpn", 0, 14 * (n - 2) + env36.index(f), dflt=d)
    real("env%d_trig" % n, "sw", 0, 122, lo=2 * (n - 1), hi=2 * (n - 1) + 1)
real("env_tcentre", "cc", 106, dflt=64)
# ---- LFOs: NRPN 0:70.. (+9 per LFO); switches on 0:122 at 12/22/32 + ...
for n in (1, 2, 3):
    b = 70 + 9 * (n - 1)
    real("lfo%d_wave" % n, "nrpn", 0, b, hi=37); real("lfo%d_phase" % n, "nrpn", 0, b + 1, hi=119)
    real("lfo%d_slew" % n, "nrpn", 0, b + 2); real("lfo%d_delay" % n, "nrpn", 0, b + 4)
    real("lfo%d_dsync" % n, "nrpn", 0, b + 5, hi=35); real("lfo%d_rate" % n, "nrpn", 0, b + 6, dflt=68)
    real("lfo%d_rsync" % n, "nrpn", 0, b + 7, hi=35)
    s = 12 + 10 * (n - 1)
    real("lfo%d_oneshot" % n, "sw", 0, 122, lo=s, hi=s + 1); real("lfo%d_keysync" % n, "sw", 0, 122, lo=s + 2, hi=s + 3)
    real("lfo%d_common" % n, "sw", 0, 122, lo=s + 4, hi=s + 5)
# ---- FX
for i in range(5):
    real("fx%d_type" % (i + 1), "nrpn", 0, 99 + i, hi=14); real("fx%d_lvl" % (i + 1), "cc", 91 + i)
real("fx_routing", "nrpn", 0, 97, hi=7); real("fx_fb", "nrpn", 0, 98)
real("wet", "cc", 8, dflt=127)
# ---- arp
real("arp_on", "sw", 0, 122, lo=46, hi=47); real("arp_latch", "sw", 0, 122, lo=50, hi=51)
real("arp_mode", "nrpn", 1, 65, hi=6); real("arp_rate", "nrpn", 1, 63, hi=18, dflt=3); real("arp_oct", "nrpn", 1, 62, hi=3)
real("arp_gate", "nrpn", 1, 64, lo=1, hi=127, dflt=64); real("arp_swing", "nrpn", 1, 68, lo=1, hi=99, dflt=50)
real("arp_pat", "nrpn", 1, 66, hi=32)

RKEY = {r[0]: i for i, r in enumerate(REAL)}

# ---- VST parameters (order = index; append only once released) ----
P = []   # dict per VST param; "map" = list of real keys per selector value (or one), "off" = shown = raw - off
def ctl(key, name, real_keys, lo, hi, off=0, opts=None, sel=None, dflt=None, disp=None):
    p = {"key": key, "name": name, "real": real_keys if isinstance(real_keys, list) else [real_keys],
         "off": off, "sel": sel}
    if opts: p["options"] = opts
    else: p["min"], p["max"] = lo, hi
    p["display_kind"] = disp
    P.append(p)
def ui(key, name, **kw):   # editor-only (selector, button, readout)
    p = {"key": key, "name": name, "real": [], "off": 0, "sel": None}; p.update(kw); P.append(p)

# One page. Top bar: the current patch (with prev/next), EDIT/PATCHES, arp. EDIT shows the sound's essentials;
# PATCHES shows the librarian over the same space. Everything else stays on the synth itself.
ui("lcd_info", "Display", type="readout", display="string")
ui("lcd_name", "Patch Name", type="readout", display="string")
ui("linked", "Connected", options=["Off", "On"], default="Off")
ui("receive", "Receive", type="trigger", momentary=True)
ui("patch_prev", "Previous patch", type="trigger", momentary=True)
ui("patch_next", "Next patch", type="trigger", momentary=True)
ui("view", "View", options=["Edit", "Patches"], default="Edit")
ui("chan", "MIDI Channel", min=1, max=16, default=1, display="int")
# OSC (oscillator 1-3)
ui("osc_sel", "Oscillator", options=["1", "2", "3"], default="1")
O = lambda f: ["osc%d_%s" % (n, f) for n in (1, 2, 3)]
ctl("o_wave", "Wave", O("wave"), 0, 71, opts=WAVES, sel="osc_sel")
ctl("o_semi", "Semitones", O("semi"), -64, 63, off=64, sel="osc_sel")
ctl("o_cents", "Cents", O("cents"), -64, 63, off=64, sel="osc_sel")
ctl("o_pw", "Pulse Width", O("pw"), -64, 63, off=64, sel="osc_sel")
ctl("o_dens", "Density", O("dens"), 0, 127, sel="osc_sel")
ctl("o_level", "Level", ["lvl1", "lvl2", "lvl3"], 0, 127, sel="osc_sel")
# FILTER (filter 1, the one most patches use)
ctl("f_type", "Filter Type", "flt1_type", 0, 13, opts=FILTERS)
ctl("f_freq", "Cutoff", "flt1_freq", 0, 127)
ctl("f_res", "Resonance", "flt1_res", 0, 127)
ctl("f_env", "Env Amount", "flt1_env", -64, 63, off=64)
ctl("f_drive", "Drive", "flt1_drive", 0, 127)
# ENVELOPE: the amp envelope (1) or a mod envelope (2-6)
ui("env_sel", "Envelope", options=["Amp", "2", "3", "4", "5", "6"], default="Amp")
for f, nm in [("att", "Attack"), ("dec", "Decay"), ("sus", "Sustain"), ("rel", "Release")]:
    ctl("e_" + f, nm, ["env%d_%s" % (n, f) for n in range(1, 7)], 0, 127, sel="env_sel")
# LFO (1-3)
ui("lfo_sel", "LFO", options=["1", "2", "3"], default="1")
L = lambda f: ["lfo%d_%s" % (n, f) for n in (1, 2, 3)]
ctl("l_wave", "LFO Wave", L("wave"), 0, 37, opts=LFO_WAVES, sel="lfo_sel")
ctl("l_rate", "Rate", L("rate"), 0, 127, sel="lfo_sel")
ctl("l_delay", "Delay", L("delay"), 0, 127, sel="lfo_sel")
ctl("l_slew", "Slew", L("slew"), 0, 127, sel="lfo_sel")
# OUTPUT and ARP
ui("audio_in", "Audio In", options=["Off", "In 1+2", "In 1", "In 2"], default="In 1+2")
ctl("fx_mix", "FX Mix", "wet", 0, 127)
ctl("glide", "Glide", "porta", 0, 127)
ctl("arp_on", "Arp", "arp_on", 0, 1, opts=["Arp Off", "Arp On"])
# PATCHES view: banks A-C (the synth's memory, as last dumped) and Imported (.syx files), 16 rows per page
ui("lib_bank", "Bank", options=["A", "B", "C", "Imported"], default="A")
ui("lib_page", "Page", min=1, max=8, default=1, display="int", dynamic_display=True)
ui("lib_page_prev", "Previous page", type="trigger", momentary=True)
ui("lib_page_next", "Next page", type="trigger", momentary=True)
for i in range(1, 17):
    ui("row_%d" % i, "Patch", display="string")
ui("tgt_bank", "Write Bank", options=["A", "B", "C"], default="C")
ui("tgt_slot", "Write Slot", min=1, max=128, default=128, display="int")
ui("write", "Write", type="trigger", momentary=True)
ui("export", "Export", type="trigger", momentary=True)

# ---- write params.json ----
out = []
for p in P:
    q = {"key": p["key"], "name": p["name"]}
    for k in ("type", "display", "momentary", "options", "min", "max", "dynamic_display"):
        if k in p: q[k] = p[k]
    if p.get("sel") or p.get("display_kind"): q["dynamic_display"] = True
    if p["real"]:
        r = REAL[next(RKEY[k] for k in p["real"] if k in RKEY)]
        if "options" in q:
            q["default"] = q["options"][r[6] - (p["off"] or 0)] if 0 <= r[6] - p["off"] < len(q["options"]) else q["options"][0]
        else:
            dv = r[6] - p["off"]
            if p.get("display_kind") == "octave": dv = 0
            q["default"] = max(q["min"], min(q["max"], dv))
        if "options" not in q: q["display"] = "int"
    elif "default" in p:
        q["default"] = p["default"]
    out.append(q)
json.dump({"name": "Nova Editor", "params": out}, open(os.path.join(ROOT, "vst", "params.json"), "w"), indent=1)

# ---- write nova_map.h ----
K = {"cc": 0, "nrpn": 1, "sw": 2}
DK = {None: 0, "phase": 1, "db": 2, "octave": 3}
h = ["/* generated by tools/spec.py: do not edit */", "#pragma once",
     "enum { NREAL = %d, NCTL = %d };" % (len(REAL), len(P)),
     "typedef struct { const char *key; unsigned char kind, a, b, lo, hi, dflt; } nreal_t;   /* kind 0 cc, 1 nrpn, 2 switch */",
     "static const nreal_t NREAL_T[NREAL] = {"]
for r in REAL:
    h.append('    {"%s", %d, %d, %d, %d, %d, %d},' % (r[0], K[r[1]], r[2], r[3], r[4], r[5], r[6]))
h += ["};",
      "/* VST control -> real parameter(s): sel = index of the selector control (-1 none), real[] per selector value */",
      "typedef struct { const char *key; signed char sel; signed char nreal; short real[6]; short off; unsigned char disp; } nctl_t;",
      "static const nctl_t NCTL_T[NCTL] = {"]
idx = {p["key"]: i for i, p in enumerate(P)}
for p in P:
    rr = [RKEY.get(k, -1) for k in p["real"]] + [-1] * (6 - len(p["real"]))
    h.append('    {"%s", %d, %d, {%s}, %d, %d},' % (p["key"], idx[p["sel"]] if p["sel"] else -1, len(p["real"]),
                                                  ", ".join(map(str, rr)), p["off"], DK[p.get("display_kind")]))
h.append("};")
open(os.path.join(ROOT, "src", "nova_map.h"), "w").write("\n".join(h) + "\n")
print("%d MiniNova parameters, %d controls" % (len(REAL), len(P)))
