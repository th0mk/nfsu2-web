// Underground 2's movies (web/src/games/profile.ts): their picture size, which the page asks the browser's AV1
// decoder about (host/movie.ts movieCodec).  No imports: the host's leaf modules take it alone.

/** MOVIES/*.vp6: 768x512 at 30 fps */
export const MOVIE_SIZE = { w: 768, h: 512 } as const;
