// Quick chat's phrases (quickchat.ts) and the groups the player puts sixteen of them in (settings.ini quick_chat_phrases,
// quick_chat_groups; the start page's Your phrases row).  A phrase on the wire is its number here, its place in
// PHRASES: the numbers are the protocol, so a phrase is only ever added at the end, never moved or removed (a game
// with fewer shows nothing for a number it does not have).  The first sixteen were the only ones before, and were
// everyone's default until the countdown took One more?'s place (2026-11-03).  Pure (no DOM): settings.ts checks the
// settings with it.
import { N } from './i18n';

/** the library's categories: also the names a group can take (each translated) */
export const CATS = [
  { id: 'hi', name: N('Hi') },
  { id: 'reply', name: N('Reply') },
  { id: 'thanks', name: N('Thanks') },
  { id: 'nice', name: N('Nice') },
  { id: 'drive', name: N('Drive') },
  { id: 'meet', name: N('Meet') },
  { id: 'race', name: N('Race') },
  { id: 'react', name: N('React') },
  { id: 'help', name: N('Help') },
] as const;
export type CatId = typeof CATS[number]['id'];

/** every phrase, in the order of its number, with its category; special: not said as words (quickchat.ts) */
export const PHRASES: readonly { text: string; cat: CatId; special?: 'countdown' }[] = ([
  // 0-15: the first sixteen, the default layout
  [N('Hey!'), 'hi'], [N('Nice car!'), 'nice'], [N('Thanks!'), 'thanks'], [N('Bye!'), 'hi'],
  [N('Yes'), 'reply'], [N('No'), 'reply'], [N('Sorry!'), 'thanks'], [N('lol'), 'reply'],
  [N('Follow me!'), 'drive'], [N('Wait up!'), 'drive'], [N('Let\'s go!'), 'drive'], [N('Nice!'), 'nice'],
  [N('Race?'), 'race'], [N('Join my race!'), 'race'], [N('gg'), 'race'], [N('One more?'), 'race'],
  // 16-27
  [N('Yo!'), 'hi'], [N('Hi all!'), 'hi'], [N('Welcome!'), 'hi'], [N('Welcome back!'), 'hi'], [N('Long time no see!'), 'hi'],
  [N('Good luck!'), 'hi'], [N('Have fun!'), 'hi'], [N('Gotta go!'), 'hi'], [N('See you around!'), 'hi'], [N('Good night!'), 'hi'],
  [N('brb'), 'hi'], [N('Back!'), 'hi'],
  // 28-40
  [N('Sure!'), 'reply'], [N('Maybe'), 'reply'], [N('Okay'), 'reply'], [N('Nope'), 'reply'], [N('One sec'), 'reply'],
  [N('On my way!'), 'reply'], [N('Can\'t right now'), 'reply'], [N('Same'), 'reply'], [N('True'), 'reply'], [N('What?'), 'reply'],
  [N('Where?'), 'reply'], [N('Here!'), 'reply'], [N('haha'), 'reply'],
  // 41-49
  [N('ty'), 'thanks'], [N('np'), 'thanks'], [N('No worries!'), 'thanks'], [N('My bad!'), 'thanks'], [N('All good!'), 'thanks'],
  [N('Didn\'t see you!'), 'thanks'], [N('Lag, sorry!'), 'thanks'], [N('Thanks for waiting!'), 'thanks'], [N('Thanks for the race!'), 'thanks'],
  // 50-65
  [N('Clean build!'), 'nice'], [N('Love the vinyl!'), 'nice'], [N('Love the colour!'), 'nice'], [N('Sick rims!'), 'nice'],
  [N('Nice kit!'), 'nice'], [N('Sleeper!'), 'nice'], [N('What\'s under the hood?'), 'nice'], [N('Nice drift!'), 'nice'],
  [N('Nice line!'), 'nice'], [N('Smooth!'), 'nice'], [N('Nice save!'), 'nice'], [N('Great driving!'), 'nice'],
  [N('You\'re fast!'), 'nice'], [N('Respect.'), 'nice'], [N('Nice driving!'), 'nice'], [N('What a save!'), 'nice'],
  // 66-83
  [N('Let\'s cruise!'), 'drive'], [N('Convoy!'), 'drive'], [N('Lead the way!'), 'drive'], [N('I\'ll lead!'), 'drive'],
  [N('This way!'), 'drive'], [N('Turn around!'), 'drive'], [N('Slow down!'), 'drive'], [N('Speed up!'), 'drive'],
  [N('Shortcut here!'), 'drive'], [N('Jump here!'), 'drive'], [N('Watch out!'), 'drive'], [N('Traffic!'), 'drive'],
  [N('Behind you!'), 'drive'], [N('Lost you!'), 'drive'], [N('Where are you?'), 'drive'], [N('Check the map!'), 'drive'],
  [N('Pull over!'), 'drive'], [N('Send it!'), 'drive'],
  // 84-98
  [N('Car meet?'), 'meet'], [N('Meet here!'), 'meet'], [N('Park up!'), 'meet'], [N('Line up!'), 'meet'], [N('Photo time!'), 'meet'],
  [N('Hold still!'), 'meet'], [N('Show your build!'), 'meet'], [N('Going to the shop'), 'meet'], [N('Going to the garage'), 'meet'],
  [N('Join my crew?'), 'meet'], [N('Meet at City Core?'), 'meet'], [N('Meet at Beacon Hill?'), 'meet'],
  [N('Meet at Jackson Heights?'), 'meet'], [N('Meet at Coal Harbor?'), 'meet'], [N('Meet at the airport?'), 'meet'],
  // 99-117
  [N('Outrun?'), 'race'], [N('Sprint?'), 'race'], [N('Circuit?'), 'race'], [N('Drag race?'), 'race'], [N('Drift battle?'), 'race'],
  [N('Street X?'), 'race'], [N('You\'re on!'), 'race'], [N('Ready?'), 'race'], [N('Ready!'), 'race'], [N('Not ready!'), 'race'],
  [N('Wait for everyone!'), 'race'], [N('Close one!'), 'race'], [N('So close!'), 'race'], [N('Good race!'), 'race'],
  [N('Run it back?'), 'race'], [N('Best of three?'), 'race'], [N('You got me!'), 'race'], [N('Too fast for me!'), 'race'],
  [N('I\'m out'), 'race'],
  // 118-141
  [N('Wow!'), 'react'], [N('Whoa!'), 'react'], [N('No way!'), 'react'], [N('Yes!'), 'react'], [N('Let\'s gooo!'), 'react'],
  [N('Nooo!'), 'react'], [N('Oops!'), 'react'], [N('Ouch!'), 'react'], [N('Oof'), 'react'], [N('Yikes!'), 'react'],
  [N('Whew!'), 'react'], [N('omg'), 'react'], [N('What a jump!'), 'react'], [N('Huge air!'), 'react'], [N('Clean!'), 'react'],
  [N('Unreal!'), 'react'], [N('Calculated.'), 'react'], [N('Classic.'), 'react'], [N('Did you see that?'), 'react'],
  [N('W'), 'react'], [N('Let him cook'), 'react'], [N('I\'m cooked'), 'react'], [N('skill issue'), 'react'], [N('ratio'), 'react'],
  // 142-151
  [N('I\'m stuck!'), 'help'], [N('Help?'), 'help'], [N('Lag!'), 'help'], [N('AFK a sec'), 'help'], [N('Out of nitrous'), 'help'],
  [N('Tuning tips?'), 'help'], [N('Which car is that?'), 'help'], [N('New here!'), 'help'], [N('First time?'), 'help'],
  [N('On my phone!'), 'help'],
  // 152: special, a countdown for the players near (5 s, then GO!: quickchat.ts; roam_game.c SAY_COUNTDOWN)
  [N('Countdown'), 'race', 'countdown'],
] as [string, CatId, 'countdown'?][]).map(([text, cat, special]) => special ? { text, cat, special } : { text, cat });

