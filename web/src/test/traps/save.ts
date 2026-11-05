/** Keep an uncertain PUT separate from newer edits until its outcome is known. */
export class CatalogSave {
  private uncertain: string | null = null;
  constructor(private url: string, private etag: string, private saved: string, private request: typeof fetch = (input, init) => fetch(input, init)) {}
  get pending(): boolean { return this.uncertain !== null; }
  async write(text: string): Promise<void> {
    if (this.uncertain !== null) {
      const r = await this.request(this.url, { cache: 'no-store' });
      if (!r.ok) throw Error(`Could not reconcile save (${r.status}); retrying automatically.`);
      const disk = await r.text();
      if (disk !== this.uncertain && disk !== this.saved) throw new CatalogConflict();
      this.saved = disk; this.etag = r.headers.get('etag') ?? ''; this.uncertain = null;
    }
    if (text === this.saved) return;
    this.uncertain = text;
    const r = await this.request(this.url, { method: 'PUT', headers: { 'Content-Type': 'text/plain', 'If-Match': this.etag }, body: text });
    if (r.status === 412) { this.uncertain = null; throw new CatalogConflict(); }
    if (!r.ok) throw Error(`Save failed (${r.status}); retrying automatically.`);
    this.etag = r.headers.get('etag') ?? ''; this.saved = text; this.uncertain = null;
  }
}
export class CatalogConflict extends Error {
  constructor() { super('Catalog changed on disk. Close and reopen the editor to reload it; these edits were not saved.'); }
}
