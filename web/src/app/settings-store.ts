// settings.ini, read and written for the whole page (the settings panel, settings-ui.ts; Play, the menu and invite
// links, shell.ts), which keeps what it read and wrote here.  It keeps two views of it:
//  - text: the file as last read or written (what the panel shows and changes)
//  - forGame: the file as this tab's game goes by it, null until this tab read it as the owner of the saves (load) or
//    wrote it.  The panel reading again (read) leaves it as it was: a tab that does not own the saves keeps the
//    defaults for its game, and a file gone from under a running game (the browser deleting the site's data) is still
//    known for putting back (shell.ts saveGone).
// A read that does not work (savefs.ts missing) throws and leaves both views as they were, so nothing is written over
// a file that did not read.
import { readText, writeFile } from './savefs';
import { settingsUpgrade, TEMPLATE } from './settings';

const FILE = 'settings.ini';

export class SettingsFile {
  private shown: string | null = null;
  private played: string | null = null;
  /** dir: the save folder (it may change before the page reads it: a test page's address) */
  constructor(private readonly dir: () => string, private readonly log: (s: string) => void) {}

  /** the file as last read or written here (null: not read yet, or there is none) */
  get text(): string | null { return this.shown; }
  /** the file as this tab's game goes by it (null: not read by the owner of the saves yet, nor written) */
  get forGame(): string | null { return this.played; }

  /** the file as it is now, kept nowhere; throws where it did not read */
  peek(): Promise<string | null> { return readText(this.dir(), FILE); }

  /** read again, as the panel shows it; throws where it did not read */
  async read(): Promise<string | null> {
    this.shown = await this.peek();
    return this.shown;
  }

  /** read for the game; in the tab that owns the saves (owner) the template written where there is none, and the new
   *  options added (commented out) where it lacks them.  Throws where it did not read, and then writes nothing
   *  (savefs.ts missing: Firefox's "UnknownError" was taken for no file, and the template went over the player's) */
  async load(owner: boolean): Promise<string | null> {
    let text = await this.peek();
    if (text === null && owner) {
      text = TEMPLATE;
      await writeFile(this.dir(), FILE, text);
      this.log(`[app] settings: wrote template /save/settings.ini`);
    } else if (text !== null && owner) {
      const up = settingsUpgrade(text);
      if (up) { text = up.text; await writeFile(this.dir(), FILE, text); this.log(`[app] settings: added the new options ${up.added.join(', ')} to /save/settings.ini (commented out, defaults unchanged)`); }
    }
    this.played = text;
    return text;
  }

  /** the whole file written: both views have it.  It gives what forGame was before (shell.ts: another language or pack
   *  counts other game data) */
  async write(text: string): Promise<string | null> {
    await writeFile(this.dir(), FILE, text);
    const was = this.played;
    this.shown = this.played = text;
    return was;
  }

  /** the file changed by edit, on top of the file as it is now (the template where there is none): never on top of a
   *  view alone, which is null where the file did not read.  Throws where it did not read or write (nothing written
   *  then). */
  async edit(edit: (text: string) => string): Promise<string> {
    const text = edit(await this.peek() ?? TEMPLATE);
    await this.write(text);
    return text;
  }
}
