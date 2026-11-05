/** Original synthetic camera shutter: two damped mechanical clicks, no external audio asset. */
export function shutterSamples(rate: number): Float32Array<ArrayBuffer> {
  const pcm = new Float32Array(Math.ceil(rate * 0.18));
  let seed = 214013;
  for (let i = 0; i < pcm.length; i++) {
    const t = i / rate;
    seed = (Math.imul(seed, 1664525) + 1013904223) | 0;
    const noise = (seed >>> 0) / 2147483648 - 1;
    let sample = 0;
    for (const [start, level] of [[0, 0.24], [0.065, 0.17]]) {
      const dt = t - start;
      if (dt >= 0) sample += level * Math.exp(-dt * 100) * Math.min(dt * 2000, 1) *
        (noise * 0.7 + Math.sin(dt * 2 * Math.PI * 1700) * 0.3);
    }
    pcm[i] = sample;
  }
  return pcm;
}
