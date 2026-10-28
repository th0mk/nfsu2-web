// G-01, clean logs (PLAN G-01): the markers a scripted run's log must not have, in one place for the test tools.
//   import { g01, readReviewed } from './g01.mjs';
//   g01(log, game)  the reasons the log fails G-01, [] when it is clean
// The suite's markers, named as web/tools/run-suite.mjs has always named them: fault (FAULT IN GUEST, GUEST MEMORY
// FAULT, HOST SEGV), timeout (TIME LIMIT REACHED), trap ([trap]), unresolved-icall (UNRESOLVED INDIRECT),
// expect-failed ([expect] FAIL), net-blocked ([net] / [roam] BLOCKED).  And the ones Most Wanted added, which hold for
// every game's runs:
//   cxx-throw(TYPE)        a C++ throw stop: RaiseException 0xE06D7363 (runtime/win32.c stops the process), with the
//                          type it names, ".?AVlength_error@std@@" read as "class std::length_error"
//   genexec-bail(N)        N calls into generated code (the sound banks' code, GAME.md 7.3) that runtime/genexec.c
//                          could not run (its report: "N FAILED")
//   unimplemented(IMPORTS) an "[hle] UNIMPLEMENTED dll!name" line (runtime/autotest.c) for an import the game's
//                          reviewed list (games/<id>/hle-reviewed.txt) does not mark "yes"; a game with no list has
//                          none reviewed
//   watchdog-stall         a stall report (runtime/watchdog.c): no Present for 15 s, 20 s while loading
//   init-count             the boot's [init] line (runtime/dispatch.c) with a table not all run or its check NULL
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

/** games/<game>/hle-reviewed.txt: "dll!name" (dll in lower case) -> { yes: every row of it says yes, runs } */
export function readReviewed(game, file = path.join(ROOT, 'games', game, 'hle-reviewed.txt')) {
  const out = new Map();
  if (!fs.existsSync(file)) return out;
  for (const raw of fs.readFileSync(file, 'utf8').split('\n')) {
    const l = raw.replace(/#.*/, '').trim();
    if (!l) continue;
    const [, imp, , mark, ...run] = l.split(/\s+/);
    const m = /^([^!]+)!(.+)$/.exec(imp ?? '');
    if (!m || !/^(yes|no)$/.test(mark ?? '')) throw new Error(`${file}: not "<module> <dll>!<name> <IAT> <yes|no> [<run>]": ${raw}`);
    const k = `${m[1].toLowerCase()}!${m[2]}`, e = out.get(k) ?? { yes: true, runs: [] };
    e.yes &&= mark === 'yes';
    if (run.length) e.runs.push(run.join(' '));
    out.set(k, e);
  }
  return out;
}

/** an MSVC type descriptor's name as C++ writes it: ".?AVlength_error@std@@" -> "class std::length_error" */
export function typeName(d) {
  const m = /^\.\?A([VUT])([^?$]*)@@$/.exec(d);
  if (!m) return d;
  return `${{ V: 'class', U: 'struct', T: 'union' }[m[1]]} ${m[2].split('@').reverse().join('::')}`;
}

/** the reasons log fails G-01, in a fixed order; reviewed: readReviewed(game) */
export function g01(log, game, reviewed = readReviewed(game)) {
  const why = [];
  const has = re => re.test(log);
  if (has(/FAULT IN GUEST|GUEST MEMORY FAULT|HOST SEGV/)) why.push('fault');
  if (log.includes('TIME LIMIT REACHED')) why.push('timeout');
  if (has(/^\[trap\]/m)) why.push('trap');
  if (log.includes('UNRESOLVED INDIRECT')) why.push('unresolved-icall');
  if (has(/^\[expect\] FAIL/m)) why.push('expect-failed');
  if (has(/^\[(net|roam)\] BLOCKED/m)) why.push('net-blocked');
  // a C++ throw stop, and the type the next lines of its report name
  for (const m of log.matchAll(/GUEST RAISEEXCEPTION: code 0xe06d7363\b/gi)) {
    const next = log.slice(m.index).split('\n').slice(1, 16).join('\n');
    const t = /thrown type (\S+)/.exec(next);
    why.push(`cxx-throw(${t ? typeName(t[1]) : /rethrow \(throw;\)/.test(next) ? 'rethrow' : 'type not known'})`);
    break;
  }
  const bail = /generated guest code: \d+ calls, \d+ instructions, (\d+) FAILED/.exec(log);
  if (bail) why.push(`genexec-bail(${bail[1]})`);
  const off = [...new Set([...log.matchAll(/\[hle\] UNIMPLEMENTED ([^!\s]+)!(\S+)/g)]
    .filter(m => !reviewed.get(`${m[1].toLowerCase()}!${m[2]}`)?.yes).map(m => `${m[1]}!${m[2]}`))];
  if (off.length) why.push(`unimplemented(${off.join(',')})`);
  if (has(/\[watchdog\] no Present for [\d.]+ s/)) why.push('watchdog-stall');
  if ([...log.matchAll(/^\[init\] (.*)$/mg)].some(l => [...l[1].matchAll(/(\d+) of (\d+) /g)].some(m => m[1] !== m[2])
      || /stopped:|not run|not finished|: NULL/.test(l[1]))) why.push('init-count');
  return why;
}
