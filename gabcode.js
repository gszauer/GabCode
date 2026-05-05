// gabcode web shell — entry point.
// Orchestrates VFS + chats + config + history + LLM + UI. Each open chat gets
// a Session bound to its own VFS prefix.

import { chatVfs, listChats, createChat, deleteChat, getActiveChatId,
         setActiveChatId, renameChat } from './chats.js';
import { loadChatConfig, saveChatConfig, DEFAULT_CONFIG, loadDefaults, probeModels } from './config.js';
import { History } from './history.js';
import { TurnRunner } from './llm.js';
import { SkillLoader } from './skills.js';
import { writeDefaultPrompts } from './prompts.js';
import { seedBundledSkills } from './bundled-skills.js';
import { TOOLS } from './tools.js';
import { AGENT_DEFS, runAgent, compact } from './agents.js';
import * as ui from './ui.js';

// ── Session: per-chat in-memory state ──────────────────────

class Session {
    constructor(id, vfs, config) {
        this.id = id;
        this.vfs = vfs;
        this.config = config;
        this.history = new History(vfs);
        this.messages = [];       // {role, content} — conversation for the LLM
        this.readSet = new Set();
        this.loadedSkills = new Map();
        this.touchedFiles = false;
        this.skills = new SkillLoader(vfs);
        this.lastTotalTokens = 0;
        this.currentAbortSignal = null;
    }
}

// ── App state ──────────────────────────────────────────────

const state = {
    chats: [],
    activeId: null,
    session: null,
    runner: null,
    generating: false,
};

// ── Rendering helpers ──────────────────────────────────────

async function refreshChatList() {
    state.chats = await listChats();
    ui.renderChatList(state.chats, state.activeId, {
        onSelect: handleSelectChat,
        onDelete: handleDeleteChat,
    });
}

async function refreshFileTree() {
    if (!state.session) {
        document.getElementById('tree-root').innerHTML = '';
        return;
    }
    await ui.renderFileTree(state.session.vfs, async () => {
        // After a file is saved via the modal, rescan skills (in case a
        // SKILL.md was edited) and refresh the tree listing for size updates.
        await state.session.skills.scan();
        await refreshFileTree();
    });
}

async function renderConversation() {
    ui.clearMessages();
    if (!state.session) {
        if (state.chats.length === 0) {
            ui.showEmptyConversation('No chats yet.', { label: 'Create chat', onClick: handleNewChat });
        } else {
            ui.showEmptyConversation('Select or create a chat to start.');
        }
        return;
    }
    if (state.session.messages.length === 0) {
        ui.showEmptyConversation('Send a message to get started.');
        return;
    }
    for (const m of state.session.messages) {
        if (m.role === 'user') {
            // user messages can be bare input or <result>...</result> from a tool
            if (m.content.startsWith('<result>') && m.content.endsWith('</result>')) {
                const inner = m.content.slice('<result>'.length, -'</result>'.length);
                ui.renderToolResult(!m.isError, inner);
            } else {
                ui.renderUserMessage(m.content);
            }
        } else if (m.role === 'assistant') {
            ui.renderAssistantMessage(m.content);
        }
    }
}

// ── Event handlers ─────────────────────────────────────────

async function handleNewChat() {
    const chat = await createChat();
    await refreshChatList();
    await handleSelectChat(chat.id);
}

async function handleDeleteChat(id, name) {
    ui.openConfirmModal({
        title: 'Delete chat?',
        body: `"${name}" will be permanently deleted along with its files and history.`,
        onConfirm: async () => {
            if (state.activeId === id) {
                if (state.generating) state.runner?.cancel();
                state.session = null;
                state.activeId = null;
                await setActiveChatId('');
                ui.setConvTitle(null);
                ui.setComposerEnabled(false);
                renderConversation();
                refreshFileTree();
            }
            await deleteChat(id);
            await refreshChatList();
        },
    });
}

