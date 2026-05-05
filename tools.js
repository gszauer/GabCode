// Tool registry and implementations. Mirrors the core's tool set in shape:
// each tool has a name, description, arg types, and an async `invoke(session, args)`
// that returns {ok, output}.
//
// All file-touching tools operate against `session.vfs` (the chat-scoped VFS).
// The bash tool dispatches to the same six portable builtins the CLI uses
// (ls, mkdir, rmdir, rm, cp, mv) against the VFS; non-builtin shell commands
// return an error in-browser.

const SHELL_META = /[|;><&`]|\$\(/;

function tokenizeShell(cmd) {
    const tokens = [];
    let cur = '';
    let inSingle = false, inDouble = false;
    for (let i = 0; i < cmd.length; ++i) {
        const c = cmd[i];
        if (c === "'" && !inDouble) { inSingle = !inSingle; continue; }
        if (c === '"' && !inSingle) { inDouble = !inDouble; continue; }
        if (c === '\\' && inDouble && i + 1 < cmd.length) { cur += cmd[++i]; continue; }
        if (/\s/.test(c) && !inSingle && !inDouble) {
            if (cur) { tokens.push(cur); cur = ''; }
        } else {
            cur += c;
        }
    }
    if (cur) tokens.push(cur);
    return tokens;
}

function dirname(p) {
    const i = p.lastIndexOf('/');
    return i < 0 ? '' : p.slice(0, i);
}
function basename(p) {
    const i = p.lastIndexOf('/');
    return i < 0 ? p : p.slice(i + 1);
}

// ── File tools ─────────────────────────────────────────────

async function toolReadFile(session, args) {
    const path = args[0]?.value;
    if (!path) return { ok: false, output: 'readFile: missing path' };
    const content = await session.vfs.readFile(path);
    if (content === null) return { ok: false, output: `readFile: not found: ${path}` };
    session.readSet.add(path);
    return { ok: true, output: content };
}

async function toolWriteFile(session, args) {
    const [pathArg, contentArg] = args;
    if (!pathArg || contentArg === undefined) {
        return { ok: false, output: 'writeFile: usage writeFile(path, content)' };
    }
    await session.vfs.writeFile(pathArg.value, contentArg.value ?? '');
    session.readSet.add(pathArg.value);
    session.touchedFiles = true;
    return { ok: true, output: 'ok' };
}

async function toolEditFile(session, args) {
    const [pathArg, oldArg, newArg] = args;
    if (!pathArg || oldArg === undefined || newArg === undefined) {
        return { ok: false, output: 'editFile: usage editFile(path, oldString, newString)' };
    }
    const path = pathArg.value;
    if (!session.readSet.has(path)) {
        return { ok: false, output: `editFile: file not in read set; call readFile first: ${path}` };
    }
    const content = await session.vfs.readFile(path);
    if (content === null) return { ok: false, output: `editFile: not found: ${path}` };
    const oldStr = String(oldArg.value);
    const newStr = String(newArg.value);
    if (!oldStr) return { ok: false, output: 'editFile: oldString must not be empty' };
    let count = 0, firstPos = -1, pos = 0;
    while (true) {
        const found = content.indexOf(oldStr, pos);
        if (found < 0) break;
        if (count === 0) firstPos = found;
        ++count;
        pos = found + oldStr.length;
    }
    if (count === 0) return { ok: false, output: 'editFile: oldString not found' };
    if (count > 1) {
        return { ok: false, output: `editFile: oldString found ${count} times; must be unique. Add surrounding context.` };
    }
    const updated = content.slice(0, firstPos) + newStr + content.slice(firstPos + oldStr.length);
    await session.vfs.writeFile(path, updated);
    session.touchedFiles = true;
    return { ok: true, output: 'ok' };
}

// ── grep / grepIn ──────────────────────────────────────────
// Mirrors core/grep_tool.cpp: `grep(pattern)` walks the whole VFS; `grepIn(pattern, path)`
// takes an explicit file or directory. Both skip hidden names, binaries, and files >1MB,
// truncate overlong lines, and cap at 500 matches.

const BINARY_EXTS = new Set([
    '.png', '.jpg', '.jpeg', '.gif', '.bmp', '.ico', '.webp',
    '.pdf', '.zip', '.gz', '.tar', '.bz2', '.xz', '.7z',
    '.exe', '.dll', '.so', '.dylib', '.o', '.a', '.lib',
    '.class', '.jar', '.wasm', '.bin',
    '.mp3', '.mp4', '.mov', '.wav', '.avi', '.mkv',
    '.sqlite', '.db', '.DS_Store',
]);

function isBinaryExt(name) {
    const dot = name.lastIndexOf('.');
    if (dot < 0) return false;
    return BINARY_EXTS.has(name.slice(dot).toLowerCase());
}

const GREP_MAX_MATCHES = 500;
const GREP_MAX_FILE_SIZE = 1024 * 1024;

async function grepFile(vfs, path, regex, matches) {
    if (matches.length >= GREP_MAX_MATCHES) return;
    const content = await vfs.readFile(path);
    if (content === null) return;
    const lines = content.split('\n');
    for (let i = 0; i < lines.length && matches.length < GREP_MAX_MATCHES; ++i) {
        if (!regex.test(lines[i])) continue;
        let line = lines[i];
        if (line.length > 200) line = line.slice(0, 200) + '...';
        matches.push(`${path}:${i + 1}: ${line}`);
    }
}

async function grepDir(vfs, dir, regex, matches) {
    if (matches.length >= GREP_MAX_MATCHES) return;
    const entries = await vfs.listDir(dir);
    for (const e of entries) {
        if (matches.length >= GREP_MAX_MATCHES) break;
        if (e.name.startsWith('.')) continue;
        const full = dir ? `${dir}/${e.name}` : e.name;
        if (e.isDir) {
            await grepDir(vfs, full, regex, matches);
        } else {
            if (isBinaryExt(e.name)) continue;
            if (e.sizeBytes > GREP_MAX_FILE_SIZE) continue;
            await grepFile(vfs, full, regex, matches);
        }
    }
}

function formatMatches(matches) {
    let out = matches.join('\n');
    if (matches.length >= GREP_MAX_MATCHES) out += '\n[truncated at 500 matches]';
    if (!out) out = '(no matches)';
    return out;
}

function compileRegex(pattern) {
    try { return { re: new RegExp(pattern), err: null }; }
    catch (e) { return { re: null, err: `invalid regex: ${e.message}` }; }
}

async function toolGrep(session, args) {
    const patternArg = args[0];
    if (!patternArg) return { ok: false, output: 'grep: missing pattern' };
    const { re, err } = compileRegex(patternArg.value);
    if (err) return { ok: false, output: err };
    const matches = [];
    await grepDir(session.vfs, '', re, matches);
    return { ok: true, output: formatMatches(matches) };
}

async function toolGrepIn(session, args) {
    const [patternArg, pathArg] = args;
    if (!patternArg) return { ok: false, output: 'grepIn: missing pattern' };
    if (!pathArg) return { ok: false, output: 'grepIn: missing path' };
    const path = String(pathArg.value);
    const exists = await session.vfs.fileExists(path);
    if (!exists) return { ok: false, output: `path not found: ${path}` };
    const { re, err } = compileRegex(patternArg.value);
    if (err) return { ok: false, output: err };
    const matches = [];
    const isDir = await session.vfs.isDir(path);
    if (isDir) await grepDir(session.vfs, path, re, matches);
    else await grepFile(session.vfs, path, re, matches);
    return { ok: true, output: formatMatches(matches) };
}

// ── bash builtins ──────────────────────────────────────────

async function builtinLs(session, argv) {
    let long = false, all = false, one = false;
    const targets = [];
    for (let i = 1; i < argv.length; ++i) {
        const a = argv[i];
        if (a.startsWith('-')) {
            for (const f of a.slice(1)) {
                if (f === 'l') long = true;
                else if (f === 'a') all = true;
                else if (f === '1') one = true;
                else return { ok: false, output: `ls: unknown flag -${f}` };
            }
        } else targets.push(a);
    }
    if (targets.length === 0) targets.push('');
    let out = '';
    for (const t of targets) {
        if (targets.length > 1) out += `${t || '.'}:\n`;
        const exists = t === '' || (await session.vfs.fileExists(t));
        if (!exists) { out += `ls: ${t}: No such file or directory\n`; continue; }
        const entries = await session.vfs.listDir(t);
        for (const e of entries) {
            if (!all && e.name.startsWith('.')) continue;
            if (long) {
                out += `${e.isDir ? 'd' : '-'} ${String(e.sizeBytes).padStart(8)}  ${e.name}${e.isDir ? '/' : ''}\n`;
            } else {
                out += e.name + (e.isDir ? '/' : '') + (one || long ? '\n' : '  ');
            }
        }
        if (!long && !one && entries.length) out += '\n';
    }
    return { ok: true, output: out };
}

async function builtinMkdir(session, argv) {
    let parents = false;
    const dirs = [];
    for (let i = 1; i < argv.length; ++i) {
        if (argv[i] === '-p') { parents = true; continue; }
        if (argv[i].startsWith('-')) return { ok: false, output: `mkdir: unknown flag ${argv[i]}` };
        dirs.push(argv[i]);
    }
    if (dirs.length === 0) return { ok: false, output: 'mkdir: missing operand' };
    let err = '';
    for (const d of dirs) {
        if (parents) {
            const parts = d.split('/');
            let acc = '';
            for (const p of parts) {
                acc = acc ? `${acc}/${p}` : p;
                await session.vfs.makeDir(acc);
            }
        } else {
            const ok = await session.vfs.makeDir(d);
            if (!ok) err += `mkdir: ${d}: failed\n`;
        }
    }
    session.touchedFiles = true;
    return err ? { ok: false, output: err } : { ok: true, output: '' };
}

async function builtinRmdir(session, argv) {
    let err = '';
    for (let i = 1; i < argv.length; ++i) {
        if (argv[i].startsWith('-')) return { ok: false, output: `rmdir: unknown flag ${argv[i]}` };
        const ok = await session.vfs.removeDir(argv[i]);
        if (!ok) err += `rmdir: ${argv[i]}: failed (not empty or not a directory)\n`;
    }
    session.touchedFiles = true;
    return err ? { ok: false, output: err } : { ok: true, output: '' };
}

async function builtinRm(session, argv) {
    let recursive = false, force = false;
    const targets = [];
    for (let i = 1; i < argv.length; ++i) {
        const a = argv[i];
        if (a.startsWith('-')) {
            for (const f of a.slice(1)) {
                if (f === 'r' || f === 'R') recursive = true;
                else if (f === 'f') force = true;
                else return { ok: false, output: `rm: unknown flag -${f}` };
            }
        } else targets.push(a);
    }
    if (targets.length === 0) return { ok: false, output: 'rm: missing operand' };
    let err = '';
    for (const t of targets) {
        const exists = await session.vfs.fileExists(t);
        if (!exists) { if (!force) err += `rm: ${t}: No such file or directory\n`; continue; }
        const isDir = await session.vfs.isDir(t);
        if (isDir) {
            if (!recursive) { err += `rm: ${t}: is a directory (use -r)\n`; continue; }
            await session.vfs.rmRecursive(t);
        } else {
            await session.vfs.deleteFile(t);
        }
    }
    session.touchedFiles = true;
    return err ? { ok: false, output: err } : { ok: true, output: '' };
}

async function copyRecursive(vfs, src, dst) {
    const isDir = await vfs.isDir(src);
    if (isDir) {
        await vfs.makeDir(dst);
        const entries = await vfs.listDir(src);
        for (const e of entries) {
            await copyRecursive(vfs, `${src}/${e.name}`, `${dst}/${e.name}`);
        }
    } else {
        const content = await vfs.readFile(src);
        if (content !== null) await vfs.writeFile(dst, content);
    }
}

async function builtinCp(session, argv) {
    let recursive = false;
    const parts = [];
    for (let i = 1; i < argv.length; ++i) {
        const a = argv[i];
        if (a.startsWith('-')) {
            for (const f of a.slice(1)) {
                if (f === 'r' || f === 'R') recursive = true;
                else return { ok: false, output: `cp: unknown flag -${f}` };
            }
        } else parts.push(a);
    }
    if (parts.length < 2) return { ok: false, output: 'cp: need source and destination' };
    const dst = parts.pop();
    const dstIsDir = await session.vfs.isDir(dst);
    if (parts.length > 1 && !dstIsDir) return { ok: false, output: `cp: target '${dst}' is not a directory` };
    for (const src of parts) {
        const actualDst = dstIsDir ? `${dst}/${basename(src)}` : dst;
        const srcIsDir = await session.vfs.isDir(src);
        if (srcIsDir && !recursive) return { ok: false, output: `cp: ${src}: is a directory (use -r)` };
        await copyRecursive(session.vfs, src, actualDst);
    }
    session.touchedFiles = true;
    return { ok: true, output: '' };
}

async function builtinMv(session, argv) {
    const parts = argv.slice(1).filter(a => !a.startsWith('-'));
    if (parts.length < 2) return { ok: false, output: 'mv: need source and destination' };
    const dst = parts.pop();
    const dstIsDir = await session.vfs.isDir(dst);
    if (parts.length > 1 && !dstIsDir) return { ok: false, output: `mv: target '${dst}' is not a directory` };
    for (const src of parts) {
        const actualDst = dstIsDir ? `${dst}/${basename(src)}` : dst;
        await copyRecursive(session.vfs, src, actualDst);
        await session.vfs.rmRecursive(src);
    }
    session.touchedFiles = true;
    return { ok: true, output: '' };
}

const BUILTINS = {
    ls: builtinLs,
    mkdir: builtinMkdir,
    rmdir: builtinRmdir,
    rm: builtinRm,
    cp: builtinCp,
    mv: builtinMv,
};

async function toolBash(session, args) {
    const cmd = String(args[0]?.value || '').trim();
    if (!cmd) return { ok: false, output: 'bash: empty command' };
    if (SHELL_META.test(cmd)) {
        return { ok: false, output: 'bash: shell operators (pipes, redirects, &&, $()) not supported in the browser shell' };
    }
    const argv = tokenizeShell(cmd);
    if (argv.length === 0) return { ok: true, output: '' };
    const handler = BUILTINS[argv[0]];
    if (!handler) return { ok: false, output: `bash: command not supported in browser: ${argv[0]}` };
    return handler(session, argv);
}

// ── Web tools ──────────────────────────────────────────────

async function toolWebFetch(session, args) {
    const url = String(args[0]?.value || '');
    if (!url) return { ok: false, output: 'webFetch: missing url' };
    try {
        const r = await fetch(url);
        const text = await r.text();
        const truncated = text.length > 100_000 ? text.slice(0, 100_000) + '\n[truncated]' : text;
        return { ok: r.ok, output: `HTTP ${r.status}\n\n${truncated}` };
    } catch (e) {
        return { ok: false, output: `webFetch: ${e.message} (likely CORS — needs a same-origin proxy)` };
    }
}

async function toolBraveSearch(session, args) {
    const query = String(args[0]?.value || '');
    if (!query) return { ok: false, output: 'braveSearch: missing query' };
    const key = session.config.braveApiKey;
    if (!key) return { ok: false, output: 'braveSearch: no Brave API key configured' };
    try {
        const url = `https://api.search.brave.com/res/v1/web/search?q=${encodeURIComponent(query)}`;
        const r = await fetch(url, {
            headers: { 'Accept': 'application/json', 'X-Subscription-Token': key },
        });
        if (!r.ok) return { ok: false, output: `braveSearch: HTTP ${r.status}` };
        const data = await r.json();
        const results = (data.web?.results || []).slice(0, 10);
        const out = results.map(x => `${x.title}\n${x.url}\n${x.description}\n`).join('\n');
        return { ok: true, output: out || '(no results)' };
    } catch (e) {
        return { ok: false, output: `braveSearch: ${e.message} (likely CORS)` };
    }
}

