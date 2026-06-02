#include "daemon.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <syslog.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define DAEMON_RUNTIME_FALLBACK_PREFIX "/tmp"

static volatile sig_atomic_t g_shutdown_requested = 0;

static void handle_shutdown_signal(const int signal_number) {
  (void) signal_number;
  g_shutdown_requested = 1;
}

static int write_all(const int fd, const char *buffer, const size_t size) {
  size_t written = 0;

  while (written < size) {
    const ssize_t chunk = write(fd, buffer + written, size - written);
    if (chunk < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    written += (size_t) chunk;
  }

  return 0;
}

static ssize_t read_line(const int fd, char *buffer, const size_t size) {
  if (size == 0) {
    errno = EINVAL;
    return -1;
  }

  size_t used = 0;
  while (used + 1 < size) {
    char ch = '\0';
    const ssize_t chunk = read(fd, &ch, 1);
    if (chunk < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (chunk == 0 || ch == '\n') {
      break;
    }
    buffer[used++] = ch;
  }

  buffer[used] = '\0';
  return (ssize_t) used;
}

static void trim_whitespace(char *text) {
  if (text == nullptr) {
    return;
  }

  const char *start = text;
  while (*start != '\0' && isspace((unsigned char) *start)) {
    ++start;
  }

  if (start != text) {
    memmove(text, start, strlen(start) + 1);
  }

  size_t length = strlen(text);
  while (length > 0 && isspace((unsigned char) text[length - 1])) {
    text[--length] = '\0';
  }
}

static int ensure_directory_exists(const char *path, const mode_t mode) {
  if (mkdir(path, mode) == 0) {
    return 0;
  }

  if (errno == EEXIST) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
      return 0;
    }
    errno = ENOTDIR;
  }

  return -1;
}

static int build_path(char *buffer, const size_t size, const char *left, const char *right) {
  const int written = snprintf(buffer, size, "%s/%s", left, right);
  if (written < 0 || (size_t) written >= size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}

static int sanitize_app_name(const char *app_name) {
  if (app_name == nullptr || app_name[0] == '\0') {
    errno = EINVAL;
    return -1;
  }

  for (auto p = (const unsigned char *) app_name; *p != '\0'; ++p) {
    if (!(isalnum(*p) || *p == '-' || *p == '_' || *p == '.')) {
      errno = EINVAL;
      return -1;
    }
  }

  return 0;
}

static int close_open_file_descriptors(void) {
  struct rlimit limit;
  long max_fd = 1024;

  if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_max != RLIM_INFINITY) {
    max_fd = (long) limit.rlim_max;
  }

  for (long fd = 3; fd < max_fd; ++fd) {
    close((int) fd);
  }

  return 0;
}

static int redirect_standard_streams(void) {
  const int dev_null = open("/dev/null", O_RDWR);
  if (dev_null < 0) {
    return -1;
  }

  if (dup2(dev_null, STDIN_FILENO) < 0 || dup2(dev_null, STDOUT_FILENO) < 0 || dup2(dev_null, STDERR_FILENO) < 0) {
    close(dev_null);
    return -1;
  }

  if (dev_null > STDERR_FILENO) {
    close(dev_null);
  }

  return 0;
}

static int daemonize_process(void) {
  pid_t pid = fork();
  if (pid < 0) {
    return -1;
  }
  if (pid > 0) {
    _exit(EXIT_SUCCESS);
  }

  if (setsid() < 0) {
    return -1;
  }

  pid = fork();
  if (pid < 0) {
    return -1;
  }
  if (pid > 0) {
    _exit(EXIT_SUCCESS);
  }

  umask(027);

  if (chdir("/") < 0) {
    return -1;
  }

  close_open_file_descriptors();
  if (redirect_standard_streams() < 0) {
    return -1;
  }

  return 0;
}

static int write_pid_file(const char *path) {
  const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0640);
  if (fd < 0) {
    return -1;
  }

  char pid_buffer[32];
  const int written = snprintf(pid_buffer, sizeof(pid_buffer), "%ld\n", (long) getpid());
  if (written < 0 || (size_t) written >= sizeof(pid_buffer)) {
    close(fd);
    errno = ENAMETOOLONG;
    return -1;
  }

  const int rc = write_all(fd, pid_buffer, (size_t) written);
  close(fd);
  return rc;
}

static int make_server_socket(const char *socket_path) {
  const int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_fd < 0) {
    return -1;
  }

  if (unlink(socket_path) < 0 && errno != ENOENT) {
    close(server_fd);
    return -1;
  }

  struct sockaddr_un address = {0};
  address.sun_family = AF_UNIX;

  if (strlen(socket_path) >= sizeof(address.sun_path)) {
    close(server_fd);
    errno = ENAMETOOLONG;
    return -1;
  }

  strncpy(address.sun_path, socket_path, sizeof(address.sun_path) - 1);
  const socklen_t address_length = (socklen_t) (offsetof(struct sockaddr_un, sun_path) + strlen(address.sun_path) + 1);

  if (bind(server_fd, (struct sockaddr *) &address, address_length) < 0) {
    close(server_fd);
    return -1;
  }

  if (listen(server_fd, 16) < 0) {
    close(server_fd);
    return -1;
  }

  return server_fd;
}

