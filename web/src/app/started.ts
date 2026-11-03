// main.js has started.  main.ts imports this first, so it runs before anything else of the page's: boot.ts then
// never says the game is blocked, or takes it back if it said so while main.js was still on its way.
document.documentElement.dataset.started = '';
dispatchEvent(new Event('nfsu2-started'));
export {};
