// Streaming OpenAI-compatible chat client with tool-call dispatch, loop-break
// guard, cancellation, and rollback-on-cancel semantics — the browser-side
// equivalent of the core's Session::send().
//
// On cancel, messages appended during the in-flight turn are reverted to the
// snapshot taken at turn start, so the user sees a clean "cancelled" state.

import { findToolCall, formatResult, callFingerprint } from './parser.js';
import { findTool, renderToolsForSystemPrompt } from './tools.js';
import { readSystemPrompt } from './prompts.js';
import { shouldCompact, compact } from './agents.js';

// Substitutes {{TOOLS}} and {{SKILLS}} in the user's system-prompt template.
// Available skills (from SkillLoader.generateSummaries) are listed; loaded
// skill bodies are injected as separate system messages at API-call time.
export function buildSystemPrompt(template, availableSkillsSummary) {
    return template
        .replace('{{TOOLS}}', renderToolsForSystemPrompt())
        .replace('{{SKILLS}}', availableSkillsSummary);
}

// Build the full API message list for a session: system prompt (with
// {{TOOLS}}/{{SKILLS}} substituted), each loaded skill as its own system
// message, then the user/assistant history. Re-reads the prompt file each
// call so edits in the UI take effect immediately.
export async function buildApiMessages(session) {
    const template = await readSystemPrompt(session.vfs);
    const skillsSummary = session.skills ? session.skills.generateSummaries() : '(no skills available)\n';
    const system = buildSystemPrompt(template, skillsSummary);
    const out = [{ role: 'system', content: system }];
    for (const [name, body] of session.loadedSkills) {
        out.push({ role: 'system', content: `### Active Skill: ${name}\n\n${body}` });
    }
    for (const m of session.messages) {
        if (m.role === 'user' || m.role === 'assistant') {
            out.push({ role: m.role, content: m.content });
        }
    }
    return out;
}

// Exact-count probe (mirrors core/compactor.cpp::probe_token_count). Fires a
// max_tokens=1 request so the server returns a fresh `prompt_tokens` count
// that reflects the live message list, not the stale last-turn total.
// Returns 0 on any failure — the caller treats that as "skip compaction".
export async function probeTokenCount(session, signal) {
    const messages = await buildApiMessages(session);
    const body = JSON.stringify({
        model: session.config.model,
        max_tokens: 1,
        stream: false,
        messages,
    });
    try {
        const r = await fetch(`${session.config.apiBaseUrl}/chat/completions`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                ...(session.config.apiKey ? { 'Authorization': `Bearer ${session.config.apiKey}` } : {}),
            },
            body,
            signal,
        });
        if (!r.ok) return 0;
        const data = await r.json();
        return data.usage?.prompt_tokens || 0;
    } catch (err) {
        if (err.name === 'AbortError') throw err;
        return 0;
    }
}

export class TurnRunner {
    constructor(session, emit) {
        this.session = session;
        this.emit = emit;          // (event) => void
        this.abort = null;
    }

    cancel() {
        if (this.abort) this.abort.abort();
    }

