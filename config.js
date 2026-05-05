// Chat configuration: stored as `.gab/config.json` inside each chat's VFS, and
// as a global default at `defaults/config.json` in the root VFS so new chats
// inherit the last-used settings.

import { rootVfs } from './vfs.js';

const DEFAULTS_PATH = 'defaults/config.json';
const CONFIG_PATH = '.gab/config.json';

export const DEFAULT_CONFIG = {
    apiBaseUrl: 'http://localhost:1234/v1',
    apiKey: '',
    model: '',
    maxContextTokens: 0, // 0 = auto-detect / unlimited
    reserveTokens: 0,
    maxToolCallsPerTurn: 10,
    temperature: 0.7,
    topP: 1.0,
    braveApiKey: '',
};

function normalizeBaseUrl(raw) {
    let url = String(raw || '').trim();
    if (!url) return DEFAULT_CONFIG.apiBaseUrl;
    if (!/^https?:\/\//i.test(url)) url = 'http://' + url;
    url = url.replace(/\/+$/, '');
    if (!/\/v\d+$/i.test(url) && !/\/api\/v\d+$/i.test(url)) url += '/v1';
    return url;
}

export async function loadChatConfig(chatVfs) {
    const raw = await chatVfs.readFile(CONFIG_PATH);
    if (!raw) return null;
    try { return { ...DEFAULT_CONFIG, ...JSON.parse(raw) }; }
    catch { return null; }
}

export async function saveChatConfig(chatVfs, config) {
    const normalized = {
        ...DEFAULT_CONFIG,
        ...config,
        apiBaseUrl: normalizeBaseUrl(config.apiBaseUrl),
    };
    await chatVfs.writeFile(CONFIG_PATH, JSON.stringify(normalized, null, 2));
    await rootVfs.writeFile(DEFAULTS_PATH, JSON.stringify(normalized, null, 2));
    return normalized;
}

export async function loadDefaults() {
    const raw = await rootVfs.readFile(DEFAULTS_PATH);
    if (!raw) return { ...DEFAULT_CONFIG };
    try { return { ...DEFAULT_CONFIG, ...JSON.parse(raw) }; }
    catch { return { ...DEFAULT_CONFIG }; }
}

// Mirrors core/model_registry.cpp:extract_ctx_len — tries the five field
// names LM Studio and OpenAI-compatible servers have historically used.
function extractContextLength(m) {
    return m.loaded_context_length
        || m.max_context_length
        || m.context_length
        || m.context_window
        || m.n_ctx
        || 0;
}

// Re-implementation of the CLI's model probe. Starts with the OpenAI-style
// `/v1/models`, then falls back to LM Studio's native endpoints — both
// `/api/v1/models` (LM Studio 0.4+) and `/api/v0/models` (LM Studio 0.3) —
// which expose richer context-length metadata. Returns
// { models: [{id, contextLength?}], error? }.
export async function probeModels(apiBaseUrl, apiKey) {
    const baseUrl = normalizeBaseUrl(apiBaseUrl);
    const headers = {};
    if (apiKey) headers['Authorization'] = `Bearer ${apiKey}`;

    let models = [];
    try {
        const r = await fetch(`${baseUrl}/models`, { headers });
        if (!r.ok) throw new Error(`HTTP ${r.status}`);
        const data = await r.json();
        const list = data?.data || [];
        models = list.map(m => ({ id: m.id, contextLength: extractContextLength(m) }));
    } catch (err) {
        return { models: [], error: `Could not reach ${baseUrl}/models: ${err.message}` };
    }

    // LM Studio native API fallbacks (post-0.4 exposes /api/v1/models,
    // 0.3 uses /api/v0/models). Either may report loaded_context_length
    // when the OpenAI-compat endpoint omits it.
    const lmBase = baseUrl.replace(/\/v\d+\/?$/, '');
    for (const nativeBase of [`${lmBase}/api/v1`, `${lmBase}/api/v0`]) {
        try {
            const r = await fetch(`${nativeBase}/models`, { headers });
            if (!r.ok) continue;
            const data = await r.json();
            const list = data?.data || (data?.id ? [data] : []);
            for (const m of list) {
                const ctx = extractContextLength(m);
                if (!ctx) continue;
                const match = models.find(x => x.id === m.id);
                if (match) {
                    if (!match.contextLength) match.contextLength = ctx;
                } else {
                    models.push({ id: m.id, contextLength: ctx });
                }
            }
        } catch { /* try next */ }
    }

    return { models };
}

export { normalizeBaseUrl };
