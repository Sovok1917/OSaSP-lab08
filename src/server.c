/*
 * server.c
 * Implements a multi-threaded TCP server that handles client requests
 * according to a simple protocol. It supports commands like ECHO, QUIT, INFO,
 * CD (change directory within a server-defined root), and LIST (list directory contents).
 * Each client connection is handled in a separate thread.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700 // For realpath, dirname
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <dirent.h>
#include <libgen.h>
#include <stdarg.h>
#include <signal.h> // For signal handling

#include "common.h"
#include "protocol.h"

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

typedef struct client_thread_data_s {
    int client_sockfd;
    char client_ip[INET_ADDRSTRLEN];
    int client_port;
    char server_root_abs[MAX_PATH_LEN];
    char current_wd_abs[MAX_PATH_LEN];
} client_thread_data_t;

// --- Global variables for graceful shutdown ---
static volatile sig_atomic_t g_shutdown_flag = 0;
static int g_server_sockfd = -1;
static char server_root_global[MAX_PATH_LEN];
// ---

static void *client_handler_thread(void *arg);
static void log_event(const char *format, ...);
static void handle_cd(client_thread_data_t *data, const char *path_arg, char *response_buffer, size_t response_max_len);
static void handle_list(client_thread_data_t *data, int client_sockfd);
static char *get_relative_path(const char *abs_path, const char *root_path, char *rel_path_buf, size_t buf_len);
static void format_list_item(char *buffer, size_t buf_size, const char *name, const char *middle, const char *target, const char *suffix);
static void signal_handler(int signum);

/*
 * main
 * Entry point for the server application.
 */
int main(int argc, char *argv[]) {
    initialize_static_memory();

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <port_no> <root_directory>\n", argv[0]);
        return 1;
    }

    // --- Setup Signal Handlers ---
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // Do not restart syscalls like accept()

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction for SIGINT failed");
        return 1;
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction for SIGTERM failed");
        return 1;
    }
    // ---

    char *endptr;
    long port_long = strtol(argv[1], &endptr, 10);
    if (endptr == argv[1] || *endptr != '\0' || port_long <= 0 || port_long > 65535) {
        fprintf(stderr, "Error: Invalid port number '%s'. Must be an integer between 1 and 65535.\n", argv[1]);
        return 1;
    }
    uint16_t port = (uint16_t)port_long;

    if (realpath(argv[2], server_root_global) == NULL) {
        perror("Error resolving server root directory (realpath)");
        fprintf(stderr, "Failed to resolve path: %s\n", argv[2]);
        return 1;
    }
    struct stat root_stat;
    if (stat(server_root_global, &root_stat) != 0) {
        perror("Error stating server root directory (stat)");
        fprintf(stderr, "Failed to stat path: %s\n", server_root_global);
        return 1;
    }
    if (!S_ISDIR(root_stat.st_mode)) {
        fprintf(stderr, "Error: Server root '%s' is not a directory.\n", server_root_global);
        return 1;
    }
    log_event("Server root set to: %s", server_root_global);

    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    g_server_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_sockfd == -1) {
        perror("socket creation failed");
        return 1;
    }

    int optval = 1;
    if (setsockopt(g_server_sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
        perror("setsockopt SO_REUSEADDR failed");
        close(g_server_sockfd);
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(g_server_sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind failed");
        close(g_server_sockfd);
        return 1;
    }

    if (listen(g_server_sockfd, 10) == -1) {
        perror("listen failed");
        close(g_server_sockfd);
        return 1;
    }

    log_event("Ready. Listening on port %u", port);

    while (!g_shutdown_flag) {
        int client_sockfd = accept(g_server_sockfd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_sockfd == -1) {
            if (errno == EINTR && g_shutdown_flag) {
                break;
            }
            if (errno == EINTR) continue;
            perror("accept failed");
            continue;
        }

        client_thread_data_t *thread_data = malloc(sizeof(client_thread_data_t));
        if (thread_data == NULL) {
            perror("malloc for thread_data failed");
            close(client_sockfd);
            log_event("CRITICAL: malloc failed for new client thread_data. Aborting server.");
            abort();
        }

        thread_data->client_sockfd = client_sockfd;
        inet_ntop(AF_INET, &client_addr.sin_addr, thread_data->client_ip, INET_ADDRSTRLEN);
        thread_data->client_port = ntohs(client_addr.sin_port);

        strncpy(thread_data->server_root_abs, server_root_global, MAX_PATH_LEN -1);
        thread_data->server_root_abs[MAX_PATH_LEN-1] = '\0';
        strncpy(thread_data->current_wd_abs, server_root_global, MAX_PATH_LEN -1);
        thread_data->current_wd_abs[MAX_PATH_LEN-1] = '\0';

        log_event("Connection request from %s accepted on port %d", thread_data->client_ip, thread_data->client_port);

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_handler_thread, thread_data) != 0) {
            perror("pthread_create failed");
            free(thread_data);
            close(client_sockfd);
        } else {
            pthread_detach(tid);
        }
    }

    log_event("Shutdown signal received. Closing listener socket.");
    if (close(g_server_sockfd) == -1) perror("close server_sockfd failed");
    log_event("Server shut down.");
    return 0;
}

