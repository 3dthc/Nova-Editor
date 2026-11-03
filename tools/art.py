"""Nova Editor's panel artwork, drawn as SVG from the layout's own geometry (tools/layout.py calls write()).
A graphite faceplate with fine grain and a soft top light; each section is a recessed well (inner shadow, a lit lower
bevel, a dark upper lip); the patch display is a tinted glass window. Everything sits behind MPC's live controls.
Layout coordinates (y from 86) are shifted into the plugin area (y from 0)."""
Y0 = 86
W, H = 1280, 628

DEFS = """<defs>
 <linearGradient id="plate" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#363b42"/><stop offset="0.5" stop-color="#2b3036"/><stop offset="1" stop-color="#23272c"/>
 </linearGradient>
 <radialGradient id="toplight" cx="0.5" cy="-0.15" r="0.9">
  <stop offset="0" stop-color="#ffffff" stop-opacity="0.07"/><stop offset="1" stop-color="#ffffff" stop-opacity="0"/>
 </radialGradient>
 <linearGradient id="well" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#1a1e23"/><stop offset="0.12" stop-color="#1f2329"/><stop offset="1" stop-color="#22272d"/>
 </linearGradient>
 <filter id="inset" x="-5%" y="-5%" width="110%" height="110%">
  <feGaussianBlur in="SourceAlpha" stdDeviation="5" result="b"/>
  <feOffset in="b" dy="3" result="o"/>
  <feComposite in="SourceAlpha" in2="o" operator="out" result="ring"/>
  <feFlood flood-color="#000" flood-opacity="0.75"/>
  <feComposite in2="ring" operator="in" result="shadow"/>
  <feComposite in="shadow" in2="SourceAlpha" operator="in"/>
 </filter>
 <linearGradient id="glass" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#0c1820"/><stop offset="1" stop-color="#0f1f28"/>
 </linearGradient>
 <linearGradient id="glare" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#ffffff" stop-opacity="0.10"/><stop offset="0.45" stop-color="#ffffff" stop-opacity="0.02"/>
  <stop offset="0.5" stop-color="#ffffff" stop-opacity="0"/>
 </linearGradient>
 <radialGradient id="glow" cx="0.5" cy="0.5" r="0.6">
  <stop offset="0" stop-color="#2fb7ff" stop-opacity="0.10"/><stop offset="1" stop-color="#2fb7ff" stop-opacity="0"/>
 </radialGradient>
</defs>"""


def well(x, y, w, h, r=14):
    y -= Y0
    return (
        # lit lower bevel (the plate's edge catching light), then the dark upper lip
        '<rect x="%g" y="%g" width="%g" height="%g" rx="%g" fill="#ffffff" fill-opacity="0.07"/>' % (x - 1, y - 1, w + 2, h + 3, r + 1) +
        '<rect x="%g" y="%g" width="%g" height="%g" rx="%g" fill="#0b0d10" fill-opacity="0.85"/>' % (x - 1, y - 1.5, w + 2, h + 1.5, r + 1) +
        '<rect x="%g" y="%g" width="%g" height="%g" rx="%g" fill="url(#well)"/>' % (x, y, w, h, r) +
        '<rect x="%g" y="%g" width="%g" height="%g" rx="%g" fill="#000" filter="url(#inset)"/>' % (x, y, w, h, r) +
        # a hairline under the section title
        '<rect x="%g" y="%g" width="%g" height="1" fill="#ffffff" fill-opacity="0.05"/>' % (x + 18, y + 44, w - 36))


def display(x, y, w, h, r=10):
    y -= Y0
    return (
        '<rect x="%g" y="%g" width="%g" height="%g" rx="%g" fill="#ffffff" fill-opacity="0.08"/>' % (x - 2, y - 2, w + 4, h + 5, r + 2) +
        '<rect x="%g" y="%g" width="%g" height="%g" rx="%g" fill="#07090b"/>' % (x - 2, y - 2.5, w + 4, h + 3, r + 2) +
        '<rect x="%g" y="%g" width="%g" height="%g" rx="%g" fill="url(#glass)"/>' % (x, y, w, h, r) +
        '<rect x="%g" y="%g" width="%g" height="%g" rx="%g" fill="url(#glow)"/>' % (x, y, w, h, r) +
        '<rect x="%g" y="%g" width="%g" height="%g" rx="%g" fill="#000" filter="url(#inset)"/>' % (x, y, w, h, r) +
        '<rect x="%g" y="%g" width="%g" height="%g" rx="%g" fill="url(#glare)"/>' % (x + 2, y + 2, w - 4, h * 0.55, r - 2))


M = 4   # margin around a well's own drawing (its bevel reaches 2 px out)


def _svg(w, h, body):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">' % (w, h, w, h) +
            DEFS + body + "</svg>")


