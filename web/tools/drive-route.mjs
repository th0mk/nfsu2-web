// Browser-test driver: read the game's trace and operate normal keyboard input.
// Never writes the car's position, velocity, collision state, or streaming state.
export function driveRoute(page, points, log) {
  let index = 0, previous, busy = false, latest, stopped = false;
  const held = new Set();
  async function keys(wanted) {
    for (const key of [...held]) if (!wanted.has(key)) {
      await page.keyboard.up(key); held.delete(key);
    }
    for (const key of wanted) if (!held.has(key)) {
      await page.keyboard.down(key); held.add(key);
    }
  }
  async function update() {
    busy = true;
    try {
      while (latest && !stopped) {
        const state = latest; latest = null;
        const { px, py, fx, fy, t } = state;
        const dt = previous ? (t - previous.t) / 1000 : 0;
        const speed = dt > 0 ? Math.hypot(px - previous.px, py - previous.py) / dt : 0;
        previous = state;
        while (index < points.length && Math.hypot(points[index][0] - px, points[index][1] - py) < 7) {
          log(`[route] reached ${index + 1}/${points.length} at ${px.toFixed(1)},${py.toFixed(1)}`);
          index++;
        }
        if (index === points.length) { stopped = true; await keys(new Set()); break; }
        const [x, y] = points[index];
        const target = Math.atan2(y - py, x - px), heading = Math.atan2(fy, fx);
        const angle = Math.atan2(Math.sin(target - heading), Math.cos(target - heading));
        const wanted = new Set();
        // Slow enough to follow bends without bouncing off the road's edges.
        const limit = Math.abs(angle) > 0.3 ? 7 : 12;
        if (speed < limit) wanted.add('ArrowUp');
        if (speed > limit + 2) wanted.add('ArrowDown');
        if (angle > 0.045) wanted.add('ArrowLeft');
        if (angle < -0.045) wanted.add('ArrowRight');
        await keys(wanted);
      }
    } catch (error) {
      stopped = true;
      log(`[route] failed: ${error}`);
      await keys(new Set()).catch(() => {});
    } finally { busy = false; }
  }
  return text => {
    if (stopped || !text.includes('[trace]')) return;
    const state = Object.fromEntries([...text.matchAll(/\b(t|px|py|fx|fy)=(-?(?:\d+(?:\.\d*)?|\.\d+)(?:e[+-]?\d+)?)/g)].map(m => [m[1], Number(m[2])]));
    if (!['t', 'px', 'py', 'fx', 'fy'].every(k => Number.isFinite(state[k]))) return;
    latest = state;
    if (!busy) void update();
  };
}