// ── Skills ─────────────────────────────────────────────────

async function toolSkill(session, args) {
    const name = String(args[0]?.value || '').trim();
    if (!name) return { ok: false, output: 'skill: missing name' };
    if (!session.skills) return { ok: false, output: 'skill: loader not initialized' };
    if (session.skills.isLoaded(name)) return { ok: true, output: 'already loaded' };
    if (!session.skills.exists(name)) return { ok: false, output: `unknown skill: ${name}` };
    const content = await session.skills.load(name);
    if (!content) return { ok: false, output: `failed to load skill: ${name}` };
    session.skills.markLoaded(name, content);
    session.loadedSkills.set(name, content);
    return { ok: true, output: 'loaded' };
}

// ── Agents ─────────────────────────────────────────────────

async function toolAgent(session, args) {
    const name = String(args[0]?.value || '').trim();
    const query = String(args[1]?.value || '').trim();
    if (!name) return { ok: false, output: 'agent: missing name' };
    if (!query) return { ok: false, output: 'agent: missing query' };
    const { runAgent, AGENT_DEFS } = await import('./agents.js');
    if (!AGENT_DEFS[name]) {
        return { ok: false, output: `agent: unknown "${name}". Known: ${Object.keys(AGENT_DEFS).join(', ')}` };
    }
    const text = await runAgent(name, query, session, session.currentAbortSignal);
    if (!text || text.startsWith('[agent')) return { ok: false, output: text };
    return { ok: true, output: text };
}