static int send_text_response(const int fd, const char *text) { return write_all(fd, text, strlen(text)); }

static int handle_client_request(const int client_fd, const daemon_request_fn on_request, void *user_data,
                                 int *shutdown_requested) {
  char request[512];
  if (read_line(client_fd, request, sizeof(request)) < 0) {
    return -1;
  }

  trim_whitespace(request);
  if (request[0] == '\0') {
    return send_text_response(client_fd, "ERR empty command\n");
  }

  char response[1024];
  response[0] = '\0';

  const int result = on_request(user_data, request, response, sizeof(response));
  if (response[0] == '\0') {
    if (send_text_response(client_fd, "OK\n") < 0) {
      return -1;
    }
  } else if (write_all(client_fd, response, strlen(response)) < 0) {
    return -1;
  }

  if (result == DAEMON_REQUEST_SHUTDOWN) {
    *shutdown_requested = 1;
  }

  return 0;
}

int daemon_runtime_dir(const char *app_name, char *buffer, const size_t size) {
  if (sanitize_app_name(app_name) < 0 || buffer == nullptr || size == 0) {
    errno = EINVAL;
    return -1;
  }

  const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
  if (runtime_dir != nullptr && runtime_dir[0] != '\0') {
    const int written = snprintf(buffer, size, "%s/%s", runtime_dir, app_name);
    if (written < 0 || (size_t) written >= size) {
      errno = ENAMETOOLONG;
      return -1;
    }
    return 0;
  }

  const int written =
          snprintf(buffer, size, "%s/%s-%lu", DAEMON_RUNTIME_FALLBACK_PREFIX, app_name, (unsigned long) getuid());
  if (written < 0 || (size_t) written >= size) {
    errno = ENAMETOOLONG;
    return -1;
  }

  return 0;
}

int daemon_socket_path(const char *app_name, char *buffer, const size_t size) {
  if (sanitize_app_name(app_name) < 0) {
    return -1;
  }

  char runtime_dir[PATH_MAX];
  if (daemon_runtime_dir(app_name, runtime_dir, sizeof(runtime_dir)) < 0) {
    return -1;
  }

  char socket_name[PATH_MAX];
  const int written = snprintf(socket_name, sizeof(socket_name), "%s.sock", app_name);
  if (written < 0 || (size_t) written >= sizeof(socket_name)) {
    errno = ENAMETOOLONG;
    return -1;
  }

  return build_path(buffer, size, runtime_dir, socket_name);
}

int daemon_send_request(const char *socket_path, const char *request, char *response, const size_t response_size,
                        const int timeout_ms) {
  if (socket_path == nullptr || request == nullptr || response == nullptr || response_size == 0 || timeout_ms < 0) {
    errno = EINVAL;
    return -1;
  }

  const int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (client_fd < 0) {
    return -1;
  }

  struct sockaddr_un address = {0};
  address.sun_family = AF_UNIX;

  if (strlen(socket_path) >= sizeof(address.sun_path)) {
    close(client_fd);
    errno = ENAMETOOLONG;
    return -1;
  }

  strncpy(address.sun_path, socket_path, sizeof(address.sun_path) - 1);
  const socklen_t address_length = (socklen_t) (offsetof(struct sockaddr_un, sun_path) + strlen(address.sun_path) + 1);

  if (connect(client_fd, (struct sockaddr *) &address, address_length) < 0) {
    close(client_fd);
    return -1;
  }

  char request_line[512];
  const int request_length =
          snprintf(request_line, sizeof(request_line), "%s%s", request, strchr(request, '\n') != nullptr ? "" : "\n");
  if (request_length < 0 || (size_t) request_length >= sizeof(request_line)) {
    close(client_fd);
    errno = ENAMETOOLONG;
    return -1;
  }

  if (write_all(client_fd, request_line, (size_t) request_length) < 0) {
    close(client_fd);
    return -1;
  }

  shutdown(client_fd, SHUT_WR);

  size_t used = 0;
  while (used + 1 < response_size) {
    struct pollfd pollfd;
    pollfd.fd = client_fd;
    pollfd.events = POLLIN;
    pollfd.revents = 0;

    const int ready = poll(&pollfd, 1, timeout_ms);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      close(client_fd);
      return -1;
    }
    if (ready == 0) {
      close(client_fd);
      errno = ETIMEDOUT;
      return -1;
    }

    const ssize_t chunk = read(client_fd, response + used, response_size - 1 - used);
    if (chunk < 0) {
      if (errno == EINTR) {
        continue;
      }
      close(client_fd);
      return -1;
    }
    if (chunk == 0) {
      break;
    }

    used += (size_t) chunk;
    if (memchr(response, '\n', used) != nullptr) {
      break;
    }
  }

  response[used] = '\0';
  close(client_fd);
  return 0;
}

