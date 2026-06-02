#include "daemon.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libnotify/notify.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

constexpr int CIGNALS_POLL_MS = 60000;
const char *APP_NAME = "cignals";

static void print_usage(FILE *stream, const char *program_name) {
  fprintf(stream,
          "Usage:\n"
          "  %s start [--interval N]\n"
          "  %s status\n"
          "  %s quit\n"
          "  %s get interval\n"
          "  %s set interval N\n"
          "  %s --daemon [--interval N]\n",
          program_name, program_name, program_name, program_name, program_name, program_name);
}

static int parse_unsigned(const char *text, unsigned *value) {
  if (text == nullptr || value == nullptr) {
    errno = EINVAL;
    return -1;
  }

  errno = 0;
  char *end = nullptr;
  const unsigned long parsed = strtoul(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || parsed > UINT_MAX) {
    errno = EINVAL;
    return -1;
  }

  *value = (unsigned) parsed;
  return 0;
}

struct [[maybe_unused]] cignals_state {
  unsigned interval_minutes;
  int running;
  uint64_t last_monotonic_ms;
};

static uint64_t monotonic_now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
    return 0;
  }
  return ((uint64_t) ts.tv_sec * 1000ULL) + ((uint64_t) ts.tv_nsec / 1000000ULL);
}

static int parse_interval_option(const int argc, char **argv, int *index, struct cignals_state *state) {
  if (*index < argc && strcmp(argv[*index], "--interval") == 0) {
    if (*index + 1 >= argc) {
      errno = EINVAL;
      return -1;
    }

    unsigned interval = 0;
    if (parse_unsigned(argv[*index + 1], &interval) < 0 || interval < 1U) {
      return -1;
    }

    state->interval_minutes = interval;
    state->last_monotonic_ms = monotonic_now_ms();
    *index += 2;
  }

  return 0;
}

static int wait_for_daemon(const char *socket_path) {
  char response[256];

  for (int attempt = 0; attempt < 50; ++attempt) {
    if (daemon_send_request(socket_path, "STATUS", response, sizeof(response), 200) == 0) {
      return 0;
    }

    struct timespec delay = {
            .tv_sec = 0,
            .tv_nsec = 100000000L,
    };
    nanosleep(&delay, nullptr);
  }

  errno = ETIMEDOUT;
  return -1;
}

static int send_command(const char *socket_path, const char *request) {
  char response[256];
  if (daemon_send_request(socket_path, request, response, sizeof(response), 1000) < 0) {
    return -1;
  }

  fputs(response, stdout);
  if (response[0] == '\0' || response[strlen(response) - 1] != '\n') {
    fputc('\n', stdout);
  }
  return 0;
}

static int start_daemon_process(const char *program_name, const struct cignals_state *state) {
  char socket_path[PATH_MAX];
  if (daemon_socket_path(APP_NAME, socket_path, sizeof(socket_path)) < 0) {
    perror(APP_NAME);
    return -1;
  }

  char response[256];
  if (daemon_send_request(socket_path, "STATUS", response, sizeof(response), 1000) == 0) {
    puts("daemon already running");
    return 0;
  }

  char interval_buffer[32];
  const int interval_written = snprintf(interval_buffer, sizeof(interval_buffer), "%u", state->interval_minutes);
  if (interval_written < 0 || (size_t) interval_written >= sizeof(interval_buffer)) {
    errno = ENAMETOOLONG;
    perror(APP_NAME);
    return -1;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    perror(APP_NAME);
    return -1;
  }

  if (pid == 0) {
    char *const child_argv[] = {
            (char *) program_name, (char *) "--daemon", (char *) "--interval", interval_buffer, nullptr,
    };

    execvp(program_name, child_argv);
    perror("execvp");
    _exit(127);
  }

  if (wait_for_daemon(socket_path) < 0) {
    perror(APP_NAME);
    return -1;
  }

  puts("daemon started");
  return 0;
}

static int on_daemon_init(void *user_data) {
  const auto state = (struct cignals_state *) user_data;
  if (!notify_init(APP_NAME)) {
    return -1;
  }
  notify_set_app_icon("alarm-clock");
  char message[1024];
  snprintf(message, 1024, "You will be notified to move in %u minutes.", state->interval_minutes);
  NotifyNotification *notif = notify_notification_new("cignals was set up!", message, "alarm-clock");
  if (notif != nullptr) {
    notify_notification_show(notif, nullptr);
    g_object_unref(G_OBJECT(notif));
  }
  state->last_monotonic_ms = monotonic_now_ms();
  return 0;
}

static void on_daemon_deinit(void *user_data) {
  (void) user_data;
  notify_uninit();
}

