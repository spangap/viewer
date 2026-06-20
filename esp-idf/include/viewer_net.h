/**
 * viewer_net — http(s) fetch interface (implemented in conditional/spangap-net/).
 *
 * The implementation is compiled only when spangap-net is staged; the viewer
 * calls it under #if CONFIG_SPANGAP_NET. Declared here so both the caller (the
 * lcd slice) and the implementation share one prototype.
 */
#ifndef SPANGAP_VIEWER_NET_H
#define SPANGAP_VIEWER_NET_H

#include <string>

/** GET `url` over http(s), following redirects (301/302/…). On a 2xx response
 *  returns true with the body in `out`, the (lowercased) Content-Type in
 *  `contentType`, and the final post-redirect URL in `finalUrl`. Blocking, with
 *  TLS via the IDF cert bundle — call from a worker task, never the lcd task. */
bool viewerFetch(const std::string& url, std::string& out, std::string& contentType,
                 std::string& finalUrl);

#endif /* SPANGAP_VIEWER_NET_H */