/*
 * signal_handler
 * Catches SIGINT and SIGTERM to allow for a graceful shutdown.
 */
static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        g_shutdown_flag = 1;
    }
}

/*
 * client_handler_thread
 * Handles all communication with a single connected client.
 */
static void *client_handler_thread(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    char buffer[MAX_BUFFER_SIZE];
    char response[MAX_BUFFER_SIZE];
    ssize_t nbytes;

    if (send_all(data->client_sockfd, SERVER_DEFAULT_WELCOME_MSG, strlen(SERVER_DEFAULT_WELCOME_MSG)) == -1) {
        log_event("Error sending welcome message to %s:%d. Closing connection.", data->client_ip, data->client_port);
        goto cleanup;
    }

    while ((nbytes = recv_line(data->client_sockfd, buffer, MAX_BUFFER_SIZE)) > 0) {
        buffer[strcspn(buffer, "\r\n")] = 0;
        log_event("Client %s:%d sent command: '%s'", data->client_ip, data->client_port, buffer);

        char command[MAX_CMD_LEN];
        char cmd_arg[MAX_ARGS_LEN];
        memset(command, 0, sizeof(command));
        memset(cmd_arg, 0, sizeof(cmd_arg));

        sscanf(buffer, "%s %[^\n]", command, cmd_arg);
        response[0] = '\0';

        if (strcmp(command, CMD_ECHO) == 0) {
            snprintf(response, MAX_BUFFER_SIZE, "%s\n", cmd_arg);
        } else if (strcmp(command, CMD_QUIT) == 0) {
            snprintf(response, MAX_BUFFER_SIZE, "%s\n", RESP_BYE);
            if(send_all(data->client_sockfd, response, strlen(response)) == -1) {
                log_event("Error sending BYE response to %s:%d.", data->client_ip, data->client_port);
            }
            log_event("Client %s:%d initiated QUIT. Closing connection.", data->client_ip, data->client_port);
            break;
        } else if (strcmp(command, CMD_INFO) == 0) {
            snprintf(response, MAX_BUFFER_SIZE, "%s", SERVER_DEFAULT_WELCOME_MSG);
        } else if (strcmp(command, CMD_CD) == 0) {
            handle_cd(data, cmd_arg, response, MAX_BUFFER_SIZE);
        } else if (strcmp(command, CMD_LIST) == 0) {
            handle_list(data, data->client_sockfd);
            continue;
        } else {
            if (strlen(command) > 0) {
                snprintf(response, MAX_BUFFER_SIZE, "%sUnknown command: %s\n", RESP_ERROR_PREFIX, command);
            }
        }

        if (strlen(response) > 0) {
            if (send_all(data->client_sockfd, response, strlen(response)) == -1) {
                log_event("Error sending response to %s:%d for command '%s'. Closing connection.",
                          data->client_ip, data->client_port, command);
                break;
            }
        }
    }

    if (nbytes == 0) {
        log_event("Client %s:%d disconnected (received EOF).", data->client_ip, data->client_port);
    } else if (nbytes == -1) {
        log_event("Error receiving data from %s:%d. Connection may be broken.", data->client_ip, data->client_port);
    } else if (nbytes == -2) {
        log_event("Timeout receiving data from %s:%d (unexpected).", data->client_ip, data->client_port);
    }

    cleanup:
    log_event("Closing connection for %s:%d.", data->client_ip, data->client_port);
    if (close(data->client_sockfd) == -1) {
        perror("close client_sockfd failed in client_handler_thread");
    }
    free(data);
    pthread_exit(NULL);
}

/*
 * log_event
 * Logs a message to standard output, prefixed with a timestamp.
 */
static void log_event(const char *format, ...) {
    char timestamp[64];
    char log_buffer[MAX_BUFFER_SIZE + 128 + 64];
    va_list args;

    get_timestamp(timestamp, sizeof(timestamp));

    va_start(args, format);
    int prefix_len = snprintf(log_buffer, sizeof(log_buffer), "%s ", timestamp);
    if (prefix_len < 0 || (size_t)prefix_len >= sizeof(log_buffer)) {
        fprintf(stderr, "Error formatting log prefix in log_event.\n");
        va_end(args);
        return;
    }
    vsnprintf(log_buffer + prefix_len, sizeof(log_buffer) - (size_t)prefix_len, format, args);
    va_end(args);

    printf("%s\n", log_buffer);
    fflush(stdout);
}

