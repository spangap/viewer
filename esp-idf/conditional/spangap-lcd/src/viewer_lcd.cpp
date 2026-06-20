/**
 * viewer_lcd — the document viewer's LCD slice.
 *
 * Pipeline:  bytes ──(.md? MD4C)──► HTML ──subset parser──► IR (blocks+runs)
 *                                                              │
 *                                                              ▼
 *                                          LVGL: flex column of spangroups
 *                                          (link-free prose) / flex-wrap rows
 *                                          of labels (link blocks) + mono labels
 *
 * The IR is the spine: both Markdown (converted to HTML on-device via vendored
 * MD4C) and HTML lower into one render-oriented tree, so growing the supported
 * markup is "one block/flag here + one case in the renderer". The subset is also
 * the sanitizer — <script>/<style> and unknown tags never reach the IR.
 *
 * Emphasis is by colour on proportional faces (no bold/italic font blobs):
 * body = dark grey, bold = black, italic = light grey; code uses the platform
 * mono font; headings use the larger montserrat_16_latin. Background is white.
 *
 * I/O off the lcd task: a nav worker task does file/http loads (which block) and
 * then hops the render onto the lcd task via lcdRun — the lcd task never blocks
 * on disk or the network. Links are clickable (worker-dispatched), with a
 * history stack + a floating Back button and relative-URL resolution.
 *
 * This whole file lives under conditional/spangap-lcd/, compiled only when the
 * lcd straddle is staged. http(s) is #if CONFIG_SPANGAP_NET-gated; without net
 * the viewer serves file:// (and a bare path) only.
 *
 * Config:    s.viewer.urlbar (bool)
 * Ephemeral: viewer.lcd.url   (current LCD location; webview uses viewer.web.url)
 */
#include "viewer.h"
#include "viewer_net.h"
#include "spangap.h"      /* cli.h / log.h / storage.h / spawnTask */
#include "mem.h"          /* STACK_PSRAM */
#include "lcd.h"          /* pulls in lvgl.h; fonts */
#include "fs.h"
#include "storage.h"

#include "md4c-html.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cctype>
#include <strings.h>
#include <sys/stat.h>

/* ─────────────── palette (RGB, on white) ─────────────── */
#define C_BODY   0x404040u
#define C_BOLD   0x000000u
#define C_ITALIC 0x808080u
#define C_CODE   0x36474fu
#define C_LINK   0x1a56dbu
#define C_HEAD   0x000000u
#define C_RULE   0xccccccu

#define BODY_FONT (&lv_font_montserrat_12_latin)
#define HEAD_FONT (&lv_font_montserrat_16_latin)
#define MONO_FONT (&lv_font_spleen_5x8)

/* Hard caps. Rendering an unbounded page (e.g. wikipedia) builds tens of
 * thousands of LVGL objects in PSRAM, exhausts it, and then ITS allocation
 * fails system-wide (storage/net/lora time out) — i.e. a huge doc takes the
 * whole node down, not just the viewer. These bound input, IR, and widget count
 * so the viewer can only ever consume a safe slice of PSRAM; oversize content is
 * truncated with a notice. (Real fix for huge docs is viewport virtualization,
 * deferred.) */
static const size_t VIEWER_MAX_BYTES  = 128 * 1024;  /* fetched / read input   */
static const size_t VIEWER_MAX_BLOCKS = 2000;        /* IR blocks              */
static const int    VIEWER_MAX_OBJS   = 800;         /* LVGL objects per render */

/* ─────────────── IR ─────────────── */

enum BType : uint8_t { BT_PARA, BT_HEADING, BT_ITEM, BT_RULE, BT_PRE };
enum : uint8_t { F_BOLD = 1, F_ITALIC = 2, F_CODE = 4, F_LINK = 8 };

struct Run {
    std::string text;
    uint8_t     flags;
    std::string href;
};
struct Block {
    BType            type    = BT_PARA;
    int              level   = 0;
    int              indent  = 0;
    bool             ordered = false;
    int              index   = 0;
    bool             quote   = false;
    std::string      pre;
    std::vector<Run> runs;
};
typedef std::vector<Block> Doc;

/* ─────────────── HTML entity decode ─────────────── */