    async run(userInput) {
        const s = this.session;
        const snapshot = s.messages.length;
        const historySnapshotLen = (await s.history.readAll()).length;

        // Two-stage compaction check before appending the new user turn:
        //   1. Rough gate: does last-known usage + reserve overflow the window?
        //   2. Exact probe: max_tokens=1 call to get fresh `prompt_tokens`.
        //      Only compact if remaining < reserve after that exact count.
        // Matches core/session.cpp::check_and_compact behavior.
        if (shouldCompact(s.config, s.lastTotalTokens || 0)) {
            try {
                this.abort = new AbortController();
                s.currentAbortSignal = this.abort.signal;
                const exact = await probeTokenCount(s, this.abort.signal);
                const reserve = s.config.reserveTokens
                    || Math.floor(s.config.maxContextTokens / 10);
                const remaining = s.config.maxContextTokens > exact
                    ? s.config.maxContextTokens - exact : 0;
                if (exact > 0 && remaining < reserve) {
                    this.emit({ type: 'compacting' });
                    const ok = await compact(s, this.abort.signal);
                    this.emit({ type: 'compact_done', ok });
                    if (ok) s.lastTotalTokens = 0;
                }
            } catch (err) {
                if (err.name === 'AbortError') throw err;
                this.emit({ type: 'compact_failed', error: err.message });
            } finally {
                this.abort = null;
            }
        }

        // 1. Append the user turn.
        s.messages.push({ role: 'user', content: userInput });
        await s.history.append({ t: 'user', content: userInput });
        this.emit({ type: 'user_message', content: userInput });

        let toolCallCount = 0;
        let lastFingerprint = null;

        try {
            while (true) {
                if (toolCallCount >= s.config.maxToolCallsPerTurn) {
                    const note = `(tool-call limit of ${s.config.maxToolCallsPerTurn} reached; ending turn)`;
                    s.messages.push({ role: 'assistant', content: note });
                    await s.history.append({ t: 'assistant', content: note });
                    this.emit({ type: 'assistant_message', content: note });
                    break;
                }

                this.emit({ type: 'turn_step_start' });

                const assistantText = await this.streamAssistant();
                if (assistantText === null) break;  // cancelled

                const tc = findToolCall(assistantText);

                // (a) No tool tag at all → model is done; final answer.
                if (tc === null) {
                    s.messages.push({ role: 'assistant', content: assistantText });
                    await s.history.append({ t: 'assistant', content: assistantText });
                    this.emit({ type: 'assistant_message', content: assistantText });
                    break;
                }

                // (b) Malformed tool call → surface the parse error back to the
                //     model as a <result> and let it try again. Matches the
                //     CLI's agent_runner.cpp ParseError branch.
                if (tc.parseError) {
                    s.messages.push({ role: 'assistant', content: assistantText });
                    await s.history.append({ t: 'assistant', content: assistantText });
                    this.emit({ type: 'assistant_message', content: assistantText });

                    const errMsg = `parse error: ${tc.parseError}`;
                    s.messages.push({ role: 'user', content: formatResult(errMsg), isError: true });
                    await s.history.append({ t: 'tool_result', ok: false, output: errMsg });
                    this.emit({ type: 'tool_result', ok: false, output: errMsg });
                    ++toolCallCount;
                    continue;
                }

                // (c) Valid call — preserve any preamble text + the tool call
                //     itself in the assistant message.
                const assistantForLog = assistantText.slice(0, tc.end);
                s.messages.push({ role: 'assistant', content: assistantForLog });
                await s.history.append({ t: 'assistant', content: assistantForLog });
                this.emit({ type: 'assistant_message', content: assistantForLog });

                // Loop-break guard.
                const fp = callFingerprint(tc.name, tc.args);
                if (fp === lastFingerprint) {
                    const msg = 'DUPLICATE TOOL EXECUTION — ending turn.';
                    const resultMsg = { role: 'user', content: formatResult(msg), isError: true };
                    s.messages.push(resultMsg);
                    await s.history.append({ t: 'tool_result', ok: false, output: msg });
                    this.emit({ type: 'tool_result', ok: false, output: msg });
                    break;
                }
                lastFingerprint = fp;

                // Dispatch the tool.
                this.emit({ type: 'tool_call', name: tc.name, argsRaw: tc.raw });
                await s.history.append({
                    t: 'tool_call',
                    name: tc.name,
                    args: tc.raw,
                });

                const tool = findTool(tc.name);
                let result;
                if (!tool) {
                    result = { ok: false, output: `unknown tool: ${tc.name}` };
                } else {
                    try {
                        result = await tool.invoke(s, tc.args);
                    } catch (e) {
                        result = { ok: false, output: `tool threw: ${e.message}` };
                    }
                }

                const resultBlock = formatResult(result.output);
                s.messages.push({ role: 'user', content: resultBlock, isError: !result.ok });
                await s.history.append({ t: 'tool_result', ok: result.ok, output: result.output });
                this.emit({ type: 'tool_result', ok: result.ok, output: result.output });

                ++toolCallCount;
            }

            this.emit({ type: 'turn_done' });
        } catch (err) {
            if (err.name === 'AbortError') {
                // Rollback: truncate messages and history file back to pre-turn state.
                s.messages.length = snapshot;
                await this.truncateHistory(historySnapshotLen);
                this.emit({ type: 'turn_cancelled' });
            } else {
                this.emit({ type: 'turn_error', error: err.message });
                throw err;
            }
        } finally {
            this.abort = null;
        }
    }

