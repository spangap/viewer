<!-- Viewer window (Window → Viewer). Renders a LOCAL document fetched from the
     device's file server (e.g. /sdcard/readme.md): Markdown → our subset, with
     the browser's fonts; HTML → passed through in a sandboxed iframe. http(s)
     URLs (typed, or links inside a doc) open in the user's own browser tab —
     there is no device proxy (see modules/viewer.ts). -->
<template>
  <FloatingWindow
    id="viewer"
    :title="title"
    :visible="visible"
    :default-geom="defaultGeom"
    :min-size="{ w: 40, h: 20 }"
    @update:visible="v => emit('update:visible', v)"
  >
    <div class="viewer">
      <div class="bar">
        <input
          v-model="address"
          class="addr"
          placeholder="/sdcard/readme.md"
          @keyup.enter="go"
        />
        <button class="go" title="Open" @click="go">Open</button>
      </div>

      <div class="doc">
        <div v-if="error" class="msg err">{{ error }}</div>
        <iframe
          v-else-if="mode === 'html'"
          class="frame"
          :srcdoc="htmlDoc"
          sandbox="allow-popups allow-popups-to-escape-sandbox"
        />
        <div
          v-else-if="mode === 'md'"
          class="md"
          v-html="rendered"
          @click="onClick"
        />
        <pre v-else-if="mode === 'text'" class="text">{{ raw }}</pre>
        <div v-else class="msg dim">
          Open a local Markdown or HTML file — e.g. <code>/sdcard/readme.md</code>.
        </div>
      </div>
    </div>
  </FloatingWindow>
</template>

<script setup lang="ts">
import { ref, watch } from 'vue'
import FloatingWindow from 'spangap-browser/components/FloatingWindow.vue'
import { viewerWebUrl } from '../modules/viewer'
import { markdownToHtml } from '../lib/markdown'

defineProps<{ visible: boolean; title: string }>()
const emit = defineEmits<{ 'update:visible': [value: boolean] }>()

const defaultGeom = { x: 16, y: 10, w: 60, h: 72 }

const address = ref('')
const mode = ref<'' | 'md' | 'html' | 'text'>('')
const rendered = ref('')   /* md → HTML */
const htmlDoc = ref('')    /* html passthrough (iframe srcdoc) */
const raw = ref('')        /* plain text */
const error = ref('')

/* Normalize a webview target to a device path. file:// is stripped; a bare
 * path gets a leading slash. http(s) is not a local doc. */