static void decodeEntity(const std::string& e, std::string& out) {
    if (!e.empty() && e[0] == '#') {
        long cp = (e.size() > 1 && (e[1] == 'x' || e[1] == 'X'))
                      ? strtol(e.c_str() + 2, nullptr, 16)
                      : strtol(e.c_str() + 1, nullptr, 10);
        if (cp <= 0) return;
        if (cp < 0x80) { out.push_back((char)cp); return; }
        if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
        return;
    }
    if      (e == "amp")  out.push_back('&');
    else if (e == "lt")   out.push_back('<');
    else if (e == "gt")   out.push_back('>');
    else if (e == "quot") out.push_back('"');
    else if (e == "apos") out.push_back('\'');
    else if (e == "nbsp") out.push_back(' ');
    else if (e == "hellip") out.append("...");
    else if (e == "mdash" || e == "ndash") out.push_back('-');
}

/* ─────────────── HTML subset → IR ─────────────── */

/* Allow-list of tags we understand. Rendered tags (h1-6, p, br, hr, ul/ol/li,
 * b/strong, i/em, code, pre, blockquote, a, title) are handled explicitly in
 * handleTag. These two lists cover the rest:
 *   - transparent: structural containers — the tag is ignored but its content
 *     flows (so real pages, which wrap everything in div/span/section, render).
 *   - void: empty elements — ignored, no content to skip.
 * ANY other tag is unknown: its whole content is dropped (skipToClose), so we
 * never render markup we don't understand (script/style/svg/forms/etc.). */
static bool nameInList(const std::string& n, const char* const* list) {
    for (int i = 0; list[i]; i++) if (n == list[i]) return true;
    return false;
}
static bool htmlTransparent(const std::string& n) {
    static const char* const L[] = {
        "html", "head", "body", "div", "span", "section", "article", "main",
        "header", "footer", "nav", "aside", "figure", "figcaption", "table",
        "thead", "tbody", "tfoot", "tr", "td", "th", "caption", "colgroup",
        "dl", "dt", "dd", "small", "big", "u", "s", "strike", "mark", "sub",
        "sup", "abbr", "cite", "q", "time", "label", "fieldset", "legend",
        "details", "summary", "font", "tt", "kbd", "samp", "var", "ins", "del",
        "center", "dfn", "ruby", "picture", nullptr};
    return nameInList(n, L);
}
static bool htmlVoid(const std::string& n) {
    static const char* const L[] = {
        "img", "meta", "link", "input", "area", "base", "col", "embed",
        "source", "track", "wbr", "param", nullptr};
    return nameInList(n, L);
}

struct HtmlParser {
    const char* p;
    const char* end;
    Doc&        doc;

    Block       cur;
    bool        curOpen = false;
    uint8_t     flags   = 0;
    std::string href;
    std::string run;
    bool        pre        = false;
    int         quoteDepth = 0;
    bool        lastSpace  = true;
    std::string title;            /* <title> text (head); falls back later */
    bool        titleMode = false;

    struct LCtx { bool ordered; int counter; };
    std::vector<LCtx> lists;

    HtmlParser(const std::string& html, Doc& d)
        : p(html.c_str()), end(html.c_str() + html.size()), doc(d) {}

    void flushRun() {
        if (!run.empty()) { cur.runs.push_back({run, flags, href}); run.clear(); }
    }
    void startBlock(BType t) {
        endBlock();
        cur = Block{};
        cur.type = t;
        cur.quote = quoteDepth > 0;
        cur.indent = (int)lists.size();
        curOpen = true;
        lastSpace = true;
    }
    void endBlock() {
        if (!curOpen) return;
        flushRun();
        if (!cur.runs.empty()) {
            std::string& t = cur.runs.back().text;
            while (!t.empty() && t.back() == ' ') t.pop_back();
            if (t.empty()) cur.runs.pop_back();
        }
        if ((cur.type == BT_RULE || cur.type == BT_PRE || !cur.runs.empty()) &&
            doc.size() < VIEWER_MAX_BLOCKS)
            doc.push_back(cur);
        cur = Block{};
        curOpen = false;
    }
    void ensureBlock() { if (!curOpen) startBlock(BT_PARA); }

