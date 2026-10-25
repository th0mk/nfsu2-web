#!/bin/bash
# No game addresses in the runtime's core.  The files runtime/core.list names
# and every runtime/web/*.c are shared by every game, so a hex literal in the
# games' image range 0x401000-0xA79000 (SPEED2.EXE's image ends at 0x932000,
# speed.exe's at 0xA79000) is one game's address in shared code, unless
# tools/gamelit.allow lists it with a reason.  A game's addresses belong in
# its own files, which core reaches through runtime/game.h.
#
#   tools/gamelit.sh            exit 0 = none outside the allowlist
#   tools/gamelit.sh --all      also print the allowed ones
#   GAMELIT_ROOT=<dir> ...      check <dir>/runtime with <dir>/tools/gamelit.allow
#                               (the check's own test)
# Exit 1: a literal not on the allowlist (file:line), an allowlist line that
# matches nothing any more or has no reason, or a core.list file that is
# missing.  Exit 2: no runtime/core.list or no allowlist.
#
# Comments are skipped (a comment may name the address it explains); string
# literals are not (a message that prints a game address is game code).  A
# literal is matched by value: 0x5c7fee, 0x005C7FEEu and 0X5C7FEE are one.
# tools/gamelit.allow: "<file> <literal> <reason>", file relative to runtime/;
# one line covers every use of that literal in that file.
set -e -o pipefail
ROOT="${GAMELIT_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
ALL=0
case "${1:-}" in
  "") ;;
  --all) ALL=1 ;;
  *) echo "usage: tools/gamelit.sh [--all]"; exit 2 ;;
esac
LIST="$ROOT/runtime/core.list"; ALLOW="$ROOT/tools/gamelit.allow"
[ -f "$LIST" ] || { echo "gamelit: no $LIST"; exit 2; }
[ -f "$ALLOW" ] || { echo "gamelit: no $ALLOW"; exit 2; }
exec perl -e '
use strict; use warnings;
my ($root, $list, $allow, $all) = @ARGV;
my $bad = 0;
sub key { my $h = lc shift; $h =~ s/^0x0*//; $h eq "" ? "0" : $h }
# the files: core.list (relative to runtime/) and runtime/web/*.c
my @files;
open my $L, "<", $list or die "$list: $!";
while (<$L>) {
  s/#.*//; my ($f) = split " "; next unless defined $f;
  if (-f "$root/runtime/$f") { push @files, $f }
  else { print "runtime/core.list:$.: $f: no such file in runtime/\n"; $bad = 1 }
}
close $L;
opendir my $D, "$root/runtime/web" or die "runtime/web: $!";
push @files, map { "web/$_" } sort grep { /\.c$/ } readdir $D;
closedir $D;
# the allowlist
my (%ok, %used, %where);
open my $A, "<", $allow or die "$allow: $!";
while (<$A>) {
  chomp; next if /^\s*(#|$)/;
  my ($f, $lit, $why) = /^\s*(\S+)\s+(0[xX][0-9a-fA-F]+)[uUlL]*\s*(.*?)\s*$/;
  if (!defined $f) { print "tools/gamelit.allow:$.: not \"<file> <literal> <reason>\": $_\n"; $bad = 1; next }
  if ($why eq "") { print "tools/gamelit.allow:$.: $f $lit has no reason\n"; $bad = 1; next }
  my $k = "$f\t" . key($lit);
  if (exists $ok{$k}) { print "tools/gamelit.allow:$.: $f $lit is listed twice (line $where{$k} too)\n"; $bad = 1; next }
  $ok{$k} = $why; $where{$k} = $.;
}
close $A;
for my $f (@files) {
  open my $F, "<", "$root/runtime/$f" or die "runtime/$f: $!";
  local $/; my $t = <$F>; close $F;
  # strings and character literals stay; comments become blank (their newlines kept)
  $t =~ s{("(?:\\.|[^"\\\n])*")|(\x27(?:\\.|[^\x27\\\n])*\x27)|(/\*.*?\*/)|(//[^\n]*)}
         {defined $1 ? $1 : defined $2 ? $2 : do { (my $c = defined $3 ? $3 : $4) =~ s/[^\n]//g; $c }}gse;
  my $n = 0;
  for my $line (split /\n/, $t, -1) {
    $n++;
    while ($line =~ /(?<![\w.])(0[xX][0-9a-fA-F]+)[uUlL]*(?![\w.])/g) {
      my $lit = $1; my $k = key($lit);
      next if length($k) > 8;
      my $v = hex($k);
      next unless $v >= 0x401000 && $v < 0xA79000;
      my $id = "$f\t$k";
      if (exists $ok{$id}) { $used{$id} = 1; print "runtime/$f:$n: $lit (allowed: $ok{$id})\n" if $all; next }
      print "runtime/$f:$n: $lit: a game address in a core file (move it to the game, or list it in tools/gamelit.allow with a reason)\n";
      $bad = 1;
    }
  }
}
for my $id (sort { $where{$a} <=> $where{$b} } keys %ok) {
  next if $used{$id};
  my ($f, $k) = split /\t/, $id;
  print "tools/gamelit.allow:$where{$id}: $f 0x$k matches nothing (remove the line)\n"; $bad = 1;
}
exit $bad;
' "$ROOT" "$LIST" "$ALLOW" "$ALL"