function toLocalPath(u: string): string {
  let p = u.trim()
  if (/^file:\/\//i.test(p)) p = p.slice(7)
  if (!p.startsWith('/')) p = '/' + p
  return p
}

function isExternal(u: string): boolean {
  return /^https?:\/\//i.test(u.trim())
}

async function load(u: string) {
  error.value = ''
  if (!u) { mode.value = ''; return }

  if (isExternal(u)) {                 /* no device proxy — hand off to the real browser */
    window.open(u, '_blank', 'noopener')
    error.value = `Opened ${u} in a new browser tab (the viewer renders local files only).`
    mode.value = ''
    return
  }

  const path = toLocalPath(u)
  try {
    const r = await fetch(path, { credentials: 'same-origin' })
    if (!r.ok) { error.value = `Could not load ${path} (HTTP ${r.status}).`; mode.value = ''; return }
    const text = await r.text()

    if (!text) { raw.value = '(empty file)'; mode.value = 'text'; return }

    const lower = path.toLowerCase()
    if (lower.endsWith('.md') || lower.endsWith('.markdown') || lower.endsWith('.mkd')) {
      const html = markdownToHtml(text)
      /* fall back to showing the raw source if the render came out empty, so a
       * converter gap never presents as a blank page. */
      if (html.trim()) { rendered.value = html; mode.value = 'md' }
      else             { raw.value = text;     mode.value = 'text' }
    } else if (lower.endsWith('.html') || lower.endsWith('.htm')) {
      /* passthrough in a sandboxed iframe; <base target=_blank> makes its links
       * open in a new tab rather than navigating the sandboxed frame. */
      htmlDoc.value = '<base target="_blank">' + text
      mode.value = 'html'
    } else {
      raw.value = text
      mode.value = 'text'
    }
  } catch (e) {
    error.value = `Could not load ${path}: ${e}`; mode.value = ''
  }
}

/* Click inside rendered Markdown: local (relative) links load in-window;
 * http(s) links already carry target=_blank and fall through to the browser. */
function onClick(ev: MouseEvent) {
  const a = (ev.target as HTMLElement | null)?.closest('a.local') as HTMLAnchorElement | null
  if (!a) return
  ev.preventDefault()
  const href = a.getAttribute('href') || ''
  if (!href) return
  /* resolve relative to the current document's directory */
  const base = toLocalPath(viewerWebUrl.value || address.value)
  const dir = base.slice(0, base.lastIndexOf('/') + 1)
  const target = href.startsWith('/') ? href : dir + href
  address.value = target
  viewerWebUrl.value = target
  load(target)
}

function go() {
  if (address.value.trim()) { viewerWebUrl.value = address.value.trim(); load(address.value.trim()) }
}

/* Driven by `webview` (viewer.web.url → viewerWebUrl) or in-window navigation. */
watch(viewerWebUrl, (u) => { if (u && u !== address.value) { address.value = u; load(u) } }, { immediate: true })
</script>

<style scoped>
.viewer { display: flex; flex-direction: column; height: 100%; background: #fff; color: #202020; }
.bar {
  display: flex; gap: 6px; padding: 6px; background: #ececec;
  border-bottom: 1px solid rgba(0,0,0,0.12);
}
.addr {
  flex: 1; min-width: 0; border: 1px solid rgba(0,0,0,0.2); border-radius: 5px;
  padding: 4px 8px; font-family: 'JetBrains Mono', 'Menlo', monospace; font-size: 12px;
  background: #fff; color: #202020;
}
.go {
  flex: none; border: 1px solid rgba(0,0,0,0.2); background: #f7f7f7; color: #202020;
  border-radius: 5px; padding: 4px 12px; cursor: pointer;
}
.go:hover { background: #efefef; }
.doc { flex: 1; min-height: 0; overflow: auto; }
.frame { width: 100%; height: 100%; border: 0; background: #fff; }
.msg { padding: 20px; color: #555; }
.msg.err { color: #b3261e; }
.msg.dim { color: #888; }
.text { margin: 0; padding: 14px 18px; white-space: pre-wrap; word-break: break-word;
        font-family: 'JetBrains Mono', 'Menlo', monospace; font-size: 13px; color: #202020; }

/* Rendered Markdown — browser fonts, document look. */
.md { padding: 16px 22px; line-height: 1.5; font-size: 15px;
      font-family: system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif; }
.md :deep(h1), .md :deep(h2), .md :deep(h3),
.md :deep(h4), .md :deep(h5), .md :deep(h6) { margin: 0.8em 0 0.4em; line-height: 1.25; color: #111; }
.md :deep(h1) { font-size: 1.7em; }
.md :deep(h2) { font-size: 1.4em; }
.md :deep(h3) { font-size: 1.2em; }
.md :deep(p), .md :deep(ul), .md :deep(ol), .md :deep(blockquote) { margin: 0.5em 0; }
.md :deep(ul), .md :deep(ol) { padding-left: 1.6em; }
.md :deep(a) { color: #1a56db; }
.md :deep(code) {
  font-family: 'JetBrains Mono', 'Menlo', monospace; font-size: 0.9em;
  background: #f1f1f1; border-radius: 4px; padding: 1px 4px;
}
.md :deep(pre) {
  background: #f6f6f6; border: 1px solid rgba(0,0,0,0.1); border-radius: 6px;
  padding: 10px 12px; overflow-x: auto;
  font-family: 'JetBrains Mono', 'Menlo', monospace; font-size: 0.9em; line-height: 1.4;
}
.md :deep(blockquote) {
  border-left: 3px solid #ccc; margin-left: 0; padding-left: 12px; color: #555;
}
.md :deep(hr) { border: 0; border-top: 1px solid #ddd; margin: 1em 0; }
</style>
