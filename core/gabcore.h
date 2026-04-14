#ifndef GABCORE_H
#define GABCORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Non-owning string view ───────────────────────── */
typedef struct {
    const char* data;
    size_t      len;
} gab_str_t;

/* ── Error codes ──────────────────────────────────── */
typedef enum {
    GAB_OK = 0,
    GAB_ERR_IO,
    GAB_ERR_NETWORK,
    GAB_ERR_PARSE,
    GAB_ERR_TIMEOUT,
    GAB_ERR_CANCELLED,
    GAB_ERR_NOT_FOUND
} gab_error_t;

/* ── Async result ─────────────────────────────────── */
typedef struct {
    gab_error_t error;
    gab_str_t   data;
} gab_result_t;

typedef void (*gab_result_cb)(gab_result_t result, void* user_data);

/* ── Directory listing ────────────────────────────── */
typedef struct {
    gab_str_t name;
    int       is_dir;
    uint64_t  size_bytes;
} gab_dir_entry_t;

typedef void (*gab_dir_cb)(gab_error_t error,
                           const gab_dir_entry_t* entries,
                           size_t count,
                           void* user_data);

/* ── HTTP ─────────────────────────────────────────── */
typedef struct {
    gab_str_t       method;
    gab_str_t       url;
    gab_str_t       body;
    const gab_str_t* headers;
    size_t          header_count;
} gab_http_request_t;

typedef void (*gab_http_cb)(gab_error_t error,
                            int status_code,
                            gab_str_t body,
                            void* user_data);

/* Return 0 to continue, non-zero to abort the HTTP transfer. */
typedef int (*gab_http_stream_cb)(gab_str_t chunk,
                                  int is_done,
                                  gab_error_t error,
                                  void* user_data);

/* ── Host function table ──────────────────────────── */
typedef struct {
    /* File I/O */
    void (*read_file)(gab_str_t path, gab_result_cb cb, void* ud);
    void (*write_file)(gab_str_t path, gab_str_t content, gab_result_cb cb, void* ud);
    void (*append_file)(gab_str_t path, gab_str_t content, gab_result_cb cb, void* ud);
    void (*delete_file)(gab_str_t path, gab_result_cb cb, void* ud);
    void (*file_exists)(gab_str_t path, gab_result_cb cb, void* ud);

    /* Directory I/O */
    void (*list_dir)(gab_str_t path, gab_dir_cb cb, void* ud);
    void (*make_dir)(gab_str_t path, gab_result_cb cb, void* ud);
    void (*remove_dir)(gab_str_t path, gab_result_cb cb, void* ud);

    /* Network */
    void (*http_request)(gab_http_request_t req, gab_http_cb cb, void* ud);
    void (*http_request_stream)(gab_http_request_t req, gab_http_stream_cb cb, void* ud);

    /* Shell */
    void (*run_shell)(gab_str_t command, gab_str_t cwd, gab_result_cb cb, void* ud);

    /* Time */
    uint64_t (*get_time_ms)(void);

    /* UI output */
    void (*log_output)(gab_str_t text, void* ud);

    /* Interactive prompt: show prompt+default, read a line of user input.
       If the user presses Enter with no input, the default should be returned. */
    void (*prompt_input)(gab_str_t prompt, gab_str_t default_val,
                         gab_result_cb cb, void* ud);

    void* host_data;
} gab_host_fns_t;

/* ── Session events (core → shell) ────────────────── */
typedef enum {
    GAB_EVENT_TEXT_DELTA,
    GAB_EVENT_TOOL_START,
    GAB_EVENT_TOOL_RESULT,
    GAB_EVENT_TURN_END,
    GAB_EVENT_ERROR,
    GAB_EVENT_COMPACTING
} gab_event_type_t;

typedef struct {
    gab_event_type_t type;
    gab_str_t        data;
} gab_event_t;

typedef void (*gab_event_cb)(gab_event_t event, void* user_data);

/* ── Session lifecycle ────────────────────────────── */
typedef struct gab_session_s* gab_session_t;

gab_session_t gab_session_create(gab_host_fns_t host, gab_str_t project_dir);
void          gab_session_destroy(gab_session_t session);
void          gab_session_send(gab_session_t session, gab_str_t input,
                               gab_event_cb cb, void* user_data);
void          gab_session_cancel(gab_session_t session);

/* Run the interactive configuration wizard. Writes .gab/config.json.
   Returns 1 on success, 0 if aborted. */
int           gab_run_config_wizard(gab_host_fns_t host, gab_str_t project_dir);

#ifdef __cplusplus
}
#endif

#endif /* GABCORE_H */
