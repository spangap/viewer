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

#include "service.h"

/** Normal init service (services: ViewerService), always compiled. Registers the
 *  `webview` CLI verb (sets the ephemeral viewer.web.url that the browser-side
 *  window watches); LCD-independent, so it works on any build with the web UI. */
class ViewerService : public Service { public: void onInit() override; };

#endif /* SPANGAP_VIEWER_H */
