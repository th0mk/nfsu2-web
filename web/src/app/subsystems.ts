// The one place the app shell gets its subsystems from: the real modules,
// adapted to the shell's contract (./contract.ts).
//   renderer  web/src/gpu    WebGPU, draws into the shell's canvas
//   audio     web/src/audio  AudioWorklet, plays into the context made in the Play click
//   input     web/src/input  keyboard, mouse, Gamepad API, focus and visibility
//   net       web/src/net    the server's virtual LAN, only with free roam on
// Each module's own handle has detach(); the adapters pass the options the
// shell owns and pick the counters the detailed overlay shows.
import { attachRenderer as gpuAttach } from '../gpu/index';
export { OtherBuild } from '../gpu/index';
import { attachAudio as audioAttach } from '../audio/index';
import { attachInput as inputAttach } from '../input/index';
import { attachNet as netAttach } from '../net/index';
import type { AttachAudio, AttachInput, AttachNet, AttachRenderer } from './contract';

export type {
  Subsystem, RendererSubsystem, AudioSubsystem, InputSubsystem, NetSubsystem,
  RendererOptions, AudioOptions, InputOptions, NetOptions, CommonOptions,
} from './contract';

export const attachRenderer: AttachRenderer = async (proc, canvas, o) => {
  const r = await gpuAttach(proc, canvas, { log: o.log, artifacts: o.artifacts, dumpScale: o.dumpScale, onLost: o.onLost, onMarks: o.onMarks });
  return {
    detach: () => r.detach(),
    imageRect: () => r.imageRect(),
    dump: (name: string) => r.dump(name),
    doneMs: () => r.doneMs(),
    textures: () => { const s = r.stats(); return { bytes: s.texBytes, unpacked: s.unpackBytes, astc: s.astcBytes }; },
    stats: () => {
      const s = r.stats();
      return { 'gpu encode ms': s.encodeMsAvg, 'draws': s.drawsLast, 'passes': s.passesLast, 'pipelines': s.pipelines, 'gpu errors': s.errors };
    },
  };
};

export const attachAudio: AttachAudio = async (proc, o) => {
  const a = await audioAttach(proc, { context: o.context, log: o.log });
  return { detach: () => a.detach(), stats: () => a.stats(), state: () => a.context?.state ?? 'closed' };
};

export const attachInput: AttachInput = (proc, element, o) =>
  inputAttach(proc, element, { log: o.log, viewRect: o.viewRect, toggleFullscreen: o.toggleFullscreen, padPage: o.padPage });

export const attachNet: AttachNet = (proc, o) => {
  const link = netAttach(proc, { name: o.name, id: o.id, claim: o.claim, block: o.block, log: o.log, onNotice: o.notice, onLobby: o.lobby, onSay: o.say, onBack: o.back,
    onQueue: o.queue, onRemoved: o.removed });
  const friends = () => link.peers.filter(p => !p.self);
  return {
    detach: () => link.close(),
    vip: () => link.vip,
    peers: () => friends().map(p => `${p.name} (${p.vip})`),
    holding: () => link.state === 'connecting' || link.state === 'online' || link.state === 'offline' || link.state === 'queued',
    away: (on: boolean) => link.setAway(on),
    offline: () => link.goOffline(),
    solid: (on: boolean) => link.setSolid(on),
    say: (p: number) => link.say(p),
    crews: (hashes: readonly number[], rgbs: readonly number[]) => link.setCrews(hashes, rgbs),
    stats: () => {
      const s = link.stats();
      return { 'lan online': s.state === 'online' ? 1 : 0, 'lan friends': friends().length, 'lan rtt ms': s.switchRttMs };
    },
  };
};
