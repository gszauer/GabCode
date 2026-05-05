// Chat registry: creates, lists, deletes chats. Each chat is identified by a
// UUID and rooted at `chats/<id>/` in IndexedDB. Metadata (name, createdAt,
// lastActiveAt) lives at `.gab/chat_meta.json` inside that chat's VFS.

import { VFS, rootVfs } from './vfs.js';
import { DEFAULT_CONFIG, loadDefaults, saveChatConfig } from './config.js';
import { writeDefaultPrompts } from './prompts.js';
import { seedBundledSkills } from './bundled-skills.js';

const META_PATH = '.gab/chat_meta.json';
const ACTIVE_PATH = 'defaults/active_chat.txt';

function uid() {
    return 'chat_' + Math.random().toString(36).slice(2, 10) + Date.now().toString(36);
}

export function chatVfs(id) {
    return new VFS(`chats/${id}`);
}

export async function listChats() {
    const entries = await rootVfs.listDir('chats');
    const chats = [];
    for (const e of entries) {
        if (!e.isDir) continue;
        const v = chatVfs(e.name);
        const raw = await v.readFile(META_PATH);
        if (raw) {
            try {
                const meta = JSON.parse(raw);
                chats.push({ id: e.name, ...meta });
            } catch { /* skip corrupt */ }
        } else {
            chats.push({ id: e.name, name: e.name, createdAt: 0, lastActiveAt: 0 });
        }
    }
    chats.sort((a, b) => (b.createdAt || 0) - (a.createdAt || 0));
    return chats;
}

export async function createChat(name) {
    const id = uid();
    const now = Date.now();
    const displayName = name || defaultName();
    const v = chatVfs(id);
    await v.makeDir('.gab');
    await v.writeFile(META_PATH, JSON.stringify({
        name: displayName,
        createdAt: now,
        lastActiveAt: now,
    }, null, 2));

    // Seed the chat's config from the last-used defaults
    const defaults = await loadDefaults();
    await saveChatConfig(v, defaults);

    // Seed the default prompt files (system, compactor, web_search, explore).
    await writeDefaultPrompts(v);

    // Seed any bundled skills the web build ships with (e.g. phaser).
    await seedBundledSkills(v);

    return { id, name: displayName, createdAt: now, lastActiveAt: now };
}

function defaultName() {
    const d = new Date();
    const pad = n => String(n).padStart(2, '0');
    return `Chat ${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

export async function renameChat(id, newName) {
    const v = chatVfs(id);
    const raw = await v.readFile(META_PATH);
    const meta = raw ? JSON.parse(raw) : {};
    meta.name = newName;
    await v.writeFile(META_PATH, JSON.stringify(meta, null, 2));
}

export async function deleteChat(id) {
    const v = chatVfs(id);
    await v.rmRecursive('');
    // Also nuke the top-level `chats/<id>` directory record
    await rootVfs.rmRecursive(`chats/${id}`);
}

export async function getActiveChatId() {
    const raw = await rootVfs.readFile(ACTIVE_PATH);
    return (raw || '').trim() || null;
}

export async function setActiveChatId(id) {
    await rootVfs.writeFile(ACTIVE_PATH, id || '');
}
