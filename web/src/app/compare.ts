// Two pictures of one moment to compare: the second over the first from a line the player drags (or moves with the
// arrow keys), as Play's Full Scene Anti-Aliasing question shows the car's edges off and on (shell.ts askFsaa).  The
// line's place is a CSS variable set through the CSSOM: the page's CSP takes no inline style attributes.

/** a picture pair shown: its element, and dispose() for the object URLs once it is gone */
export interface Compare { el: HTMLElement; dispose(): void }

/** two pictures of the server's game folder (it sends its files as bytes, whatever their names: given `type` here), or
 *  null when either is missing, unreadable, or the server does not answer in `ms` (offline, a server without them) */
export async function fetchPair(a: string, b: string, type: string, ms = 3000): Promise<[Blob, Blob] | null> {
  const ctl = new AbortController(), timer = setTimeout(() => ctl.abort(), ms);
  const one = async (url: string) => {
    const r = await fetch(url, { signal: ctl.signal });
    if (!r.ok) throw new Error(`${url}: ${r.status}`);
    return new Blob([await r.arrayBuffer()], { type });
  };
  try { return await Promise.all([one(a), one(b)]); } catch { return null; } finally { clearTimeout(timer); }
}

/** the pair, `before` on the left of the line and `after` on its right, the line in the middle */
export function compare(before: Blob, after: Blob, text: { before: string; after: string; drag: string }): Compare {
  const urls = [URL.createObjectURL(before), URL.createObjectURL(after)];
  const el = document.createElement('div');
  el.className = 'compare';
  const img = (src: string, cls: string) => Object.assign(document.createElement('img'), { src, alt: '', className: cls, draggable: false });
  const tag = (s: string, cls: string) => Object.assign(document.createElement('span'), { textContent: s, className: `compare-tag ${cls}` });
  const split = Object.assign(document.createElement('span'), { className: 'compare-split' });
  const range = Object.assign(document.createElement('input'), { type: 'range', min: '0', max: '100', value: '50' });
  range.setAttribute('aria-label', text.drag);
  const place = () => el.style.setProperty('--x', `${range.value}%`);
  range.addEventListener('input', place);
  place();
  el.append(img(urls[0], 'compare-before'), img(urls[1], 'compare-after'), split, tag(text.before, 'before'), tag(text.after, 'after'), range);
  return { el, dispose: () => { for (const u of urls) URL.revokeObjectURL(u); } };
}