/*
 * get_relative_path
 * Converts an absolute path to a path relative to the server's root directory.
 */
static char *get_relative_path(const char *abs_path, const char *root_path, char *rel_path_buf, size_t buf_len) {
    if (rel_path_buf == NULL || buf_len == 0) return NULL;
    rel_path_buf[0] = '\0';

    size_t root_len = strlen(root_path);
    if (strncmp(abs_path, root_path, root_len) != 0) return NULL;

    if (strlen(abs_path) == root_len) {
        if (buf_len < 2) return NULL;
        strcpy(rel_path_buf, "/");
        return rel_path_buf;
    }

    if (abs_path[root_len] != '/' && strcmp(root_path, "/") != 0) return NULL;

    const char *path_after_root;
    if (strcmp(root_path, "/") == 0) path_after_root = abs_path;
    else path_after_root = abs_path + root_len;

    if (strlen(path_after_root) + 1 > buf_len) return NULL;
    strcpy(rel_path_buf, path_after_root);

    if (rel_path_buf[0] == '\0') {
        if (buf_len < 2) return NULL;
        strcpy(rel_path_buf, "/");
    } else if (rel_path_buf[0] != '/') {
        if (strlen(rel_path_buf) + 2 > buf_len) return NULL;
        memmove(rel_path_buf + 1, rel_path_buf, strlen(rel_path_buf) + 1);
        rel_path_buf[0] = '/';
    }
    return rel_path_buf;
}

/*
 * handle_cd
 * Processes the CD (Change Directory) command from a client.
 */