    void addText(const char* s, size_t n) {
        if (titleMode) {     /* capture <title>, whitespace-collapsed */
            for (size_t i = 0; i < n; i++) {
                char c = s[i];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    if (!title.empty() && title.back() != ' ') title.push_back(' ');
                } else title.push_back(c);
            }
            return;
        }
        if (pre) { run.append(s, n); return; }
        for (size_t i = 0; i < n; i++) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (lastSpace) continue;
                ensureBlock(); run.push_back(' '); lastSpace = true;
            } else {
                ensureBlock(); run.push_back(c); lastSpace = false;
            }
        }
    }
    void addChars(const std::string& s) { for (char c : s) addText(&c, 1); }

    static std::string attr(const std::string& s, const char* name) {
        std::string key = std::string(name) + "=";
        size_t at = 0;
        for (; at + key.size() <= s.size(); at++)
            if (strncasecmp(s.c_str() + at, key.c_str(), key.size()) == 0) break;
        if (at + key.size() > s.size()) return "";
        size_t v = at + key.size();
        if (v < s.size() && (s[v] == '"' || s[v] == '\'')) {
            char q = s[v++]; size_t e = s.find(q, v);
            return e == std::string::npos ? s.substr(v) : s.substr(v, e - v);
        }
        size_t e = v; while (e < s.size() && !isspace((unsigned char)s[e])) e++;
        return s.substr(v, e - v);
    }

    /* Skip a tag's whole content up to and including its close — for tags we
     * don't render (script/style/svg/forms/unknown). */
    void skipToClose(const std::string& name) {
        std::string close = "</" + name;
        for (; p + close.size() <= end; p++)
            if (strncasecmp(p, close.c_str(), close.size()) == 0) break;
        const char* gt = (const char*)memchr(p, '>', end - p);
        p = gt ? gt + 1 : end;
    }

    void handleTag(const std::string& tag) {
        if (tag.empty()) return;
        bool closing = tag[0] == '/';
        size_t i = closing ? 1 : 0;
        std::string name;
        while (i < tag.size() && isalnum((unsigned char)tag[i])) {
            name.push_back((char)tolower((unsigned char)tag[i])); i++;
        }
        std::string rest = tag.substr(i);

        if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
            if (closing) endBlock();
            else { startBlock(BT_HEADING); cur.level = name[1] - '0'; }
        } else if (name == "p") {
            if (closing) endBlock(); else startBlock(BT_PARA);
        } else if (name == "br") {
            ensureBlock(); run.push_back('\n'); lastSpace = true;
        } else if (name == "hr") {
            endBlock(); Block b; b.type = BT_RULE; doc.push_back(b);
        } else if (name == "ul" || name == "ol") {
            if (closing) { if (!lists.empty()) lists.pop_back(); }
            else { endBlock(); lists.push_back({name == "ol", 0}); }
        } else if (name == "li") {
            if (closing) endBlock();
            else {
                startBlock(BT_ITEM);
                if (!lists.empty()) {
                    cur.ordered = lists.back().ordered;
                    cur.index   = ++lists.back().counter;
                }
            }
        } else if (name == "pre") {
            if (closing) { pre = false; cur.pre = run; run.clear(); endBlock(); }
            else { startBlock(BT_PRE); pre = true; }
        } else if (name == "blockquote") {
            if (closing) { if (quoteDepth) quoteDepth--; endBlock(); }
            else { endBlock(); quoteDepth++; }
        } else if (name == "code") {
            if (!pre) { flushRun(); if (closing) flags &= ~F_CODE; else flags |= F_CODE; }
        } else if (name == "b" || name == "strong") {
            flushRun(); if (closing) flags &= ~F_BOLD; else flags |= F_BOLD;
        } else if (name == "i" || name == "em") {
            flushRun(); if (closing) flags &= ~F_ITALIC; else flags |= F_ITALIC;
        } else if (name == "a") {
            flushRun();
            if (closing) { flags &= ~F_LINK; href.clear(); }
            else { flags |= F_LINK; href = attr(rest, "href"); }
        } else if (name == "title") {
            if (closing) titleMode = false;
            else { titleMode = true; title.clear(); }
        } else if (!closing) {
            /* not a rendered tag: structural containers flow, void tags are
             * empty, and anything else (unknown) has its content dropped. */
            if (!htmlTransparent(name) && !htmlVoid(name)) skipToClose(name);
        }
        /* closing tag of a non-rendered element: ignored */
    }

    void run_() {
        while (p < end) {
            char c = *p;
            if (c == '<') {
                if (p + 4 <= end && strncmp(p, "<!--", 4) == 0) {
                    const char* e = (const char*)memmem(p, end - p, "-->", 3);
                    p = e ? e + 3 : end; continue;
                }
                const char* gt = (const char*)memchr(p, '>', end - p);
                if (!gt) break;
                const char* afterTag = gt + 1;
                handleTag(std::string(p + 1, gt - p - 1));
                /* handleTag may have skipped a whole element (skipToClose advances
                 * p past its </close>); only step over the tag itself otherwise. */
                if (p < afterTag) p = afterTag;
                continue;
            }
            if (c == '&') {
                size_t span = (size_t)(end - p); if (span > 32) span = 32;
                const char* semi = (const char*)memchr(p, ';', span);
                if (semi) {
                    std::string dec; decodeEntity(std::string(p + 1, semi - p - 1), dec);
                    addChars(dec); p = semi + 1; continue;
                }
                addText("&", 1); p++; continue;
            }
            const char* q = p;
            while (q < end && *q != '<' && *q != '&') q++;
            addText(p, q - p); p = q;
        }
        endBlock();
    }
};

