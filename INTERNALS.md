# viewer — internals

The user-facing surface (verbs, config, hooks, exports) is in
[`README.md`](README.md). This file is the *how*.

## Data path

```
                       ┌─ LCD: lcdview / Info tile / link
   request a URL  ─────┤
                       └─ Web: webview / Dock app / iframe link

   local path / file://        http(s)://
        │                          │
        ▼                          ▼
   device web server          remote host
   (*.md → HTML, *.html as-is)     │
        │                          │
   ┌────┴───────────────┐          │
   ▼                    ▼          ▼
  LCD: fetch 127.0.0.1   Web: <iframe src>  (browser renders HTML directly)
   → HTML → subset parser → IR → LVGL
```

The one invariant: **Markdown→HTML happens in the web server, once**, so neither
front end carries a Markdown parser at the presentation layer. The LCD has an
HTML→IR→LVGL renderer (it can't host a browser engine); the web side is a plain
`<iframe>`.

## `conditional/spangap-web/src/md_transform.cpp` — server-side Markdown

Registers a file-extension transform with spangap-web at init
(`viewerWebRegister` → `webRegisterFileExt("md,markdown,mkd", mdTransform)`),
gated on `spangap-web` (NOT lcd) so headless builds still render Markdown over the
web. The web server calls `mdTransform` on its file worker with the file's full
bytes (already gunzipped if the on-disk file was `.gz`); we:

1. `md_html()` (vendored MD4C) → an HTML fragment.
2. `addHeadingAnchors()` — MD4C emits bare `<hN>`; we rewrite each to
   `<hN id="slug">`, slug = heading text (inline tags stripped) keeping only
   `[a-zA-Z0-9_]` with spaces → `_`. So `#anchor` links resolve (the web iframe
   scrolls to them natively; the LCD ignores the id).
3. Wrap in a minimal document: `<meta charset>`, a `<title>` from the first ATX
   heading (else the file's base name), and an inline `<style>` carrying the
   web viewer's typographic CSS. The LCD's parser ignores `<head>`/`<style>` and
   renders the body.

The output is a PSRAM buffer the web server frees after sending.

`webRegisterFileExt`, whole-file serving, gzip inflate, `Accept-Encoding`
negotiation, and the **loopback exemption** (so the LCD fetches
`http://127.0.0.1/…` in plain HTTP, no self-signed cert, no session cookie) all
belong to spangap-web — see
[`/straddles/spangap-web/docs/web.md`](../spangap-web/docs/web.md). This straddle
only registers the transform.

## `conditional/spangap-lcd/src/viewer_lcd.cpp` — the LCD viewer

### IR

A small render-oriented tree, the spine both inputs lower into:

- `Run` = `{ text, flags(F_BOLD|F_ITALIC|F_CODE|F_LINK), href }`.
- `Block` = `{ type(BT_PARA|HEADING|ITEM|RULE|PRE), level, indent, ordered,
  index, quote, pre, runs[] }`.

Growing the supported markup is "one block/flag + one renderer case".

### HTML subset parser (`HtmlParser`)

A strict **allow-list**, which doubles as the sanitizer:

- Rendered tags (`h1-6, p, br, hr, ul/ol/li, b/strong, i/em, code, pre,
  blockquote, a, title`) are handled in `handleTag`.
- `htmlTransparent` containers (`div/span/section/table/td/…`) are ignored but
  flow their content (so real pages render).
- `htmlVoid` tags are ignored.
- **Everything else** (`script/style/svg/forms/unknown`) has its whole content
  dropped via `skipToClose` — we only ever render markup we understand.

Entities are decoded (`decodeEntity`, incl. numeric). The parser also captures
`<title>` and falls back to the first heading, else the URL.

### IR → LVGL

- Link-free prose → one `lv_spangroup` per block (best wrapping/kerning).
- Link-bearing blocks → a flex-wrap row of per-word `lv_label`s, so link words
  are individually tappable (`linkClicked` → `requestNav(resolveUrl(base,href))`).
- Emphasis is by **colour on proportional faces** (no bold/italic font blobs):
  body grey, bold black, italic light-grey, `code` mono, headings 16px.
- The page title is a full-width grey, centered, word-wrapped widget — a child of
  the **scroll container**, so it scrolls away with the content (greedy
  `LV_LABEL_LONG_WRAP`; LVGL has no balanced wrap).
- **Caps** (`VIEWER_MAX_BLOCKS` = 2000, `VIEWER_MAX_OBJS` = 800): an unbounded
  page builds tens of thousands of LVGL objects in PSRAM, exhausts it, and then
  ITS allocation fails system-wide — a huge doc takes the whole node down, not
  just the viewer. So input/IR/widget count are bounded; oversize content is
  truncated with a notice. The deliberate non-goal is viewport virtualization:
  LVGL already clips off-screen draws and PSRAM-allocates widgets, so a capped
  whole-tree render is the chosen trade.

### Threading — never block the lcd task

The lcd task owns the one LVGL context. The blocking HTTP load runs on a separate
**nav worker** task (`viewerWorker`, PSRAM stack — it runs mbedtls for external
https). Flow: `requestNav` sets the pending request + notifies the worker → the
worker `loadUrl`s and stores `s_html`/`s_url` under `s_mux` → `lcdRun(navRenderCb)`
hops the render back onto the lcd task. A 50-deep history stack backs the Back
button.

### `loadUrl` — local goes through localhost

A bare path / `file://` becomes `http://127.0.0.1<path>` and is fetched via
`viewerFetch` (so the server converts `*.md`); `http(s)://` is fetched directly.
For local docs the original bare URL is kept as `finalUrl` so relative links
resolve back to local paths (and re-fetch via loopback); for external, the
post-redirect URL is kept. Everything reaching the renderer is HTML (the subset
parser treats plain text as a one-paragraph body). All of this is
`#if CONFIG_SPANGAP_NET`-gated.

### The launcher app — `ViewerApp : public LcdApp`

The LCD front end is an `LcdApp` subclass, installed on the lcd task with
`lcdInstall(new ViewerApp())`:

```cpp
class ViewerApp : public LcdApp {
public:
    ViewerApp() : LcdApp({ .name = "Info", .iconBasename = "viewer" }) {}
    void onCreate(lv_obj_t* root) override { viewerApp(root); }   // build page + address bar once
    void onShow()                override { viewerOnShow(nullptr); }
    void onClose()               override { s_page = s_bar = s_urlbar = nullptr; s_linkHrefs.clear(); }
};
```

- `onCreate` builds the page scroll container + the hidden `[Back][URL]` bar once.
- `onShow` runs the manual-launch home navigation (below).
- `onClose` nulls the widget handles so the next open rebuilds — `viewerApp`'s
  `if (s_page) return` reopen guard depends on `s_page` being null after the
  layer is freed.

`onShow` (the LcdApp lifecycle's foreground callback) is part of the subclass, so
it is set atomically with the app — there is no separate registration to race.

### Manual-vs-programmatic open, and boot

- `viewerOnShow` fires on every show. A **manual** open (tile tap) navigates to
  `s.viewer.home_lcd` (fallback `/WELCOME.md`); a **programmatic** open (lcdview /
  link / boot) sets `s_navInitiatedShow` first (in `navRenderCb`, before
  `lcdShowProgram`), so `viewerOnShow` leaves its target alone. `navRenderCb`
  shows first, renders second.
- **Boot**: only a one-shot `s.viewer.once_lcd` auto-opens (then `storageSet("")`
  consumes it); the load **retries** (~6× / 5 s) because at boot the web server
  may not be listening yet. `home_lcd` is only where a manual launch goes — the
  device otherwise lands on the launcher.

### Settings pane

`lcdRegisterSettings("Viewer", "Viewer", viewerSettings)` adds a generated pane:
`lcdSettingSection("Viewer")`, a `lcdSettingCaption` hint ("Press Space in the
viewer to show the address bar."), and a read-only `lcdSettingValue("Location",
"viewer.lcd.url")` mirroring the current LCD location.

### Address bar on Space

`s_page` joins the keypad group and is default-focused, so the keyboard reaches it
while reading: **Space** → reveal + focus the (hidden) `[Back][URL]` row;
Up/Down → scroll. The bar hides on the URL field losing focus, on Enter (after
navigating), or on a tap in the page.

## `conditional/spangap-net/src/fetch.cpp` — http(s) GET

A thin `esp_http_client` GET with TLS via the IDF cert bundle (like acme/duckdns;
no shared HTTP-client wrapper). Follows redirects, accumulates only the final 2xx
body/headers, caps the body (`FETCH_CAP` = 128 KB). Blocking — called from the nav
worker, never the lcd task. Loopback fetches are plain HTTP (no TLS).

## Browser — `browser/src/{panels/ViewerWindow.vue, modules/viewer.ts}`

`<ViewerWindow>` is a `FloatingWindow` wrapping a single `<iframe :src>`. Because
the frame runs at the device origin, same-origin device pages get cookies (admin
realms), images, links, and `#anchor` scrolling for free.

- `toSrc` rewrites `127.0.0.1`/`localhost` URLs to a **same-origin path** — in the
  browser those hosts mean the *user's* machine, not the device, so `webview
  http://127.0.0.1/x` must load `/x` from the SPA origin.
- On `load` (same-origin only, in `try`): adopt the page `<title>` for the window
  title; reflect `location` in the address bar; wire the iframe's document so that
  while it holds focus — **Space** reveals the address bar, a **click** hides it
  and **foregrounds** the window (a click inside an iframe never reaches the
  window's own `@mousedown="bringToFront"`, so we bump the focus nonce), and the
  content is focused so Space/scroll work without a click first.
- **Start-up** (`registerViewer`): the viewer **owns its open state** each load,
  overriding FloatingWindow's localStorage visibility restore. It triggers on
  `device.synced` (the first complete `{__dump:'e'}` storage dump; raw `connected`
  fires on DataChannel open, before any `s.*` values exist). Only a one-shot
  `s.viewer.once_web` opens it (then `device.set('s.viewer.once_web','')` consumes
  it); otherwise it forces closed.
- **Dock app**: `registerApp({ id: 'viewer', label: 'Viewer', icon: 'viewer',
  placement: 7, open: showViewer, isOpen: () => viewerWebVisible.value })`.
- **Z-order**: other windows call `bringToFront()` from their own localStorage
  restore on mount; `raise()` re-bumps the focus nonce 150 ms later so the viewer
  ends up on top regardless of that ordering (and even if the storage sync beat
  component mount). A manual launch (`showViewer`) goes to `s.viewer.home_web`
  (fallback `/WELCOME.md`) only when it wasn't already open.

## `/fixed` shipping — `project_include.cmake` + `SPANGAP_EXTRA_DATA_DIRS`

The factory image (`spangap_create_factory_image`, spangap-core) merges
spangap-core's `data/` + the buildable's `data/`. Non-buildable straddles opt in:
`project_include.cmake` does
`set_property(GLOBAL APPEND PROPERTY SPANGAP_EXTRA_DATA_DIRS "${CMAKE_CURRENT_LIST_DIR}/data")`,
and the factory builder merges each such dir **after** the core defaults and
**before** the consumer (so the buildable still wins on collision). This straddle
ships `data/webroot/{WELCOME,ABOUT}.md` → `/fixed/webroot/` → served at
`/WELCOME.md`, `/ABOUT.md` (public, via the `/` mapping). `data/` is staged as a
symlink under the component, so the path resolves through it.

## Conventions / gotchas

- The launcher app **name is the identifier** (`lcdShowProgram("Info")` matches
  it), so the `"Info"` string must agree across the `LcdApp` name, the show
  navigation, and `lcdShowProgram`; the icon basename is `viewer` (the `.bin`).
- MD4C is vendored, not a registry component (none exists), compiled with `-w` so
  the repo's `-Werror` doesn't trip on upstream code.
- The home-bar drag pill (spangap-lcd) is opaque mid-grey, not white-at-40% —
  white vanishes on the viewer's white page.