static void handle_cd(client_thread_data_t *data, const char *path_arg, char *response_buffer, size_t response_max_len) {
    if (response_buffer == NULL || response_max_len == 0) return;
    response_buffer[0] = '\0';

    if (path_arg == NULL || strlen(path_arg) == 0) {
        snprintf(response_buffer, response_max_len, "%sCD: Missing argument\n", RESP_ERROR_PREFIX);
        return;
    }

    char target_path_trial[MAX_PATH_LEN];

    if (path_arg[0] == '/') { // Absolute path from server root
        const char *effective_path_arg = (strcmp(path_arg, "/") == 0) ? "" : path_arg;
        size_t root_len = strlen(data->server_root_abs);
        size_t arg_len = strlen(effective_path_arg);

        if (root_len + arg_len + 1 > sizeof(target_path_trial)) {
            snprintf(response_buffer, response_max_len, "%sCD: Resulting path is too long\n", RESP_ERROR_PREFIX);
            return;
        }
        strcpy(target_path_trial, data->server_root_abs);
        strcat(target_path_trial, effective_path_arg);
    } else { // Relative path
        size_t cwd_len = strlen(data->current_wd_abs);
        size_t arg_len = strlen(path_arg);
        if (cwd_len + 1 + arg_len + 1 > sizeof(target_path_trial)) {
            snprintf(response_buffer, response_max_len, "%sCD: Resulting path is too long\n", RESP_ERROR_PREFIX);
            return;
        }
        strcpy(target_path_trial, data->current_wd_abs);
        strcat(target_path_trial, "/");
        strcat(target_path_trial, path_arg);
    }

    char resolved_path[MAX_PATH_LEN];
    if (realpath(target_path_trial, resolved_path) == NULL) {
        snprintf(response_buffer, response_max_len, "%sCD: Invalid path or path does not exist: %s\n", RESP_ERROR_PREFIX, path_arg);
        return;
    }

    struct stat st;
    if (stat(resolved_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        snprintf(response_buffer, response_max_len, "%sCD: Not a directory: %s\n", RESP_ERROR_PREFIX, path_arg);
        return;
    }

    size_t root_len = strlen(data->server_root_abs);
    if (strncmp(resolved_path, data->server_root_abs, root_len) != 0 ||
        (resolved_path[root_len] != '\0' && resolved_path[root_len] != '/' && root_len > 1)) {
        snprintf(response_buffer, response_max_len, "%sCD: Operation not permitted (outside root jail)\n", RESP_ERROR_PREFIX);
    return;
        }

        strncpy(data->current_wd_abs, resolved_path, MAX_PATH_LEN -1);
        data->current_wd_abs[MAX_PATH_LEN-1] = '\0';

        char rel_path_for_client[MAX_PATH_LEN];
        if (get_relative_path(data->current_wd_abs, data->server_root_abs, rel_path_for_client, MAX_PATH_LEN)) {
            char display_path[MAX_PATH_LEN];
            if (strcmp(rel_path_for_client, "/") == 0) {
                strcpy(display_path, "/");
            } else if (rel_path_for_client[0] == '/' && strlen(rel_path_for_client) > 1) {
                strncpy(display_path, rel_path_for_client + 1, sizeof(display_path) - 1);
                display_path[sizeof(display_path) - 1] = '\0';
            } else {
                strncpy(display_path, rel_path_for_client, sizeof(display_path) - 1);
                display_path[sizeof(display_path) - 1] = '\0';
            }
            snprintf(response_buffer, response_max_len, "%s\n", display_path);
        } else {
            snprintf(response_buffer, response_max_len, "%sCD: Error determining relative path\n", RESP_ERROR_PREFIX);
        }
}

/*
 * format_list_item
 * Safely formats a single line for the LIST command output by building it incrementally.
 */
static void format_list_item(char *buffer, size_t buf_size, const char *name, const char *middle, const char *target, const char *suffix) {
    if (buffer == NULL || buf_size == 0) return;
    buffer[0] = '\0';

    size_t current_pos = 0;
    int written;

    // 1. Append name
    written = snprintf(buffer + current_pos, buf_size - current_pos, "%s", name);
    if (written < 0 || (size_t)written >= (buf_size - current_pos)) goto end_format;
    current_pos += written;

    // 2. Append middle
    if (middle) {
        written = snprintf(buffer + current_pos, buf_size - current_pos, "%s", middle);
        if (written < 0 || (size_t)written >= (buf_size - current_pos)) goto end_format;
        current_pos += written;
    }

    // 3. Append target
    if (target) {
        written = snprintf(buffer + current_pos, buf_size - current_pos, "%s", target);
        if (written < 0 || (size_t)written >= (buf_size - current_pos)) goto end_format;
        current_pos += written;
    }

    // 4. Append suffix
    if (suffix) {
        written = snprintf(buffer + current_pos, buf_size - current_pos, "%s", suffix);
        if (written < 0 || (size_t)written >= (buf_size - current_pos)) goto end_format;
        current_pos += written;
    }

    end_format:
    if (buf_size > 0) {
        buffer[buf_size - 1] = '\0';
    }
}

/*
 * handle_list
 * Processes the LIST command from a client.
 */
static void handle_list(client_thread_data_t *data, int client_sockfd) {
    DIR *dirp;
    struct dirent *entry;
    char item_path_abs[MAX_PATH_LEN];
    char response_line[MAX_BUFFER_SIZE];

    dirp = opendir(data->current_wd_abs);
    if (dirp == NULL) {
        snprintf(response_line, sizeof(response_line), "%sLIST: Cannot open directory: %s\n", RESP_ERROR_PREFIX, strerror(errno));
        send_all(client_sockfd, response_line, strlen(response_line));
        return;
    }

    errno = 0;
    while ((entry = readdir(dirp)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Safely construct the absolute path for the directory entry
        size_t cwd_len = strlen(data->current_wd_abs);
        size_t name_len = strlen(entry->d_name);

        if (strcmp(data->current_wd_abs, "/") == 0) {
            if (1 + name_len + 1 > sizeof(item_path_abs)) {
                log_event("Path too long for item: /%s", entry->d_name);
                continue;
            }
            strcpy(item_path_abs, "/");
            strcat(item_path_abs, entry->d_name);
        } else {
            if (cwd_len + 1 + name_len + 1 > sizeof(item_path_abs)) {
                log_event("Path too long for item: %s/%s", data->current_wd_abs, entry->d_name);
                continue;
            }
            strcpy(item_path_abs, data->current_wd_abs);
            strcat(item_path_abs, "/");
            strcat(item_path_abs, entry->d_name);
        }

        struct stat st;
        if (lstat(item_path_abs, &st) == -1) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            format_list_item(response_line, sizeof(response_line), entry->d_name, NULL, NULL, "/\n");
        } else if (S_ISLNK(st.st_mode)) {
            char target_buf[MAX_PATH_LEN];
            ssize_t len = readlink(item_path_abs, target_buf, sizeof(target_buf) - 1);
            if (len != -1) {
                target_buf[len] = '\0';
                format_list_item(response_line, sizeof(response_line), entry->d_name, " -> ", target_buf, "\n");
            } else {
                format_list_item(response_line, sizeof(response_line), entry->d_name, " -> ", "[broken link]", "\n");
            }
        } else {
            format_list_item(response_line, sizeof(response_line), entry->d_name, NULL, NULL, "\n");
        }

        if (send_all(client_sockfd, response_line, strlen(response_line)) == -1) {
            break;
        }
        errno = 0;
    }

    if (errno != 0 && entry == NULL) {
        snprintf(response_line, sizeof(response_line), "%sLIST: Error reading directory: %s\n", RESP_ERROR_PREFIX, strerror(errno));
        send_all(client_sockfd, response_line, strlen(response_line));
    }

    if (closedir(dirp) == -1) {
        perror("closedir in handle_list");
    }
}
