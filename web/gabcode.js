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
        ui.showEmptyConversation('Select or create a chat to start.');
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
                ui.renderToolResult(true, inner);
            } else {
                ui.renderUserMessage(m.content);
            }
        } else if (m.role === 'assistant') {
            // If the assistant message contains a tool call, split the display
            const toolIdx = m.content.indexOf('<tool>');
            if (toolIdx >= 0) {
                const preamble = m.content.slice(0, toolIdx).trim();
                if (preamble) ui.renderAssistantMessage(preamble);
                const closeIdx = m.content.indexOf('</tool>', toolIdx);
                if (closeIdx >= 0) {
                    ui.renderToolCall(m.content.slice(toolIdx, closeIdx + '</tool>'.length));
                }
            } else {
                ui.renderAssistantMessage(m.content);
            }
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

        // Seed any missing default prompt files (for chats that predate this
        // feature). Existing user edits are preserved.
        await writeDefaultPrompts(vfs);
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
            messages.push({ role: 'user', content: `<result>${r.output}</result>` });
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
    input.value = '';

    state.generating = true;
    ui.setGenerating(true);
    ui.setStatus('Generating...');

    let stream = null;
    const runner = new TurnRunner(state.session, (ev) => {
        switch (ev.type) {
            case 'user_message':
                ui.renderUserMessage(ev.content);
                break;
            case 'stream_start':
                stream = ui.beginAssistantStream();
                break;
            case 'delta':
                stream?.append(ev.content);
                break;
            case 'stream_end':
                stream?.commit();
                stream = null;
                break;
            case 'assistant_message':
                // Already streamed; replace the last streamed node with
                // final formatted markdown (the commit call above handled it).
                break;
            case 'tool_call':
                ui.renderToolCall(ev.argsRaw);
                break;
            case 'tool_result':
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
                ui.setStatus(`${state.session.config.model} — ${state.session.lastTotalTokens || 0} tokens`);
                break;
            case 'turn_cancelled':
                stream?.remove();
                ui.setStatus('Cancelled — turn rolled back.');
                renderConversation();
                break;
            case 'turn_error':
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
        ui.showEmptyConversation('Click + in the sidebar to create your first chat.');
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
