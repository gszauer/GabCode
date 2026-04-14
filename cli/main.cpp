#include "host_impl.h"
#include "terminal.h"
#include "gabcore.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <sys/stat.h>

// ── First-run setup ──────────────────────────────────

static bool directory_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static void mkdir_p(const std::string& path) {
    // Recursive mkdir — create each path component along the way.
    std::string acc;
    for (size_t i = 0; i < path.size(); ++i) {
        acc += path[i];
        if (path[i] == '/' || i == path.size() - 1) {
            if (!acc.empty() && acc != "/") {
                ::mkdir(acc.c_str(), 0755); // ignore EEXIST
            }
        }
    }
}

static void write_default_file(const std::string& path, const std::string& content) {
    FILE* f = ::fopen(path.c_str(), "w");
    if (!f) return;
    ::fwrite(content.data(), 1, content.size(), f);
    ::fclose(f);
}

static bool file_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static bool write_default_prompts_and_skills(const std::string& gab_dir);

// Ensure the project directory tree, default prompts, and history file exist.
// Safe to call repeatedly — only writes files that are missing.
static void ensure_project_dirs(const std::string& project_dir) {
    std::string gab_dir = project_dir + "/.gab";
    mkdir_p(gab_dir);
    mkdir_p(gab_dir + "/prompts");
    mkdir_p(gab_dir + "/skills");
    write_default_prompts_and_skills(gab_dir);
    if (!file_exists(gab_dir + "/history.jsonl")) {
        write_default_file(gab_dir + "/history.jsonl", "");
    }
}

static bool write_default_prompts_and_skills(const std::string& gab_dir) {
    // Only writes files that don't already exist, so user edits are preserved.
    auto write_if_missing = [&](const std::string& path, const std::string& content) {
        if (!file_exists(path)) write_default_file(path, content);
    };

    // Write default prompts
    write_if_missing(gab_dir + "/prompts/system.md",
        "You are Gab, an AI coding assistant. You help users with programming tasks.\n\n"
        "## How you respond\n\n"
        "Every response you generate is exactly one of these two shapes:\n\n"
        "  1. A single tool call, and nothing else:\n\n"
        "       <tool>toolName(\"arg\", \"arg\")</tool>\n\n"
        "  2. A final answer to the user, as plain text, with no tool call.\n\n"
        "## How tool calls work\n\n"
        "When you emit a <tool>...</tool> call, the harness runs the tool and\n"
        "replies with a user-role message containing <result>...</result>.\n"
        "That <result> is harness output, not a new user request — the user's\n"
        "original task is still in progress, and it is your job to continue it.\n\n"
        "## Multi-step tasks\n\n"
        "If the user asks for multiple steps, chain one tool call per response\n"
        "until every step is complete. Do NOT stop after a single tool call\n"
        "when more work remains.\n\n"
        "Worked example — user: \"Create hello.txt with 'world' and foo.txt\n"
        "with 'bar'.\"\n\n"
        "    you:     <tool>writeFile(\"hello.txt\", \"world\")</tool>\n"
        "    harness: <result>ok</result>\n"
        "    you:     <tool>writeFile(\"foo.txt\", \"bar\")</tool>\n"
        "    harness: <result>ok</result>\n"
        "    you:     Created hello.txt and foo.txt.\n\n"
        "Only stop emitting tool calls when the user's entire request is\n"
        "complete. Until then, keep going. When you're finished, say\n"
        "plainly that you're done making tool calls, then write your final\n"
        "answer as plain text.\n\n"
        "## Available Tools\n\n"
        "{{TOOLS}}\n\n"
        "## Available Skills\n\n"
        "{{SKILLS}}\n");

    write_if_missing(gab_dir + "/prompts/compactor.md",
        "You are a conversation compactor. Summarize the following conversation concisely.\n"
        "Preserve:\n"
        "- Key decisions made\n"
        "- Important facts and context\n"
        "- Current task state and progress\n"
        "- File paths and code references mentioned\n"
        "Target length: approximately 2000 tokens.\n"
        "Output only the summary, no preamble.\n");

    write_if_missing(gab_dir + "/prompts/web_search.md",
        "You are a web search agent. You have two tools: braveSearch and webFetch.\n\n"
        "Steps:\n"
        "1. Call braveSearch(\"<user query>\") — returns a JSON array of up to 5 results,\n"
        "   each with `title`, `url`, and `description` fields.\n"
        "2. Pick the 3-5 most relevant results. For each, call webFetch(<url>) to read the page.\n"
        "3. Synthesize a concise, well-cited answer to the user's original question.\n\n"
        "When finished, respond with ONLY your final summary — no more tool calls.\n");

    write_if_missing(gab_dir + "/prompts/explore.md",
        "You are a codebase exploration agent. You have three tools: grep, grepIn, readFile.\n\n"
        "Given a natural-language query about the codebase, explore iteratively:\n"
        "1. Start with grep to find relevant keywords across the project.\n"
        "2. Narrow down with grepIn to specific directories or files.\n"
        "3. Use readFile to examine the most promising files in detail.\n"
        "4. Repeat until you have enough context to answer the query.\n\n"
        "When finished, respond with ONLY your findings: what files are relevant, what\n"
        "functions/types matter, and a clear answer to the original query. No more tool calls.\n");

    return true;
}

