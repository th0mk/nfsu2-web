// Underground 2's names in the browser's storage (web/src/games/profile.ts StorageNames): the ones it always had.
// No imports: the service worker (sw.ts) takes these alone.
export const STORAGE = {
  OFFLINE_CACHE: 'nfsu2-offline-2',
  OFFLINE_PREFIX: 'nfsu2-offline-',
} as const;
