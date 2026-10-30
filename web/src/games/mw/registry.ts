// Most Wanted's registry.txt as a first launch writes it (shell.ts; settings.ini seed_registry): registry-defaults.txt
// beside this, every video option at the highest the game's Video options offer on this renderer (the same values as
// tests/mw/seed/registry_max.txt, which the graphics scripts run with: GAME.md 10.3, 10.8).  FirstTime = 1 has the game
// load these (LOAD_SETTINGS) instead of running its hardware detection, whose values are lower; g_PerformanceLevel 4
// keeps the sound at 44.1 kHz (GAME.md 10.3).  The game writes the file back as it changes its options.
export { default as REGISTRY_DEFAULTS } from './registry-defaults.txt';
