// The page side of runtime/web/host_abi.h: kinds the C runtime publishes with
// nfsu2_register(kind, ptr, size).  Subsystems attach to a running process:
//   gpu:   web/src/gpu/index.ts    attachRenderer(proc, canvas, opts)
//   audio: web/src/audio/index.ts  attachAudio(proc, opts)
//   input: web/src/input/index.ts  attachInput(proc, element, opts)
//   net:   web/src/net/remote.ts   attachNet(proc, opts)   (free roam: Net, LAN play: Lan; the link in net/worker.ts)
export const enum HostKind { Gpu = 1, Audio = 2, Input = 3, Net = 4, Status = 5, Vsync = 6, Lan = 7 }
export interface Registration { kind: HostKind; ptr: number; size: number }
