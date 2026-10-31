// What fails a game's run in the test runners (run-suite.mjs, input-suite.mjs, roam-pair.mjs): the lines the runtime
// and the page print when something went wrong.  Each runner adds its own checks (the exit code, its marker, results).
//   logWhy(log)                        -> [] or ['fault', 'wait-timeout', ...]
//   logWhy(log, { allow: ['freeze'] }) -> the same without those (a scenario that expects them)
// The log's lines as the runners keep them; a thread tag ([3] or [3!]) in front of a line is fine.

/** [why, the line that says it] */
export const LOG_RULES = [
  ['fault', /FAULT IN GUEST|GUEST MEMORY FAULT|HOST SEGV/],
  ['timeout', /TIME LIMIT REACHED/],
  ['trap', /^\[trap\]/m],
  ['unresolved-icall', /UNRESOLVED INDIRECT/],
  ['expect-failed', /^\[expect\] FAIL/m],
  ['wasm-trap', /^TRAP in thread/m],
  ['page-error', /^PAGEERROR|^WORKERERROR|^CRASH/m],
  ['unimplemented-import', /unimplemented import/],
  ['stack-overflow', /RangeError|Maximum call stack/],
  ['net-blocked', /^\[(net|roam)\] BLOCKED/m],
  // a script's wait that ran out (waitfng, waitmem, waitexpr, tapuntil ...): runtime/autotest.c goes on with the next
  // line, so a marker the script logs after it still comes although the step it waited for never did
  ['wait-timeout', /^\[autotest\] f=\d+ .*\bTIMEOUT\b/m],
  // a script line the runtime could not read (it leaves it out), or no script at all
  ['script-error', /^\[autotest\] (script line \d+:|cannot open input script)/m],
  // the stall watchdog (runtime/watchdog.c): no frame for 15 s (20 s while loading), also when the game went on later
  ['freeze', /^\[watchdog\] no Present for/m],
];

/** the log without the thread tags the test pages put in front of the game's lines */
export const untagged = log => log.replace(/^\[\d+!?\] /gm, '');

/** why the log fails the run ([] when it does not) */
export function logWhy(log, { allow = [] } = {}) {
  const l = untagged(log);
  return LOG_RULES.filter(([why, re]) => !allow.includes(why) && re.test(l)).map(([why]) => why);
}

/** the lines that made logWhy fail the run (to print beside its verdict) */
export function whyLines(log, { allow = [], max = 3 } = {}) {
  const res = LOG_RULES.filter(([why]) => !allow.includes(why)).map(([, re]) => new RegExp(re.source));
  return untagged(log).split('\n').filter(l => res.some(re => re.test(l))).slice(0, max);
}
