/**
 * md_transform — the viewer's Markdown→HTML server slice (spangap-web).
 *
 * Compiled only when spangap-web is staged (this dir is a conditional slice).
 * Registers a file-extension transform with the web server so that any GET for
 * a *.md / *.markdown / *.mkd document (stored plain OR gzipped — the web
 * server inflates .gz before calling us) is converted on the device, via the
 * vendored MD4C, and served as a full text/html page.
 *
 * This is where Markdown→HTML now lives for BOTH front ends: the web UI fetches
 * the converted HTML straight into an <iframe>, and the LCD viewer fetches it
 * from localhost — neither converts Markdown itself. The conversion runs on
 * web's file worker; MD4C buffers and the gzip window are heap-backed, so the
 * worker stack only carries the parser's own frames.
 *
 * Independent of the LCD: gated on spangap-web (not spangap-lcd), so a headless
 * build still serves rendered Markdown over the web.
 */
#include "viewer.h"
#include "viewer_web.h"
#include "web.h"
#include "log.h"

#include "md4c-html.h"

#include "esp_heap_caps.h"

#include <string>
#include <cctype>
#include <cstring>

/* Document chrome wrapped around MD4C's HTML fragment: charset, a <title> (so
 * the viewer's window/status bar can adopt the page title), and the web viewer's
 * lightweight typographic CSS. The LCD's HTML subset parser ignores
 * <head>/<style> and renders the body. */
static const char* DOC_HEAD_1 =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>";
static const char* DOC_HEAD_2 =
    "</title><style>"
    "body{margin:0;padding:16px 22px;line-height:1.5;font-size:15px;color:#202020;"
    "font-family:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;background:#fff}"
    "h1,h2,h3,h4,h5,h6{margin:.8em 0 .4em;line-height:1.25;color:#111}"
    "h1{font-size:1.7em}h2{font-size:1.4em}h3{font-size:1.2em}"
    "p,ul,ol,blockquote{margin:.5em 0}ul,ol{padding-left:1.6em}a{color:#1a56db}"
    "code{font-family:'JetBrains Mono',Menlo,monospace;font-size:.9em;"
    "background:#f1f1f1;border-radius:4px;padding:1px 4px}"
    "pre{background:#f6f6f6;border:1px solid rgba(0,0,0,.1);border-radius:6px;"
    "padding:10px 12px;overflow-x:auto;font-size:.9em;line-height:1.4}"
    "pre code{background:none;padding:0}"
    "blockquote{border-left:3px solid #ccc;margin-left:0;padding-left:12px;color:#555}"
    "hr{border:0;border-top:1px solid #ddd;margin:1em 0}"
    "img{max-width:100%}table{border-collapse:collapse}"
    "td,th{border:1px solid #ddd;padding:4px 8px}"
    "</style></head><body>";
static const char* DOC_TAIL = "</body></html>";

static void mdSink(const MD_CHAR* d, MD_SIZE n, void* u) {
    ((std::string*)u)->append(d, (size_t)n);
}

/* Slug for a heading anchor: the heading's text (inline tags stripped) keeping
 * only [a-zA-Z0-9_], with spaces turned into underscores. */
static std::string slugify(const std::string& innerHtml) {
    std::string slug;
    bool inTag = false;
    for (char c : innerHtml) {
        if (c == '<') { inTag = true; continue; }
        if (c == '>') { inTag = false; continue; }
        if (inTag) continue;
        if (c == ' ') slug.push_back('_');
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_') slug.push_back(c);
        /* everything else (punctuation, entities) dropped */
    }
    return slug;
}

/* Give every heading an id= anchor derived from its text, so `#anchor` links
 * resolve. MD4C emits plain `<hN>…</hN>` (no attributes), which we rewrite to
 * `<hN id="slug">…</hN>`. The browser/iframe then scrolls to the fragment for
 * free; the LCD viewer ignores the id but renders the heading unchanged. */