// ── Event callback ───────────────────────────────────

struct CliState {
    Terminal* terminal;
    gab_session_t session;
    bool got_error;
};

static void event_callback(gab_event_t event, void* user_data) {
    auto* state = static_cast<CliState*>(user_data);

    switch (event.type) {
        case GAB_EVENT_TEXT_DELTA: {
            std::string token(event.data.data, event.data.len);
            state->terminal->print_token(token);
            break;
        }
        case GAB_EVENT_TOOL_START: {
            // The <tool>...</tool> call is already visible in the stream;
            // no extra status line is needed from the CLI.
            break;
        }
        case GAB_EVENT_TOOL_RESULT: {
            // Event data from the core is already the full "\n<result>...</result>"
            // block. Write it verbatim — no extra wrapping.
            std::string block(event.data.data, event.data.len);
            state->terminal->print_token(block);
            break;
        }
        case GAB_EVENT_TURN_END: {
            state->terminal->println(""); // newline after streaming
            break;
        }
        case GAB_EVENT_ERROR: {
            std::string err(event.data.data, event.data.len);
            state->terminal->eprintln("[Error: " + err + "]");
            state->got_error = true;
            break;
        }
        case GAB_EVENT_COMPACTING: {
            state->terminal->eprintln("[Compacting conversation...]");
            break;
        }
    }
}

// ── Main ─────────────────────────────────────────────

int main(int argc, char* argv[]) {
    Terminal terminal;

    // Determine project directory (cwd by default)
    char cwd_buf[4096];
    std::string project_dir;
    if (::getcwd(cwd_buf, sizeof(cwd_buf))) {
        project_dir = cwd_buf;
    } else {
        terminal.eprintln("Error: cannot determine working directory.");
        return 1;
    }

    // Check for --project-dir argument
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--project-dir" && i + 1 < argc) {
            project_dir = argv[++i];
        }
    }

    // Initialize host (curl) before any HTTP calls — the wizard needs it
    cli_host_init();

    gab_host_fns_t host_fns = create_cli_host_fns();

    // Always ensure the project dir tree + default prompts exist (idempotent)
    ensure_project_dirs(project_dir);

    gab_str_t proj_str = {project_dir.data(), project_dir.size()};

    // If config.json is missing, run the wizard immediately (first-run case)
    if (!file_exists(project_dir + "/.gab/config.json")) {
        terminal.println("Welcome to Gab Code!");
        if (!gab_run_config_wizard(host_fns, proj_str)) {
            cli_host_cleanup();
            return 1;
        }
    }

    // Try to create the session. If it fails (bad config, missing model,
    // unknown context length), fall back to the wizard.
    gab_session_t session = gab_session_create(host_fns, proj_str);
    if (!session) {
        terminal.eprintln("\nLaunching setup wizard to fix configuration...\n");
        if (!gab_run_config_wizard(host_fns, proj_str)) {
            cli_host_cleanup();
            return 1;
        }
        session = gab_session_create(host_fns, proj_str);
        if (!session) {
            terminal.eprintln("Error: failed to create session after setup.");
            cli_host_cleanup();
            return 1;
        }
    }

    terminal.println("Gab Code v0.1.0");
    terminal.println("Type /help for commands, /quit to exit.\n");

    // REPL loop
    while (true) {
        auto input = terminal.read_line("gab> ");
        if (!input) break; // EOF

        std::string line = *input;

        // Trim whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        size_t end = line.find_last_not_of(" \t\r\n");
        line = line.substr(start, end - start + 1);

        if (line.empty()) continue;

        // Quit handled by CLI (session never sees it)
        if (line == "/quit" || line == "/exit") break;

        // All other input (including slash commands) goes to the session
        CliState state = {&terminal, session, false};
        gab_str_t input_str = {line.data(), line.size()};

        // Only watch stdin for /stop during model streaming. Slash commands
        // may need stdin themselves (e.g. /config opens the wizard, which
        // calls host_.prompt_input) and the monitor would race for bytes.
        const bool is_slash = line[0] == '/';
        if (!is_slash) terminal.start_stop_monitor();
        gab_session_send(session, input_str, event_callback, &state);
        if (!is_slash) terminal.stop_stop_monitor();

        // If /stop was detected, cancel the session
        if (terminal.stop_requested()) {
            gab_session_cancel(session);
            terminal.reset_stop();
        }
    }

    gab_session_destroy(session);
    cli_host_cleanup();

    terminal.println("Goodbye!");
    return 0;
}
