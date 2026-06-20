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

/** Normal init hook (init: viewerInit). No-op: all work is the LCD viewer,
 *  registered by viewerLcdRegister. Kept so the dispatcher always has a hook. */
void viewerInit(void);

/** LCD slice registration — the when:-gated init hook (spangap/spangap-lcd).
 *  Seeds config defaults, spawns the nav worker, registers the launcher program
 *  + CLI verb + Settings pane. Defined in conditional/spangap-lcd/src/
 *  viewer_lcd.cpp; only linked when the lcd straddle is staged. */
void viewerLcdRegister(void);

#endif /* SPANGAP_VIEWER_H */
