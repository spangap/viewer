/**
 * viewer — browser-side local document viewer (SPA side).
 *
 * A floating window (Window → Viewer) that renders LOCAL Markdown (browser
 * fonts, same subset as the LCD) and LOCAL HTML (passed through). It does NOT
 * fetch http(s) — there's no device proxy (would need APSTA); Markdown links to
 * http(s) just open in the user's own browser tab.
 *
 * The device CLI verb `webview <path>` sets the ephemeral key `viewer.web.url`;
 * this module watches it and pops the window open on that document.
 */
import { ref, watch } from 'vue'
import { useDeviceStore } from 'spangap-browser/stores/device'
import { useMenuStore } from 'spangap-browser/stores/menu'

/* Window visibility + focus nonce — MainLayout binds these to the window. */
export const viewerWebVisible = ref(false)
export const viewerWebFocus = ref(0)

/* Current document location, driven by `webview` (viewer.web.url) or the
 * window's own address bar. */
export const viewerWebUrl = ref('')

/** Show + raise the viewer window (menu action / on a new viewer.web.url). */
export function showViewer() { viewerWebVisible.value = true; viewerWebFocus.value++ }

export function registerViewer() {
  const menu = useMenuStore()
  const device = useDeviceStore()

  /* Join the existing "Window" menu (alongside CLI / System Log). */
  menu.setMenu('window', { label: 'Window' })
  menu.register('window/viewer', 'Viewer', { type: 'action', action: showViewer })

  /* `webview <path>` on the device sets ephemeral viewer.web.url → open it. */
  watch(() => device.get('viewer.web.url'), (u) => {
    if (typeof u === 'string' && u && u !== 'welcome') { viewerWebUrl.value = u; showViewer() }
  })
}
