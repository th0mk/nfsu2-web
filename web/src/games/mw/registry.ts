// Most Wanted's registry.txt as a first launch writes it (shell.ts; settings.ini seed_registry): registry-defaults.txt
// beside this, every video option at the highest the game's Video options offer on this renderer (the same values as
// tests/mw/seed/registry_max.txt, which the graphics scripts run with: GAME.md 10.3, 10.8).  FirstTime = 1 has the game
// load these (LOAD_SETTINGS) instead of running its hardware detection, whose values are lower; g_PerformanceLevel 4
// keeps the sound at 44.1 kHz (GAME.md 10.3).  The game writes the file back as it changes its options.
export { default as REGISTRY_DEFAULTS } from './registry-defaults.txt';

/** a phone's graphics (app/phonegraphics.ts, TRACKER M6-58): only options Most Wanted reads (GAME.md 10.3; it has no
 *  depth of field or bleach bypass, Underground 2's other two), the same choice as Underground 2's phones: the car's
 *  environment map (its cube's faces drawn again as the car moves) and the road's reflection (the world drawn a second
 *  time, view 4) off, and motion blur and overbright (full-screen passes) off.  Measured over race_perf.txt's 1800
 *  frames (one run each, 1280x720): every option at its highest 2,338 draws and 27 render passes a frame, these four
 *  off 1,216 and 9 (the environment map alone off: 1,166 and 18).  Rain, particles, the world's and the car's detail,
 *  shadows and the visual treatment (5 more passes) stay; the game's Options, Display raise them again. */
export const PHONE_GRAPHICS: Readonly<Record<string, number>> = {
  g_CarEnvironmentMapEnable: 0, g_RoadReflectionEnable: 0, g_MotionBlurEnable: 0, g_OverBrightEnable: 0,
};
