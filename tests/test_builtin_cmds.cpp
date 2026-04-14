#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

// We test the shell tokenizer and is_simple_builtin directly
// by including the source (since builtin_cmds.h is in src/)
namespace gab {
    std::vector<std::string> tokenize_shell(const std::string& cmd);
    bool is_simple_builtin(const std::string& cmd);
}

using namespace gab;

static void test_basic_tokenize() {
    auto tokens = tokenize_shell("ls -la /tmp");
    assert(tokens.size() == 3);
    assert(tokens[0] == "ls");
    assert(tokens[1] == "-la");
    assert(tokens[2] == "/tmp");
    std::printf("  PASS: basic tokenize\n");
}

static void test_quoted_string() {
    auto tokens = tokenize_shell(R"(echo "hello world")");
    assert(tokens.size() == 2);
    assert(tokens[0] == "echo");
    assert(tokens[1] == "hello world");
    std::printf("  PASS: double-quoted string\n");
}

static void test_single_quoted() {
    auto tokens = tokenize_shell("echo 'hello world'");
    assert(tokens.size() == 2);
    assert(tokens[0] == "echo");
    assert(tokens[1] == "hello world");
    std::printf("  PASS: single-quoted string\n");
}

static void test_escaped_in_double_quotes() {
    auto tokens = tokenize_shell(R"(echo "hello \"world\"")");
    assert(tokens.size() == 2);
    assert(tokens[1] == R"(hello "world")");
    std::printf("  PASS: escaped chars in double quotes\n");
}

static void test_empty_command() {
    auto tokens = tokenize_shell("");
    assert(tokens.empty());
    std::printf("  PASS: empty command\n");
}

static void test_simple_builtin_ls() {
    assert(is_simple_builtin("ls -la"));
    std::printf("  PASS: ls -la is simple\n");
}

static void test_pipe_not_simple() {
    assert(!is_simple_builtin("ls | grep foo"));
    std::printf("  PASS: pipe is not simple\n");
}

static void test_chain_not_simple() {
    assert(!is_simple_builtin("mkdir foo && cd foo"));
    std::printf("  PASS: chain is not simple\n");
}

static void test_redirect_not_simple() {
    assert(!is_simple_builtin("echo hello > out.txt"));
    std::printf("  PASS: redirect is not simple\n");
}

static void test_subshell_not_simple() {
    assert(!is_simple_builtin("echo $(date)"));
    std::printf("  PASS: subshell is not simple\n");
}

int main() {
    std::printf("Built-in Command Tests:\n");
    test_basic_tokenize();
    test_quoted_string();
    test_single_quoted();
    test_escaped_in_double_quotes();
    test_empty_command();
    test_simple_builtin_ls();
    test_pipe_not_simple();
    test_chain_not_simple();
    test_redirect_not_simple();
    test_subshell_not_simple();
    std::printf("All tests passed.\n");
    return 0;
}