// ── Registry ───────────────────────────────────────────────

export const TOOLS = [
    { name: 'readFile',    description: "Read a file's contents.", sig: 'readFile(path)', invoke: toolReadFile },
    { name: 'writeFile',   description: 'Create or overwrite a file.', sig: 'writeFile(path, content)', invoke: toolWriteFile },
    { name: 'editFile',    description: 'Replace exactly one occurrence of oldString with newString in a file. File must have been read first.', sig: 'editFile(path, oldString, newString)', invoke: toolEditFile },
    { name: 'grep',        description: 'Search for a regex pattern in all text files under the project directory (ECMAScript regex).', sig: 'grep(pattern)', invoke: toolGrep },
    { name: 'grepIn',      description: 'Search for a regex pattern in a specific file or directory.', sig: 'grepIn(pattern, path)', invoke: toolGrepIn },
    { name: 'bash',        description: 'Run a shell command. Browser build supports only the portable builtins: ls, mkdir, rmdir, rm, cp, mv (no pipes, redirects, or non-builtin commands).', sig: 'bash(command)', invoke: toolBash },
    { name: 'webFetch',    description: 'Fetch a URL and return the response body.', sig: 'webFetch(url)', invoke: toolWebFetch },
    { name: 'braveSearch', description: 'Search the web via Brave Search API and return JSON array of top results.', sig: 'braveSearch(query)', invoke: toolBraveSearch },
    { name: 'skill',       description: 'Load a skill by name to add specialized context.', sig: 'skill(name)', invoke: toolSkill },
    { name: 'agent',       description: 'Run a sub-agent with its own system prompt and restricted tools.', sig: 'agent(name, query)', invoke: toolAgent },
];

export function findTool(name) {
    return TOOLS.find(t => t.name === name) || null;
}

export function renderToolsForSystemPrompt() {
    return TOOLS.map(t => `- \`${t.sig}\` — ${t.description}`).join('\n');
}