async function handleSelectChat(id) {
    if (state.activeId === id) return;
    const doSwitch = async () => {
        if (state.generating) state.runner?.cancel();
        state.activeId = id;
        await setActiveChatId(id);

        const vfs = chatVfs(id);
        let config = await loadChatConfig(vfs);
        if (!config) config = { ...DEFAULT_CONFIG };
        state.session = new Session(id, vfs, config);

        // Seed any missing default prompt files + bundled skills (for chats
        // that predate this feature). Existing user edits are preserved.
        await writeDefaultPrompts(vfs);
        await seedBundledSkills(vfs);
        await state.session.skills.scan();

        // Rehydrate messages from history.jsonl
        state.session.messages = await rehydrateMessagesForApi(state.session.history);

        const chat = state.chats.find(c => c.id === id);
        ui.setConvTitle(chat?.name || id);
        ui.setComposerEnabled(!!config.model);
        if (!config.model) {
            ui.setStatus('No model configured. Open Settings.');
        } else {
            ui.setStatus(`${config.model} @ ${config.apiBaseUrl}`);
        }

        await renderConversation();
        await refreshFileTree();
        await refreshChatList();  // update the "active" highlight

        // First-use nudge: if no model is configured, open the settings modal
        // so the user can probe and pick one without hunting for the gear.
        if (!config.model) handleOpenConfig('This chat has no model configured. Pick one to continue.');
    };
    if (state.generating) {
        ui.openSwitchModal({
            onConfirm: doSwitch,
            onCancel: () => {},
        });
    } else {
        await doSwitch();
    }
}

async function rehydrateMessagesForApi(history) {
    const records = await history.readAll();
    const messages = [];
    for (const r of records) {
        if (r.t === 'user') {
            messages.push({ role: 'user', content: r.content });
        } else if (r.t === 'assistant') {
            messages.push({ role: 'assistant', content: r.content });
        } else if (r.t === 'tool_result') {
            messages.push({
                role: 'user',
                content: `<result>${r.output}</result>`,
                isError: !r.ok,
            });
        }
        // tool_call records aren't separately appended as messages — they live
        // as part of the preceding assistant message (which already includes
        // the `<tool>...</tool>` block).
    }
    return messages;
}

async function handleSend() {
    if (!state.session || state.generating) return;
    const input = document.getElementById('input');
    const text = input.value.trim();
    if (!text) return;

    if (text.startsWith('/')) {
        input.value = '';
        await tryHandleSlash(text);
        return;
    }

    input.value = '';
    state.generating = true;
    ui.setGenerating(true);
    ui.setStatus('Generating...');

    let stream = null;
    let streamHadToolCall = false;
    let activeToolBubble = null;  // bubble awaiting its tool_result (spinner spinning)
    const stopSpinner = () => {
        if (activeToolBubble) { activeToolBubble.setBusy(false); activeToolBubble = null; }
    };
    const runner = new TurnRunner(state.session, (ev) => {
        switch (ev.type) {
            case 'user_message':
                ui.renderUserMessage(ev.content);
                break;
            case 'stream_start':
                stream = ui.beginAssistantStream();
                streamHadToolCall = false;
                break;
            case 'delta':
                stream?.append(ev.content);
                break;
            case 'stream_end':
                if (stream) {
                    streamHadToolCall = stream.hasToolCall?.() || false;
                    if (streamHadToolCall) activeToolBubble = stream.getToolBubble?.() || null;
                    stream.commit();
                }
                stream = null;
                break;
            case 'assistant_message':
                // Already streamed; replace the last streamed node with
                // final formatted markdown (the commit call above handled it).
                break;
            case 'tool_call':
                // The streaming bubble morphed into a tool-call view as soon
                // as <tool> appeared; skip rendering a duplicate here.
                if (!streamHadToolCall) activeToolBubble = ui.renderToolCall(ev.argsRaw);
                streamHadToolCall = false;
                break;
            case 'tool_result':
                stopSpinner();
                ui.renderToolResult(ev.ok, ev.output);
                if (state.session?.touchedFiles) {
                    refreshFileTree();
                    state.session.touchedFiles = false;
                }
                break;
            case 'compacting':
                ui.setStatus('Compacting conversation...');
                ui.renderSystemNote('Compacting conversation...');
                break;
            case 'compact_done':
                ui.setStatus(ev.ok ? 'Compaction complete.' : 'Compaction skipped.');
                if (ev.ok) renderConversation();
                break;
            case 'compact_failed':
                ui.renderSystemNote(`Compaction failed: ${ev.error}`);
                break;
            case 'usage':
                ui.setStatus(`${state.session.config.model} — ${ev.tokens} tokens`);
                break;
            case 'turn_done':
                stopSpinner();
                ui.setStatus(`${state.session.config.model} — ${state.session.lastTotalTokens || 0} tokens`);
                break;
            case 'turn_cancelled':
                stopSpinner();
                stream?.remove();
                ui.setStatus('Cancelled — turn rolled back.');
                renderConversation();
                break;
            case 'turn_error':
                stopSpinner();
                ui.setStatus(`Error: ${ev.error}`);
                ui.renderToolResult(false, ev.error);
                break;
        }
    });
    state.runner = runner;

    try {
        await runner.run(text);
    } catch (e) {
        ui.setStatus(`Error: ${e.message}`);
    } finally {
        state.generating = false;
        state.runner = null;
        ui.setGenerating(false);
        await refreshFileTree();
    }
}