/** the countdown's number */
export const COUNTDOWN = 152;

/** the slots: four groups of four */
export const SLOTS = 16;
/** the default: the first sixteen, in the groups they always had, but the countdown in the last place (One more?'s) */
export const DEFAULT_PHRASES: readonly number[] = [...Array.from({ length: SLOTS - 1 }, (_, i) => i), COUNTDOWN];
export const DEFAULT_GROUPS: readonly CatId[] = ['hi', 'reply', 'drive', 'race'];

const CAT_IDS = new Set<string>(CATS.map(c => c.id));
const nums = (v: string) => v.split(/[, \t]+/).filter(Boolean);

/** a quick_chat_phrases value: sixteen phrase numbers, separated by commas */
export function phrasesOk(v: string): boolean {
  const n = nums(v);
  return n.length === SLOTS && n.every(x => /^\d{1,3}$/.test(x) && +x < PHRASES.length);
}
/** a quick_chat_groups value: four category names, separated by commas */
export function groupsOk(v: string): boolean {
  const n = nums(v.toLowerCase());
  return n.length === 4 && n.every(x => CAT_IDS.has(x));
}
/** the player's sixteen (settings.ini's value; missing or refused: the default) */
export function phrasesOf(v: string | undefined): number[] {
  return v && phrasesOk(v) ? nums(v).map(Number) : [...DEFAULT_PHRASES];
}
export function groupsOf(v: string | undefined): CatId[] {
  return v && groupsOk(v) ? nums(v.toLowerCase()) as CatId[] : [...DEFAULT_GROUPS];
}
export function catName(id: string): string { return CATS.find(c => c.id === id)?.name ?? id; }
