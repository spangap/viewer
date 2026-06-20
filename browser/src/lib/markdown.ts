/**
 * markdown — a small Markdown → HTML converter for the local viewer.
 *
 * Mirrors the LCD viewer's subset (headings, bold/italic, inline + fenced code,
 * unordered/ordered lists, blockquote, hr, links, paragraphs) so a document
 * reads the same on the device and in the browser — just with the browser's
 * fonts. Dependency-free.
 *
 * Output is sanitized: the input is HTML-escaped first, so only the tags this
 * file emits can appear (no raw HTML / scripts from the Markdown). Links to
 * http(s) get target="_blank" so they open in the user's own browser tab;
 * local/relative links are left for the viewer to resolve in-window.
 */

function esc(s: string): string {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;')
          .replace(/>/g, '&gt;').replace(/"/g, '&quot;')
}

/* Inline spans on an already-escaped string: `code`, [text](url), **bold**, *italic*. */
function inline(s: string): string {
  let out = ''
  const parts = s.split('`')          /* odd indices = inline code spans */
  for (let i = 0; i < parts.length; i++) {
    if (i % 2 === 1) { out += '<code>' + parts[i] + '</code>'; continue }
    let t = parts[i]
    t = t.replace(/\[([^\]]+)\]\(([^)\s]+)\)/g, (_m, txt, url) => {
      const ext = /^https?:\/\//i.test(url)
      const attr = ext ? ' target="_blank" rel="noopener"' : ' class="local"'
      return `<a href="${url}"${attr}>${txt}</a>`
    })
    t = t.replace(/(\*\*|__)(.+?)\1/g, '<strong>$2</strong>')
    t = t.replace(/(\*|_)(.+?)\1/g, '<em>$2</em>')
    out += t
  }
  return out
}

export function markdownToHtml(md: string): string {
  const lines = md.replace(/\r\n?/g, '\n').split('\n')
  const out: string[] = []
  let para: string[] = []
  let list: 'ul' | 'ol' | null = null

  const flushPara = () => {
    if (para.length) { out.push('<p>' + inline(esc(para.join(' '))) + '</p>'); para = [] }
  }
  const closeList = () => { if (list) { out.push('</' + list + '>'); list = null } }

  for (let i = 0; i < lines.length; i++) {
    const raw = lines[i]
    const t = raw.trim()

    if (/^```/.test(t)) {                       /* fenced code block */
      flushPara(); closeList()
      const body: string[] = []
      i++
      while (i < lines.length && !/^```/.test(lines[i].trim())) { body.push(lines[i]); i++ }
      out.push('<pre>' + esc(body.join('\n')) + '</pre>')
      continue
    }
    if (t === '') { flushPara(); closeList(); continue }
    if (/^(-{3,}|\*{3,}|_{3,})$/.test(t)) { flushPara(); closeList(); out.push('<hr>'); continue }

    const h = /^(#{1,6})\s+(.*)$/.exec(t)
    if (h) { flushPara(); closeList(); const n = h[1].length
             out.push(`<h${n}>` + inline(esc(h[2])) + `</h${n}>`); continue }

    if (/^>\s?/.test(t)) {
      flushPara(); closeList()
      out.push('<blockquote>' + inline(esc(t.replace(/^>\s?/, ''))) + '</blockquote>')
      continue
    }

    const ul = /^[-*+]\s+(.*)$/.exec(t)
    const ol = /^\d+\.\s+(.*)$/.exec(t)
    if (ul || ol) {
      flushPara()
      const want = ul ? 'ul' : 'ol'
      if (list && list !== want) closeList()
      if (!list) { out.push('<' + want + '>'); list = want }
      out.push('<li>' + inline(esc(ul ? ul[1] : ol![1])) + '</li>')
      continue
    }

    closeList()
    para.push(t)
  }
  flushPara(); closeList()
  return out.join('\n')
}
