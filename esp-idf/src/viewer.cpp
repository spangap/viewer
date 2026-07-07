/**
 * viewer — normal init hook (always compiled).
 *
 * Registers the `webview` CLI verb, which sets the ephemeral key viewer.web.url;
 * the browser-side viewer module subscribes and pops its "Viewer" Dock window
 * open on that local document. (The LCD viewer + `lcdview` verb live in the
 * when:-gated ViewerApp service, conditional/spangap-lcd/.)
 *
 * webview is intentionally LCD-independent: it just writes a config key, so it
 * works on any build that has the web UI, with or without a display.
 */
#include "viewer.h"
#include "spangap.h"      /* cli.h + storage.h */
#include "storage.h"

#include <string>

static void cliWebview(const char* args) {
    if (args && cliWantsHelp(args)) {
        cliPrintf("%-*s open a LOCAL doc in the web Viewer window (e.g. /sdcard/x.md)\n",
                  CLI_HELP_COL, "webview <path>");
        return;
    }
    if (!args || !*args) {
        cliPrintf("web location: %s\n", storageGetStr("viewer.web.url", "-").c_str());
        return;
    }
    storageSet("viewer.web.url", args);
    cliPrintf("opening %s in the web Viewer\n", args);
}

void ViewerService::onInit() {
    cliRegisterCmd("webview", cliWebview);

    /* Start-up locations (URLs, both front ends), all UNSET by default:
     *   once_{lcd,web} — a one-shot: auto-opens the viewer once on the next boot /
     *                    page load, then is consumed (e.g. a CHANGELOG after an
     *                    update). This is the ONLY thing that auto-opens — out of
     *                    the box the device lands on the launcher / closed window.
     *   home_{lcd,web} — where the viewer goes when launched manually (tile tap /
     *                    Dock app); falls back to the welcome page if unset.
     * Nothing is defaulted — absent = nothing to show / fall back to welcome. */
}
