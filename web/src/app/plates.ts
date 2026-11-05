/** One shared plate text for this browser's cars. Only text is saved or sent. */
export const PLATE_MAX = 8;
export function plateText(s: string): string {
  return s.replace(/[a-z]/g, c => c.toUpperCase()).replace(/[^A-Z0-9]/g, '').slice(0, PLATE_MAX);
}
export function plateValid(s: string): boolean { return /^[A-Z0-9]{0,8}$/.test(s); }
export function plateInput(input: HTMLInputElement) {
  input.maxLength = PLATE_MAX;
  input.pattern = '[A-Za-z0-9]{0,8}';
  input.autocomplete = 'off'; input.spellcheck = false;
  input.addEventListener('input', () => {
    const at = input.selectionStart ?? input.value.length;
    const caret = plateText(input.value.slice(0, at)).length;
    input.value = plateText(input.value); input.setSelectionRange(caret, caret);
  });
}
