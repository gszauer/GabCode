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

export function showEmptyConversation(text, action) {
    const el = document.getElementById('messages');
    el.innerHTML = '';
    const empty = document.createElement('div');
    empty.className = 'empty-state';
    const msg = document.createElement('div');
    msg.className = 'empty-state-text';
    msg.textContent = text;
    empty.appendChild(msg);
    if (action) {
        const btn = document.createElement('button');
        btn.className = 'primary-btn neutral';
        btn.textContent = action.label;
        btn.addEventListener('click', action.onClick);
        empty.appendChild(btn);
    }
    el.appendChild(empty);
}

function scrollToBottom() {
    const el = document.getElementById('messages');
    el.scrollTop = el.scrollHeight;
}

// "Stuck to bottom" means the user hasn't scrolled up to read earlier content.
// Streaming updates only auto-scroll if this is true, so a user who scrolls
// up mid-generation stays where they are.
function isStuckToBottom(el) {
    return el.scrollHeight - el.scrollTop - el.clientHeight < 50;
}

function appendMessageNode(node, forceScroll = false) {
    const el = document.getElementById('messages');
    const empty = el.querySelector('.empty-state');
    if (empty) empty.remove();
    const shouldScroll = forceScroll || isStuckToBottom(el);
    el.appendChild(node);
    if (shouldScroll) scrollToBottom();
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
    // The user just hit send — jump to the bottom unconditionally so they
    // see their own message land.
    appendMessageNode(wrapper, true);
}

// Parse an assistant buffer into ordered segments of type 'text', 'think', or
// 'tool'. `<think>...</think>` is extracted as a separate segment and rendered
// as a blue thinking bubble; `<tool>...` consumes everything to end-of-buffer
// (tool is terminal). Each segment carries a `complete` flag so streaming code
// can tell whether the tail is still growing.
//
// For streaming, we also hold back any trailing bytes that could be the start
// of a `<think>` or `<tool>` tag — otherwise "fo" + "o<th" + "ink>..." would
// briefly render "foo<th" as plain text before reinterpreting it.
function parseAssistantSegments(buf, { streaming = false } = {}) {
    const TAG_PREFIXES = ['<think>', '<tool>'];
    const segs = [];
    let i = 0;
    let textStart = 0;

    const pushText = (from, to, complete) => {
        if (to > from) segs.push({ type: 'text', content: buf.slice(from, to), complete });
    };

    while (i < buf.length) {
        if (buf.startsWith('<think>', i)) {
            pushText(textStart, i, true);
            const end = buf.indexOf('</think>', i + 7);
            if (end < 0) {
                segs.push({ type: 'think', content: buf.slice(i + 7), complete: false });
                return segs;
            }
            segs.push({ type: 'think', content: buf.slice(i + 7, end), complete: true });
            i = end + '</think>'.length;
            textStart = i;
            continue;
        }
        if (buf.startsWith('<tool>', i)) {
            pushText(textStart, i, true);
            segs.push({
                type: 'tool',
                content: buf.slice(i),
                complete: buf.indexOf('</tool>', i) >= 0,
            });
            return segs;
        }
        i++;
    }

    let end = buf.length;
    if (streaming) {
        const maxHold = Math.min(6, end - textStart);
        for (let len = maxHold; len >= 1; len--) {
            const tail = buf.slice(end - len);
            if (TAG_PREFIXES.some(p => p.startsWith(tail))) {
                end -= len;
                break;
            }
        }
    }
    pushText(textStart, end, !streaming);
    return segs;
}

function createAssistantTextBubble() {
    const { wrapper, body } = messageShell('assistant', 'Assistant');
    body.textContent = '';
    return {
        type: 'text',
        wrapper,
        update(seg) {
            if (seg.complete) body.innerHTML = renderMarkdown(seg.content);
            else body.textContent = seg.content;
        },
    };
}

function createThinkingBubble() {
    const { wrapper, body } = messageShell('thinking', 'Thinking');
    body.textContent = '';
    return {
        type: 'think',
        wrapper,
        update(seg) { body.textContent = seg.content; },
    };
}

function createToolSegmentBubble(seg) {
    const bubble = createToolCallBubble(seg.content);
    return {
        type: 'tool',
        wrapper: bubble.wrapper,
        _inner: bubble,
        update(s) { bubble.update(s.content); },
        setBusy(b) { bubble.setBusy(b); },
    };
}

function makeSegmentBubble(seg) {
    if (seg.type === 'text') return createAssistantTextBubble();
    if (seg.type === 'think') return createThinkingBubble();
    return createToolSegmentBubble(seg);
}

export function renderAssistantMessage(content) {
    const segs = parseAssistantSegments(content);
    for (const seg of segs) {
        if (seg.type === 'text' && seg.content.trim() === '') continue;
        const bubble = makeSegmentBubble(seg);
        if (seg.type === 'tool') bubble.setBusy(false);
        bubble.update(seg);
        appendMessageNode(bubble.wrapper);
    }
}

