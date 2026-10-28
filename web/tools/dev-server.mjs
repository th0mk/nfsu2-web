// The test tools load the test pages (/test/) and the repo's test scripts
// (/tests/), which the server serves only when started with --dev:
//   out/web/server-rs/nfsu2-server --data "<game folder>" --http-port <http> --dev [--artifacts <dir>]
// requireDev(base) stops a tool early, with that hint, when the server at base
// answers without them.  (A server it cannot reach, or one behind a
// certificate node does not trust, is left to the browser to report.)
export async function requireDev(base) {
  let r;
  // under base's path: a server may serve the page under one (http://localhost:8615/u2)
  try { r = await fetch(new URL('test/hostcheck.html', base.replace(/\/*$/, '/')), { method: 'HEAD' }); } catch { return; }
  if (r.status === 404) {
    console.error(`${base} does not serve the test pages: start the server with --dev ` +
      '(out/web/server-rs/nfsu2-server --data "<game folder>" --http-port <http> --dev)');
    process.exit(2);
  }
}
