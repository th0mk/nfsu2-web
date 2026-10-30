// One game per browser profile and server: the saves live in this origin's
// storage, and two tabs writing the same profile or registry would corrupt
// it.  A Web Lock held for the life of the tab says who owns the saves; a
// second tab waits for it and takes over when the first one closes.

export interface Instance {
  /** true once this tab holds the lock */
  readonly owner: boolean;
  /** resolves when this tab holds the lock (immediately if it already does) */
  whenOwner: Promise<void>;
}

/** name: the lock of the save folder (src/games/<id>/storage.ts SAVES_LOCK and the folder's name) */
export function claimInstance(name: string): Instance {
  let owner = false;
  let gotIt!: () => void;
  const whenOwner = new Promise<void>(r => (gotIt = r));
  const hold = () => new Promise<void>(() => { /* released with the tab */ });
  const inst = { get owner() { return owner; }, whenOwner };
  if (!navigator.locks) { owner = true; gotIt(); return inst; }   // no Web Locks: nothing to coordinate
  void navigator.locks.request(name, { ifAvailable: true }, lock => {
    if (lock) { owner = true; gotIt(); return hold(); }
    // someone else has it: queue for it
    void navigator.locks.request(name, () => { owner = true; gotIt(); return hold(); });
    return undefined;
  });
  return inst;
}