static void parseHtml(const std::string& html, Doc& doc, std::string& title) {
    HtmlParser pr(html, doc);
    pr.run_();
    title = pr.title;
    while (!title.empty() && title.back() == ' ') title.pop_back();
}

/* ─────────────── Markdown → HTML (vendored MD4C) ─────────────── */

static void mdSink(const MD_CHAR* d, MD_SIZE n, void* u) {
    ((std::string*)u)->append(d, (size_t)n);
}
static std::string mdToHtml(const std::string& md) {
    std::string out;
    md_html(md.data(), (MD_SIZE)md.size(), mdSink, &out, 0, 0);
    return out;
}

/* ─────────────── state ─────────────── */

static SemaphoreHandle_t s_mux  = nullptr;          /* guards s_html/s_url/req/history */
static std::string       s_html;
static std::string       s_url   = "welcome";
static std::vector<std::string> s_history;
static std::string       s_reqUrl;
static bool              s_reqPending = false;
static bool              s_reqIsBack  = false;
static TaskHandle_t      s_worker = nullptr;

static lv_obj_t*               s_page   = nullptr;  /* scroll column (lcd task) */
static lv_obj_t*               s_urlbar = nullptr;
static std::vector<std::string> s_linkHrefs;        /* per-doc, lcd task only */

static void requestNav(const std::string& url);     /* defined below */

/* ─────────────── URL resolution ─────────────── */

static std::string normalizePath(const std::string& in) {
    bool abs = !in.empty() && in[0] == '/';
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i <= in.size(); i++) {
        if (i == in.size() || in[i] == '/') {
            if (!cur.empty()) {
                if (cur == "..") { if (!parts.empty()) parts.pop_back(); }
                else if (cur != ".") parts.push_back(cur);
            }
            cur.clear();
        } else cur.push_back(in[i]);
    }
    std::string out = abs ? "/" : "";
    for (size_t k = 0; k < parts.size(); k++) {
        out += parts[k];
        if (k + 1 < parts.size()) out += "/";
    }
    if (out.empty()) out = abs ? "/" : ".";
    return out;
}

static std::string resolveUrl(const std::string& base, const std::string& href) {
    if (href.empty()) return base;
    if (href.find("://") != std::string::npos) return href;     /* absolute */

    size_t se = base.find("://");
    if (href.rfind("//", 0) == 0) {                              /* protocol-relative */
        std::string scheme = se != std::string::npos ? base.substr(0, se) : "https";
        return scheme + ":" + href;
    }

    std::string prefix, path;
    if (se != std::string::npos) {                              /* http(s)://host/… */
        size_t ps = base.find('/', se + 3);
        if (ps == std::string::npos) { prefix = base; path = "/"; }
        else { prefix = base.substr(0, ps); path = base.substr(ps); }
    } else if (base.rfind("file://", 0) == 0) {
        prefix = "file://"; path = base.substr(7); if (path.empty()) path = "/";
    } else {                                                     /* bare path */
        prefix = ""; path = base.empty() ? "/" : base;
    }

    std::string rel;
    if (href[0] == '/') rel = href;                             /* root-relative */
    else {
        size_t slash = path.rfind('/');
        std::string dir = slash == std::string::npos ? "" : path.substr(0, slash + 1);
        rel = dir + href;
    }
    return prefix + normalizePath(rel);
}

/* ─────────────── loading (worker task) ─────────────── */

static const char* const WELCOME_MD =
    "# Viewer\n\n"
    "A tiny **HTML** / *Markdown* viewer that renders a growing subset into LVGL.\n\n"
    "- `file://` documents, a bare path, and `http(s)://` (with the network straddle)\n"
    "- Markdown is converted to HTML on-device\n"
    "- Links are tappable; the **<** button goes back\n\n"
    "Try a link: [About this viewer](about)\n\n"
    "Open one from the CLI:\n\n"
    "```\nlcdview /sdcard/readme.md\nlcdview https://example.com/\n```\n\n"
    "---\n\n"
    "Emphasis is shown by colour: **bold** is black, *italic* is grey, "
    "`code` is monospace.\n";

static const char* const ABOUT_MD =
    "# About\n\n"
    "You tapped a link — and *this* page loaded. Tap the **<** button (bottom-left) "
    "or this link to go [back home](welcome).\n\n"
    "## How it renders\n\n"
    "Markdown is parsed to HTML by MD4C, then a small subset parser builds an "
    "intermediate tree, which the LVGL backend draws as spangroups (prose) and "
    "tappable label rows (links).\n\n"
    "1. parse\n2. build IR\n3. render\n\n"
    "> Block quotes, lists, rules, and `inline code` are all supported.\n";