function handleStop() {
    if (state.runner) state.runner.cancel();
}

// ── Slash commands ─────────────────────────────────────────

async function tryHandleSlash(text) {
    const space = text.indexOf(' ');
    const name = (space < 0 ? text.slice(1) : text.slice(1, space)).toLowerCase();
    const arg  = space < 0 ? '' : text.slice(space + 1);
    const handler = SLASH_COMMANDS[name];
    if (!handler) {
        ui.renderSystemNote(`Unknown command: /${name}. Try /help.`);
        return;
    }
    try {
        await handler(arg);
    } catch (e) {
        ui.renderSystemNote(`/${name} failed: ${e.message}`);
    }
}

function cmdHelp() {
    ui.renderSystemNote([
        'Slash commands:',
        '  /help              Show this help',
        '  /tools             List registered tools',
        '  /skills            List available skills',
        '  /agents            List available agents',
        '  /search <query>    Run the web_search agent',
        '  /explore <query>   Run the explore agent',
        '  /compact           Force compaction now',
        '  /clear             Wipe conversation and history',
        '  /config            Open the settings modal',
        '  /guard <n>         Set max tool calls per turn',
        '  /limit <n>|<n>%    Set compaction reserve (absolute or % of context)',
        '  /system <text>     Replace the system prompt',
        '  /prompt [path]     Send a file as a user turn (default .gab/prompt.md)',
    ].join('\n'));
}

function cmdConfig() { handleOpenConfig(); }

async function cmdClear() {
    const s = state.session;
    if (!s) { ui.renderSystemNote('/clear: no active chat'); return; }
    s.messages = [];
    s.readSet.clear();
    s.loadedSkills.clear();
    if (s.skills) s.skills.clearLoaded();
    s.lastTotalTokens = 0;
    await s.history.clear();
    await renderConversation();
    ui.renderSystemNote('Conversation cleared.');
}

function cmdTools() {
    const lines = ['Registered tools:'];
    for (const t of TOOLS) lines.push(`  ${t.sig}  — ${t.description}`);
    ui.renderSystemNote(lines.join('\n'));
}

function cmdSkills() {
    const s = state.session;
    if (!s?.skills) { ui.renderSystemNote('/skills: no active chat'); return; }
    const lines = ['Available skills:'];
    lines.push(s.skills.generateSummaries().trimEnd());
    const loaded = [...s.skills.loaded.keys()];
    if (loaded.length > 0) {
        lines.push('', 'Currently loaded:');
        for (const name of loaded) lines.push(`  - ${name}`);
    }
    ui.renderSystemNote(lines.join('\n'));
}

