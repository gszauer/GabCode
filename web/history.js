// JSONL conversation log: append one JSON record per line to
// `.gab/history.jsonl` inside the chat's VFS. Used both for observability and
// for conversation rehydration when switching back to a prior chat.
//
// Record types:
//   {t: 'user',      content: string}
//   {t: 'assistant', content: string}
//   {t: 'tool_call', name: string, args: string}   // args pre-formatted
//   {t: 'tool_result', ok: bool, output: string}
//   {t: 'system',    content: string}              // system prompt at start of each session
//   {t: 'meta',      key: string, value: any}      // e.g. {key:'compacted', value:true}

const HISTORY_PATH = '.gab/history.jsonl';

export class History {
    constructor(vfs) {
        this.vfs = vfs;
    }

    async append(record) {
        const line = JSON.stringify({ ts: Date.now(), ...record }) + '\n';
        await this.vfs.appendFile(HISTORY_PATH, line);
    }

    async readAll() {
        const raw = await this.vfs.readFile(HISTORY_PATH);
        if (!raw) return [];
        return raw
            .split('\n')
            .filter(Boolean)
            .map(line => {
                try { return JSON.parse(line); }
                catch { return null; }
            })
            .filter(Boolean);
    }

    async clear() {
        await this.vfs.writeFile(HISTORY_PATH, '');
    }

    // Rehydrate a chronological message list for the conversation UI.
    // Tool calls and tool results appear as their own visible entries.
    async rehydrate() {
        const records = await this.readAll();
        const messages = [];
        for (const r of records) {
            if (r.t === 'user') {
                messages.push({ role: 'user', content: r.content });
            } else if (r.t === 'assistant') {
                messages.push({ role: 'assistant', content: r.content });
            } else if (r.t === 'tool_call') {
                messages.push({ role: 'tool_call', name: r.name, args: r.args });
            } else if (r.t === 'tool_result') {
                messages.push({ role: 'tool_result', ok: r.ok, output: r.output });
            } else if (r.t === 'system') {
                messages.push({ role: 'system', content: r.content });
            }
        }
        return messages;
    }
}
