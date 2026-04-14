// DOM renderers, modals, and a minimal inline-markdown pass for assistant text.
// Kept stateless where possible: render functions take data, update the DOM,
// return nothing.

// ── Tiny markdown renderer (paragraphs, code blocks, inline code, bold/italic, links) ──

function escapeHtml(s) {
    return String(s)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;');
}

function renderInline(text) {
    // Inline code first (so ** inside `code` is preserved)
    let out = escapeHtml(text);
    out = out.replace(/`([^`]+)`/g, (_, c) => `<code>${c}</code>`);
    out = out.replace(/\*\*([^*]+)\*\*/g, (_, c) => `<strong>${c}</strong>`);
    out = out.replace(/(^|[^*])\*([^*]+)\*/g, (_, pre, c) => `${pre}<em>${c}</em>`);
    out = out.replace(/\[([^\]]+)\]\(([^)]+)\)/g,
        (_, t, u) => `<a href="${u}" target="_blank" rel="noopener">${t}</a>`);
    return out;
}

export function renderMarkdown(text) {
    const lines = String(text).split('\n');
    const out = [];
    let inCode = false;
    let codeBuf = [];
    let paraBuf = [];

    const flushPara = () => {
        if (paraBuf.length === 0) return;
        out.push(paraBuf.map(renderInline).join('<br>'));
        paraBuf = [];
    };

    for (const line of lines) {
        if (line.startsWith('```')) {
            if (inCode) {
                out.push(`<pre><code>${escapeHtml(codeBuf.join('\n'))}</code></pre>`);
                codeBuf = [];
                inCode = false;
            } else {
                flushPara();
                inCode = true;
            }
            continue;
        }
        if (inCode) { codeBuf.push(line); continue; }
        if (line.trim() === '') { flushPara(); continue; }
        paraBuf.push(line);
    }
    if (inCode) out.push(`<pre><code>${escapeHtml(codeBuf.join('\n'))}</code></pre>`);
    flushPara();
    return out.join('');
}

// ── Sidebar ─────────────────────────────────────────────────

export function renderChatList(chats, activeId, handlers) {
    const el = document.getElementById('chat-list');
    el.innerHTML = '';
    if (chats.length === 0) {
        const empty = document.createElement('div');
        empty.className = 'sidebar-empty';
        empty.textContent = 'No chats yet. Click + to create one.';
        el.appendChild(empty);
        return;
    }
    for (const c of chats) {
        const item = document.createElement('div');
        item.className = 'chat-item' + (c.id === activeId ? ' active' : '');
        item.dataset.id = c.id;

        const name = document.createElement('span');
        name.className = 'chat-item-name';
        name.textContent = c.name;
        item.appendChild(name);

        const del = document.createElement('button');
        del.className = 'chat-item-delete';
        del.textContent = '×';
        del.title = 'Delete chat';
        del.addEventListener('click', ev => {
            ev.stopPropagation();
            handlers.onDelete(c.id, c.name);
        });
        item.appendChild(del);

        item.addEventListener('click', () => handlers.onSelect(c.id));
        el.appendChild(item);
    }
}

// ── Conversation ────────────────────────────────────────────

export function setConvTitle(title, meta) {
    document.getElementById('conv-title').textContent = title || 'No chat selected';
    document.getElementById('conv-meta').textContent = meta || '';
    document.getElementById('btn-rename').style.display = title ? '' : 'none';
}

// Inline rename: swap the #conv-title h2 for a text input. Enter or blur
// commits (if non-empty and changed); Escape cancels. `onCommit(newName)`
// is invoked with the trimmed value only when the rename should persist.
export function enterRenameMode(onCommit) {
    const titleEl = document.getElementById('conv-title');
    const current = titleEl.textContent;
    const input = document.createElement('input');
    input.type = 'text';
    input.value = current;
    input.className = 'rename-input';
    titleEl.style.display = 'none';
    titleEl.parentNode.insertBefore(input, titleEl);
    input.focus();
    input.select();

    let done = false;
    const finish = async (commit) => {
        if (done) return;
        done = true;
        const next = input.value.trim();
        input.remove();
        titleEl.style.display = '';
        if (commit && next && next !== current) {
            titleEl.textContent = next;
            await onCommit(next);
        }
    };
    input.addEventListener('blur', () => finish(true));
    input.addEventListener('keydown', (ev) => {
        if (ev.key === 'Enter') { ev.preventDefault(); input.blur(); }
        else if (ev.key === 'Escape') { ev.preventDefault(); finish(false); }
    });
}

export function clearMessages() {
    const el = document.getElementById('messages');
    el.innerHTML = '';
}

export function showEmptyConversation(text) {
    const el = document.getElementById('messages');
    el.innerHTML = '';
    const empty = document.createElement('div');
    empty.className = 'empty-state';
    empty.textContent = text;
    el.appendChild(empty);
}

function scrollToBottom() {
    const el = document.getElementById('messages');
    el.scrollTop = el.scrollHeight;
}

