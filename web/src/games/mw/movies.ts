// Most Wanted's movies (web/src/games/profile.ts): their picture size, which the page asks the browser's AV1
// decoder about (host/movie.ts movieCodec).  No imports: the host's leaf modules take it alone.

/** MOVIES/*_english_ntsc.vp6: 1024x512 at 29.97 fps (GAME.md 8.1) */
export const MOVIE_SIZE = { w: 1024, h: 512 } as const;
