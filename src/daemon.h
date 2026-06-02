#pragma once

#include <stddef.h>

enum daemon_request_result {
  DAEMON_REQUEST_CONTINUE = 0,
  DAEMON_REQUEST_SHUTDOWN = 1,
};

typedef int (*daemon_init_fn)(void *user_data);
typedef int (*daemon_tick_fn)(void *user_data);
typedef int (*daemon_request_fn)(void *user_data, const char *request, char *response, size_t response_size);
typedef void (*daemon_deinit_fn)(void *user_data);

int daemon_runtime_dir(const char *app_name, char *buffer, size_t size);
int daemon_socket_path(const char *app_name, char *buffer, size_t size);
int daemon_send_request(const char *socket_path, const char *request, char *response, size_t response_size,
                        int timeout_ms);
int daemon_run(const char *app_name, daemon_init_fn on_init, daemon_request_fn on_request, daemon_tick_fn on_tick,
               daemon_deinit_fn on_deinit, void *user_data, int daemon_poll_ms);
