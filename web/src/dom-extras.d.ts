// What the browsers have and TypeScript's own libraries (tsconfig.json lib) do not declare yet.

/** Atomics.waitAsync, of ES2024 (the libraries are ES2023's): the doorbells of the renderer, the network and the I/O
 *  worker.  As ES2024's own declaration has it, so that the two merge once the libraries have it */
interface Atomics {
  waitAsync(typedArray: Int32Array, index: number, value: number, timeout?: number):
    { async: false; value: 'not-equal' | 'timed-out' } | { async: true; value: Promise<'ok' | 'timed-out'> };
}

/** a sync access handle's mode, of the File System spec (the libraries take no options) */
interface FileSystemFileHandle {
  createSyncAccessHandle(options?: { mode?: 'readwrite' | 'read-only' | 'readwrite-unsafe' }): Promise<FileSystemSyncAccessHandle>;
}