// For streaming — reconciles a list of DOM bubbles against the current parse
// of the buffer. Each new `<think>` block spawns a blue thinking bubble; any
// `<tool>` flips the tail into a yellow tool-call bubble. Updates coalesce
// onto a requestAnimationFrame to keep fast token streams from thrashing
// layout.
export function beginAssistantStream() {
    const messagesEl = document.getElementById('messages');
    const bubbles = [];
    let buf = '';
    let raf = 0;

    function reconcile() {
        const segs = parseAssistantSegments(buf, { streaming: true });
        for (let i = 0; i < segs.length; i++) {
            const seg = segs[i];
            if (!bubbles[i]) {
                bubbles[i] = makeSegmentBubble(seg);
                appendMessageNode(bubbles[i].wrapper);
            }
            bubbles[i].update(seg);
        }
    }

    function scheduleReconcile() {
        if (raf) return;
        raf = requestAnimationFrame(() => { raf = 0; reconcile(); });
    }

    return {
        append(delta) {
            buf += delta;
            const stick = isStuckToBottom(messagesEl);
            scheduleReconcile();
            if (stick) scrollToBottom();
        },
        commit(finalText) {
            if (finalText != null) buf = finalText;
            const stick = isStuckToBottom(messagesEl);
            // Final parse treats everything as complete so text segments get
            // markdown-rendered instead of staying as plain textContent.
            if (raf) { cancelAnimationFrame(raf); raf = 0; }
            const segs = parseAssistantSegments(buf);
            for (let i = 0; i < segs.length; i++) {
                if (!bubbles[i]) {
                    bubbles[i] = makeSegmentBubble(segs[i]);
                    appendMessageNode(bubbles[i].wrapper);
                }
                bubbles[i].update(segs[i]);
            }
            // Drop a trailing empty-text bubble (can appear if the model
            // finishes a think block with no following prose).
            while (bubbles.length > segs.length) bubbles.pop().wrapper.remove();
            const last = segs[segs.length - 1];
            if (last?.type === 'text' && last.content.trim() === '') {
                bubbles[segs.length - 1]?.wrapper.remove();
                bubbles.length = segs.length - 1;
            }
            if (stick) scrollToBottom();
        },
        remove() {
            if (raf) { cancelAnimationFrame(raf); raf = 0; }
            for (const b of bubbles) b.wrapper.remove();
        },
        hasToolCall() { return bubbles.some(b => b.type === 'tool'); },
        getToolBubble() {
            const b = bubbles.find(b => b.type === 'tool');
            return b?._inner || null;
        },
    };
}

// Extract the tool name from a partial or complete <tool>name(args)</tool> block.
// Returns '…' if we haven't seen enough yet (e.g. just `<tool>`).
function parseToolName(raw) {
    const m = String(raw).match(/<tool>\s*(\w+)/);
    return m ? m[1] : '…';
}

// Strip the <tool>...</tool> wrapper for display in the collapsed body. The
// bubble's color and header label already convey what this is; showing the
// tags inside would be noise. Tolerates partial streams (no closing tag yet).
function stripToolTags(raw) {
    let s = String(raw);
    if (s.startsWith('<tool>')) s = s.slice('<tool>'.length);
    if (s.endsWith('</tool>')) s = s.slice(0, -'</tool>'.length);
    return s;
}

// Builds a collapsible tool-call bubble. Header shows caret + tool name +
// spinner; the full raw call lives in a <pre> that's hidden by default.
// Returns a handle so streaming code can rewrite the content as tokens arrive
// and flip the spinner off when the tool result arrives.
function createToolCallBubble(initialRaw, { busy = true } = {}) {
    const { wrapper, body } = messageShell('tool-call collapsed', 'Tool call');
    body.innerHTML = '';

    const header = document.createElement('button');
    header.type = 'button';
    header.className = 'tool-header';

    const caret = document.createElement('span');
    caret.className = 'tool-caret';
    caret.textContent = '▸';

    const nameEl = document.createElement('span');
    nameEl.className = 'tool-name';
    nameEl.textContent = parseToolName(initialRaw);

    const spinner = document.createElement('span');
    spinner.className = 'tool-spinner';
    if (!busy) spinner.classList.add('done');

    header.appendChild(caret);
    header.appendChild(nameEl);
    header.appendChild(spinner);
    body.appendChild(header);

    const rawEl = document.createElement('pre');
    rawEl.className = 'tool-raw';
    rawEl.textContent = stripToolTags(initialRaw);
    rawEl.hidden = true;
    body.appendChild(rawEl);

    header.addEventListener('click', () => {
        const willShow = rawEl.hidden;
        rawEl.hidden = !willShow;
        caret.textContent = willShow ? '▾' : '▸';
        wrapper.classList.toggle('collapsed', !willShow);
    });

    return {
        wrapper,
        update(raw) {
            rawEl.textContent = stripToolTags(raw);
            nameEl.textContent = parseToolName(raw);
        },
        setBusy(b) {
            spinner.classList.toggle('done', !b);
        },
    };
}

export function renderToolCall(raw, options = {}) {
    const bubble = createToolCallBubble(raw, options);
    appendMessageNode(bubble.wrapper);
    return bubble;
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