static void addHeadingAnchors(std::string& body) {
    std::string out;
    out.reserve(body.size() + 64);
    size_t i = 0, n = body.size();
    while (i < n) {
        if (body[i] == '<' && i + 3 < n && body[i + 1] == 'h' &&
            body[i + 2] >= '1' && body[i + 2] <= '6' && body[i + 3] == '>') {
            char level = body[i + 2];
            size_t openEnd = i + 4;
            std::string close = std::string("</h") + level + ">";
            size_t cpos = body.find(close, openEnd);
            size_t innerEnd = (cpos == std::string::npos) ? n : cpos;
            std::string inner = body.substr(openEnd, innerEnd - openEnd);
            std::string slug = slugify(inner);
            out += "<h"; out.push_back(level);
            if (!slug.empty()) { out += " id=\""; out += slug; out += '"'; }
            out += '>';
            out += inner;
            if (cpos != std::string::npos) { out += close; i = cpos + close.size(); }
            else i = n;
            continue;
        }
        out.push_back(body[i++]);
    }
    body.swap(out);
}

/* Append `s` (length n) HTML-escaped, for use inside <title>. */
static void appendEscaped(std::string& out, const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if      (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else               out.push_back(c);
    }
}

/* Title = the document's first ATX heading text, else the file's base name with
 * the extension stripped. */
static std::string deriveTitle(const char* name, const uint8_t* md, size_t len) {
    size_t i = 0;
    while (i < len) {
        size_t ls = i;
        while (i < len && md[i] != '\n') i++;
        size_t le = i;                       /* [ls,le) is one line */
        if (i < len) i++;                    /* step over '\n'      */
        size_t p = ls;
        while (p < le && (md[p] == ' ' || md[p] == '\t')) p++;
        if (p < le && md[p] == '#') {
            while (p < le && md[p] == '#') p++;
            while (p < le && (md[p] == ' ' || md[p] == '\t')) p++;
            size_t e = le;
            while (e > p && (md[e - 1] == ' ' || md[e - 1] == '\t' ||
                             md[e - 1] == '#'  || md[e - 1] == '\r')) e--;
            if (e > p) {
                std::string t;
                appendEscaped(t, (const char*)md + p, e - p);
                return t;
            }
        }
    }
    /* Fallback: base name without a trailing .md/.markdown/.mkd. */
    std::string t = name ? name : "";
    size_t dot = t.rfind('.');
    if (dot != std::string::npos) t.erase(dot);
    std::string esc; appendEscaped(esc, t.data(), t.size());
    return esc;
}

static bool mdTransform(const char* name, const uint8_t* in, size_t inLen,
                        uint8_t** out, size_t* outLen, const char** contentType) {
    std::string body;
    if (md_html((const MD_CHAR*)in, (MD_SIZE)inLen, mdSink, &body, 0, 0) != 0) {
        warn("md4c parse failed for %s\n", name ? name : "?");
        return false;
    }
    addHeadingAnchors(body);

    std::string doc;
    doc.reserve(body.size() + 1024);
    doc += DOC_HEAD_1;
    doc += deriveTitle(name, in, inLen);
    doc += DOC_HEAD_2;
    doc += body;
    doc += DOC_TAIL;

    uint8_t* buf = (uint8_t*)heap_caps_malloc(doc.size(), MALLOC_CAP_SPIRAM);
    if (!buf) return false;
    memcpy(buf, doc.data(), doc.size());
    *out = buf;
    *outLen = doc.size();
    *contentType = "text/html; charset=utf-8";
    return true;
}

/* when:-gated service (services: ViewerWebService, spangap/spangap-web).
 * Registers the markdown transform; without spangap-web staged this slice isn't
 * compiled and the service is absent. */
void ViewerWebService::onInit() {
    webRegisterFileExt("md,markdown,mkd", mdTransform);
    info("markdown→HTML transform registered\n");
}