def write_plate(path, lcd):
    """The faceplate and the display glass: static, behind everything."""
    body = ('<rect width="%d" height="%d" fill="url(#plate)"/>' % (W, H) +
            '<rect width="%d" height="%d" fill="url(#toplight)"/>' % (W, H) + display(*lcd, r=14))
    open(path, "w").write(_svg(W, H, body))


KEY = """<defs>
 <linearGradient id="kb" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="%s"/><stop offset="1" stop-color="%s"/></linearGradient>
 <linearGradient id="kh" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity="0.16"/>
  <stop offset="0.5" stop-color="#fff" stop-opacity="0.02"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></linearGradient>
 <filter id="kg" x="-50%%" y="-50%%" width="200%%" height="200%%"><feGaussianBlur stdDeviation="%g"/></filter>
</defs>"""


def _key(w, h, left, on, chev_h=16):
    """A rounded keycap with a chevron; pressed: lit from inside."""
    top, bot = ("#3f6f8c", "#244a61") if on else ("#3b4149", "#2a2f36")
    cx, cy = w / 2, h / 2
    d = 9 if left else -9
    d = d * chev_h / 16
    chev = "M%g,%g L%g,%g L%g,%g" % (cx + d * 0.6, cy - chev_h, cx - d * 0.6, cy, cx + d * 0.6, cy + chev_h)
    glow = '<path d="%s" fill="none" stroke="#6fd6ff" stroke-width="7" stroke-linecap="round" stroke-linejoin="round" opacity="%g" filter="url(#kg)"/>' % (chev, 0.9 if on else 0.35)
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">' % (w, h, w, h) +
            KEY % (top, bot, 3) +
            '<rect x="1" y="2" width="%d" height="%d" rx="13" fill="#000" opacity="0.55"/>' % (w - 2, h - 2) +
            '<rect x="1" y="1" width="%d" height="%d" rx="13" fill="url(#kb)" stroke="#15181c" stroke-width="1"/>' % (w - 2, h - 3) +
            '<rect x="2" y="2" width="%d" height="%d" rx="12" fill="url(#kh)"/>' % (w - 4, h * 0.5) +
            glow + '<path d="%s" fill="none" stroke="%s" stroke-width="4" stroke-linecap="round" stroke-linejoin="round"/>' % (
                chev, "#e6f8ff" if on else "#8fdcff") + "</svg>")


def _led(on):
    core = ("#5dff9a", "#c9ffe0", 1.0) if on else ("#2b3a33", "#3d4d45", 0.0)
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="40" height="24" viewBox="0 0 40 24">'
            '<defs><radialGradient id="lg" cx="0.4" cy="0.35" r="0.7"><stop offset="0" stop-color="%s"/>'
            '<stop offset="1" stop-color="%s"/></radialGradient><filter id="lb" x="-100%%" y="-100%%" width="300%%" height="300%%">'
            '<feGaussianBlur stdDeviation="3"/></filter></defs>'
            '<circle cx="20" cy="12" r="9" fill="#3dff8a" opacity="%g" filter="url(#lb)"/>'
            '<circle cx="20" cy="12" r="7" fill="#0b0e10"/>'
            '<circle cx="20" cy="12" r="5.2" fill="url(#lg)"/></svg>') % (core[1], core[0], 0.55 * core[2])


def write_keys(d):
    import os
    for name, left in (("prev", 1), ("next", 0)):
        open(os.path.join(d, "key_%s.svg" % name), "w").write(_key(64, 112, left, 0))
        open(os.path.join(d, "key_%s_on.svg" % name), "w").write(_key(64, 112, left, 1))
        open(os.path.join(d, "small_%s.svg" % name), "w").write(_key(56, 48, left, 0, 9))
        open(os.path.join(d, "small_%s_on.svg" % name), "w").write(_key(56, 48, left, 1, 9))
    open(os.path.join(d, "led_off.svg"), "w").write(_led(0))
    open(os.path.join(d, "led_on.svg"), "w").write(_led(1))


def write_well(path, x, y, w, h):
    """One recessed section, drawn in its own small file (placed at x-M, y-M, size w+2M x h+2M). It repaints the plate
    under it too, so it can switch with the view without leaving gaps."""
    fw, fh = w + 2 * M, h + 2 * M
    body = ('<g transform="translate(%g,%g)">' % (-(x - M), -(y - M - Y0)) +
            '<rect x="%g" y="%g" width="%g" height="%g" fill="url(#plate)"/>' % (x - M, y - M - Y0, fw, fh) +
            well(x, y, w, h) + "</g>")
    # the plate gradient is drawn in plugin coordinates: give it the whole page's span
    body = body.replace('fill="url(#plate)"', 'fill="url(#plateabs)"', 1)
    defs_extra = ('<defs><linearGradient id="plateabs" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="0" y2="%d">'
                  '<stop offset="0" stop-color="#363b42"/><stop offset="0.5" stop-color="#2b3036"/>'
                  '<stop offset="1" stop-color="#23272c"/></linearGradient></defs>' % H)
    open(path, "w").write(_svg(fw, fh, defs_extra + body))
