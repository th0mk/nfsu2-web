// Underground 2's save files behind '@game/profile' (web/src/games/profile.ts SaveFormat): src/app/profile.ts, the
// page's own from before Most Wanted, and the folder they are in.
import type * as self from './profile';
import type { Saves } from '../profile';

export * from '../../app/profile';
export { PROFILES_DIR } from './run';

export type Checked = Saves<typeof self>;