static bool endsWithCI(const std::string& s, const char* suf) {
    size_t n = strlen(suf);
    return s.size() >= n && strcasecmp(s.c_str() + s.size() - n, suf) == 0;
}

static bool sniffMd(const std::string& url, const std::string& body, const std::string& ctype) {
    if (ctype.find("markdown") != std::string::npos) return true;
    if (ctype.find("html") != std::string::npos)     return false;
    if (endsWithCI(url, ".md") || endsWithCI(url, ".markdown") || endsWithCI(url, ".mkd")) return true;
    if (endsWithCI(url, ".html") || endsWithCI(url, ".htm")) return false;
    size_t i = 0; while (i < body.size() && isspace((unsigned char)body[i])) i++;
    return !(i < body.size() && body[i] == '<');
}

static bool loadUrl(const std::string& url, std::string& out, std::string& finalUrl) {
    finalUrl = url;
    if (url == "welcome") { out = mdToHtml(WELCOME_MD); return true; }
    if (url == "about")   { out = mdToHtml(ABOUT_MD);   return true; }

    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
#if CONFIG_SPANGAP_NET
        std::string body, ctype;
        if (!viewerFetch(url, body, ctype, finalUrl)) {
            out = "<h2>Fetch failed</h2><p><code>" + url + "</code></p>";
            return false;
        }
        out = sniffMd(finalUrl, body, ctype) ? mdToHtml(body) : body;
        return true;
#else
        out = "<h2>Not supported in this build</h2><p>http(s) needs the network "
              "straddle (<code>spangap-net</code>). <code>file://</code> only.</p>";
        return false;
#endif
    }

    std::string path = url.rfind("file://", 0) == 0 ? url.substr(7) : url;
    struct stat st;
    if (fs_stat(path.c_str(), &st) != 0) {
        out = "<h2>Not found</h2><p><code>" + path + "</code></p>";
        return false;
    }
    size_t n = (size_t)st.st_size;
    if (n > VIEWER_MAX_BYTES) n = VIEWER_MAX_BYTES;   /* bound PSRAM use */
    std::string buf;
    buf.resize(n);
    int f = fs_open(path.c_str(), "rb");
    if (f < 0) { out = "<h2>Cannot open</h2><p><code>" + path + "</code></p>"; return false; }
    size_t got = fs_read(&buf[0], 1, n, f);
    fs_close(f);
    buf.resize(got);

    out = sniffMd(url, buf, "") ? mdToHtml(buf) : buf;
    return true;
}

/* ─────────────── IR → LVGL (lcd task) ─────────────── */

static void applyStyle(lv_obj_t* obj, uint8_t flags, bool heading, bool quote) {
    const lv_font_t* font = heading ? HEAD_FONT : BODY_FONT;
    uint32_t col = heading ? C_HEAD : (quote ? C_ITALIC : C_BODY);
    if (!heading) {
        if      (flags & F_LINK)   col = C_LINK;
        else if (flags & F_CODE) { col = C_CODE; font = MONO_FONT; }
        else if (flags & F_BOLD)   col = C_BOLD;
        else if (flags & F_ITALIC) col = C_ITALIC;
    }
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(col), 0);
}

static void linkClicked(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (int)s_linkHrefs.size()) return;
    std::string href = s_linkHrefs[idx];
    std::string base;
    xSemaphoreTake(s_mux, portMAX_DELAY); base = s_url; xSemaphoreGive(s_mux);
    requestNav(resolveUrl(base, href));
}

