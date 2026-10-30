#!/bin/bash
# Suite check for "lang_menu" (tests/mw/scripts/lang_menu.txt with FILELIST=1, TRACKER M7-13): the module under test
# is named (L-020); the registry answered "Language" with one of the ten languages the game has text in
# (runtime/mw/registry_mw.c), and the game opened that language's text file, LANGUAGES\<NAME>.BIN, and no other
# language's (English US opens none of them: seen in its runs, where no LANGUAGES\ENGLISH.BIN, locale or subtitle
# file is opened at all); the title and the main menu came up; no wait ran out; the game left by WM_CLOSE.  Prints
# the language, its file and the page's language the runtime had no text for, if any.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -qE 'LOG lang_menu: title$' "$LOG" || fail "the title did not come up"
grep -qE 'LOG lang_menu: main menu$' "$LOG" || fail "the main menu did not come up"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
lang=$(grep -m1 -oE '^\[reg\] Software\\EA Games\\Need for Speed Most Wanted\\Language = "[^"]*"' "$LOG" | sed -E 's/.*= "(.*)"/\1/')
[ -n "$lang" ] || fail "the registry's Language was not read"
case "$lang" in
  "English US") want="" ;;
  French|German|Italian|Spanish|Dutch|Swedish|Danish|Polish|Finnish) want="LANGUAGES\\$(echo "$lang" | tr a-z A-Z).BIN" ;;
  *) fail "Language \"$lang\": not one of the ten the game has text in" ;;
esac
grep -qE '^  \[file\] CreateFileA\(' "$LOG" || fail "no file opens logged (FILELIST=1)"
# the language files opened: the text ones, not the textures, labels or the largest-strings file
texts=$(grep -oiE 'CreateFileA\("[^"]*LANGUAGES\\[A-Za-z]+\.BIN"\) -> "[^"]*"  OPEN' "$LOG" | grep -oiE 'LANGUAGES\\[A-Za-z]+\.BIN' \
  | tr a-z A-Z | grep -vE '\\(LANGUAGETEXTURES|LABELS|LARGEST)\.BIN$' | sort -u)
[ "$texts" = "$want" ] || fail "Language \"$lang\", but the text files opened were: ${texts:-none} (${want:-none} wanted)"
none=$(grep -m1 -oE '^\[reg\] language "[^"]*": the game has no text in it' "$LOG" | sed -E 's/.*language "([^"]*)".*/\1/')
note="" read="$want"
[ -n "$none" ] && note=" (the page's \"$none\": no text in the game)"
[ -n "$read" ] || read="no language's text file"
echo "Language \"$lang\"$note: $read read; the title and the main menu came up"