function cmdAgents() {
    const s = state.session;
    const lines = ['Available agents:'];
    for (const [name, def] of Object.entries(AGENT_DEFS)) {
        const avail = s ? def.availableCheck(s.config) : { ok: true };
        const suffix = avail.ok ? '' : ` (unavailable: ${avail.reason})`;
        lines.push(`  - ${name}: ${def.description}${suffix}`);
    }
    ui.renderSystemNote(lines.join('\n'));
}

async function cmdCompact() {
    const s = state.session;
    if (!s || state.generating) return;
    state.generating = true;
    ui.setGenerating(true);
    ui.setStatus('Compacting...');
    ui.renderSystemNote('Compacting conversation...');
    try {
        const ok = await compact(s);
        if (ok) {
            s.lastTotalTokens = 0;
            await renderConversation();
            ui.renderSystemNote('Compaction complete.');
        } else {
            ui.renderSystemNote('Compaction skipped (nothing to compact).');
        }
    } catch (e) {
        ui.renderSystemNote(`Compaction failed: ${e.message}`);
    } finally {
        state.generating = false;
        ui.setGenerating(false);
        ui.setStatus(`${s.config.model} — ${s.lastTotalTokens || 0} tokens`);
    }
}

async function cmdGuard(arg) {
    const s = state.session;
    if (!s) return;
    const trimmed = arg.trim();
    if (!trimmed) {
        ui.renderSystemNote(`Usage: /guard <n> — currently ${s.config.maxToolCallsPerTurn}`);
        return;
    }
    const n = parseInt(trimmed, 10);
    if (!n || n < 1) { ui.renderSystemNote('/guard: invalid value'); return; }
    s.config.maxToolCallsPerTurn = n;
    await saveChatConfig(s.vfs, s.config);
    ui.renderSystemNote(`Max tool calls per turn set to ${n}.`);
}

async function cmdLimit(arg) {
    const s = state.session;
    if (!s) return;
    const trimmed = arg.trim();
    if (!trimmed) {
        const cur = s.config.reserveTokens;
        ui.renderSystemNote(`Usage: /limit <n>[%] — currently ${cur || 'auto (10% of context)'}`);
        return;
    }
    let tokens = 0;
    if (trimmed.endsWith('%')) {
        const pct = parseFloat(trimmed.slice(0, -1));
        if (!s.config.maxContextTokens) {
            ui.renderSystemNote('/limit %: requires maxContextTokens to be set in settings.');
            return;
        }
        tokens = Math.floor(s.config.maxContextTokens * pct / 100);
    } else {
        tokens = parseInt(trimmed, 10);
    }
    if (!tokens || tokens < 1) { ui.renderSystemNote('/limit: invalid value'); return; }
    s.config.reserveTokens = tokens;
    await saveChatConfig(s.vfs, s.config);
    ui.renderSystemNote(`Compaction reserve set to ${tokens} tokens.`);
}

async function cmdSystem(arg) {
    const s = state.session;
    if (!s) return;
    if (!arg) { ui.renderSystemNote('Usage: /system <text>. Replaces .gab/prompts/system.md.'); return; }
    await s.vfs.writeFile('.gab/prompts/system.md', arg);
    ui.renderSystemNote('System prompt updated.');
    await refreshFileTree();
}

async function cmdSearch(arg) { await runSlashAgent('web_search', arg.trim()); }
async function cmdExplore(arg) { await runSlashAgent('explore', arg.trim()); }

async function runSlashAgent(name, query) {
    const s = state.session;
    if (!s || state.generating) return;
    const alias = name === 'web_search' ? 'search' : 'explore';
    if (!query) { ui.renderSystemNote(`Usage: /${alias} <query>`); return; }
    state.generating = true;
    ui.setGenerating(true);
    ui.setStatus(`Running ${name} agent...`);
    try {
        const result = await runAgent(name, query, s);
        ui.renderSystemNote(`[${name}]\n${result}`);
    } catch (e) {
        ui.renderSystemNote(`${name} failed: ${e.message}`);
    } finally {
        state.generating = false;
        ui.setGenerating(false);
        ui.setStatus(`${s.config.model} — ${s.lastTotalTokens || 0} tokens`);
    }
}

