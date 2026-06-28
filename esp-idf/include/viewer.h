/**
 * viewer — a micro HTML/Markdown document viewer.
 *
 * Named "viewer" (not "browser") to avoid confusion with the user's web browser.
 * Renders a growing subset of HTML (and Markdown, converted to HTML on-device
 * via vendored MD4C) into LVGL on the LCD. The viewer lives in conditional
 * slices; this header just declares the init hooks the generated dispatcher
 * calls.
 */
#ifndef SPANGAP_VIEWER_H
#define SPANGAP_VIEWER_H

/** Normal init hook (init: viewerInit), always compiled. Registers the `webview`
 *  CLI verb (sets the ephemeral viewer.web.url that the browser-side window
 *  watches); LCD-independent, so it works on any build with the web UI. */
void viewerInit(void);

/** LCD slice registration — the when:-gated init hook (spangap/spangap-lcd).
 *  Spawns the nav worker and registers the launcher app + CLI verb + Settings
 *  pane. Defined in conditional/spangap-lcd/src/viewer_lcd.cpp; only linked when
 *  the lcd straddle is staged. */
void viewerLcdRegister(void);

/** Web slice registration — the when:-gated init hook (spangap/spangap-web).
 *  Registers the Markdown→HTML file-extension transform with the web server, so
 *  *.md / *.md.gz are converted on-device and served as text/html. Defined in
 *  conditional/spangap-web/src/md_transform.cpp; only linked when the web
 *  straddle is staged. Independent of the LCD. */
void viewerWebRegister(void);

#endif /* SPANGAP_VIEWER_H */