static int on_daemon_request(void *user_data, const char *request, char *response, const size_t response_size) {
  const auto state = (struct cignals_state *) user_data;

  if (strcmp(request, "STATUS") == 0) {
    if (snprintf(response, response_size, "OK interval=%u running=%d\n", state->interval_minutes, state->running) < 0) {
      snprintf(response, response_size, "ERR response error\n");
    }
    return DAEMON_REQUEST_CONTINUE;
  }

  if (strcmp(request, "GET interval") == 0) {
    if (snprintf(response, response_size, "VALUE interval=%u\n", state->interval_minutes) < 0) {
      snprintf(response, response_size, "ERR response error\n");
    }
    return DAEMON_REQUEST_CONTINUE;
  }

  if (strncmp(request, "SET interval ", 13) == 0) {
    char *end = nullptr;
    errno = 0;
    const unsigned long value = strtoul(request + 13, &end, 10);
    if (errno != 0 || end == request + 13 || *end != '\0' || value < 1UL) {
      snprintf(response, response_size, "ERR invalid interval\n");
      return DAEMON_REQUEST_CONTINUE;
    }

    state->interval_minutes = (unsigned) value;
    state->last_monotonic_ms = monotonic_now_ms();
    char message[256];
    const unsigned int minutes = state->interval_minutes;
    snprintf(message, sizeof(message), "Interval changed to %u minute%s. It will trigger again in %u minute%s.",
             minutes, minutes == 1U ? "" : "s", minutes, minutes == 1U ? "" : "s");
    NotifyNotification *notif = notify_notification_new("cignals interval updated", message, "alarm-clock");
    if (notif != nullptr) {
      notify_notification_show(notif, nullptr);
      g_object_unref(G_OBJECT(notif));
    }
    snprintf(response, response_size, "OK interval updated\n");
    return DAEMON_REQUEST_CONTINUE;
  }

  if (strcmp(request, "QUIT") == 0) {
    NotifyNotification *notif =
            notify_notification_new("cignals is shutting down", "The daemon will quit now.", "alarm-clock");
    if (notif != nullptr) {
      notify_notification_show(notif, nullptr);
      g_object_unref(G_OBJECT(notif));
    }
    snprintf(response, response_size, "OK shutting down\n");
    return DAEMON_REQUEST_SHUTDOWN;
  }

  snprintf(response, response_size, "ERR unknown command\n");
  return DAEMON_REQUEST_CONTINUE;
}

static int on_daemon_tick(void *user_data) {
  const auto state = (struct cignals_state *) user_data;
  if (state == nullptr) {
    return DAEMON_REQUEST_CONTINUE;
  }

  const uint64_t now = monotonic_now_ms();
  if (now == 0) {
    return DAEMON_REQUEST_CONTINUE;
  }

  const uint64_t last = state->last_monotonic_ms;
  if (last == 0) {
    state->last_monotonic_ms = now;
    return DAEMON_REQUEST_CONTINUE;
  }

  const uint64_t delta = (now > last) ? (now - last) : 0ULL;
  const uint64_t interval_ms = (uint64_t) state->interval_minutes * 60000ULL;
  if (interval_ms == 0ULL) {
    return DAEMON_REQUEST_CONTINUE;
  }

  if (delta >= interval_ms) {
    char message[256];
    const unsigned long minutes = state->interval_minutes;
    snprintf(message, sizeof(message), "Time to move! (%lu minute%s)", minutes, minutes == 1 ? "" : "s");

    NotifyNotification *n = notify_notification_new("Time to move", message, "alarm-clock");
    if (n != nullptr) {
      notify_notification_show(n, nullptr);
      g_object_unref(G_OBJECT(n));
    }

    /* advance last_monotonic_ms by whole intervals that passed to keep drift small */
    const uint64_t intervals_passed = delta / interval_ms;
    state->last_monotonic_ms = last + (intervals_passed * interval_ms);
  }

  return DAEMON_REQUEST_CONTINUE;
}

int main(const int argc, char **argv) {
  struct cignals_state state;
  state.interval_minutes = 60U;
  state.running = 1;

  if (argc < 2) {
    print_usage(stderr, argv[0]);
    return EXIT_FAILURE;
  }

  if (strcmp(argv[1], "--daemon") == 0) {
    int index = 2;
    if (parse_interval_option(argc, argv, &index, &state) < 0 || index != argc) {
      print_usage(stderr, argv[0]);
      return EXIT_FAILURE;
    }

    return daemon_run(APP_NAME, on_daemon_init, on_daemon_request, on_daemon_tick, on_daemon_deinit, &state,
                      CIGNALS_POLL_MS) == 0
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
  }

  if (strcmp(argv[1], "start") == 0) {
    int index = 2;
    if (parse_interval_option(argc, argv, &index, &state) < 0 || index != argc) {
      print_usage(stderr, argv[0]);
      return EXIT_FAILURE;
    }

    return start_daemon_process(argv[0], &state) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  char socket_path[PATH_MAX];
  if (daemon_socket_path(APP_NAME, socket_path, sizeof(socket_path)) < 0) {
    perror(APP_NAME);
    return EXIT_FAILURE;
  }

  if (strcmp(argv[1], "status") == 0) {
    return send_command(socket_path, "STATUS") == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  if (strcmp(argv[1], "quit") == 0 || strcmp(argv[1], "stop") == 0) {
    return send_command(socket_path, "QUIT") == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  if (strcmp(argv[1], "get") == 0 && argc == 3 && strcmp(argv[2], "interval") == 0) {
    return send_command(socket_path, "GET interval") == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  if (strcmp(argv[1], "set") == 0 && argc == 4 && strcmp(argv[2], "interval") == 0) {
    char request[64];
    const int written = snprintf(request, sizeof(request), "SET interval %s", argv[3]);
    if (written < 0 || (size_t) written >= sizeof(request)) {
      errno = ENAMETOOLONG;
      perror(APP_NAME);
      return EXIT_FAILURE;
    }
    return send_command(socket_path, request) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  print_usage(stderr, argv[0]);
  return EXIT_FAILURE;
}