async function cmdPrompt(arg) {
    const s = state.session;
    if (!s) return;
    const path = arg.trim() || '.gab/prompt.md';
    const content = await s.vfs.readFile(path);
    if (content === null) { ui.renderSystemNote(`/prompt: cannot read ${path}`); return; }
    const trimmed = content.replace(/\s+$/, '');
    if (!trimmed) { ui.renderSystemNote(`/prompt: ${path} is empty`); return; }
    ui.renderSystemNote(`[Running prompt from ${path}]`);
    const input = document.getElementById('input');
    input.value = trimmed;
    await handleSend();
}

const SLASH_COMMANDS = {
    help:    cmdHelp,
    config:  cmdConfig,
    clear:   cmdClear,
    tools:   cmdTools,
    skills:  cmdSkills,
    agents:  cmdAgents,
    compact: cmdCompact,
    guard:   cmdGuard,
    limit:   cmdLimit,
    system:  cmdSystem,
    search:  cmdSearch,
    explore: cmdExplore,
    prompt:  cmdPrompt,
};

// Panel collapse state (persisted in localStorage). Defaults: chat list open,
// file tree closed — matches the requested initial layout but remembers any
// user toggle across reloads.
const UI_PREFS_KEY = 'gabcode.ui';

function loadUiPrefs() {
    try { return JSON.parse(localStorage.getItem(UI_PREFS_KEY)) || {}; }
    catch { return {}; }
}

function saveUiPrefs(patch) {
    const merged = { ...loadUiPrefs(), ...patch };
    localStorage.setItem(UI_PREFS_KEY, JSON.stringify(merged));
}

function setupSidebarToggles() {
    const app = document.getElementById('app');
    const prefs = loadUiPrefs();
    // On mobile, force both panels closed on every load regardless of saved
    // prefs — those prefs were typically set on desktop where "sidebar open"
    // is sensible, and on a phone they'd cover the whole conversation on
    // first paint. Desktop default: chat list open, file tree closed.
    const isMobile = window.matchMedia('(max-width: 768px)').matches;
    const sidebarCollapsed  = isMobile ? true : (prefs.sidebarCollapsed  ?? false);
    const filetreeCollapsed = isMobile ? true : (prefs.filetreeCollapsed ?? true);
    app.classList.toggle('sidebar-collapsed',  sidebarCollapsed);
    app.classList.toggle('filetree-collapsed', filetreeCollapsed);

    const isMobileNow = () => window.matchMedia('(max-width: 768px)').matches;

    document.getElementById('btn-toggle-sidebar').addEventListener('click', () => {
        const opening = app.classList.contains('sidebar-collapsed');
        app.classList.toggle('sidebar-collapsed');
        // On mobile, opening one overlay closes the other so the backdrop
        // never stacks two slide-outs on top of each other.
        if (opening && isMobileNow()) app.classList.add('filetree-collapsed');
        saveUiPrefs({
            sidebarCollapsed:  app.classList.contains('sidebar-collapsed'),
            filetreeCollapsed: app.classList.contains('filetree-collapsed'),
        });
    });
    document.getElementById('btn-toggle-filetree').addEventListener('click', () => {
        const opening = app.classList.contains('filetree-collapsed');
        app.classList.toggle('filetree-collapsed');
        if (opening && isMobileNow()) app.classList.add('sidebar-collapsed');
        saveUiPrefs({
            sidebarCollapsed:  app.classList.contains('sidebar-collapsed'),
            filetreeCollapsed: app.classList.contains('filetree-collapsed'),
        });
    });

    document.getElementById('mobile-backdrop').addEventListener('click', () => {
        app.classList.add('sidebar-collapsed');
        app.classList.add('filetree-collapsed');
        saveUiPrefs({ sidebarCollapsed: true, filetreeCollapsed: true });
    });
}

