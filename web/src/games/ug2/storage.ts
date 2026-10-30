// Underground 2's names in the browser's storage (web/src/games/profile.ts StorageNames): the ones it always had.
// No imports: the service worker (sw.ts) takes these alone.
export const STORAGE = {
  OFFLINE_CACHE: 'nfsu2-offline-2',
  OFFLINE_PREFIX: 'nfsu2-offline-',
  SAVE_DIR: 'save',
  CACHE_DIR: 'cache',
  PAGE_ID_FILE: 'nfsu2.page-id',
  SAVES_LOCK: 'nfsu2-saves:',
  INVITE_CHANNEL: 'nfsu2-invite:',
  RADIO_DB: 'nfsu2.radio',
  LOCAL: {
    /** the GPU pipelines a game made, made again at the next start (gpu/index.ts) */
    GPU_PIPELINES: 'nfsu2.gpu.pipelines.v1',
    /** the key the logs' names and addresses are scrubbed with (privacy.ts) */
    LOG_SALT: 'nfsu2.logsalt',
    /** the render check's answer for this device (device.ts) */
    RENDER_CHECK: 'nfsu2.device.render',
    /** the copy of the running game's log, the save folder after it (lastgame.ts) */
    LAST_GAME: 'nfsu2.lastgame.',
    /** the page's LAN id (page-id.ts) */
    PAGE_ID: 'nfsu2.net.page-id',
    /** the time played, and what of it the server has been told (playtime.ts) */
    PLAYED_MS: 'nfsu2.play.ms',
    SENT_MS: 'nfsu2.play.sms',
    /** the radio's folder modes, whether it was left on, its next tracks, shuffle (radio.ts) */
    RADIO_MODES: 'nfsu2.radio.modes',
    RADIO_ON: 'nfsu2.radio.on',
    RADIO_NEXT: 'nfsu2.radio.next',
    RADIO_SHUFFLE: 'nfsu2.radio.shuffle',
    /** the touch controls' steering, its levelling, their size, the menus' size (touch.ts) */
    TOUCH_STEERING: 'nfsu2-touch-steering',
    TOUCH_LEVEL: 'nfsu2-touch-level',
    TOUCH_SIZE: 'nfsu2-touch-size',
    MENU_SIZE: 'nfsu2-menu-size',
    /** a save folder's cloud-save link, and its games counted until Play offers cloud saves (vault-ui.ts) */
    KEYPHRASE_LINK: 'nfsu2-keyphrase:',
    CLOUD_OFFER: 'nfsu2-cloud-offer:',
    /** the online name: asked for a save folder, claimed, a claim tried (claim-ui.ts) */
    NAME_ASKED: 'nfsu2-name-asked:',
    NAME_CLAIMED: 'nfsu2-name-claimed:',
    CLAIM_TRIED: 'nfsu2-claim-tried:',
  },
  SESSION: {
    UPDATE: 'nfsu2-update',
    OTHER_BUILD: 'nfsu2-other-build',
  },
} as const;
