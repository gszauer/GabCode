// Parses the model's tool-call syntax: `<tool>funcName("arg", "escaped \"quote\"")</tool>`
// Returns {name, args, raw, start, end} or null if no complete tool call found.

const TOOL_OPEN = '<tool>';
const TOOL_CLOSE = '</tool>';

function unescapeString(s) {
    let out = '';
    for (let i = 0; i < s.length; ++i) {
        const c = s[i];
        if (c === '\\' && i + 1 < s.length) {
            const next = s[i + 1];
            switch (next) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                case "'": out += "'"; break;
                default: out += next;
            }
            ++i;
        } else {
            out += c;
        }
    }
    return out;
}

function escapeString(s) {
    return String(s)
        .replace(/\\/g, '\\\\')
        .replace(/"/g, '\\"')
        .replace(/\n/g, '\\n')
        .replace(/\r/g, '\\r')
        .replace(/\t/g, '\\t');
}

function parseArgs(src) {
    // Parse comma-separated argument list. Each argument is either:
    //   - a JSON-style quoted string: "..." or '...'
    //   - a number: 42, -3.14
    //   - a bareword: true, false, null
    const args = [];
    let i = 0;
    const skipWs = () => { while (i < src.length && /\s/.test(src[i])) ++i; };
    while (i < src.length) {
        skipWs();
        if (i >= src.length) break;
        const c = src[i];
        if (c === '"' || c === "'" || c === '`') {
            const quote = c;
            ++i;
            let buf = '';
            while (i < src.length && src[i] !== quote) {
                if (src[i] === '\\' && i + 1 < src.length) {
                    buf += src[i] + src[i + 1];
                    i += 2;
                } else {
                    buf += src[i++];
                }
            }
            if (src[i] === quote) ++i;
            args.push({ type: 'string', value: unescapeString(buf) });
        } else if (/[-0-9]/.test(c)) {
            let buf = '';
            while (i < src.length && /[-0-9.]/.test(src[i])) buf += src[i++];
            args.push({ type: 'number', value: parseFloat(buf) });
        } else {
            let buf = '';
            while (i < src.length && /\w/.test(src[i])) buf += src[i++];
            if (buf === 'true') args.push({ type: 'bool', value: true });
            else if (buf === 'false') args.push({ type: 'bool', value: false });
            else if (buf === 'null') args.push({ type: 'null', value: null });
            else if (buf.length > 0) args.push({ type: 'ident', value: buf });
        }
        skipWs();
        if (src[i] === ',') ++i;
    }
    return args;
}

// Parse for a tool call. Return shapes:
//   null                         — no `<tool>` tag present; model is producing a final answer
//   {parseError: "..."}          — malformed tool call; feed error back to model as a <result>
//   {name, args, raw, start, end} — valid call ready to dispatch
export function findToolCall(text) {
    const start = text.indexOf(TOOL_OPEN);
    if (start < 0) return null;
    const end = text.indexOf(TOOL_CLOSE, start + TOOL_OPEN.length);
    if (end < 0) return { parseError: 'missing </tool> close tag' };

    const inner = text.slice(start + TOOL_OPEN.length, end);
    const parenOpen = inner.indexOf('(');
    const parenClose = inner.lastIndexOf(')');
    if (parenOpen < 0) return { parseError: 'missing ( in tool call' };
    if (parenClose < 0) return { parseError: 'missing ) in tool call' };
    if (parenClose < parenOpen) return { parseError: 'malformed parentheses in tool call' };

    const name = inner.slice(0, parenOpen).trim();
    if (!name) return { parseError: 'missing tool name' };

    const argsSrc = inner.slice(parenOpen + 1, parenClose);
    const args = parseArgs(argsSrc);

    return {
        name,
        args,
        raw: text.slice(start, end + TOOL_CLOSE.length),
        start,
        end: end + TOOL_CLOSE.length,
    };
}

export function formatToolCall(name, args) {
    const rendered = args.map(a => {
        if (typeof a === 'string') return `"${escapeString(a)}"`;
        if (typeof a === 'number' || typeof a === 'boolean') return String(a);
        if (a === null) return 'null';
        return `"${escapeString(String(a))}"`;
    }).join(', ');
    return `<tool>${name}(${rendered})</tool>`;
}

export function formatResult(output) {
    return `<result>${output}</result>`;
}

// Extract {name, args[]} as raw JS values (unwrapping {type,value}).
export function callFingerprint(name, args) {
    return `${name}(${args.map(a => JSON.stringify(a.value)).join(',')})`;
}
