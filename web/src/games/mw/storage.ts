// Most Wanted's names in the browser's storage (web/src/games/profile.ts StorageNames): its own, beside Underground 2's
// on the origin both games share.  No imports: the service worker (sw.ts) takes these alone.
export const STORAGE = {
  OFFLINE_CACHE: 'nfsmw-offline-1',
  OFFLINE_PREFIX: 'nfsmw-offline-',
} as const;