int daemon_run(const char *app_name, const daemon_init_fn on_init, const daemon_request_fn on_request,
               const daemon_tick_fn on_tick, const daemon_deinit_fn on_deinit, void *user_data,
               const int daemon_poll_ms) {
  if (sanitize_app_name(app_name) < 0 || on_request == nullptr) {
    errno = EINVAL;
    return -1;
  }

  if (daemonize_process() < 0) {
    return -1;
  }

  openlog(app_name, LOG_PID | LOG_NDELAY, LOG_DAEMON);

  char runtime_dir[PATH_MAX];
  char socket_path[PATH_MAX];
  char pid_path[PATH_MAX];
  char lock_path[PATH_MAX];

  if (daemon_runtime_dir(app_name, runtime_dir, sizeof(runtime_dir)) < 0) {
    syslog(LOG_ERR, "failed to determine runtime directory: %s", strerror(errno));
    closelog();
    return -1;
  }

  if (ensure_directory_exists(runtime_dir, 0700) < 0) {
    syslog(LOG_ERR, "failed to create runtime directory '%s': %s", runtime_dir, strerror(errno));
    closelog();
    return -1;
  }

  char socket_name[PATH_MAX];
  const int socket_name_written = snprintf(socket_name, sizeof(socket_name), "%s.sock", app_name);
  if (socket_name_written < 0 || (size_t) socket_name_written >= sizeof(socket_name) ||
      build_path(socket_path, sizeof(socket_path), runtime_dir, socket_name) < 0 ||
      build_path(pid_path, sizeof(pid_path), runtime_dir, "daemon.pid") < 0 ||
      build_path(lock_path, sizeof(lock_path), runtime_dir, "daemon.lock") < 0) {
    syslog(LOG_ERR, "failed to build runtime paths: %s", strerror(errno));
    closelog();
    return -1;
  }

  const int lock_fd = open(lock_path, O_RDWR | O_CREAT, 0600);
  if (lock_fd < 0) {
    syslog(LOG_ERR, "failed to open lock file '%s': %s", lock_path, strerror(errno));
    closelog();
    return -1;
  }

  if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
    syslog(LOG_ERR, "another daemon instance is already running");
    close(lock_fd);
    closelog();
    errno = EBUSY;
    return -1;
  }

  if (write_pid_file(pid_path) < 0) {
    syslog(LOG_ERR, "failed to write pid file '%s': %s", pid_path, strerror(errno));
    close(lock_fd);
    closelog();
    return -1;
  }

  const int server_fd = make_server_socket(socket_path);
  if (server_fd < 0) {
    syslog(LOG_ERR, "failed to create control socket '%s': %s", socket_path, strerror(errno));
    unlink(pid_path);
    close(lock_fd);
    closelog();
    return -1;
  }

  struct sigaction action = {0};
  action.sa_handler = handle_shutdown_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGTERM, &action, nullptr);
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGHUP, &action, nullptr);

  syslog(LOG_INFO, "daemon started: pid=%ld", (long) getpid());

  if (on_init != nullptr && on_init(user_data) < 0) {
    syslog(LOG_ERR, "failed to init daemon: %s", strerror(errno));
  }

  int shutdown_requested = 0;
  while (!shutdown_requested && !g_shutdown_requested) {
    struct pollfd pollfd;
    pollfd.fd = server_fd;
    pollfd.events = POLLIN;
    pollfd.revents = 0;

    const int ready = poll(&pollfd, 1, daemon_poll_ms);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      syslog(LOG_ERR, "poll failed: %s", strerror(errno));
      break;
    }

    if (ready == 0) {
      if (on_tick != nullptr && on_tick(user_data) == DAEMON_REQUEST_SHUTDOWN) {
        shutdown_requested = 1;
      }
      continue;
    }

    if ((pollfd.revents & POLLIN) == 0) {
      continue;
    }

    const int client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      syslog(LOG_ERR, "accept failed: %s", strerror(errno));
      break;
    }

    if (handle_client_request(client_fd, on_request, user_data, &shutdown_requested) < 0) {
      syslog(LOG_ERR, "failed to process client request: %s", strerror(errno));
    }
    close(client_fd);
  }

  syslog(LOG_INFO, "daemon stopping");
  if (on_deinit != nullptr) {
    on_deinit(user_data);
  }
  close(server_fd);
  unlink(socket_path);
  unlink(pid_path);
  close(lock_fd);
  closelog();
  return 0;
}
