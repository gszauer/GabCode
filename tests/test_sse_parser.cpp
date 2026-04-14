#include "sse_parser.h"
#include <cassert>
#include <cstdio>
#include <vector>

using namespace gab;

static void test_basic_event() {
    std::vector<SSEEvent> events;
    SSEParser parser([&](const SSEEvent& evt) { events.push_back(evt); });

    parser.feed("data: hello\n\n");
    assert(events.size() == 1);
    assert(events[0].data == "hello");
    std::printf("  PASS: basic event\n");
}

static void test_multiple_events() {
    std::vector<SSEEvent> events;
    SSEParser parser([&](const SSEEvent& evt) { events.push_back(evt); });

    parser.feed("data: first\n\ndata: second\n\n");
    assert(events.size() == 2);
    assert(events[0].data == "first");
    assert(events[1].data == "second");
    std::printf("  PASS: multiple events\n");
}

static void test_split_across_chunks() {
    std::vector<SSEEvent> events;
    SSEParser parser([&](const SSEEvent& evt) { events.push_back(evt); });

    parser.feed("data: hel");
    assert(events.empty());
    parser.feed("lo\n\n");
    assert(events.size() == 1);
    assert(events[0].data == "hello");
    std::printf("  PASS: split across chunks\n");
}

static void test_comment_ignored() {
    std::vector<SSEEvent> events;
    SSEParser parser([&](const SSEEvent& evt) { events.push_back(evt); });

    parser.feed(": this is a comment\ndata: actual\n\n");
    assert(events.size() == 1);
    assert(events[0].data == "actual");
    std::printf("  PASS: comment ignored\n");
}

static void test_done_sentinel() {
    std::vector<SSEEvent> events;
    SSEParser parser([&](const SSEEvent& evt) { events.push_back(evt); });

    parser.feed("data: [DONE]\n\n");
    assert(events.size() == 1);
    assert(events[0].data == "[DONE]");
    std::printf("  PASS: [DONE] sentinel\n");
}

static void test_crlf_line_endings() {
    std::vector<SSEEvent> events;
    SSEParser parser([&](const SSEEvent& evt) { events.push_back(evt); });

    parser.feed("data: crlf\r\n\r\n");
    assert(events.size() == 1);
    assert(events[0].data == "crlf");
    std::printf("  PASS: CRLF line endings\n");
}

static void test_multiline_data() {
    std::vector<SSEEvent> events;
    SSEParser parser([&](const SSEEvent& evt) { events.push_back(evt); });

    parser.feed("data: line1\ndata: line2\n\n");
    assert(events.size() == 1);
    assert(events[0].data == "line1\nline2");
    std::printf("  PASS: multiline data\n");
}

static void test_finish_flushes() {
    std::vector<SSEEvent> events;
    SSEParser parser([&](const SSEEvent& evt) { events.push_back(evt); });

    parser.feed("data: no-newline");
    assert(events.empty());
    parser.finish();
    assert(events.size() == 1);
    assert(events[0].data == "no-newline");
    std::printf("  PASS: finish flushes\n");
}

int main() {
    std::printf("SSE Parser Tests:\n");
    test_basic_event();
    test_multiple_events();
    test_split_across_chunks();
    test_comment_ignored();
    test_done_sentinel();
    test_crlf_line_endings();
    test_multiline_data();
    test_finish_flushes();
    std::printf("All tests passed.\n");
    return 0;
}
