# Gab Code — Technical Reference

This document goes deep on the architecture, the C ABI boundary, the tool-call
protocol, and the mechanics of every subsystem. It also covers how to extend
the harness (new tools, agents, skills) and how to write a brand-new shell
that embeds the core.

---

## Table of Contents

1. [Core / Shell Architecture](#core--shell-architecture)
2. [The C ABI Boundary](#the-c-abi-boundary)
3. [Session Lifecycle and Data Flow](#session-lifecycle-and-data-flow)
4. [Tool-Call Protocol](#tool-call-protocol)
5. [Loop-Break Guard](#loop-break-guard)
6. [Tool Dispatch](#tool-dispatch)
7. [Adding a Tool](#adding-a-tool)
8. [Agents](#agents)
9. [Adding an Agent](#adding-an-agent)
10. [Skills](#skills)
11. [Adding a Skill](#adding-a-skill)
12. [Compaction](#compaction)
13. [Context-Length Discovery](#context-length-discovery)
14. [Streaming and Cancellation](#streaming-and-cancellation)
15. [Slash Commands](#slash-commands)
16. [History Log](#history-log)
17. [Config Wizard](#config-wizard)
18. [Writing a New Shell](#writing-a-new-shell)
19. [File Layout Reference](#file-layout-reference)

---

## Core / Shell Architecture

```
┌──────────────────────────────────────────────┐
│  Shell  (cli/, future: wasm/, gui/)          │
│  - Terminal I/O, libcurl, POSIX files        │
│  - Implements gab_host_fns_t                 │
└──────────────────┬───────────────────────────┘
                   │  C ABI (gabcore.h)
                   ▼
┌──────────────────────────────────────────────┐
│  Core  (core/, gabcore_static.a)             │
│  - Session / conversation manager            │
│  - Prompt builder                            │
│  - Tool parser + dispatcher                  │
│  - Skill loader                              │
│  - Agent runner                              │
│  - Compactor                                 │
│  - Slash command parser                      │
│  - SSE parser + stream consumer              │
│  - Zero direct syscalls                      │
└──────────────────────────────────────────────┘
```

The **core library** (`core/`) builds as a single static lib `gabcore_static`.
It:

- Never calls `fopen`, `socket`, `time`, `popen`, etc. directly.
- Gets all I/O by calling function pointers in `gab_host_fns_t`.
- Exposes only a C ABI (`gabcore.h`) to the outside world so that any shell
  — native, WASM, FFI-from-Rust — can drive it.

The **shell** (`cli/`) is a thin layer that:

- Implements every function pointer in `gab_host_fns_t` using whatever platform
  APIs make sense (POSIX `open`/`read`/`write`, libcurl for HTTP, `popen` for
  shell, `termios` for terminal input, etc).
- Drives the core's REPL loop, handling stdin and streaming stdout.
- Handles UI concerns the core doesn't know about (SIGINT, colors, prompts).

The two sides communicate exclusively through `gab_host_fns_t` (calls from core
into the host) and the event callback `gab_event_cb` (events flowing from core
out to the shell).

---

## The C ABI Boundary

The entire cross-boundary contract lives in **`core/gabcore.h`**. The design
rules:

1. **All I/O is async-shaped.** Even when the host is synchronous, host
   functions take a callback + user-data. The core's C++ wrapper
   (`HostFunctions` in `host_fns.h`) blocks by spinning a tiny stack-local
   "result slot" structure that the callback fills. A future WASM host would
   keep the slot alive on the heap and resume from an event loop.
2. **Strings are non-owning views** (`gab_str_t { const char* data; size_t len; }`).
   Callers must keep the memory alive across the call. If data needs to outlive
   the call, the callee copies it.
3. **No exceptions cross the ABI.** All errors are reported via `gab_error_t`
   codes in `gab_result_t`.

### Key types

```c
typedef struct {
    const char* data;
    size_t      len;
} gab_str_t;

typedef enum {
    GAB_OK = 0, GAB_ERR_IO, GAB_ERR_NETWORK, GAB_ERR_PARSE,
    GAB_ERR_TIMEOUT, GAB_ERR_CANCELLED, GAB_ERR_NOT_FOUND
} gab_error_t;

typedef struct { gab_error_t error; gab_str_t data; } gab_result_t;
typedef void (*gab_result_cb)(gab_result_t result, void* user_data);
```

### The host function table

`gab_host_fns_t` (14 callbacks) is the complete list of things the core needs
the world to do for it:

| Callback | Purpose |
|---|---|
| `read_file(path, cb, ud)` | Return file contents as bytes |
| `write_file(path, content, cb, ud)` | Truncate+write |
| `append_file(path, content, cb, ud)` | Append (used by history.jsonl) |
| `delete_file(path, cb, ud)` | Remove a file |
| `file_exists(path, cb, ud)` | Result data is `"1"` or `"0"` |
| `list_dir(path, cb, ud)` | Return array of `gab_dir_entry_t` |
| `make_dir(path, cb, ud)` | Create directory (non-recursive) |
| `remove_dir(path, cb, ud)` | Remove empty directory |
| `http_request(req, cb, ud)` | Plain one-shot HTTP |
| `http_request_stream(req, cb, ud)` | SSE-capable streaming HTTP; callback returns `int` — non-zero aborts |
| `run_shell(command, cwd, cb, ud)` | Execute a shell command, return combined stdout+stderr with `[exit: N]` appended |
| `get_time_ms(void)` | Monotonic-ish milliseconds |
| `log_output(text, ud)` | Write text to the user-facing stream (stdout in CLI) |
| `prompt_input(prompt, default_val, cb, ud)` | Ask the user a question (used by `/config`) |

The core C++ wrapper `HostFunctions` (`core/host_fns.h`) turns these into
typed methods:

```cpp
GabResult<std::string> read_file(std::string_view path);
GabResult<HttpResponse> http_request(std::string_view method,
                                     std::string_view url,
                                     std::string_view body,
                                     const std::vector<std::string>& headers);
void http_request_stream(std::string_view method, ..., StreamChunkCb cb);
std::string prompt_input(std::string_view prompt, std::string_view default_val);
// ...
```

### Session API

```c
typedef struct gab_session_s* gab_session_t;

gab_session_t gab_session_create(gab_host_fns_t host, gab_str_t project_dir);
void          gab_session_destroy(gab_session_t session);
void          gab_session_send(gab_session_t session, gab_str_t input,
                               gab_event_cb cb, void* user_data);
void          gab_session_cancel(gab_session_t session);

int           gab_run_config_wizard(gab_host_fns_t host, gab_str_t project_dir);
```

`gab_session_send` drives one complete user turn — slash command handling,
compaction check, one or more model roundtrips with tool calls, and history
logging. It returns when the turn is over or cancelled. Events flow out via
`gab_event_cb`:

| Event | Data |
|---|---|
| `GAB_EVENT_TEXT_DELTA` | One token of assistant output (stream this to the user) |
| `GAB_EVENT_TOOL_START` | Name of the tool about to execute |
| `GAB_EVENT_TOOL_RESULT` | Full `"\n<result>...</result>"` block (already formatted) |
| `GAB_EVENT_TURN_END` | Turn is complete, return control to the user |
| `GAB_EVENT_ERROR` | A fatal error message |
| `GAB_EVENT_COMPACTING` | Compaction started (useful for a spinner) |

### Memory ownership rules

- `gab_str_t` inside events is valid only for the duration of the callback.
  Copy out anything you need to retain.
- The `HostFunctions` C++ wrapper copies result data from host callbacks into
  owned `std::string`s before returning.
- No allocation crosses the ABI as a pointer-to-free — everything is either a
  non-owning view or owned internally on one side or the other.

---

## Session Lifecycle and Data Flow

A single user turn runs this pipeline (`core/session.cpp` → `Session::send`):

```
1.  Input arrives                    gab_session_send(input)
2.  Slash command?                   parse_slash_command(input)
       └─ yes: handle_slash_command, emit TURN_END, return
3.  Reload config.json               reload_config()
4.  Append user message              messages_.push_back({User, input})
                                     history_->append("user", input)
5.  Rebuild system prompt            {{TOOLS}} / {{SKILLS}} substitution
6.  Compaction check                 check_and_compact()
       └─ triggers: run compactor agent, replace messages
7.  Tool-call loop:
    a. Stream API request            http_request_stream(...)
       └─ emit TEXT_DELTA per token
       └─ stop sequence: ["</tool>"]
    b. Parse assistant output        parse_tool_call(text)
    c. No tool?                      emit TURN_END, return
    d. Re-emit the stripped </tool>  emit TEXT_DELTA "</tool>\n"
    e. Build call signature          name + "|" + each arg, joined
    f. Push assistant message        messages_.push_back({Assistant, text})
    g. Duplicate guard               signature matches last iteration's?
       └─ yes: skip dispatch, emit
          "<result>DUPLICATE TOOL EXECUTION,
          BREAKING TOOL LOOP</result>",
          end turn
    h. Execute tool                  tools_.dispatch(call, *this)
    i. Push result as USER message   messages_.push_back({User, "<result>...</result>"})
       and emit TOOL_RESULT
    j. Tool-call counter check       if hit, inject "continue" notice
                                     and end turn
    k. Remember signature, loop to (a)
```

Note step (i): the tool result is stored as a **user-role** message, not
glued onto the assistant message. This keeps the conversation alternating
`user → assistant → user → assistant` — the same shape as OpenAI's native
tool-calling format — which is crucial for multi-step chains. See the
[Tool-Call Protocol](#tool-call-protocol) section below for why.

Note step (g): some models get stuck emitting the exact same tool call over
and over when confused. The signature is a trivial concatenation of the
tool name and every argument; if two consecutive calls match, the harness
short-circuits the dispatch and ends the turn. See
[Loop-Break Guard](#loop-break-guard) below.

Every assistant-side utterance from the whole turn (all tool calls + their
results + the final reply) is concatenated into one `history.jsonl` entry so
the on-disk log matches what the user saw on screen.

---

## Tool-Call Protocol

The model emits tool calls as plain text inside `<tool>...</tool>` tags:

```
Let me read that file.
<tool>readFile("src/main.cpp")</tool>
```

When the harness sends the chat completion, it passes `"stop": ["</tool>"]` in
the request body. Most OpenAI-compatible servers honour this and halt
generation at (and strip) the closing tag. The core:

1. Accumulates streamed tokens.
2. When generation ends with the stop sequence, re-appends `</tool>` to the
   accumulated text (and emits it as a `TEXT_DELTA` so the user sees a
   complete tag on screen).
3. Parses the body with `parse_tool_call` in `core/tool_parser.cpp`.
4. Dispatches via `ToolDispatcher::dispatch`.
5. Pushes the assistant's text (just the tool call) as a `Role::Assistant`
   message and the `<result>...</result>` block as a **separate
   `Role::User` message**.
6. Continues the loop — the next model call sees the updated conversation.

### Why a separate user message for `<result>`?

When the harness used to concatenate the `<result>` block onto the
assistant message, a multi-step request like "create three files" would
often stall after the first tool call. The model saw:

```
user:       create three files
assistant:  <tool>writeFile(...)</tool>\n<result>ok</result>
```

…and, asked for a new assistant message with no user message in between,
frequently returned nothing. The format looks like a completed assistant
turn, so the model has no prompt to continue.

Splitting the result into a user-role message fixes this:

```
user:       create three files
assistant:  <tool>writeFile(...)</tool>
user:       <result>ok</result>
assistant:  <-- model naturally fills this in with the next tool call
```

Now the conversation alternates cleanly and matches OpenAI's native
tool-calling shape (where tool outputs arrive as `tool`-role messages
that the model is expected to respond to). Local models and
instruction-tuned chat models both handle this well.

The `<result>` is still marked as user-role purely at the API-request
layer — the CLI still renders a single continuous assistant turn
(text → `<tool>` → `<result>` → text) and `history.jsonl` still logs
one entry per turn. The split is a conversation-shape concern, not a
display concern.

### The parser

`core/tool_parser.cpp` is a hand-written state machine (no regex, no external
dep). Four states:

- `SCAN` — looking for `<`
- `TAG_OPEN` — saw `<`, matching against `tool>`
- `BODY` — inside the tool tag, accumulating call text
- `TAG_CLOSE` — saw `<` inside body, matching against `/tool>`

End-of-input while still in `BODY` or `TAG_CLOSE` is treated as "the stop
sequence was stripped" — the accumulated body is parsed as the call text.

The inner call text is parsed as `name(arg, arg, ...)` where each arg is
either:

- A double-quoted string (`"..."`) with `\"`, `\\`, `\n`, `\t`, `\r` escapes
- A number (integer or decimal, via `std::from_chars`)

No objects, no arrays, no expressions, no variables. If the model wants
logic, it emits multiple tool calls across multiple turns.

### Return format

A `<result>` block contains the raw return value of the tool. No JSON
envelope, no header. On error the block contains the error message verbatim:

```
<result>
file not found: src/missing.cpp
</result>
```

---

## Loop-Break Guard

Weak or confused local models sometimes get stuck repeating the same tool
call instead of continuing the task. To cut those loops short, the core
tracks a **call signature** — the tool name plus every argument, joined
with `|` — across iterations of the tool-call loop within a single turn.

On every iteration:

- Successful parses produce a signature: `writeFile|hello.txt|world`,
  `grep|foo`, `readFile|src/main.cpp`, etc.
- If the new signature matches the previous iteration's signature exactly,
  the harness **does not** dispatch the tool. Instead it emits
  `<result>DUPLICATE TOOL EXECUTION, BREAKING TOOL LOOP</result>` as the
  user-role result message, records the turn to history, and ends the turn.
- Parse errors are never signatures (a repeated parse error won't trigger
  the guard — the model may still be recovering from a typo).
- The signature state is local to `Session::send`, so it resets every time
  the user types something new. A legitimate repeat call after a user reply
  will still go through.

The signature scheme is deliberately simple: arg values are concatenated
as-is (strings verbatim, numbers via `std::to_string`), separated by `|`.
There's a theoretical false-positive if two calls differ only by a `|`
character inside an argument moving between slots — e.g., `f("a|b", "c")`
vs `f("a", "b|c")` — but in practice these collide only under contrived
inputs and the cost (one wrong early exit, recoverable on the next user
turn) is acceptable.

The user-facing rendering:

```
<tool>writeFile("hello.txt", "world")</tool>
<result>ok</result>
<tool>writeFile("hello.txt", "world")</tool>
<result>DUPLICATE TOOL EXECUTION, BREAKING TOOL LOOP</result>
```

---

## Tool Dispatch

```cpp
// core/tool_dispatch.h
struct ToolResult { bool ok; std::string text; };
enum class ArgType { Str, Num };

using ToolFn = std::function<ToolResult(Session&, std::span<const ToolArg>)>;

struct ToolDef {
    std::string name;
    std::string description;    // shown to the model via {{TOOLS}}
    std::vector<ArgType> arg_types;
    ToolFn impl;
};

class ToolDispatcher {
    std::unordered_map<std::string, ToolDef> tools_;
public:
    void register_tool(ToolDef def);
    ToolResult dispatch(const ParsedToolCall& call, Session& session) const;
    std::string generate_descriptions() const;    // for {{TOOLS}} in system.md
    ToolDispatcher create_restricted(std::span<const std::string> allowed) const;
    bool has_tool(std::string_view name) const;
};
```

`dispatch` validates argument count and argument types against the `ToolDef`
and calls the `impl` lambda. Type errors come back as `ToolResult{false, "..."}`
and are appended to the conversation as a `<result>` block so the model can
self-correct.

---

## Adding a Tool

A tool is a single `.cpp` file in `core/` and a line in `register_all.cpp`.

### 1. Write the implementation

Create `core/my_tool.cpp`:

```cpp
#include "tool_dispatch.h"
#include "session.h"

namespace gab {

static ToolResult tool_word_count(Session& session, std::span<const ToolArg> args) {
    const std::string& path = args[0].sval;

    auto result = session.host().read_file(path);
    if (!result) {
        return {false, "cannot read: " + path};
    }

    size_t words = 0;
    bool in_word = false;
    for (char c : result.value()) {
        bool is_space = (c == ' ' || c == '\n' || c == '\t' || c == '\r');
        if (!is_space && !in_word) { ++words; in_word = true; }
        else if (is_space) { in_word = false; }
    }
    return {true, std::to_string(words) + "\n"};
}

void register_word_count(ToolDispatcher& d) {
    d.register_tool({
        "wordCount",                                      // name the model will use
        "Count whitespace-separated words in a file.",    // shown to the model
        {ArgType::Str},                                   // one string argument
        tool_word_count
    });
}

} // namespace gab
```

### 2. Wire it into the dispatcher

Edit `core/register_all.cpp`:

```cpp
void register_word_count(ToolDispatcher& d);   // add declaration

void register_builtin_tools(ToolDispatcher& dispatcher) {
    // ... existing registrations ...
    register_word_count(dispatcher);
}
```

### 3. Build and go

```sh
./build.sh
```

The tool appears in `/tools` output and in the `{{TOOLS}}` section of the
system prompt automatically. The model can now call `<tool>wordCount("file.txt")</tool>`.

### Conventions

- **Use `session.host()` for every I/O.** Never `fopen`/`open`/`curl_easy_init`
  inside a tool — the core must stay portable.
- **Track the read set** if your tool reads files that `editFile` should be
  allowed to modify: `session.add_to_read_set(path)`.
- **Return plain text.** No JSON envelopes. Errors are just text with `ok =
  false`.
- **Cap your output** at a sensible size (existing tools use 100–200KB) and
  append `\n[truncated]` when you cut.
- **Keep args to ≤3.** If you need more, split the tool.

---

## Agents

An **agent** is a sub-session with its own system prompt and a restricted tool
set. It runs to completion (its own internal tool-call loop) and returns a
single text response that becomes the result of the parent's
`<tool>agent(...)</tool>` call.

```cpp
// core/agent_runner.h
struct AgentDef {
    std::string name;
    std::string description;
    std::string system_prompt_path;              // loaded from disk each invocation
    std::vector<std::string> allowed_tools;      // everything else is hidden
    std::function<bool(const SessionConfig&)> available_check;  // optional
    std::string unavail_reason;
    int max_turns = 20;
};

std::string run_agent(const AgentDef& def,
                      std::string_view task,
                      HostFunctions& host,
                      const SessionConfig& config,
                      const ToolDispatcher& parent_tools);
```

Critical constraints:

- Agents **cannot** call `skill()` or `agent()` — those aren't in
  `allowed_tools` for any built-in agent, and `create_restricted` filters out
  anything not explicitly listed.
- Agents have **no conversation history**. Each invocation is a fresh
  message list seeded with the agent's system prompt and the task.
- Agents do **not** trigger compaction; they have their own `max_turns` cap.

The three built-in agents are defined in `Session::register_agents` in
`core/session.cpp`:

| Agent | Tools | Use |
|---|---|---|
| `web_search` | `braveSearch`, `webFetch` | Query Brave Search and summarise pages. Only registered when `brave_api_key` is set. |
| `explore` | `grep`, `grepIn`, `readFile` | Iteratively navigate a codebase. |
| `compactor` | (none) | Called internally by the compactor to summarise a conversation. |

---

## Adding an Agent

1. **Create the system prompt** at `.gab/prompts/my_agent.md`.
2. **Register it** in `Session::register_agents` (`core/session.cpp`):

```cpp
AgentDef critic;
critic.name = "critic";
critic.description = "Review code and list issues.";
critic.system_prompt_path = config_.project_dir + "/.gab/prompts/critic.md";
critic.allowed_tools = {"readFile", "grep", "grepIn"};
critic.max_turns = 15;
agent_defs_["critic"] = std::move(critic);
```

3. If the agent needs extra config (like `web_search` needs `brave_api_key`),
   set `available_check` and `unavail_reason`:

```cpp
critic.available_check = [](const SessionConfig& c) { return !c.api_key.empty(); };
critic.unavail_reason = "needs an API key";
```

The agent is now listed under `/agents` and callable by the model via
`<tool>agent("critic", "review src/main.cpp")</tool>`.

---

## Skills

A skill is a folder under `.gab/skills/<name>/` containing a `SKILL.md` (with
YAML front matter) plus any number of supporting `.md` files. Skills let you
swap chunks of specialised context in and out on demand.

```
.gab/skills/
└── pdf-tools/
    ├── SKILL.md      ← required, case-insensitive filename
    ├── examples.md   ← optional supporting files
    └── reference.md
```

The `SKILL.md` front matter:

```markdown
---
name: pdf-tools
description: Read, merge, split, and watermark PDFs.
---

# PDF Tools

Instructions the model should follow when this skill is loaded. Can reference
the sibling files (they'll be concatenated after SKILL.md when loaded).
```

### Lifecycle

1. At session start, `SkillLoader::scan()` walks `.gab/skills/`, parses the
   front matter of every `SKILL.md`, and builds a `{name, description}`
   registry.
2. The registry is injected into the system prompt via the `{{SKILLS}}`
   placeholder.
3. The model loads a skill with `<tool>skill("pdf-tools")</tool>`. The harness
   concatenates every `.md` file in the skill folder (SKILL.md first, the rest
   in sorted order) and injects it as a `Role::System` message right after the
   main system prompt.
4. Only one copy of any skill sits in context at a time.
5. On compaction, all skill messages are dropped (`is_skill = true` flag), and
   the loaded-set is cleared — the model can re-request them.

The loader (`core/skill_loader.cpp`) parses YAML front matter itself — no full
YAML engine. Scans for `---` delimiters, then `key: value` lines; only `name`
and `description` are read.

---

## Adding a Skill

```sh
mkdir -p .gab/skills/pdf-tools
cat > .gab/skills/pdf-tools/SKILL.md <<'EOF'
---
name: pdf-tools
description: Read, merge, split PDFs via pdftk and qpdf.
---

# PDF Tools

When the user asks to manipulate a PDF, prefer `bash("pdftk ...")` for merge
and split, and `bash("qpdf ...")` for encryption.

Read-only inspection: `bash("pdftk in.pdf dump_data")`.
EOF
```

That's it. Restart or run `/skills` to see it appear. No code changes needed.

---

## Compaction

Compaction triggers before a model call if `remaining_context < reserve_tokens`.
The reserve defaults to 10% of the model's context window and can be tuned
per-session with `/limit <n>` or `/limit <n>%`.

### Token counting without a tokenizer

`core/compactor.cpp`:

- **Between turns:** `usage_.total_tokens` (tracked from the API's own
  accounting in the `usage` field of the streaming response — requires
  `stream_options: {include_usage: true}` in the request) gives a baseline.
- **Authoritative check:** a zero-completion probe — the same request body
  with `max_tokens: 1` and `stream: false`. Read `usage.prompt_tokens`,
  discard the body.
- **Trigger:** if `prompt_tokens + reserve > max_context_tokens`, compact.

### Compaction flow

1. Serialise the conversation (minus the system prompt and any skill messages)
   into a plain transcript: `[user] ...\n\n[assistant] ...`.
2. Run the `compactor` agent on the transcript.
3. Replace `messages_` with `[system prompt] + [assistant summary] + [latest user message]`.
4. Clear all loaded skills (they can be re-requested).
5. Reset `usage_` — the next API response will refresh it.

### Tuning

- `/limit 20000` — reserve 20k tokens of headroom.
- `/limit 20%` — reserve 20% of the context window.
- `/compact` — force compaction right now.
- `.gab/config.json` → `api.max_context_tokens` — override the detected
  context length.

---

## Context-Length Discovery

`core/model_registry.cpp` resolves the model's context window in order:

1. **Built-in registry** — common OpenAI, Anthropic, and open-weight model
   families.
2. **OpenAI-compatible `{base_url}/models`** — parses the model's entry for
   any of: `loaded_context_length`, `max_context_length`, `context_length`,
   `max_model_len`, `context_window`, `n_ctx`.
3. **LM Studio native `{host}/api/v1/models`** — post-0.4 LM Studio's native
   API exposes `loaded_context_length` and `max_context_length` even though
   the OpenAI-compatible endpoint doesn't.
4. **LM Studio 0.3 fallback `{host}/api/v0/models`**.
5. **Prompt the user during the wizard** — value is saved to
   `config.json` so the next startup doesn't ask again.

`loaded_context_length` is preferred over `max_context_length` because the
former reflects the window actually allocated at runtime (the user may have
loaded a 128K model with only 8K enabled).

---

## Streaming and Cancellation

### SSE parsing

`core/sse_parser.cpp` consumes raw bytes from the HTTP stream. It splits on
`\n\n` boundaries, concatenates multi-line `data:` fields, and passes each
completed event to a callback. Handles CRLF, partial lines across chunk
boundaries, and SSE `:` comments (LM Studio's `: ping` keepalives).

### Stream consumer

`core/stream_consumer.cpp` sits on top of the SSE parser. For each event:

- Ignores `[DONE]` (signals stream end).
- Parses JSON, extracts `choices[0].delta.content`, appends to
  `accumulated_`, and fires `on_token`.
- Captures `finish_reason` and `usage` from the final chunk.

### Cancellation

`gab_session_cancel` sets an atomic flag. The streaming callback the core
passes to `http_request_stream` checks this flag and returns `false` (→
non-zero from the C trampoline) when cancelled. In the CLI's libcurl
implementation, returning 0 from `CURLOPT_WRITEFUNCTION` produces
`CURLE_WRITE_ERROR`, which aborts the HTTP connection immediately.

`/stop` in the terminal works by a background thread that watches stdin in
non-canonical mode (VMIN=0, VTIME=1 — polling every 100 ms). When it sees
`/stop\n`, it calls `gab_session_cancel`. After the cancellation lands, the
partial assistant response stays in the conversation with a
`[cancelled by user]` marker.

---

## Slash Commands

Parsed in `core/slash_commands.cpp` as a `std::variant`. The parser is a
simple prefix match — if the input starts with `/` and the token matches a
known command, it's routed to `Session::handle_slash_command`, which uses
`std::visit` with a large `if constexpr` chain.

Some commands (`/quit`, `/stop`) are intercepted by the CLI *before* the
session, because they need shell-level semantics (exiting the REPL, cancelling
an in-flight request).

### `/prompt` and `send_as_user`

Most slash commands run to completion inside `handle_slash_command`, emit a
text response + `TURN_END`, and return. `/prompt` is different: it loads a
file's contents and runs them through the normal user-input flow (prompt
build, compaction check, tool-call loop, history logging).

To avoid recursing through `send()` — which would re-parse the file
contents as a potential slash command if it happened to start with `/` —
the normal non-slash path is factored out as a private helper:

```cpp
void Session::send(std::string_view input, gab_event_cb cb, void* ud) {
    if (parse_slash_command(input)) { handle_slash_command(...); return; }
    send_as_user(input, cb, ud);
}

void Session::send_as_user(std::string_view input, gab_event_cb cb, void* ud) {
    reload_config();
    messages_.push_back({Role::User, std::string(input)});
    history_->append("user", input);
    rebuild_system_prompt();
    check_and_compact(cb, ud);
    // ...tool-call loop with duplicate guard...
}
```

`/prompt`'s handler reads the file (default `.gab/prompt.md`; absolute
paths respected; otherwise relative to `project_dir`), echoes a
`[Running prompt from <path>]` header, and delegates to `send_as_user`.
It sets a `delegate_as_user` flag so the slash-command scaffolding skips
its own `TURN_END` emission — `send_as_user` will emit that itself when
the turn finishes.

This is also useful for any future automation layer (scripted replay,
scheduled agents, WASM web UI with "load draft" buttons) — expose the
same `send_as_user` entry point and you get identical behaviour without
triggering slash parsing.

---

## History Log

`.gab/history.jsonl` — one JSON object per line:

```jsonl
{"ts":"2026-04-13T18:30:01Z","role":"user","content":"Read main.cpp"}
{"ts":"2026-04-13T18:30:04Z","role":"assistant","content":"I'll take a look.\n<tool>readFile(\"main.cpp\")</tool>\n<result>\n#include ...\n</result>\nThis is ..."}
```

- One log entry **per user turn** — all tool calls and results within a turn
  are concatenated into a single assistant entry.
- Written via `host.append_file` (flushed immediately).
- `/clear` truncates to zero bytes.
- On `/stop`, the partial content is saved with a trailing
  `[cancelled by user]` marker.
- The harness **never reads** its own history — it's purely for users and
  external tools.
- What's excluded: system prompt, skill bodies, compaction summaries, the
  zero-completion probes, slash command output.

---

## Config Wizard

Lives in `core/config_wizard.cpp`. Callable two ways:

- From the C ABI as `gab_run_config_wizard(host, project_dir)` — the CLI
  calls this on first run and whenever `gab_session_create` returns null
  (missing/invalid config, unknown context length, etc).
- From the session as `run_config_wizard(host_, project_dir, defaults)` when
  the user types `/config`.

### Flow

1. Print header.
2. **Connection loop** — prompt for URL + API key, normalize the URL (see
   below), then call `validate_api(host, url, key)`, which does GET
   `/models`. On any failure (network, non-200, unparseable JSON), print
   the exact error and offer retry.
3. **Model selection** — if `/models` returned:
   - 1 model: auto-select it.
   - >1 models: numbered list, pick by number or type a name.
   - 0 models: fall back to free-text prompt.
4. **Context length** — try `discover_context_length`; if it fails, prompt
   the user and save the value.
5. **Optional Brave key**.
6. Write `.gab/config.json` (temperature fixed at 0.7, max tool calls fixed
   at 10 — users edit these in-file).

### URL normalization

`normalize_base_url` (`core/config_wizard.cpp`) tolerates sloppy input so
users can type as little as possible:

| Input | Normalized |
|---|---|
| `localhost:1234` | `http://localhost:1234/v1` |
| `192.168.1.10:1234` | `http://192.168.1.10:1234/v1` |
| `http://localhost:1234` | `http://localhost:1234/v1` |
| `http://localhost:1234/` | `http://localhost:1234/v1` |
| `http://localhost:1234/v1` | unchanged |
| `http://host:8080/api/v2` | unchanged (non-/v1 paths respected) |
| `https://api.openai.com` | `https://api.openai.com/v1` |

The rules: prepend `http://` if no scheme; append `/v1` if there's no path
component after the host. When normalization changes anything, the wizard
prints `(using <normalized>)` so the user sees what's happening before the
connection attempt.

---

## Writing a New Shell

A shell is anything that builds `gabcore_static` and provides:

1. An implementation of every `gab_host_fns_t` callback.
2. A loop that calls `gab_session_send` with user input.
3. An event callback that renders `GAB_EVENT_TEXT_DELTA` tokens as they arrive.

### Minimal synchronous shell

```cpp
#include "gabcore.h"
#include <cstdio>
#include <cstring>
#include <string>

// ---- Host function implementations (abridged) -------------------------------

static void my_read_file(gab_str_t path, gab_result_cb cb, void* ud) {
    std::string p(path.data, path.len);
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) { cb({GAB_ERR_IO, {"", 0}}, ud); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::string buf(n, '\0');
    fread(buf.data(), 1, n, f); fclose(f);
    cb({GAB_OK, {buf.data(), buf.size()}}, ud);
}
// ... implement the other 13 callbacks the same way ...

// ---- Event callback -----------------------------------------------------

static void on_event(gab_event_t e, void* /*ud*/) {
    switch (e.type) {
        case GAB_EVENT_TEXT_DELTA:
        case GAB_EVENT_TOOL_RESULT:
            fwrite(e.data.data, 1, e.data.len, stdout);
            fflush(stdout);
            break;
        case GAB_EVENT_TURN_END:
            fputc('\n', stdout);
            break;
        case GAB_EVENT_ERROR:
            fprintf(stderr, "\n[error: %.*s]\n", (int)e.data.len, e.data.data);
            break;
        default: break;
    }
}

// ---- Main ---------------------------------------------------------------

int main() {
    gab_host_fns_t host = {};
    host.read_file           = my_read_file;
    // ... set the rest ...

    gab_str_t pd = {".", 1};
    gab_session_t sess = gab_session_create(host, pd);
    if (!sess) {
        gab_run_config_wizard(host, pd);
        sess = gab_session_create(host, pd);
    }

    char line[4096];
    while (fgets(line, sizeof(line), stdin)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) --n;
        gab_session_send(sess, {line, n}, on_event, nullptr);
    }
    gab_session_destroy(sess);
}
```

Link with `gabcore_static` and you have a working harness. That's the entire
contract.

### WASM / async shells

The core's callback-centric design anticipates an event-loop-driven host
where host functions return immediately and invoke their callbacks
asynchronously from the JS runtime. The key adjustments compared to the CLI:

- **`HostFunctions` synchronous-wrapper strategy breaks.** The CLI works
  because host callbacks fire *before* the host function returns, so the
  stack-local result slot is valid when the wrapper reads it. In WASM, the
  host function returns, control yields to the JS event loop, and the
  callback fires later — by which time the stack frame is gone.

- **Solution:** port `core/host_fns.cpp` to use heap-allocated continuations
  and `co_await` (C++20 coroutines). `Session::send` becomes a coroutine;
  each `host_.read_file(path)` co-awaits the callback. From the model's
  perspective nothing changes — tools still look synchronous.

- **JS host side:** implement each host callback to perform the operation
  asynchronously (e.g., `fetch` for HTTP, `IndexedDB` or `File System
  Access API` for files) and call the passed-in C callback with the result
  when done. The Emscripten docs on `emscripten_run_script` or module
  exports cover the FFI details.

- **Compile the core with Emscripten:**

  ```sh
  emcmake cmake -B build-wasm
  cmake --build build-wasm
  # Produces gabcore_static.a compatible with Emscripten-linked JS shells.
  ```

Nothing in `core/` currently uses threads, file descriptors, sockets, or
`std::filesystem` directly, so the port is mechanical once the
synchronous-wrapper → coroutine rewrite is done.

### GUI shell

The architecture also supports a Qt/GTK/Dear ImGui GUI: just implement the
host callbacks using Qt/GTK/Win32 native APIs, drive `gab_session_send` from
an input widget, and render events in a chat-view widget. The `TOOL_START`
event is especially useful here — it fires before the tool runs, perfect for
a "running tool..." spinner.

---

## File Layout Reference

```
gabcode/
├── build.sh
├── CMakeLists.txt                     # root: C++20, subdirs
├── .gitignore
├── README.md
├── TECHNICAL.md
│
├── core/                              # portable C++ library
│   ├── CMakeLists.txt                 # GLOB *.cpp → gabcore_static
│   │
│   ├── gabcore.h                      # C ABI boundary
│   ├── gabcore_api.cpp                # extern "C" wrappers
│   ├── host_fns.{h,cpp}               # C++ typed wrappers around C callbacks
│   ├── types.h                        # Message, Role, TokenUsage
│   ├── error.h                        # GabResult<T>
│   │
│   ├── session.{h,cpp}                # Central orchestrator, tool-call loop
│   ├── slash_commands.{h,cpp}         # Parse /command
│   ├── config_wizard.{h,cpp}          # Interactive config + validation
│   ├── model_registry.{h,cpp}         # Context-length discovery
│   ├── history.{h,cpp}                # JSONL append-only log
│   │
│   ├── tool_parser.{h,cpp}            # <tool>name("arg")</tool> state machine
│   ├── tool_dispatch.{h,cpp}          # Registry, validation, dispatch
│   │
│   ├── read_file.cpp                  # ← one .cpp per tool
│   ├── write_file.cpp
│   ├── edit_file.cpp                  # editFile + editFileAll
│   ├── grep_tool.cpp                  # grep + grepIn (std::regex ECMAScript)
│   ├── bash_tool.cpp                  # shell + built-in dispatch
│   ├── web_fetch.cpp
│   ├── brave_search.cpp               # Brave API (with X-Subscription-Token)
│   ├── skill_tool.cpp
│   ├── agent_tool.cpp
│   ├── register_all.cpp               # Registers everything above
│   │
│   ├── agent_runner.{h,cpp}           # Sub-sessions with restricted tools
│   ├── skill_loader.{h,cpp}           # Scan .gab/skills/, YAML front matter
│   ├── compactor.{h,cpp}              # Trigger + rebuild
│   ├── sse_parser.{h,cpp}             # SSE byte stream → events
│   ├── stream_consumer.{h,cpp}        # Events → tokens + usage
│   │
│   ├── builtin_cmds.{h,cpp}           # Shell tokenizer, is_simple check
│   └── commands.cpp                   # Built-in ls/cat/rm/cp/mv/etc.
│
├── cli/                               # CLI shell
│   ├── CMakeLists.txt                 # Links gabcore_static + libcurl
│   ├── main.cpp                       # REPL, event callback, first-run
│   ├── host_impl.{h,cpp}              # POSIX + libcurl implementations
│   └── terminal.{h,cpp}               # Line input, /stop background thread
│
├── deps/
│   ├── CMakeLists.txt                 # Header-only INTERFACE target
│   └── json.hpp                       # nlohmann/json v3.11.3
│
└── tests/
    ├── CMakeLists.txt                 # One executable per test_*.cpp
    ├── test_tool_parser.cpp           # 13 tests
    ├── test_slash_commands.cpp        # 10 tests
    ├── test_sse_parser.cpp            # 8 tests
    └── test_builtin_cmds.cpp          # 10 tests
```

Build system: one `GLOB *.cpp` per subdirectory. Adding a new tool means
dropping a file into `core/` and adding a registration line — no CMake edits
required.
