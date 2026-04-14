// IndexedDB-backed virtual filesystem for gabcode.
// Each chat gets a VFS rooted at `chats/<chatId>/`; the chat's core sees paths
// relative to that root (e.g. `.gab/history.jsonl`), and the VFS transparently
// prefixes them before touching the object store.
//
// Object store schema: keyed by absolute path (`chats/abc/.gab/config.json`),
// value is `{path, isDir, content, size, mtime}`. Directories have `isDir=true`
// and empty content; files hold their full content as a string.

const DB_NAME = 'gabcode';
const DB_VERSION = 1;
const STORE = 'files';

let dbPromise = null;

function openDb() {
    if (dbPromise) return dbPromise;
    dbPromise = new Promise((resolve, reject) => {
        const req = indexedDB.open(DB_NAME, DB_VERSION);
        req.onupgradeneeded = () => {
            const db = req.result;
            if (!db.objectStoreNames.contains(STORE)) {
                const store = db.createObjectStore(STORE, { keyPath: 'path' });
                store.createIndex('by_parent', 'parent');
            }
        };
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
    });
    return dbPromise;
}

function txStore(mode) {
    return openDb().then(db => db.transaction(STORE, mode).objectStore(STORE));
}

function promisify(req) {
    return new Promise((resolve, reject) => {
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
    });
}

function normalize(p) {
    // Collapse `./`, strip leading `/`, remove trailing `/`.
    const parts = String(p).split('/').filter(seg => seg && seg !== '.');
    const out = [];
    for (const seg of parts) {
        if (seg === '..') out.pop();
        else out.push(seg);
    }
    return out.join('/');
}

function parentOf(path) {
    const i = path.lastIndexOf('/');
    return i < 0 ? '' : path.slice(0, i);
}

function basename(path) {
    const i = path.lastIndexOf('/');
    return i < 0 ? path : path.slice(i + 1);
}

// Ensure all parent directories of `path` exist as directory records.
async function ensureParents(store, path) {
    const segments = path.split('/').filter(Boolean);
    segments.pop(); // drop the leaf itself
    let acc = '';
    for (const seg of segments) {
        acc = acc ? `${acc}/${seg}` : seg;
        const existing = await promisify(store.get(acc));
        if (!existing) {
            await promisify(store.put({
                path: acc,
                parent: parentOf(acc),
                isDir: true,
                content: '',
                size: 0,
                mtime: Date.now(),
            }));
        }
    }
}

export class VFS {
    constructor(prefix) {
        // Always stored without trailing slash; empty string = root.
        this.prefix = normalize(prefix);
    }

    resolve(path) {
        const rel = normalize(path);
        if (!this.prefix) return rel;
        if (!rel) return this.prefix;
        return `${this.prefix}/${rel}`;
    }

    stripPrefix(fullPath) {
        if (!this.prefix) return fullPath;
        if (fullPath === this.prefix) return '';
        return fullPath.startsWith(this.prefix + '/')
            ? fullPath.slice(this.prefix.length + 1)
            : fullPath;
    }

    async readFile(path) {
        const full = this.resolve(path);
        const store = await txStore('readonly');
        const rec = await promisify(store.get(full));
        if (!rec || rec.isDir) return null;
        return rec.content;
    }

    async writeFile(path, content) {
        const full = this.resolve(path);
        const db = await openDb();
        const tx = db.transaction(STORE, 'readwrite');
        const store = tx.objectStore(STORE);
        await ensureParents(store, full);
        await promisify(store.put({
            path: full,
            parent: parentOf(full),
            isDir: false,
            content: String(content),
            size: new Blob([content]).size,
            mtime: Date.now(),
        }));
        await new Promise((resolve, reject) => {
            tx.oncomplete = resolve;
            tx.onerror = () => reject(tx.error);
        });
        return true;
    }

    async appendFile(path, chunk) {
        const existing = (await this.readFile(path)) ?? '';
        return this.writeFile(path, existing + chunk);
    }