function appendMessageNode(node) {
    const el = document.getElementById('messages');
    // Remove empty-state placeholder if present
    const empty = el.querySelector('.empty-state');
    if (empty) empty.remove();
    el.appendChild(node);
    scrollToBottom();
}

function messageShell(roleClass, roleLabel) {
    const wrapper = document.createElement('div');
    wrapper.className = `msg ${roleClass}`;
    const label = document.createElement('div');
    label.className = 'msg-role';
    label.textContent = roleLabel;
    const body = document.createElement('div');
    body.className = 'msg-body';
    wrapper.appendChild(label);
    wrapper.appendChild(body);
    return { wrapper, body };
}

export function renderUserMessage(content) {
    const { wrapper, body } = messageShell('user', 'You');
    body.textContent = content;
    appendMessageNode(wrapper);
}

export function renderAssistantMessage(content) {
    const { wrapper, body } = messageShell('assistant', 'Assistant');
    body.innerHTML = renderMarkdown(content);
    appendMessageNode(wrapper);
}

// For streaming — returns a handle with .append(delta) and .commit(finalText).
export function beginAssistantStream() {
    const { wrapper, body } = messageShell('assistant', 'Assistant');
    body.textContent = '';
    appendMessageNode(wrapper);
    let buf = '';
    return {
        append(delta) {
            buf += delta;
            body.textContent = buf;
            scrollToBottom();
        },
        commit(finalText) {
            body.innerHTML = renderMarkdown(finalText ?? buf);
            scrollToBottom();
        },
        remove() {
            wrapper.remove();
        },
    };
}

export function renderToolCall(raw) {
    const { wrapper, body } = messageShell('tool-call', 'Tool call');
    body.textContent = raw;
    appendMessageNode(wrapper);
}

export function renderToolResult(ok, output) {
    const { wrapper, body } = messageShell(ok ? 'tool-result' : 'tool-result error', ok ? 'Result' : 'Error');
    body.textContent = output || '(empty)';
    appendMessageNode(wrapper);
}

export function renderSystemNote(text) {
    const { wrapper, body } = messageShell('system', 'System');
    body.textContent = text;
    appendMessageNode(wrapper);
}

// ── File tree ───────────────────────────────────────────────

export async function renderFileTree(vfs, onChange) {
    const root = document.getElementById('tree-root');
    root.innerHTML = '';
    const build = async (path, container) => {
        const entries = await vfs.listDir(path);
        if (entries.length === 0 && path === '') {
            const empty = document.createElement('div');
            empty.className = 'tree-empty';
            empty.textContent = '(empty)';
            container.appendChild(empty);
            return;
        }
        for (const e of entries) {
            const full = path ? `${path}/${e.name}` : e.name;
            const node = document.createElement('div');
            node.className = `tree-node ${e.isDir ? 'dir' : 'file'}`;
            const icon = document.createElement('span');
            icon.className = 'tree-icon';
            icon.textContent = e.isDir ? '▸' : '·';
            node.appendChild(icon);
            const label = document.createElement('span');
            label.textContent = e.name;
            node.appendChild(label);
            container.appendChild(node);

            if (e.isDir) {
                const children = document.createElement('div');
                children.className = 'tree-children';
                children.style.display = 'none';
                container.appendChild(children);
                let loaded = false;
                node.addEventListener('click', async () => {
                    const open = children.style.display !== 'none';
                    children.style.display = open ? 'none' : '';
                    icon.textContent = open ? '▸' : '▾';
                    if (!loaded) { loaded = true; await build(full, children); }
                });
            } else {
                node.addEventListener('click', () => openFileModal(vfs, full, onChange));
            }
        }
    };
    await build('', root);
}

// ── Modals ──────────────────────────────────────────────────

function mountTemplate(id) {
    const tpl = document.getElementById(id);
    const frag = tpl.content.cloneNode(true);
    const root = document.getElementById('modal-root');
    root.appendChild(frag);
    return root.lastElementChild;
}

function unmountModal(node) {
    node.remove();
}

export function isModalOpen() {
    return document.getElementById('modal-root').children.length > 0;
}

