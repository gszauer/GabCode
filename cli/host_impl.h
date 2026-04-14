#pragma once

#include "gabcore.h"

// Create a fully populated host function table using POSIX I/O and libcurl.
gab_host_fns_t create_cli_host_fns();

// Must be called once before using host functions.
void cli_host_init();

// Must be called once at shutdown.
void cli_host_cleanup();