    async deleteFile(path) {
        const full = this.resolve(path);
        const store = await txStore('readwrite');
        const rec = await promisify(store.get(full));
        if (!rec || rec.isDir) return false;
        await promisify(store.delete(full));
        return true;
    }

    async fileExists(path) {
        const full = this.resolve(path);
        const store = await txStore('readonly');
        const rec = await promisify(store.get(full));
        return !!rec;
    }

    async isDir(path) {
        const full = this.resolve(path);
        const store = await txStore('readonly');
        const rec = await promisify(store.get(full));
        return !!(rec && rec.isDir);
    }

    async listDir(path) {
        const full = this.resolve(path);
        const store = await txStore('readonly');
        const index = store.index('by_parent');
        const entries = await promisify(index.getAll(full));
        return entries
            .map(e => ({
                name: basename(e.path),
                isDir: !!e.isDir,
                sizeBytes: e.size || 0,
            }))
            .sort((a, b) => {
                if (a.isDir !== b.isDir) return a.isDir ? -1 : 1;
                return a.name.localeCompare(b.name);
            });
    }

    async makeDir(path) {
        const full = this.resolve(path);
        if (!full) return true;
        const db = await openDb();
        const tx = db.transaction(STORE, 'readwrite');
        const store = tx.objectStore(STORE);
        const existing = await promisify(store.get(full));
        if (existing && !existing.isDir) return false;
        await ensureParents(store, full);
        await promisify(store.put({
            path: full,
            parent: parentOf(full),
            isDir: true,
            content: '',
            size: 0,
            mtime: Date.now(),
        }));
        await new Promise((resolve, reject) => {
            tx.oncomplete = resolve;
            tx.onerror = () => reject(tx.error);
        });
        return true;
    }

    async removeDir(path) {
        const full = this.resolve(path);
        const store = await txStore('readwrite');
        const rec = await promisify(store.get(full));
        if (!rec || !rec.isDir) return false;
        // rmdir fails if non-empty (POSIX semantics)
        const index = store.index('by_parent');
        const children = await promisify(index.getAll(full));
        if (children.length > 0) return false;
        await promisify(store.delete(full));
        return true;
    }

    async rmRecursive(path) {
        const full = this.resolve(path);
        const store = await txStore('readwrite');
        const rec = await promisify(store.get(full));
        if (!rec) return false;
        if (rec.isDir) {
            const index = store.index('by_parent');
            const children = await promisify(index.getAll(full));
            for (const child of children) {
                await this.rmRecursive(this.stripPrefix(child.path));
            }
        }
        const store2 = await txStore('readwrite');
        await promisify(store2.delete(full));
        return true;
    }

    async walk(startPath = '') {
        const result = [];
        const walk = async (p) => {
            const entries = await this.listDir(p);
            for (const e of entries) {
                const child = p ? `${p}/${e.name}` : e.name;
                result.push({ path: child, isDir: e.isDir, sizeBytes: e.sizeBytes });
                if (e.isDir) await walk(child);
            }
        };
        await walk(startPath);
        return result;
    }
}

// Global VFS rooted at the DB. Used by the chat manager to list/create/delete
// chats, and for any DB-wide defaults (e.g. stored last-used config).
export const rootVfs = new VFS('');

// Hard reset: close the open DB connection (so the delete isn't blocked) and
// drop the entire IndexedDB database. Callers typically follow this with a
// page reload to rebuild state from scratch.
export async function resetAll() {
    if (dbPromise) {
        const db = await dbPromise;
        db.close();
    }
    dbPromise = null;
    await new Promise((resolve, reject) => {
        const req = indexedDB.deleteDatabase(DB_NAME);
        req.onsuccess = () => resolve();
        req.onerror = () => reject(req.error);
        req.onblocked = () => resolve(); // best-effort: will finalize on next tick
    });
}
