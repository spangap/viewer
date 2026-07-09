/**
 * viewer — browser-side document viewer (SPA side).
 *
 * A floating window (the "Viewer" Dock app) that loads a URL into an <iframe>
 * served by the device's web server (Markdown is converted to HTML server-side).
 * It runs at the device origin, so same-origin images/links/cookies work.
 *
 * Start-up (mirrors the LCD), driven by device config (see firmware viewer.cpp):
 *   - on the first storage sync, a one-shot `s.viewer.once_web` opens the window
 *     (then is consumed) and is raised on top, after any windows the SPA restored
 *     from localStorage. That one-shot is the ONLY thing that auto-opens.
 *   - opening it from the Dock (a manual launch) goes to `home_web`.
 *   - the device CLI verb `webview <path>` (ephemeral `viewer.web.url`) opens that
 *     exact path.
 */
import { ref, watch } from 'vue'
import { useDeviceStore } from 'spangap-browser/stores/device'
import { registerApp } from 'spangap-browser/lib/apps'
import { registerWindowMount } from 'spangap-browser/lib/windowMounts'
import ViewerWindow from '../panels/ViewerWindow.vue'

/* Window visibility + focus nonce — <StraddleWindows/> binds these to the
 * window via the mount registered below. */
export const viewerWebVisible = ref(false)
export const viewerWebFocus = ref(0)

/* Current document location, driven by `webview` (viewer.web.url), the start-up
 * logic, or the window's own address bar. */
export const viewerWebUrl = ref('')

const HOME_FALLBACK = '/WELCOME.md'

function homeWeb(device: ReturnType<typeof useDeviceStore>): string {
  const h = device.get('s.viewer.home_web')
  return typeof h === 'string' && h ? h : HOME_FALLBACK
}

/* Make the window visible and raise it to the front (the focus nonce → mount
 * → FloatingWindow.bringToFront). On page load other windows call bringToFront()
 * from their localStorage restore on mount, which can land them above us if they
 * run just after; re-raise on the next macrotask so the viewer ends up on top
 * regardless of that ordering. */
function raise() {
  viewerWebVisible.value = true
  viewerWebFocus.value++
  setTimeout(() => { if (viewerWebVisible.value) viewerWebFocus.value++ }, 150)
}

/** Dock action: a manual launch opens the window on its home page (only when it
 *  wasn't already open, so re-selecting it just re-focuses). */
export function showViewer() {
  if (!viewerWebVisible.value) viewerWebUrl.value = homeWeb(useDeviceStore())
  raise()
}

export function registerViewer() {
  const device = useDeviceStore()

  /* Dock app — opens the document viewer window on its home page. */
  registerApp({ id: 'viewer', label: 'Viewer', icon: 'viewer', placement: 7,
                open: showViewer, isOpen: () => viewerWebVisible.value })

  registerWindowMount({ id: 'viewer', title: 'Viewer', component: ViewerWindow,
                        visible: viewerWebVisible, focusToken: viewerWebFocus })

  /* `webview <path>` on the device sets ephemeral viewer.web.url → open it. */
  watch(() => device.get('viewer.web.url'), (u) => {
    if (typeof u === 'string' && u && u !== 'welcome') { viewerWebUrl.value = u; raise() }
  })

  /* Start-up: the viewer OWNS its open state on each load (overriding whatever
   * FloatingWindow restored from localStorage). ONLY a one-shot once_web opens it
   * (then is consumed); otherwise it stays closed. We trigger on `device.synced`
   * — the first full storage dump — so s.viewer.* values are populated (raw
   * `connected` fires too early). raise() runs after the SPA restored its saved
   * windows, so the viewer ends up on top. */
  let started = false
  watch(() => device.synced, (s) => {
    if (started || !s) return
    started = true
    const once = device.get('s.viewer.once_web')
    if (typeof once === 'string' && once) {
      viewerWebUrl.value = once
      device.set('s.viewer.once_web', '')          /* consume the one-shot */
      raise()
    } else {
      viewerWebVisible.value = false               /* nothing to show — stay closed */
    }
  }, { immediate: true })
}
