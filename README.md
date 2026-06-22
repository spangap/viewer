# viewer

## What is this?

A micro HTML/Markdown **document viewer** for spangap devices — the platform's
README / help / CHANGELOG reader. Named **viewer** (not "browser") to avoid
confusion with the user's web browser. Two front ends, one behaviour:

- **LCD** — an "Info" launcher program (red **?** icon) that renders a safe
  subset of HTML into LVGL.
- **Web UI** — a "Window → Viewer" floating window that loads a URL into an
  `<iframe>`, served by the device's own web server.

Markdown is converted to HTML **on the device's web server**, so both front ends
just consume HTML. A bare path / `file://` is a local document (the LCD fetches
it over loopback; the web iframe loads it same-origin); `http(s)://` is fetched
directly. There is no http(s) proxy.

Owns the `viewer` / `s.viewer` config trees and the `lcdview` / `webview` verbs.

## What this straddle owns

- `esp-idf/src/viewer.cpp` — the always-compiled `webview` verb + `s.viewer.*`
  defaults (`viewerInit`).
- `esp-idf/conditional/spangap-lcd/` — the whole LCD viewer (`viewerLcdRegister`).
- `esp-idf/conditional/spangap-web/` — the Markdown→HTML transform registered with
  the web server (`viewerWebRegister`).
- `esp-idf/conditional/spangap-net/` — the http(s) GET client (`viewerFetch`).
- `esp-idf/md4c/` — vendored [MD4C](https://github.com/mity/md4c) (Markdown→HTML).
- `browser/` — the `<ViewerWindow>` Vue panel + `registerViewer` activator.
- `data/webroot/{WELCOME,ABOUT}.md` — the built-in help pages, shipped into
  `/fixed/webroot/`.

## How others use it

### CLI verbs

```
lcdview <url>     # open a document on the LCD  (a path, file://, or http(s)://)
webview <path>    # open a document in the web Viewer window
```

`webview` is always compiled (LCD-independent); it just sets the ephemeral key
`viewer.web.url`, which the browser module watches.

### Opening it by hand

- **LCD**: tap the **Info** tile → goes to `s.viewer.home_lcd` (else the welcome
  page). Press **Space** in the page to reveal the address bar (Back + URL).
- **Web**: **Window → Viewer** → goes to `s.viewer.home_web` (else the welcome
  page). Press **Space** to reveal the address bar.

### Start-up & config (`s.viewer.*`, URLs, all unset by default)

| key | meaning |
|-----|---------|
| `once_{lcd,web}` | **one-shot**: auto-opens the viewer once on the next boot / page load, then is consumed. The ONLY thing that auto-opens — how a build surfaces a `CHANGELOG.md` after an update. Otherwise the device lands on the launcher (LCD) / no window (web). |
| `home_{lcd,web}` | where a **manual** launch goes (Info tile / Window menu). Falls back to `/WELCOME.md`. |
| `viewer.lcd.url` | ephemeral — the current LCD location. |
| `viewer.web.url` | ephemeral — set by `webview`; the browser opens the window on it. |

### Shipping a help document

Put a `*.md` (or `*.md.gz`) anywhere under `/fixed` or `/sdcard` and open it by
URL. To bundle one into the firmware image, drop it in a straddle's
`esp-idf/data/webroot/` and add `data/` to `SPANGAP_EXTRA_DATA_DIRS` from a
`project_include.cmake` (as this straddle does). To push it to existing users
after an update, set `s.viewer.once_{lcd,web}` to its URL.

### Firmware integration hooks

`esp-idf/include/viewer.h` declares the three `init:` hooks the generated
dispatcher calls — you never call them yourself; staging the straddle wires them:

- `viewerInit()` — always.
- `viewerLcdRegister()` — `when: spangap/spangap-lcd`.
- `viewerWebRegister()` — `when: spangap/spangap-web`.

### Browser integration

`browser/src/modules/viewer.ts` exports the `registerViewer` activator (a
`browser_register:` hook — adds the Window-menu entry and the start-up watcher)
and the refs the buildable's `MainLayout` binds to `<ViewerWindow>`:
`viewerWebVisible`, `viewerWebFocus` (raise nonce), `viewerWebUrl`, plus
`showViewer()`.

## Dependencies

- **spangap-core** (implicit) — storage, CLI, fs.
- **spangap-net** (soft) — the http(s) client lives in `conditional/spangap-net/`,
  `#if CONFIG_SPANGAP_NET`-gated. Without it the LCD can't fetch anything.
- **spangap-web** (soft) — the Markdown transform + document serving live in
  `conditional/spangap-web/`. The LCD's local-document path fetches from this
  server over loopback.
- **spangap-lcd** (soft) — the LCD front end lives in `conditional/spangap-lcd/`.

## What it does NOT do

- No http(s) **proxy** — a device reverse-proxy needs AP+STA (APSTA), shelved.
  The web iframe loads cross-origin URLs directly (subject to the remote's
  framing policy).
- LCD rendering omits images, styled tables (text flows unstyled), and viewport
  virtualization (large documents are capped, not virtualized).

## Read next

[`INTERNALS.md`](INTERNALS.md) — the render pipeline, server-side conversion,
loopback fetch, threading, start-up logic, and the `/fixed` merge. Design history
in [`/straddles/plans/viewer-straddle-html-md-renderer.md`](../plans/viewer-straddle-html-md-renderer.md).