async function handleDownloadZip() {
    if (!state.session) return;
    const { downloadVfsAsZip } = await import('./zip.js');
    const chat = state.chats.find(c => c.id === state.activeId);
    const safeName = (chat?.name || state.activeId).replace(/[^a-zA-Z0-9_\- ]/g, '_');
    await downloadVfsAsZip(state.session.vfs, `${safeName}.zip`);
}

async function handleOpenConfig(initialError) {
    const initial = state.session?.config || await loadDefaults();
    ui.openConfigModal({
        initial,
        initialError,
        onSave: async (next) => {
            if (state.session) {
                const saved = await saveChatConfig(state.session.vfs, next);
                state.session.config = saved;
                ui.setComposerEnabled(!!saved.model);
                ui.setStatus(`${saved.model} @ ${saved.apiBaseUrl}`);
            } else {
                // No active chat — update defaults only (saveChatConfig also
                // writes to defaults path, but we need a chat VFS. Create a
                // temporary write to the root defaults.)
                const { rootVfs } = await import('./vfs.js');
                await rootVfs.writeFile('defaults/config.json', JSON.stringify(next, null, 2));
            }
        },
        onCancel: () => {},
        onReset: handleReset,
    });
}

function handleReset() {
    ui.openConfirmModal({
        title: 'Reset everything?',
        body: 'This wipes all LLM settings and deletes every chat (messages, files, history). This cannot be undone.',
        onConfirm: async () => {
            const { resetAll } = await import('./vfs.js');
            if (state.runner) state.runner.cancel();
            await resetAll();
            location.reload();
        },
    });
}

// ── Init ───────────────────────────────────────────────────

async function init() {
    document.getElementById('btn-new-chat').addEventListener('click', handleNewChat);
    document.getElementById('btn-config').addEventListener('click', handleOpenConfig);
    document.getElementById('btn-send').addEventListener('click', handleSend);
    document.getElementById('btn-stop').addEventListener('click', handleStop);
    document.getElementById('btn-refresh-tree').addEventListener('click', refreshFileTree);
    document.getElementById('btn-download-zip').addEventListener('click', handleDownloadZip);
    setupSidebarToggles();
    document.getElementById('btn-rename').addEventListener('click', () => {
        if (!state.activeId) return;
        ui.enterRenameMode(async (newName) => {
            await renameChat(state.activeId, newName);
            await refreshChatList();
        });
    });
    document.getElementById('input').addEventListener('keydown', (ev) => {
        if ((ev.metaKey || ev.ctrlKey) && ev.key === 'Enter') {
            ev.preventDefault();
            handleSend();
        }
    });

    ui.setComposerEnabled(false);
    await refreshChatList();

    const activeId = await getActiveChatId();
    if (activeId && state.chats.find(c => c.id === activeId)) {
        await handleSelectChat(activeId);
    } else if (state.chats.length > 0) {
        await handleSelectChat(state.chats[0].id);
    } else {
        ui.showEmptyConversation('No chats yet.', { label: 'Create chat', onClick: handleNewChat });
    }

    // If handleSelectChat didn't already open settings (e.g. because the
    // chat's config had a model), verify the server is actually responsive.
    // If no chat was loaded at all, fall back to the stored defaults.
    if (!ui.isModalOpen()) {
        const config = state.session?.config || await loadDefaults();
        if (!config.model) {
            await handleOpenConfig('Configure your LLM server to get started.');
        } else {
            const issue = await probeServerHealth(config);
            if (issue) await handleOpenConfig(issue);
        }
    }
}

async function probeServerHealth(config) {
    try {
        const { models, error } = await probeModels(config.apiBaseUrl, config.apiKey);
        if (error) return `Server at ${config.apiBaseUrl} not responding: ${error}`;
        if (models.length === 0) return `Server at ${config.apiBaseUrl} has no models available.`;
        return null;
    } catch (e) {
        return `Server at ${config.apiBaseUrl} not responding: ${e.message}`;
    }
}

init().catch(err => {
    console.error('gabcode init failed:', err);
    ui.setStatus(`Init failed: ${err.message}`);
});
