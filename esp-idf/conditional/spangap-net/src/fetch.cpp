/**
 * fetch — the viewer's http(s) client (spangap-net slice).
 *
 * Compiled only when spangap-net is staged (this dir is a conditional slice).
 * A thin esp_http_client GET with TLS via the IDF certificate bundle, exactly
 * like acme/duckdns — the platform has no shared HTTP-client wrapper.
 *
 * Follows redirects (301/302/…): esp_http_client auto-redirects during perform.
 * We only accumulate body/headers for the FINAL 2xx response (gated on the
 * live status code), so intermediate redirect bodies never pollute the result,
 * and report the final post-redirect URL.
 *
 * The body is hard-capped (FETCH_CAP) so a huge page can't exhaust PSRAM and
 * starve the rest of the system; the renderer caps again on its side.
 *
 * Blocking; the viewer calls it from its nav worker task, never the lcd task.
 */
#include "viewer_net.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "log.h"

#include <cctype>
#include <cstring>
#include <strings.h>
#include <string>

namespace {

constexpr size_t FETCH_CAP = 128 * 1024;   /* matches the renderer's input cap */

struct FetchCtx {
    std::string* body;
    std::string* ctype;
    bool         over;
};

bool is2xx(esp_http_client_handle_t c) {
    int s = esp_http_client_get_status_code(c);
    return s >= 200 && s < 300;
}

esp_err_t onEvent(esp_http_client_event_t* e) {
    FetchCtx* c = (FetchCtx*)e->user_data;
    switch (e->event_id) {
    case HTTP_EVENT_ON_HEADER:
        /* Only the final (2xx) response's Content-Type — ignore redirect hops. */
        if (is2xx(e->client) && e->header_key && e->header_value &&
            strcasecmp(e->header_key, "Content-Type") == 0) {
            *c->ctype = e->header_value;
            for (char& ch : *c->ctype) ch = (char)tolower((unsigned char)ch);
        }
        break;
    case HTTP_EVENT_ON_DATA:
        /* Skip bodies of redirect (3xx) responses; accumulate only the final 2xx,
         * capped. */
        if (is2xx(e->client)) {
            if (c->body->size() + e->data_len <= FETCH_CAP)
                c->body->append((const char*)e->data, e->data_len);
            else
                c->over = true;
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

}  // namespace

bool viewerFetch(const std::string& url, std::string& out, std::string& contentType,
                 std::string& finalUrl) {
    out.clear();
    contentType.clear();
    finalUrl = url;
    FetchCtx ctx{&out, &contentType, false};

    esp_http_client_config_t cfg = {};
    cfg.url                   = url.c_str();
    cfg.event_handler         = onEvent;
    cfg.user_data             = &ctx;
    cfg.timeout_ms            = 15000;
    cfg.crt_bundle_attach     = esp_crt_bundle_attach;   /* validate TLS via IDF bundle */
    cfg.user_agent            = "spangap-viewer/0.1";
    cfg.max_redirection_count = 10;                      /* follow Moved Permanently etc. */

    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) return false;
    esp_err_t err = esp_http_client_perform(cl);
    int status = esp_http_client_get_status_code(cl);
    char fu[1024] = {0};
    if (esp_http_client_get_url(cl, fu, sizeof fu) == ESP_OK && fu[0]) finalUrl = fu;
    esp_http_client_cleanup(cl);

    if (err != ESP_OK) { warn("fetch %s: %s", url.c_str(), esp_err_to_name(err)); return false; }
    if (status < 200 || status >= 300) { warn("fetch %s: HTTP %d", url.c_str(), status); return false; }
    if (ctx.over) warn("fetch %s: body capped at %u bytes", url.c_str(), (unsigned)FETCH_CAP);
    return true;
}