export function openConfigModal({ initial, initialError, onSave, onCancel, onReset }) {
    const backdrop = mountTemplate('tpl-config-modal');
    const modal = backdrop.querySelector('.modal');
    const get = name => modal.querySelector(`[name="${name}"]`);

    get('apiBaseUrl').value = initial.apiBaseUrl || '';
    get('apiKey').value = initial.apiKey || '';
    get('maxContextTokens').value = initial.maxContextTokens || '';
    get('braveApiKey').value = initial.braveApiKey || '';
    get('maxToolCallsPerTurn').value = initial.maxToolCallsPerTurn ?? 10;
    const modelSelect = get('model');
    if (initial.model) {
        const opt = document.createElement('option');
        opt.value = initial.model;
        opt.textContent = initial.model;
        opt.selected = true;
        modelSelect.insertBefore(opt, modelSelect.firstChild);
    }

    const errEl = get('error');
    const setErr = msg => { errEl.textContent = msg || ''; };
    if (initialError) setErr(initialError);

    get('probe').addEventListener('click', async () => {
        setErr('Probing...');
        const { probeModels } = await import('./config.js');
        const { models, error } = await probeModels(get('apiBaseUrl').value, get('apiKey').value);
        if (error) { setErr(error); return; }
        if (models.length === 0) { setErr('No models found.'); return; }
        modelSelect.innerHTML = '';
        for (const m of models) {
            const opt = document.createElement('option');
            opt.value = m.id;
            opt.textContent = m.id;
            opt.dataset.contextLength = m.contextLength || 0;
            modelSelect.appendChild(opt);
        }
        // Auto-fill context for the default-selected model
        const first = modelSelect.options[0];
        if (first?.dataset?.contextLength) {
            get('maxContextTokens').value = first.dataset.contextLength;
        }
        setErr(`${models.length} model(s) available.`);
    });

    modelSelect.addEventListener('change', () => {
        const opt = modelSelect.options[modelSelect.selectedIndex];
        if (opt?.dataset?.contextLength) {
            get('maxContextTokens').value = opt.dataset.contextLength;
        }
    });

    get('probeContext').addEventListener('click', async () => {
        const modelId = modelSelect.value;
        if (!modelId) { setErr('Pick a model first.'); return; }
        setErr('Probing context length...');
        const { probeModels } = await import('./config.js');
        const { models, error } = await probeModels(get('apiBaseUrl').value, get('apiKey').value);
        if (error) { setErr(error); return; }
        const match = models.find(m => m.id === modelId);
        if (!match) { setErr(`Model "${modelId}" not in server list.`); return; }
        if (!match.contextLength) {
            setErr(`Server did not report a context length for "${modelId}".`);
            return;
        }
        get('maxContextTokens').value = match.contextLength;
        setErr(`Context: ${match.contextLength} tokens.`);
    });

    get('save').addEventListener('click', () => {
        const next = {
            ...initial,
            apiBaseUrl: get('apiBaseUrl').value,
            apiKey: get('apiKey').value,
            model: modelSelect.value,
            maxContextTokens: parseInt(get('maxContextTokens').value, 10) || 0,
            braveApiKey: get('braveApiKey').value,
            maxToolCallsPerTurn: parseInt(get('maxToolCallsPerTurn').value, 10) || 10,
        };
        if (!next.model) { setErr('Pick a model (click Probe first).'); return; }
        unmountModal(backdrop);
        onSave(next);
    });

    get('cancel').addEventListener('click', () => {
        unmountModal(backdrop);
        onCancel?.();
    });

    get('reset').addEventListener('click', () => {
        unmountModal(backdrop);
        onReset?.();
    });
}

export function openSwitchModal({ onConfirm, onCancel }) {
    const backdrop = mountTemplate('tpl-switch-modal');
    const modal = backdrop.querySelector('.modal');
    modal.querySelector('[name="confirm"]').addEventListener('click', () => {
        unmountModal(backdrop); onConfirm();
    });
    modal.querySelector('[name="cancel"]').addEventListener('click', () => {
        unmountModal(backdrop); onCancel?.();
    });
}

export function openConfirmModal({ title, body, onConfirm, onCancel }) {
    const backdrop = mountTemplate('tpl-confirm-modal');
    const modal = backdrop.querySelector('.modal');
    modal.querySelector('[name="title"]').textContent = title;
    modal.querySelector('[name="body"]').textContent = body;
    modal.querySelector('[name="confirm"]').addEventListener('click', () => {
        unmountModal(backdrop); onConfirm();
    });
    modal.querySelector('[name="cancel"]').addEventListener('click', () => {
        unmountModal(backdrop); onCancel?.();
    });
}

async function openFileModal(vfs, path, onSaved) {
    const backdrop = mountTemplate('tpl-file-modal');
    const modal = backdrop.querySelector('.modal');
    modal.querySelector('[name="title"]').textContent = path;
    const textarea = modal.querySelector('[name="content"]');
    const errEl = modal.querySelector('[name="error"]');
    const content = await vfs.readFile(path);
    if (content === null) {
        textarea.value = '';
        textarea.disabled = true;
        errEl.textContent = '(could not read — file may be missing)';
    } else {
        textarea.value = content;
    }

    modal.querySelector('[name="save"]').addEventListener('click', async () => {
        try {
            await vfs.writeFile(path, textarea.value);
            unmountModal(backdrop);
            onSaved?.();
        } catch (err) {
            errEl.textContent = `Save failed: ${err.message}`;
        }
    });
    modal.querySelector('[name="cancel"]').addEventListener('click', () => {
        unmountModal(backdrop);
    });
}

// ── Composer / status ──────────────────────────────────────

export function setComposerEnabled(enabled) {
    document.getElementById('input').disabled = !enabled;
    document.getElementById('btn-send').disabled = !enabled;
}

export function setGenerating(generating) {
    document.getElementById('btn-stop').style.display = generating ? '' : 'none';
    document.getElementById('btn-send').style.display = generating ? 'none' : '';
    document.getElementById('input').disabled = generating;
}

export function setStatus(text) {
    document.getElementById('status').textContent = text || '';
}