    async truncateHistory(lineCount) {
        const s = this.session;
        const all = await s.history.readAll();
        const kept = all.slice(0, lineCount);
        const rebuilt = kept.map(r => JSON.stringify(r)).join('\n') + (kept.length ? '\n' : '');
        await s.history.vfs.writeFile('.gab/history.jsonl', rebuilt);
    }

    // Streams one assistant response from the LLM. Stops on </tool> (for a
    // tool call) or natural completion. Emits delta events as tokens arrive.
    // Returns the full assistant text, or null if cancelled.
    async streamAssistant() {
        const s = this.session;
        const url = `${s.config.apiBaseUrl}/chat/completions`;
        const messages = await buildApiMessages(s);
        const body = JSON.stringify({
            model: s.config.model,
            messages,
            stream: true,
            stream_options: { include_usage: true },
            temperature: s.config.temperature,
            top_p: s.config.topP,
            stop: ['</tool>'],
        });

        this.abort = new AbortController();
        s.currentAbortSignal = this.abort.signal;
        let r;
        try {
            r = await fetch(url, {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                    ...(s.config.apiKey ? { 'Authorization': `Bearer ${s.config.apiKey}` } : {}),
                },
                body,
                signal: this.abort.signal,
            });
        } catch (err) {
            if (err.name === 'AbortError') throw err;
            throw new Error(`LLM request failed: ${err.message}`);
        }

        if (!r.ok) {
            const text = await r.text().catch(() => '');
            throw new Error(`LLM HTTP ${r.status}: ${text.slice(0, 500)}`);
        }

        const reader = r.body.getReader();
        const decoder = new TextDecoder();
        let buffer = '';
        let assistantText = '';
        let stoppedOnToolClose = false;
        let inThink = false;  // true while a synthetic <think> block is open

        // Some OpenAI-compatible servers surface reasoning on a separate delta
        // field (OpenRouter: `reasoning`; DeepSeek: `reasoning_content`) rather
        // than inlining `<think>` tags in `content`. Wrap those chunks in
        // `<think>...</think>` so the UI parser treats them the same way.
        const pushDelta = (text, { reasoning }) => {
            if (reasoning && !inThink) {
                assistantText += '<think>';
                this.emit({ type: 'delta', content: '<think>' });
                inThink = true;
            } else if (!reasoning && inThink) {
                assistantText += '</think>';
                this.emit({ type: 'delta', content: '</think>' });
                inThink = false;
            }
            assistantText += text;
            this.emit({ type: 'delta', content: text });
        };

        this.emit({ type: 'stream_start' });

        while (true) {
            let chunk;
            try { chunk = await reader.read(); }
            catch (err) { if (err.name === 'AbortError') throw err; throw err; }
            if (chunk.done) break;
            buffer += decoder.decode(chunk.value, { stream: true });

            // Process complete SSE events (delimited by blank lines)
            let idx;
            while ((idx = buffer.indexOf('\n\n')) >= 0) {
                const evt = buffer.slice(0, idx);
                buffer = buffer.slice(idx + 2);
                for (const line of evt.split('\n')) {
                    if (!line.startsWith('data:')) continue;
                    const payload = line.slice(5).trim();
                    if (payload === '[DONE]') continue;
                    try {
                        const json = JSON.parse(payload);
                        const choice = json.choices?.[0];
                        const d = choice?.delta;
                        const reasoning = d?.reasoning ?? d?.reasoning_content;
                        if (reasoning) pushDelta(reasoning, { reasoning: true });
                        if (d?.content) pushDelta(d.content, { reasoning: false });
                        if (choice?.finish_reason === 'stop') {
                            stoppedOnToolClose = true;
                        }
                        if (json.usage?.total_tokens) {
                            s.lastTotalTokens = json.usage.total_tokens;
                            this.emit({ type: 'usage', tokens: json.usage.total_tokens });
                        }
                    } catch { /* skip malformed lines */ }
                }
            }
        }

        // Close any still-open synthetic think block (stream ended while the
        // server was only sending reasoning chunks).
        if (inThink) {
            assistantText += '</think>';
            this.emit({ type: 'delta', content: '</think>' });
            inThink = false;
        }

        // If the stop sequence was hit, the server will have consumed `</tool>`
        // before sending [DONE]; add it back so the parser finds the full tag.
        if (stoppedOnToolClose && assistantText.includes('<tool>') && !assistantText.includes('</tool>')) {
            assistantText += '</tool>';
        }

        this.emit({ type: 'stream_end' });
        return assistantText;
    }
}
