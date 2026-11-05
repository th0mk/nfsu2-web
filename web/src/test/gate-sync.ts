// Loaded for every development game, independently of the gate editor UI.
import type { GameSession } from '../app/game';
export function attach(session: GameSession) {
  let stopped = false, pending = false, applied = '', generation = 0;
  let controller: AbortController | null = null;
  async function poll() {
    if (stopped || pending) return;
    pending = true; const epoch = generation; controller = new AbortController();
    try {
      const r = await fetch('/test/gates/markers', { cache: 'no-store', signal: controller.signal });
      if (!r.ok) return;
      const text = await r.text();
      if (stopped || epoch !== generation || text === applied) return;
      const points: unknown = JSON.parse(text);
      if (!Array.isArray(points) || points.length > 64 || points.some(p => !Array.isArray(p) || p.length !== 2 || p.some(v => typeof v !== 'number' || !Number.isFinite(v) || Math.abs(v) >= 100000))) return;
      // Input attachment may still be starting. Retry until every update is accepted.
      for (let i = 0; i < points.length; i++) {
        if (!session.input?.photo?.(33, i, Math.round(points[i][0] * 100)) || !session.input?.photo?.(34, i, Math.round(points[i][1] * 100))) return;
      }
      if (session.input?.photo?.(35, points.length)) applied = text;
    } catch { /* Temporary failure: preserve the last known markers and retry. */ }
    finally { pending = false; controller = null; if (!stopped && epoch !== generation) void poll(); }
  }
  const refresh = () => { generation++; controller?.abort(); void poll(); };
  const timer = setInterval(() => void poll(), 1000);
  window.addEventListener('gate-catalog-saved', refresh);
  void poll();
  const stop = () => { stopped = true; clearInterval(timer); controller?.abort(); window.removeEventListener('gate-catalog-saved', refresh); };
  void session.exited.then(stop);
  return stop;
}
