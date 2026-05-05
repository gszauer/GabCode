// Sub-agents: web_search, explore, compactor. Mirrors core/agent_runner.cpp
// and core/compactor.cpp. Each agent runs its own turn loop with a restricted
// tool set and its own system prompt (loaded from `.gab/prompts/<name>.md`),
// and returns its final text response.

import { findToolCall, formatResult } from './parser.js';
import { findTool } from './tools.js';
import { readAgentPrompt } from './prompts.js';

// Agent registry — mirrors Session::register_agents() in core/session.cpp.
// availableCheck(config) returns {ok, reason}. If not ok, the agent tool
// refuses to invoke and surfaces `reason` back to the model.
export const AGENT_DEFS = {
    web_search: {
        name: 'web_search',
        description: 'Search the web and summarize results.',
        allowedTools: ['braveSearch', 'webFetch'],
        maxTurns: 12,
        availableCheck: (config) => config.braveApiKey
            ? { ok: true }
            : { ok: false, reason: 'No Brave Search API key configured. Open Settings to add one.' },
    },
    explore: {
        name: 'explore',
        description: 'Explore the codebase to answer questions.',
        allowedTools: ['grep', 'grepIn', 'readFile'],
        maxTurns: 20,
        availableCheck: () => ({ ok: true }),
    },
    compactor: {
        name: 'compactor',
        description: 'Summarize a long conversation concisely.',
        allowedTools: [],
        maxTurns: 1,
        availableCheck: () => ({ ok: true }),
    },
};

// Non-streaming chat completion for agent turns. Streams aren't needed here
// because agent output is accumulated and returned whole.
async function callApiBlocking(config, messages, stopSequences, signal) {
    const body = JSON.stringify({
        model: config.model,
        messages,
        stream: false,
        stop: stopSequences,
    });
    const r = await fetch(`${config.apiBaseUrl}/chat/completions`, {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json',
            ...(config.apiKey ? { 'Authorization': `Bearer ${config.apiKey}` } : {}),
        },
        body,
        signal,
    });
    if (!r.ok) {
        const text = await r.text().catch(() => '');
        throw new Error(`agent API HTTP ${r.status}: ${text.slice(0, 200)}`);
    }
    const data = await r.json();
    return data.choices?.[0]?.message?.content || '';
}

// Runs an agent to completion against `parentSession` (for shared VFS and
// config). The agent gets an isolated read-set / loaded-skills map so it
// cannot mutate the parent session's state.
export async function runAgent(defName, task, parentSession, signal) {
    const def = AGENT_DEFS[defName];
    if (!def) return `[unknown agent: ${defName}]`;

    const avail = def.availableCheck(parentSession.config);
    if (!avail.ok) return `[agent "${defName}" unavailable: ${avail.reason}]`;

    const sysPrompt = await readAgentPrompt(parentSession.vfs, defName);

    // Isolated session context — tools that touch readSet/loadedSkills here
    // must not leak state into the parent conversation.
    const agentSession = {
        vfs: parentSession.vfs,
        config: parentSession.config,
        readSet: new Set(),
        loadedSkills: new Map(),
        touchedFiles: false,
        currentAbortSignal: signal,
    };

    const messages = [
        { role: 'system', content: sysPrompt },
        { role: 'user', content: String(task) },
    ];

    for (let turn = 0; turn < def.maxTurns; ++turn) {
        let assistantText;
        try {
            assistantText = await callApiBlocking(
                agentSession.config, messages, ['</tool>'], signal
            );
        } catch (err) {
            if (err.name === 'AbortError') return '[agent aborted]';
            return `[agent error: ${err.message}]`;
        }

        if (!assistantText) return '[agent received empty response]';

        // Re-append stripped stop sequence so parser finds the full tag
        if (assistantText.includes('<tool>') && !assistantText.includes('</tool>')) {
            assistantText += '</tool>';
        }

        const tc = findToolCall(assistantText);

        // No tool tag at all → agent is done.
        if (tc === null) return assistantText;

        // Malformed tool call → feed the parse error back, let the agent retry.
        if (tc.parseError) {
            messages.push({ role: 'assistant', content: assistantText });
            messages.push({
                role: 'user',
                content: formatResult(`parse error: ${tc.parseError}`),
            });
            continue;
        }

        // Tool-allowlist check
        if (!def.allowedTools.includes(tc.name)) {
            messages.push({ role: 'assistant', content: assistantText });
            messages.push({
                role: 'user',
                content: formatResult(`tool not available to this agent: ${tc.name}`),
            });
            continue;
        }

        const tool = findTool(tc.name);
        let result;
        if (!tool) {
            result = { ok: false, output: `unknown tool: ${tc.name}` };
        } else {
            try {
                result = await tool.invoke(agentSession, tc.args);
            } catch (e) {
                result = { ok: false, output: `tool threw: ${e.message}` };
            }
        }

        messages.push({ role: 'assistant', content: assistantText });
        messages.push({ role: 'user', content: formatResult(result.output) });
    }

    return `[agent exceeded maximum turns (${def.maxTurns})]`;
}

// Flattens the conversation for the compactor into "[role] content\n\n" form.
// Skips system messages and any skill-injected messages.
function serializeForCompactor(messages) {
    let out = '';
    for (const m of messages) {
        if (m.role === 'system') continue;
        if (m.isSkill) continue;
        if (m.role !== 'user' && m.role !== 'assistant') continue;
        out += `[${m.role}] ${m.content}\n\n`;
    }
    return out;
}

// Compact the conversation: runs the compactor agent on the transcript and
// rebuilds `messages` in place as [system, assistant: summary, latest-user].
// Returns true on success.
export async function compact(parentSession, signal) {
    const transcript = serializeForCompactor(parentSession.messages);
    if (!transcript) return false;

    const summary = await runAgent('compactor', transcript, parentSession, signal);
    if (!summary || summary.startsWith('[agent')) return false;

    const lastUserIdx = [...parentSession.messages].reverse()
        .findIndex(m => m.role === 'user');
    const lastUser = lastUserIdx >= 0
        ? parentSession.messages[parentSession.messages.length - 1 - lastUserIdx]
        : null;

    parentSession.messages = [
        { role: 'assistant', content: `[Conversation compacted]\n\n${summary}` },
    ];
    if (lastUser) parentSession.messages.push(lastUser);

    // Match CLI: compaction invalidates injected skill messages (they were
    // stripped from the transcript by serializeForCompactor). The model can
    // re-load them with skill(name) if they're still relevant.
    parentSession.loadedSkills.clear();
    if (parentSession.skills) parentSession.skills.clearLoaded();
    return true;
}

// Rough check: would the previous turn's prompt tokens + reserve overflow the
// context window? If so, compaction is needed before the next turn.
export function shouldCompact(config, lastTotalTokens) {
    if (!config.maxContextTokens) return false;
    const reserve = config.reserveTokens || Math.floor(config.maxContextTokens / 10);
    if (!reserve) return false;
    return (lastTotalTokens || 0) + reserve > config.maxContextTokens;
}
