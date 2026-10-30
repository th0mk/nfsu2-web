// Most Wanted's settings rows (src/app/settings-ui.ts): the help texts it words its own way, where the panel's say what
// Underground 2's files do (its language table, boot movies, frame rate in Online and LAN races and cutscenes, and its
// own rumble).  The rows themselves are the keys its template lists (template.ts).  What each says: the language,
// runtime/mw/registry_mw.c; the startup movies, runtime/mw/intro_mw.c; the frame rate, runtime/mw/framerate_mw.c.
import { N } from '../../app/i18n';

/** a row's help text where the game words it its own way, by settings.ini key */
export const HELP: Readonly<Record<string, string>> = {
  language: N('The game\'s menus and text in English, French, German, Italian, Spanish, Dutch, Swedish, Danish or Polish (in the other languages the game stays in English), and this page.'),
  intro: N('The EA logo, the PSA and the attract movie. Skippable: Enter, Esc, Space, a left click, or A, B or Start ends each one. Original: as in the game, where the EA logo plays to its end.'),
  fps: N('Auto follows the display (120 on a 120 Hz screen, 240 on a 240 Hz one). 30 is steadier where 60 stutters, and easier on the battery.'),
  background: N('Pause freezes the game and its sound, as alt-tab does on Windows.'),
  rumble: N('The game\'s own force feedback, on controllers that have rumble.'),
};
