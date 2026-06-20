# viewer

A micro HTML/Markdown document viewer for spangap devices, with two front ends:

- **LCD** — renders a growing subset of HTML, and Markdown (converted to HTML
  on-device via vendored [MD4C](https://github.com/mity/md4c)), into LVGL.
- **Web UI** — a "Window → Viewer" floating window that renders **local**
  Markdown/HTML in the browser.

Named **viewer** (not "browser") to avoid confusion with the user's web browser.
Owns the `viewer` / `s.viewer` config trees.

## LCD viewer

```
bytes ──(.md? MD4C)──► HTML ──subset parser──► IR (blocks + runs) ──► LVGL
```

The **IR** (a small render-oriented tree of blocks + styled inline runs) is the
spine: both Markdown and HTML lower into it, so adding markup is "one block/flag
+ one renderer case". The parser is a strict **allow-list** — rendered tags are
handled, structural containers (`div/span/section/table/…`) flow their content
through, void tags are ignored, and **everything else (script/style/svg/forms/
unknown) has its content dropped** — so we only ever render markup we understand,
and it doubles as the sanitizer.

Each page shows a full-width grey **title bar** (`<title>`, else first heading,
else the URL), centered, that scrolls with the content. Emphasis is by **colour
on proportional faces** (no bold/italic font blobs): body = dark grey,
**bold** = black, *italic* = light grey; `code` uses the platform mono font;
headings use the larger `montserrat_16_latin`. Background is white. Fonts are
owned/published by `spangap-lcd`.

Links are tappable (history + a floating **<** Back button + relative-URL
resolution). I/O runs off the lcd task: a **nav worker** does file/http loads
(which block) and hands the render back to the lcd task, so the UI never blocks
on disk or the network. Hard caps bound input/IR/widget count so a huge page
can't exhaust PSRAM (oversize content is truncated with a notice).

Lives in conditional slices:
`esp-idf/conditional/spangap-lcd/src/viewer_lcd.cpp` (the whole LCD viewer, via
the `when:`-gated `viewerLcdRegister` hook) and
`esp-idf/conditional/spangap-net/src/fetch.cpp` (http(s) GET, only when
`spangap-net` is staged; `#if CONFIG_SPANGAP_NET`-gated).

### CLI: `lcdview`

```
lcdview /sdcard/readme.md         # Markdown on the LCD
lcdview file:///sdcard/help.html  # HTML
lcdview https://example.com/      # needs spangap-net (device fetches over STA)
```

Open the **Viewer** launcher icon (globe) for the built-in welcome page. Enable
**Settings → Viewer → Address bar** (`s.viewer.urlbar`) for an in-app URL field.

## Web viewer

`browser/` adds a **Window → Viewer** floating window (Vue). It renders **local**
files fetched from the device's web server (the `/sdcard` and `/state` WebDAV
roots): Markdown → the same subset, with the browser's fonts; HTML → passed
through in a sandboxed iframe. Markdown links to `http(s)` get `target="_blank"`
and open in the user's own browser tab.

It does **not** proxy http(s): a device-side reverse proxy would need AP+STA
(APSTA), which isn't implemented — so remote pages are handed to the real
browser instead.

### CLI: `webview`

```
webview /sdcard/readme.md   # pops the web Viewer window open on that local file
```

`webview` (always compiled, LCD-independent) just sets the ephemeral key
`viewer.web.url`; the browser module subscribes and opens the window.

## Config

| key | meaning |
|-----|---------|
| `s.viewer.urlbar` | show the LCD viewer's in-app address bar |
| `viewer.lcd.url`  | ephemeral — current LCD location |
| `viewer.web.url`  | ephemeral — set by `webview`; opens the web window |

## Not handled (yet)

Images, tables (table text flows, unstyled), and viewport virtualization for very
large documents (capped instead).

See [`/straddles/plans/viewer-straddle-html-md-renderer.md`](../plans/viewer-straddle-html-md-renderer.md) for the design history.