/* Link-free prose: one spangroup (best wrapping/kerning). */
static void buildSpangroup(lv_obj_t* parent, const Block& b) {
    bool heading = b.type == BT_HEADING;
    lv_obj_t* sg = lv_spangroup_create(parent);
    lv_obj_set_width(sg, lv_pct(100));
    lv_obj_set_height(sg, LV_SIZE_CONTENT);
    lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);

    int padL = b.indent * 14 + (b.quote ? 12 : 0);
    if (padL) lv_obj_set_style_pad_left(sg, padL, 0);
    if (heading) lv_obj_set_style_margin_top(sg, b.level <= 2 ? 10 : 6, 0);

    if (b.type == BT_ITEM) {
        lv_span_t* m = lv_spangroup_add_span(sg);
        char mk[16];
        if (b.ordered) snprintf(mk, sizeof mk, "%d. ", b.index);
        else           snprintf(mk, sizeof mk, "- ");
        lv_span_set_text(m, mk);
        lv_style_set_text_color(lv_span_get_style(m), lv_color_hex(b.quote ? C_ITALIC : C_BODY));
        lv_style_set_text_font(lv_span_get_style(m), BODY_FONT);
    }
    for (const Run& r : b.runs) {
        lv_span_t* sp = lv_spangroup_add_span(sg);
        lv_span_set_text(sp, r.text.c_str());
        const lv_font_t* font = heading ? HEAD_FONT : BODY_FONT;
        uint32_t col = heading ? C_HEAD : (b.quote ? C_ITALIC : C_BODY);
        if (!heading) {
            if      (r.flags & F_CODE) { col = C_CODE; font = MONO_FONT; }
            else if (r.flags & F_BOLD)   col = C_BOLD;
            else if (r.flags & F_ITALIC) col = C_ITALIC;
        }
        lv_style_t* st = lv_span_get_style(sp);
        lv_style_set_text_color(st, lv_color_hex(col));
        lv_style_set_text_font(st, font);
    }
    lv_spangroup_refresh(sg);
}

