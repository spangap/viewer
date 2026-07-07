/**
 * viewer_web.h — the viewer's Markdown→HTML server slice as a boot-registered
 * Service.
 *
 * ViewerWebService registers a .md file-extension transform with the web server,
 * so *.md / *.md.gz are converted on-device and served as text/html. The
 * straddle's `services:` entry (gated on spangap-web) points the generated boot
 * code at this header, which constructs a ViewerWebService and registers it;
 * onInit does the transform registration.
 *
 * Declared here (global, no namespace — the codebase disambiguates by the
 * viewer* symbol prefix) so the trampoline TU can `new ViewerWebService()`; the
 * method is defined in md_transform.cpp. Compiled only under
 * conditional/spangap-web/, so it exists only when the web straddle is staged —
 * matching the entry's `when: spangap/spangap-web` gate. Independent of the LCD.
 */
#pragma once

#include "service.h"

/** The Markdown→HTML transform registration. onInit registers the .md/.markdown/
 *  .mkd file-extension transform with the web server. */
class ViewerWebService : public Service { public: void onInit() override; };
