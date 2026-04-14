# Gab Code

An AI coding-assistant harness written in modern C++20. Targets the **OpenAI
Chat Completions API** (and any compatible endpoint — LM Studio, vLLM, llama.cpp
server, Ollama, OpenAI itself, etc.). Works as a prompt builder, tool dispatcher,
skill loader, agent runner, and conversation manager.

Designed with a strict **portable-core / thin-shell** split: the core library
does zero direct I/O and talks to the outside world through a table of
function-pointer callbacks. The CLI shell provides concrete POSIX + libcurl
implementations today; the same core is meant to run in a WASM browser tab
tomorrow.

## Highlights

- **Tag-based tool calling** — the model emits `<tool>funcName("arg")</tool>`,
  the harness intercepts via the `</tool>` stop sequence, runs the tool, and
  replies with `<result>...</result>` as a user-role message so the
  conversation alternates cleanly and multi-step chains work reliably.
- **Loop-break guard** — if the model emits the exact same tool call twice
  in a row (same name, same args), the harness skips the second dispatch
  and ends the turn with a clear `DUPLICATE TOOL EXECUTION` signal instead
  of grinding uselessly against the tool-call limit.
- **10 built-in tools**: `readFile`, `writeFile`, `editFile`, `grep`, `grepIn`,
  `bash`, `webFetch`, `braveSearch`, `skill`, `agent`.
- **6 portable built-in shell commands** (`ls`, `mkdir`, `rm`, `rmdir`, `cp`,
  `mv`) that work even when no host shell is registered.
- **Skills** — drop a folder under `.gab/skills/<name>/` with a `SKILL.md`
  (YAML front matter) and the model can load it on demand.
- **Agents** — sub-sessions with their own system prompt and restricted tool
  set. Three built in: `web_search`, `explore`, `compactor`.
- **Automatic compaction** — when the conversation approaches the model's
  context limit, the compactor agent summarises it and the harness rebuilds
  the message list.
- **Token counting without a tokenizer** — uses `usage.prompt_tokens` from the
  API's own accounting, with a zero-completion probe before sending.
- **Interactive config wizard** — on first run or via `/config`, prompts for
  endpoint + API key, validates the connection, lists available models
  (auto-selects if there's only one), falls back to LM Studio's native
  `/api/v1/models` for context-length discovery.
- **Full JSONL conversation log** at `.gab/history.jsonl`.
- **15 slash commands** for harness control (`/help`, `/tools`, `/skills`,
  `/agents`, `/config`, `/compact`, `/limit`, `/guard`, `/clear`, `/stop`,
  `/search`, `/explore`, `/system`, `/prompt`, `/quit`).

## Build

Requires: CMake 3.20+, a C++20 compiler, libcurl.

```sh
./build.sh          # build CLI + stage web shell (incremental)
./build.sh test     # build and run all tests
./build.sh web      # stage web/ sources to web_build/ only
./build.sh clean    # wipe build/, web_build/, and the gabcode binary
```

The CLI lands at `./gabcode` in the project root. The web shell is staged to
`web_build/` — open `web_build/index.html` via any static HTTP server (e.g.
`python3 -m http.server` from `web_build/`) and configure it from the gear
icon in the sidebar. Each chat lives in its own IndexedDB-backed `.gab/`, so
state is isolated per conversation.

## Run (CLI)

```sh
./gabcode
```

On first run, an interactive wizard walks you through:

1. API base URL (defaults to LM Studio: `http://localhost:1234/v1`; you can
   also type just `localhost:1234` or an IP — missing `http://` and `/v1`
   are filled in automatically)
2. API key (use any string for local servers)
3. Model — auto-selected if the server advertises exactly one, otherwise picked
   from a numbered list (or typed by name)
4. Context length — asked only if the server doesn't expose it via
   `/v1/models` or `/api/v1/models` and the model isn't in the built-in registry
5. Optional Brave Search API key (leave blank to disable web search)

Configuration is written to `.gab/config.json`. You can re-run the wizard any
time with `/config`, or edit the JSON directly — changes are hot-reloaded on
the next turn.

### Customising prompts

The files under `.gab/prompts/` (`system.md`, `compactor.md`, `web_search.md`,
`explore.md`) are yours to edit — gabcode only writes defaults when a file is
missing. To pick up a newer default after an upgrade, delete the file and
re-run `gabcode`; it'll regenerate only the missing one.

`system.md` supports two placeholders that are substituted on every turn:

- `{{TOOLS}}` — the registered tool list with signatures and descriptions
- `{{SKILLS}}` — the available-skills registry

Keep both in your system prompt so the model knows what it can call.

## Slash commands

| Command | Purpose |
|---|---|
| `/help` | List all slash commands |
| `/tools` | Show registered tools and their signatures |
| `/skills` | Show available skills and which are loaded |
| `/agents` | Show available agents |
| `/search <q>` | Run the web_search agent on `<q>` |
| `/explore <q>` | Run the explore agent on `<q>` |
| `/compact` | Force compaction right now |
| `/limit <n>` or `/limit <n>%` | Set compaction reserve (session-only) |
| `/guard <n>` | Set max tool calls per turn |
| `/config` | Re-run the config wizard |
| `/system <text>` | Replace the system prompt |
| `/prompt [path]` | Load a file and send its contents as a user turn (default: `.gab/prompt.md`) |
| `/clear` | Wipe conversation and history |
| `/stop` | Cancel the in-flight request |
| `/quit` | Exit |

## Project layout

```
gabcode/
├── build.sh             # build/test/clean helper
├── CMakeLists.txt
├── core/                # portable C++20 library (all .h and .cpp flat)
├── cli/                 # CLI shell (main, host_impl, terminal)
├── web/                 # Web shell (vanilla JS/HTML/CSS — IndexedDB VFS, streaming chat)
├── deps/                # vendored deps (nlohmann/json.hpp)
├── tests/               # assert-based unit tests
└── .gab/                # per-project state (created on first run)
    ├── config.json
    ├── history.jsonl
    ├── prompts/         # system, compactor, web_search, explore
    └── skills/
```

## Further reading

See **[TECHNICAL.md](TECHNICAL.md)** for the architecture, C ABI, tool-call
protocol, and instructions for adding new tools, agents, skills, and shells.

## License

No license. Personal project.