/* Append a run as word-labels into a flex-wrap row; link runs are clickable. */
static void addFlowRun(lv_obj_t* row, const Run& r, bool heading, bool quote, int& objs) {
    bool link = r.flags & F_LINK;
    int idx = -1;
    if (link) { s_linkHrefs.push_back(r.href); idx = (int)s_linkHrefs.size() - 1; }

    std::string w;
    for (size_t i = 0; i <= r.text.size(); i++) {
        char ch = i < r.text.size() ? r.text[i] : '\0';
        if (ch == '\n') ch = ' ';
        if (i < r.text.size()) w.push_back(ch);
        bool boundary = (ch == ' ') || (i == r.text.size());
        if (boundary && !w.empty()) {
            if (objs >= VIEWER_MAX_OBJS) return;       /* render cap — stop adding words */
            objs++;
            lv_obj_t* l = lv_label_create(row);
            lv_label_set_text(l, w.c_str());
            applyStyle(l, r.flags, heading, quote);
            if (link) {
                lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_style_text_decor(l, LV_TEXT_DECOR_UNDERLINE, 0);
                lv_obj_add_event_cb(l, linkClicked, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
            }
            w.clear();
        }
    }
}

/* Link-bearing block: flex-wrap row of labels so link words are tappable. */
static void buildFlowBlock(lv_obj_t* parent, const Block& b, int& objs) {
    bool heading = b.type == BT_HEADING;
    lv_obj_t* row = lv_obj_create(parent);
    objs++;
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(row, 0, 0);
    lv_obj_set_style_pad_row(row, 2, 0);

    int padL = b.indent * 14 + (b.quote ? 12 : 0);
    if (padL) lv_obj_set_style_pad_left(row, padL, 0);
    if (heading) lv_obj_set_style_margin_top(row, b.level <= 2 ? 10 : 6, 0);

    if (b.type == BT_ITEM) {
        lv_obj_t* l = lv_label_create(row);
        char mk[16];
        if (b.ordered) snprintf(mk, sizeof mk, "%d. ", b.index);
        else           snprintf(mk, sizeof mk, "- ");
        lv_label_set_text(l, mk);
        applyStyle(l, 0, false, b.quote);
        objs++;
    }
    for (const Run& r : b.runs) addFlowRun(row, r, heading, b.quote, objs);
}

static void renderDoc(void) {
    if (!s_page) return;
    std::string html, url;
    xSemaphoreTake(s_mux, portMAX_DELAY); html = s_html; url = s_url; xSemaphoreGive(s_mux);

    Doc doc;
    std::string title;
    parseHtml(html, doc, title);
    if (title.empty())               /* fall back to first heading… */
        for (const Block& b : doc)
            if (b.type == BT_HEADING) { for (const Run& r : b.runs) title += r.text; break; }
    if (title.empty()) title = url;  /* …then the location */

    lv_obj_clean(s_page);
    s_linkHrefs.clear();

    /* Title bar: full window width, grey, centered text. A child of the scroll
     * container, so it scrolls away with the content. */
    if (!title.empty()) {
        lv_obj_t* bar = lv_obj_create(s_page);
        lv_obj_remove_style_all(bar);
        lv_obj_set_width(bar, lv_pct(100));
        lv_obj_set_height(bar, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0xe0e0e0), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_top(bar, 6, 0);
        lv_obj_set_style_pad_bottom(bar, 6, 0);
        lv_obj_set_style_pad_left(bar, 8, 0);
        lv_obj_set_style_pad_right(bar, 8, 0);
        lv_obj_t* t = lv_label_create(bar);
        lv_obj_set_width(t, lv_pct(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(t, HEAD_FONT, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(0x202020), 0);
        lv_label_set_text(t, title.c_str());
    }

    /* Content column: padded so body text isn't against the window edges (the
     * title bar above is the only full-bleed element). */
    lv_obj_t* col = lv_obj_create(s_page);
    lv_obj_remove_style_all(col);
    lv_obj_set_width(col, lv_pct(100));
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(col, 8, 0);
    lv_obj_set_style_pad_right(col, 8, 0);
    lv_obj_set_style_pad_top(col, 6, 0);
    lv_obj_set_style_pad_row(col, 6, 0);

    int objs = 0;
    for (const Block& b : doc) {
        if (objs >= VIEWER_MAX_OBJS) {       /* render cap — bound PSRAM / lcd-task time */
            lv_obj_t* l = lv_label_create(col);
            lv_obj_set_width(l, lv_pct(100));
            lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_font(l, BODY_FONT, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(C_ITALIC), 0);
            lv_label_set_text(l, "... (truncated - page too large to render fully)");
            break;
        }
        if (b.type == BT_RULE) {
            lv_obj_t* hr = lv_obj_create(col);
            lv_obj_set_size(hr, lv_pct(100), 2);
            lv_obj_set_style_bg_color(hr, lv_color_hex(C_RULE), 0);
            lv_obj_set_style_border_width(hr, 0, 0);
            lv_obj_set_style_radius(hr, 0, 0);
            lv_obj_set_style_pad_all(hr, 0, 0);
            objs++;
            continue;
        }
        if (b.type == BT_PRE) {
            lv_obj_t* l = lv_label_create(col);
            lv_obj_set_width(l, lv_pct(100));
            lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_font(l, MONO_FONT, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(C_CODE), 0);
            lv_label_set_text(l, b.pre.c_str());
            objs++;
            continue;
        }
        bool hasLink = false;
        for (const Run& r : b.runs) if (r.flags & F_LINK) { hasLink = true; break; }
        if (hasLink) buildFlowBlock(col, b, objs);
        else       { buildSpangroup(col, b); objs++; }
    }
    lv_obj_scroll_to_y(s_page, 0, LV_ANIM_OFF);
}

/* ─────────────── nav worker (off the lcd task) ─────────────── */

static void requestNav(const std::string& url) {
    if (url.empty()) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_reqUrl = url; s_reqPending = true; s_reqIsBack = false;
    xSemaphoreGive(s_mux);
    if (s_worker) xTaskNotifyGive(s_worker);
}
static void requestBack(void) {
    bool have;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    have = !s_history.empty();
    if (have) {
        s_reqUrl = s_history.back(); s_history.pop_back();
        s_reqPending = true; s_reqIsBack = true;
    }
    xSemaphoreGive(s_mux);
    if (have && s_worker) xTaskNotifyGive(s_worker);
}

static void navRenderCb(void*) {
    if (s_page) renderDoc();
    lcdShowProgram("Viewer");
}

static void viewerWorker(void*) {
    info("nav worker up");
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        std::string url, cur; bool isBack;
        xSemaphoreTake(s_mux, portMAX_DELAY);
        if (!s_reqPending) { xSemaphoreGive(s_mux); continue; }
        url = s_reqUrl; isBack = s_reqIsBack; s_reqPending = false; cur = s_url;
        xSemaphoreGive(s_mux);

        if (!isBack && !cur.empty() && cur != url) {
            xSemaphoreTake(s_mux, portMAX_DELAY);
            s_history.push_back(cur);
            if (s_history.size() > 50) s_history.erase(s_history.begin());
            xSemaphoreGive(s_mux);
        }

        std::string html, finalUrl;
        loadUrl(url, html, finalUrl);     /* error pages are still rendered */
        xSemaphoreTake(s_mux, portMAX_DELAY); s_html = html; s_url = finalUrl; xSemaphoreGive(s_mux);
        storageSet("viewer.lcd.url", finalUrl.c_str());
        lcdRun(navRenderCb, nullptr);
    }
}

/* ─────────────── launcher program (lcd task) ─────────────── */

static void backClicked(lv_event_t*) { requestBack(); }

/* Tapping the URL field starts editing it. It's added to the shared keypad group
 * at build; -lcd saves/restores per-program focus across show/hide, so the viewer
 * needs to release nothing on leave. */
static void urlbarFocusCb(lv_event_t*) {
    if (!s_urlbar || !lcdInputGroup()) return;
    lv_group_focus_obj(s_urlbar);
    lv_group_set_editing(lcdInputGroup(), true);
}
static void urlbarReady(lv_event_t*) {
    if (!s_urlbar) return;
    const char* t = lv_textarea_get_text(s_urlbar);
    if (t && *t) requestNav(t);
}

static void viewerApp(void* arg) {
    lv_obj_t* layer = (lv_obj_t*)arg;
    if (s_page) { renderDoc(); return; }   /* re-open: layer kept */

    lv_obj_set_flex_flow(layer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(layer, 0, 0);
    lv_obj_set_style_pad_row(layer, 0, 0);

    if (storageGetInt("s.viewer.urlbar", 0)) {
        s_urlbar = lv_textarea_create(layer);
        lv_textarea_set_one_line(s_urlbar, true);
        lv_textarea_set_placeholder_text(s_urlbar, "file:// or http(s):// …");
        lv_obj_set_width(s_urlbar, lv_pct(100));
        lv_obj_set_style_text_font(s_urlbar, MONO_FONT, 0);
        /* Added to the shared keypad group; -lcd saves/restores focus per program
         * across show/hide, so the viewer need not release it on leave. Focused
         * only on tap (urlbarFocusCb) so opening the viewer to read doesn't grab
         * the keyboard. */
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), s_urlbar);
        lv_obj_add_event_cb(s_urlbar, urlbarFocusCb, LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(s_urlbar, urlbarReady,   LV_EVENT_READY,   nullptr);
    }

    s_page = lv_obj_create(layer);
    lv_obj_set_width(s_page, lv_pct(100));
    lv_obj_set_flex_grow(s_page, 1);
    lv_obj_set_style_bg_color(s_page, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_radius(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 0, 0);   /* full-bleed: title bar spans width; col pads the body */
    lv_obj_set_style_pad_row(s_page, 0, 0);
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_page, LV_DIR_VER);

    /* floating Back button (history), bottom-left, above the page */
    lv_obj_t* back = lv_button_create(layer);
    lv_obj_add_flag(back, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(back, 30, 30);
    lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 6, -6);
    lv_obj_set_style_bg_color(back, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_70, 0);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(back, backClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* bl = lv_label_create(back);
    lv_obj_set_style_text_font(bl, BODY_FONT, 0);   /* 12_latin carries LV_SYMBOL_* */
    lv_obj_set_style_text_color(bl, lv_color_black(), 0);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_center(bl);

    renderDoc();
}

/* ─────────────── CLI + Settings ─────────────── */

static void cliViewer(const char* args) {
    if (args && cliWantsHelp(args)) {
        cliPrintf("%-*s show current location\n", CLI_HELP_COL, "lcdview");
        cliPrintf("%-*s open a document on the LCD (file://, http(s)://, or a path)\n",
                  CLI_HELP_COL, "lcdview <url>");
        return;
    }
    if (!args || !*args) {
        std::string u;
        xSemaphoreTake(s_mux, portMAX_DELAY); u = s_url; xSemaphoreGive(s_mux);
        cliPrintf("location: %s\n", u.c_str());
        return;
    }
    requestNav(args);
    cliPrintf("opening %s\n", args);
}

static void viewerSettings(void* arg) {
    lv_obj_t* p = (lv_obj_t*)arg;
    lcdSettingSection(p, "Viewer");
    lcdSettingSwitch (p, "Address bar", "s.viewer.urlbar");
    lcdSettingCaption(p, "Show a URL entry field at the top of the viewer "
                         "(takes effect next time it is opened).");
    lcdSettingValue  (p, "Location",    "viewer.lcd.url");
}

/* ─────────────── init ─────────────── */

/* Register the viewer program — a when:-gated init hook (spangap/spangap-lcd).
 * Everything the viewer does is the LCD viewer, so all of it is gated here;
 * without lcd staged the hook is never called (viewerInit stays a no-op). */
void viewerLcdRegister(void) {
    s_mux = xSemaphoreCreateMutex();
    storageDefault("s.viewer.urlbar", 0);

    std::string html = mdToHtml(WELCOME_MD);
    xSemaphoreTake(s_mux, portMAX_DELAY); s_html = html; s_url = "welcome"; xSemaphoreGive(s_mux);
    storageSet("viewer.lcd.url", "welcome");

    /* generous stack: the worker runs the TLS handshake (mbedtls) for http(s). */
    s_worker = spawnTask(viewerWorker, "viewer", 24576, nullptr, 1, 1, STACK_PSRAM);
    cliRegisterCmd("lcdview", cliViewer);   /* LCD viewer; the web counterpart is `webview` */
    lcdRegister("Viewer", "viewer", viewerApp);
    lcdRegisterSettings("Viewer", "Viewer", viewerSettings);
    info("registered");
}
