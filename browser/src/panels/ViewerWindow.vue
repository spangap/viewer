<!-- Viewer window (Window → Viewer). Loads a URL straight into an <iframe>: the
     device's web server serves the document (Markdown is converted to HTML
     server-side, so .md arrives as text/html), and the browser renders it,
     fetching same-origin images and following links itself. Because the frame
     runs at the device's own origin, the session cookie rides along — admin-
     gated paths (/sdcard, /state) load. http(s) URLs are loaded in the frame
     too (no _blank hand-off); cross-origin sites that forbid framing are the
     browser's call.

     The address bar (Back + URL) is hidden while reading; press Space to reveal
     and focus it, click anywhere in the page (or Esc) to hide it again. -->
<template>
  <FloatingWindow
    id="viewer"
    :title="pageTitle || title"
    :visible="visible"
    :default-geom="defaultGeom"
    :min-size="{ w: 40, h: 20 }"
    @update:visible="v => emit('update:visible', v)"
  >
    <div class="viewer" tabindex="0" @keydown="onKey">
      <div v-show="showAddr" class="bar">
        <button class="nav" title="Back" @click="back">‹</button>
        <input
          ref="addr"
          v-model="address"
          class="addr"
          placeholder="/WELCOME.md"
          @keyup.enter="go"
        />
        <button class="go" title="Open" @click="go">Open</button>
      </div>

      <div class="doc" @click="hideAddr">
        <iframe
          ref="frame"
          class="frame"
          :src="src"
          @load="onLoad"
        />
      </div>
    </div>
  </FloatingWindow>
</template>

<script setup lang="ts">
import { ref, watch, nextTick } from 'vue'
import FloatingWindow from 'spangap-browser/components/FloatingWindow.vue'
import { viewerWebUrl, viewerWebFocus } from '../modules/viewer'

defineProps<{ visible: boolean; title: string }>()
const emit = defineEmits<{ 'update:visible': [value: boolean] }>()

const defaultGeom = { x: 16, y: 10, w: 60, h: 72 }

const address = ref('')           /* what the address bar shows / submits */
const src = ref('')               /* bound to the iframe — the loaded URL  */
const pageTitle = ref('')         /* <title> of the loaded page            */
const showAddr = ref(false)       /* address bar revealed (Space)          */
const frame = ref<HTMLIFrameElement | null>(null)
const addr = ref<HTMLInputElement | null>(null)

/* Normalize a viewer target into something the iframe can load. `127.0.0.1` /
 * `localhost` mean "this device" — but in the browser that's the user's own
 * machine, so rewrite those to a same-origin path (the device serves the SPA, so
 * a bare path loads from it). Other http(s) URLs pass through; file:// is
 * stripped; a bare path gets a leading slash. */
function toSrc(u: string): string {
  let p = u.trim()
  if (!p) return ''
  const local = p.match(/^https?:\/\/(?:127\.0\.0\.1|localhost)(?::\d+)?(\/.*)?$/i)
  if (local) return local[1] || '/'
  if (/^https?:\/\//i.test(p)) return p
  if (/^file:\/\//i.test(p)) p = p.slice(7)
  if (!p.startsWith('/')) p = '/' + p
  return p
}

function load(u: string) {
  const s = toSrc(u)
  address.value = u.trim()
  if (s !== src.value) src.value = s
  else if (frame.value) frame.value.src = s   /* reload if same URL re-submitted */
}

function go() {
  if (address.value.trim()) {
    viewerWebUrl.value = address.value.trim()
    load(address.value.trim())
    hideAddr()
  }
}

/* Same-origin frames share history; Back walks the frame's own stack (covers
 * links the user followed inside the document). Cross-origin pages may block it,
 * which is acceptable for a help/README viewer. */
function back() {
  try { frame.value?.contentWindow?.history.back() } catch { /* cross-origin */ }
}

function revealAddr() {
  showAddr.value = true
  nextTick(() => addr.value?.focus())
}
function hideAddr() { showAddr.value = false }

/* Space (when not already typing in the address field) reveals the bar; Esc
 * hides it. Keys are only seen here when the parent — not the iframe — has focus;
 * the in-iframe case is wired on load() via the same-origin contentDocument. */
function onKey(e: KeyboardEvent) {
  if (e.key === 'Escape') { hideAddr(); return }
  if (e.code === 'Space' && e.target !== addr.value) { e.preventDefault(); revealAddr() }
}
function onDocKey(e: KeyboardEvent) {
  if (e.code === 'Space') { e.preventDefault(); revealAddr() }
}

/* On each navigation (initial load or an in-frame link), adopt the page's
 * <title> for the window title and reflect the real location in the address bar.
 * Same-origin only — wire Space/click handlers into the frame so the address bar
 * still toggles while the iframe holds focus. All reads throw for cross-origin
 * pages, where we keep the last known title/address and skip the wiring. */
function onLoad() {
  try {
    const w = frame.value?.contentWindow
    const d = frame.value?.contentDocument
    if (d?.title) pageTitle.value = d.title
    if (w?.location?.href) {
      const loc = w.location
      address.value = loc.pathname + loc.search + loc.hash
    }
    d?.addEventListener('keydown', onDocKey)
    d?.addEventListener('click', hideAddr)
    /* A mousedown inside the iframe doesn't reach the window's own
     * @mousedown="bringToFront", so foreground the window ourselves — bumping the
     * focus nonce drives FloatingWindow.bringToFront via MainLayout. */
    d?.addEventListener('mousedown', foreground)
    /* Focus the content so Space (→ address bar) and scrolling work without a
     * click first; the document keydown above then reaches us. */
    w?.focus()
  } catch { /* cross-origin: leave title/address as-is, no in-frame wiring */ }
}

/* Raise the viewer window to the front (focus nonce → FloatingWindow). */
function foreground() { viewerWebFocus.value++ }

/* Driven by `webview`, the start-up logic, or the address bar (all via
 * viewerWebUrl). Nothing auto-opens here — start-up is owned by modules/viewer. */
watch(viewerWebUrl, (u) => { if (u && u !== address.value) load(u) }, { immediate: true })
</script>

<style scoped>
.viewer { display: flex; flex-direction: column; height: 100%; background: #fff; color: #202020; outline: none; }
.bar {
  display: flex; gap: 6px; padding: 6px; background: #ececec;
  border-bottom: 1px solid rgba(0,0,0,0.12);
}
.addr {
  flex: 1; min-width: 0; border: 1px solid rgba(0,0,0,0.2); border-radius: 5px;
  padding: 4px 8px; font-family: 'JetBrains Mono', 'Menlo', monospace; font-size: 12px;
  background: #fff; color: #202020;
}
.nav, .go {
  flex: none; border: 1px solid rgba(0,0,0,0.2); background: #f7f7f7; color: #202020;
  border-radius: 5px; padding: 4px 12px; cursor: pointer;
}
.nav { padding: 4px 10px; font-size: 16px; line-height: 1; }
.nav:hover, .go:hover { background: #efefef; }
.doc { flex: 1; min-height: 0; overflow: hidden; }
.frame { width: 100%; height: 100%; border: 0; background: #fff; }
</style>
