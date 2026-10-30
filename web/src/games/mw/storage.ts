// Most Wanted's names in the browser's storage (web/src/games/profile.ts StorageNames): its own, beside Underground 2's
// on the origin both games share (PLAN D-36: OPFS mw-save and mw-cache, keys nfsmw. and nfsmw-, lock nfsmw-saves).
// Each one's use is beside Underground 2's (src/games/ug2/storage.ts).  No imports: the service worker (sw.ts) takes
// these alone.
export const STORAGE = {
  OFFLINE_CACHE: 'nfsmw-offline-1',
  OFFLINE_PREFIX: 'nfsmw-offline-',
  SAVE_DIR: 'mw-save',
  CACHE_DIR: 'mw-cache',
  PAGE_ID_FILE: 'nfsmw.page-id',
  SAVES_LOCK: 'nfsmw-saves:',
  INVITE_CHANNEL: 'nfsmw-invite:',
  RADIO_DB: 'nfsmw.radio',
  LOCAL: {
    GPU_PIPELINES: 'nfsmw.gpu.pipelines.v1',
    LOG_SALT: 'nfsmw.logsalt',
    RENDER_CHECK: 'nfsmw.device.render',
    LAST_GAME: 'nfsmw.lastgame.',
    PAGE_ID: 'nfsmw.net.page-id',
    PLAYED_MS: 'nfsmw.play.ms',
    SENT_MS: 'nfsmw.play.sms',
    RADIO_MODES: 'nfsmw.radio.modes',
    RADIO_ON: 'nfsmw.radio.on',
    RADIO_NEXT: 'nfsmw.radio.next',
    RADIO_SHUFFLE: 'nfsmw.radio.shuffle',
    TOUCH_STEERING: 'nfsmw-touch-steering',
    TOUCH_LEVEL: 'nfsmw-touch-level',
    TOUCH_SIZE: 'nfsmw-touch-size',
    MENU_SIZE: 'nfsmw-menu-size',
    KEYPHRASE_LINK: 'nfsmw-keyphrase:',
    CLOUD_OFFER: 'nfsmw-cloud-offer:',
    NAME_ASKED: 'nfsmw-name-asked:',
    NAME_CLAIMED: 'nfsmw-name-claimed:',
    CLAIM_TRIED: 'nfsmw-claim-tried:',
  },
  SESSION: {
    UPDATE: 'nfsmw-update',
    OTHER_BUILD: 'nfsmw-other-build',
  },
} as const;
