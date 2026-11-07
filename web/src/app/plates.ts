/** The license plate on the player's car (settings.ini license_plate, Settings, Online; the game takes it at Play):
 *  up to 8 capitals and digits, '' the game's own plate (runtime/plates.c).  Other players in free roam see it; one
 *  that reads as a slur is refused here, and blanked by every game and the server that see it. */
export const PLATE_MAX = 8;
/** small letters as capitals, the rest as it is (settings.ts refuses what is still not a plate) */
export const plateUpper = (s: string) => s.trim().replace(/[a-z]/g, c => c.toUpperCase());
/** capitals and digits, at most PLATE_MAX ('' too: the game's own) */
export const plateValid = (s: string) => /^[A-Z0-9]{0,8}$/.test(s);
/** a text box for a plate: at most PLATE_MAX characters, capitals on a phone's keyboard, small letters typed shown
 *  as capitals (the caret where it was), no spelling or autofill */
export function plateInput(input: HTMLInputElement) {
  input.maxLength = PLATE_MAX;
  input.autocapitalize = 'characters';
  input.autocomplete = 'off';
  input.spellcheck = false;
  input.addEventListener('input', () => {
    const up = input.value.replace(/[a-z]/g, c => c.toUpperCase());
    if (up === input.value) return;
    const at = input.selectionStart, end = input.selectionEnd;
    input.value = up;
    if (at !== null && end !== null) input.setSelectionRange(at, end);
  });
}
