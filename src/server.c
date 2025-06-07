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
#include <signal.h>

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

// Data structure to keep track of active client sockets for graceful shutdown
typedef struct client_socket_node_s {
    int sockfd;
    struct client_socket_node_s *next;
} client_socket_node_t;

static char server_root_global[MAX_PATH_LEN];

// Globals for signal handling and graceful shutdown
static client_socket_node_t *g_active_sockets_head = NULL;
static pthread_mutex_t g_sockets_list_mutex;
static volatile sig_atomic_t g_server_running = 1;
static int g_server_sockfd = -1;

static void *client_handler_thread(void *arg);
static void log_event(const char *format, ...);
static void handle_cd(client_thread_data_t *data, const char *path_arg, char *response_buffer, size_t response_max_len);
static void handle_list(client_thread_data_t *data, int client_sockfd);
static char *get_relative_path(const char *abs_path, const char *root_path, char *rel_path_buf, size_t buf_len);
static void format_list_item(char *buffer, size_t buf_size, const char *name, const char *middle, const char *target, const char *suffix);

/*
 * sigint_handler
 * Signal handler for SIGINT/SIGTERM. Sets a flag to initiate graceful shutdown.
 */
static void sigint_handler(int signum) {
    (void)signum;
    g_server_running = 0;
}

/*
 * add_client_socket
 * Adds a client socket descriptor to the global tracked list.
 */
static void add_client_socket(int sockfd) {
    client_socket_node_t *new_node = malloc(sizeof(client_socket_node_t));
    if (!new_node) {
        log_event("CRITICAL: malloc failed for client_socket_node_t. Cannot track socket.");
        abort();
    }
    new_node->sockfd = sockfd;
    pthread_mutex_lock(&g_sockets_list_mutex);
    new_node->next = g_active_sockets_head;
    g_active_sockets_head = new_node;
    pthread_mutex_unlock(&g_sockets_list_mutex);
}

/*
 * remove_client_socket
 * Removes a client socket descriptor from the global tracked list.
 */
static void remove_client_socket(int sockfd) {
    pthread_mutex_lock(&g_sockets_list_mutex);
    client_socket_node_t *current = g_active_sockets_head;
    client_socket_node_t *prev = NULL;
    while (current != NULL) {
        if (current->sockfd == sockfd) {
            if (prev == NULL) {
                g_active_sockets_head = current->next;
            } else {
                prev->next = current->next;
            }
            free(current);
            break;
        }
        prev = current;
        current = current->next;
    }
    pthread_mutex_unlock(&g_sockets_list_mutex);
}

/*
 * main
 * Entry point for the server application.
 */
int main(int argc, char *argv[]) {
    initialize_static_memory();

    if (pthread_mutex_init(&g_sockets_list_mutex, NULL) != 0) {
        perror("pthread_mutex_init failed");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sa.sa_flags = 0; // To interrupt syscalls like accept()
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction failed");
        pthread_mutex_destroy(&g_sockets_list_mutex);
        return 1;
    }

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <port_no> <root_directory>\n", argv[0]);
        pthread_mutex_destroy(&g_sockets_list_mutex);
        return 1;
    }

    char *endptr;
    long port_long = strtol(argv[1], &endptr, 10);
    if (endptr == argv[1] || *endptr != '\0' || port_long <= 0 || port_long > 65535) {
        fprintf(stderr, "Error: Invalid port number '%s'. Must be an integer between 1 and 65535.\n", argv[1]);
        pthread_mutex_destroy(&g_sockets_list_mutex);
        return 1;
    }
    uint16_t port = (uint16_t)port_long;

    if (realpath(argv[2], server_root_global) == NULL) {
        perror("Error resolving server root directory (realpath)");
        fprintf(stderr, "Failed to resolve path: %s\n", argv[2]);
        pthread_mutex_destroy(&g_sockets_list_mutex);
        return 1;
    }
    struct stat root_stat;
    if (stat(server_root_global, &root_stat) != 0) {
        perror("Error stating server root directory (stat)");
        fprintf(stderr, "Failed to stat path: %s\n", server_root_global);
        pthread_mutex_destroy(&g_sockets_list_mutex);
        return 1;
    }
    if (!S_ISDIR(root_stat.st_mode)) {
        fprintf(stderr, "Error: Server root '%s' is not a directory.\n", server_root_global);
        pthread_mutex_destroy(&g_sockets_list_mutex);
        return 1;
    }
    log_event("Server root set to: %s", server_root_global);

    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    g_server_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_sockfd == -1) {
        perror("socket creation failed");
        pthread_mutex_destroy(&g_sockets_list_mutex);
        return 1;
    }

    int optval = 1;
    if (setsockopt(g_server_sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
        perror("setsockopt SO_REUSEADDR failed");
        close(g_server_sockfd);
        pthread_mutex_destroy(&g_sockets_list_mutex);
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(g_server_sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind failed");
        close(g_server_sockfd);
        pthread_mutex_destroy(&g_sockets_list_mutex);
        return 1;
    }

    if (listen(g_server_sockfd, 10) == -1) {
        perror("listen failed");
        close(g_server_sockfd);
        pthread_mutex_destroy(&g_sockets_list_mutex);
        return 1;
    }

    log_event("Ready. Listening on port %u", port);

    while (g_server_running) {
        int client_sockfd = accept(g_server_sockfd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_sockfd == -1) {
            if (errno == EINTR) continue; // Signal received, loop condition will handle exit.
            perror("accept failed");
            continue;
        }

        client_thread_data_t *thread_data = malloc(sizeof(client_thread_data_t));
        if (thread_data == NULL) {
            log_event("CRITICAL: malloc failed for new client thread_data. Aborting server.");
            abort();
        }

        add_client_socket(client_sockfd);

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
            remove_client_socket(client_sockfd);
            free(thread_data);
            close(client_sockfd);
        } else {
            pthread_detach(tid);
        }
    }

    // Shutdown sequence
    log_event("Shutdown signal received. Closing server socket...");
    if (close(g_server_sockfd) == -1) perror("close server_sockfd failed");

    log_event("Signaling all client threads to terminate...");
    pthread_mutex_lock(&g_sockets_list_mutex);
    client_socket_node_t *current = g_active_sockets_head;
    while(current != NULL) {
        shutdown(current->sockfd, SHUT_RDWR);
        current = current->next;
    }
    // Free the tracking list. Threads will try to remove themselves, but by the
    // time they can run, main process might be gone. This cleanup is for valgrind.
    current = g_active_sockets_head;
    while(current != NULL) {
        client_socket_node_t* temp = current;
        current = current->next;
        free(temp);
    }
    g_active_sockets_head = NULL;
    pthread_mutex_unlock(&g_sockets_list_mutex);

    log_event("Server shutting down.");
    pthread_mutex_destroy(&g_sockets_list_mutex);
    return 0;
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
            strncpy(response, SERVER_DEFAULT_WELCOME_MSG, MAX_BUFFER_SIZE - 1);
            response[MAX_BUFFER_SIZE - 1] = '\0';
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
    remove_client_socket(data->client_sockfd);
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

// NOTE: The rest of the functions (handle_cd, handle_list, get_relative_path, format_list_item)
// are unchanged and have been omitted for brevity. They are identical to the original submission.
// Only main(), client_handler_thread(), and the new helper functions/globals are shown in full.
// The omitted functions would follow here in the actual file.

/* ... handle_cd, get_relative_path, handle_list, format_list_item remain the same ... */
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
static void handle_cd(client_thread_data_t *data, const char *path_arg, char *response_buffer, size_t response_max_len) {
    if (response_buffer == NULL || response_max_len == 0) return;
    response_buffer[0] = '\0';

    if (path_arg == NULL || strlen(path_arg) == 0) return;

    char target_path_trial[MAX_PATH_LEN];
    target_path_trial[0] = '\0'; // Initialize for safety
    size_t len_current_root = strlen(data->server_root_abs);
    size_t len_cwd = strlen(data->current_wd_abs);
    size_t len_arg = strlen(path_arg);

    if (path_arg[0] == '/') { // Absolute path from server root
        const char *segment_to_use = path_arg;

        // Case 1: Server root is "/"
        if (strcmp(data->server_root_abs, "/") == 0) {
            // If path_arg is also just "/", target is "/"
            // If path_arg is "/foo", target is "/foo"
            if (len_arg + 1 > MAX_PATH_LEN) return; // path_arg + '\0'
            strcpy(target_path_trial, segment_to_use);
        }
        // Case 2: Server root is "/base"
        else {
            // If path_arg is "/", target is server_root_abs ("/base")
            if (strcmp(segment_to_use, "/") == 0) {
                if (len_current_root + 1 > MAX_PATH_LEN) return;
                strcpy(target_path_trial, data->server_root_abs);
            }
            // If path_arg is "/foo", target is server_root_abs + path_arg ("/base/foo")
            else {
                if (len_current_root + len_arg + 1 > MAX_PATH_LEN) return; // server_root/arg\0
                strcpy(target_path_trial, data->server_root_abs);
                // Ensure we don't add a double slash if server_root_abs ends with one (it shouldn't from realpath)
                // and path_arg starts with one. Realpath will clean this up later.
                strcat(target_path_trial, segment_to_use);
            }
        }
    } else { // Relative path
        if (len_cwd + 1 + len_arg + 1 > MAX_PATH_LEN) return; // cwd + '/' + arg + '\0'
        strcpy(target_path_trial, data->current_wd_abs);
        // Append '/' if current_wd_abs is not "/" and does not already end with '/'
        if (strcmp(data->current_wd_abs, "/") != 0) {
            if (len_cwd > 0 && data->current_wd_abs[len_cwd - 1] != '/') {
                strcat(target_path_trial, "/");
            }
        } else if (len_cwd == 1 && data->current_wd_abs[0] == '/') {
            // current_wd_abs is just "/", target_path_trial is currently "/"
            // If path_arg is "foo", strcat makes "/foo".
            // If path_arg is "/foo" (not expected for relative), strcat makes "//foo".
            // The path_arg for relative CD should not start with '/'.
        }
        strcat(target_path_trial, path_arg);
    }

    if (target_path_trial[0] == '\0' && strcmp(data->server_root_abs, "/") == 0 && strcmp(path_arg, "/") == 0) {
        // Special case: CD / when root is /
        strcpy(target_path_trial, "/");
    }


    char resolved_path[MAX_PATH_LEN];
    if (realpath(target_path_trial, resolved_path) == NULL) return;

    struct stat st;
    if (stat(resolved_path, &st) != 0 || !S_ISDIR(st.st_mode)) return;

    size_t resolved_len = strlen(resolved_path);
    if (resolved_len < len_current_root || strncmp(resolved_path, data->server_root_abs, len_current_root) != 0) return;
    if (resolved_len > len_current_root && resolved_path[len_current_root] != '/') {
        if (strcmp(data->server_root_abs, "/") != 0) return;
    }

    strncpy(data->current_wd_abs, resolved_path, MAX_PATH_LEN -1);
    data->current_wd_abs[MAX_PATH_LEN-1] = '\0';

    char rel_path_for_client[MAX_PATH_LEN];
    if (get_relative_path(data->current_wd_abs, data->server_root_abs, rel_path_for_client, MAX_PATH_LEN)) {
        char display_path[MAX_PATH_LEN];
        if (strcmp(rel_path_for_client, "/") == 0) {
            if (MAX_PATH_LEN > 1) strcpy(display_path, "/"); else display_path[0] = '\0';
        } else if (rel_path_for_client[0] == '/' && strlen(rel_path_for_client) > 1) {
            strncpy(display_path, rel_path_for_client + 1, MAX_PATH_LEN -1);
            display_path[MAX_PATH_LEN-1] = '\0';
        } else {
            strncpy(display_path, rel_path_for_client, MAX_PATH_LEN -1);
            display_path[MAX_PATH_LEN-1] = '\0';
        }

        if (strlen(display_path) + 2 <= response_max_len) {
            snprintf(response_buffer, response_max_len, "%s\n", display_path);
        } else if (response_max_len >= 2) {
            snprintf(response_buffer, response_max_len, "%.*s\n", (int)(response_max_len - 2), display_path);
        }
    }
}
static void format_list_item(char *buffer, size_t buf_size, const char *name, const char *middle, const char *target, const char *suffix) {
    if (buffer == NULL || buf_size == 0) return;
    buffer[0] = '\0';

    char truncated_name[NAME_MAX + 1];
    strncpy(truncated_name, name, NAME_MAX);
    truncated_name[NAME_MAX] = '\0';

    const char *actual_middle = middle ? middle : "";
    const char *actual_suffix = suffix ? suffix : "";
    const char *actual_target = target ? target : "";

    size_t name_len = strlen(truncated_name);
    size_t middle_len = strlen(actual_middle);
    size_t suffix_len = strlen(actual_suffix);
    size_t target_len = strlen(actual_target);

    size_t current_pos = 0;
    size_t space_needed;

    // Try to append name
    space_needed = name_len;
    if (current_pos + space_needed < buf_size) {
        strcpy(buffer + current_pos, truncated_name);
        current_pos += space_needed;
    } else { goto end_format; } // Not enough space even for name

    // Try to append middle (only if target is also to be appended)
    if (middle && target) {
        space_needed = middle_len;
        if (current_pos + space_needed < buf_size) {
            strcpy(buffer + current_pos, actual_middle);
            current_pos += space_needed;
        } else { goto append_suffix_only; } // No space for middle, skip target, try suffix
    }

    // Try to append target (only if middle was appended)
    if (middle && target) {
        // Calculate remaining space for target, considering suffix and null terminator
        size_t space_for_target = 0;
        if (buf_size > current_pos + suffix_len + 1) { // +1 for null terminator
            space_for_target = buf_size - current_pos - suffix_len - 1;
        }

        if (target_len <= space_for_target) {
            strcpy(buffer + current_pos, actual_target);
            current_pos += target_len;
        } else if (space_for_target > 0) { // Truncate target
            strncpy(buffer + current_pos, actual_target, space_for_target);
            current_pos += space_for_target;
            buffer[current_pos] = '\0'; // strncpy might not null terminate
        }
        // If no space for target (space_for_target is 0), it's skipped.
    }

    append_suffix_only:
    // Try to append suffix
    space_needed = suffix_len;
    if (current_pos + space_needed < buf_size) {
        strcpy(buffer + current_pos, actual_suffix);
        current_pos += space_needed; // Not strictly needed as it's the last part
    } else if (buf_size > current_pos && buf_size > 0) {
        buffer[current_pos] = '\0'; // Ensure null termination if suffix doesn't fit
    }

    end_format:
    if (buf_size > 0) buffer[buf_size - 1] = '\0'; // Final safety net for null termination
}
static void handle_list(client_thread_data_t *data, int client_sockfd) {
    DIR *dirp;
    struct dirent *entry;
    char item_path_abs[MAX_PATH_LEN];
    char response_line[MAX_BUFFER_SIZE];

    dirp = opendir(data->current_wd_abs);
    if (dirp == NULL) {
        char err_msg_strerror[128];
        strncpy(err_msg_strerror, strerror(errno), sizeof(err_msg_strerror) - 1);
        err_msg_strerror[sizeof(err_msg_strerror) - 1] = '\0';
        int fixed_len_err = strlen(RESP_ERROR_PREFIX) + strlen("LIST: Cannot open directory ") + strlen(": ") + strlen("\n") +1;
        int max_len_for_cwd_err = 0;
        if (MAX_BUFFER_SIZE > fixed_len_err + strlen(err_msg_strerror)) {
            max_len_for_cwd_err = MAX_BUFFER_SIZE - (fixed_len_err + strlen(err_msg_strerror));
        }
        snprintf(response_line, MAX_BUFFER_SIZE, "%sLIST: Cannot open directory %.*s: %s\n",
                 RESP_ERROR_PREFIX, max_len_for_cwd_err, data->current_wd_abs, err_msg_strerror);
        send_all(client_sockfd, response_line, strlen(response_line));
        return;
    }

    errno = 0;
    while (1) {
        entry = readdir(dirp);
        int readdir_errno_after_call = errno;

        if (entry == NULL) {
            if (readdir_errno_after_call != 0) {
                char err_msg_readdir[128];
                strncpy(err_msg_readdir, strerror(readdir_errno_after_call), sizeof(err_msg_readdir)-1);
                err_msg_readdir[sizeof(err_msg_readdir)-1] = '\0';
                size_t err_prefix_len = strlen(RESP_ERROR_PREFIX) + strlen("LIST: Error reading directory contents: ") + strlen("\n") + 1;
                if (MAX_BUFFER_SIZE > err_prefix_len + strlen(err_msg_readdir)) {
                    snprintf(response_line, MAX_BUFFER_SIZE, "%sLIST: Error reading directory contents: %s\n", RESP_ERROR_PREFIX, err_msg_readdir);
                    if (send_all(client_sockfd, response_line, strlen(response_line)) == -1) { /* Log or handle send error */ }
                }
            }
            break;
        }

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            errno = 0; continue;
        }

        size_t len_cwd_list = strlen(data->current_wd_abs);
        size_t len_entry_name = strlen(entry->d_name);
        size_t required_len;
        if (strcmp(data->current_wd_abs, "/") == 0) required_len = 1 + len_entry_name + 1;
        else required_len = len_cwd_list + 1 + len_entry_name + 1;

        if (required_len > sizeof(item_path_abs)) {
            format_list_item(response_line, MAX_BUFFER_SIZE, entry->d_name, NULL, NULL, " [path too long to stat]\n");
            if (send_all(client_sockfd, response_line, strlen(response_line)) == -1) goto list_cleanup_and_return;
            errno = 0; continue;
        }

        if (strcmp(data->current_wd_abs, "/") == 0) {
            item_path_abs[0] = '/'; strcpy(item_path_abs + 1, entry->d_name);
        } else {
            strcpy(item_path_abs, data->current_wd_abs); strcat(item_path_abs, "/"); strcat(item_path_abs, entry->d_name);
        }

        struct stat st;
        errno = 0;
        if (lstat(item_path_abs, &st) == -1) {
            errno = 0; continue;
        }
        response_line[0] = '\0';

        if (S_ISDIR(st.st_mode)) {
            format_list_item(response_line, MAX_BUFFER_SIZE, entry->d_name, NULL, NULL, "/\n");
        } else if (S_ISLNK(st.st_mode)) {
            char target_buf[MAX_PATH_LEN];
            errno = 0;
            ssize_t readlink_len = readlink(item_path_abs, target_buf, sizeof(target_buf) - 1);

            if (readlink_len != -1) {
                target_buf[readlink_len] = '\0';
                char first_target_abs_path[MAX_PATH_LEN]; first_target_abs_path[0] = '\0';

                if (target_buf[0] == '/') {
                    strncpy(first_target_abs_path, target_buf, MAX_PATH_LEN -1); first_target_abs_path[MAX_PATH_LEN-1] = '\0';
                } else {
                    char item_path_abs_copy_for_dirname[MAX_PATH_LEN];
                    strncpy(item_path_abs_copy_for_dirname, item_path_abs, MAX_PATH_LEN-1); item_path_abs_copy_for_dirname[MAX_PATH_LEN-1] = '\0';
                    char *link_dir = dirname(item_path_abs_copy_for_dirname);

                    if (!link_dir) {
                        format_list_item(response_line, MAX_BUFFER_SIZE, entry->d_name, " -> ", target_buf, " [error resolving link dir]\n");
                        goto send_current_item_response;
                    }
                    size_t len_link_dir = strlen(link_dir); size_t len_target_direct = strlen(target_buf);
                    if (len_link_dir + 1 + len_target_direct + 1 > MAX_PATH_LEN) {
                        format_list_item(response_line, MAX_BUFFER_SIZE, entry->d_name, " -> ", target_buf, " [resolved path too long]\n");
                        goto send_current_item_response;
                    }
                    strcpy(first_target_abs_path, link_dir);
                    if (strcmp(link_dir, "/") != 0) { // Avoid "//" if link_dir is "/"
                        if (len_link_dir > 0 && link_dir[len_link_dir - 1] != '/') strcat(first_target_abs_path, "/");
                    }
                    // Avoid double slash if first_target_abs_path is "/" and target_buf also starts with "/"
                    if (first_target_abs_path[strlen(first_target_abs_path)-1] == '/' && target_buf[0] == '/') {
                        if (strlen(target_buf) > 1) strcat(first_target_abs_path, target_buf + 1);
                        // else if target_buf is just "/", first_target_abs_path remains "/"
                    } else if (target_buf[0] != '\0') { // Avoid appending empty target_buf
                        strcat(first_target_abs_path, target_buf);
                    }
                }

                struct stat first_target_st; int is_intermediate_link = 0; errno = 0;
                if (lstat(first_target_abs_path, &first_target_st) == 0 && S_ISLNK(first_target_st.st_mode)) is_intermediate_link = 1;

                char ultimate_resolved_path_abs[MAX_PATH_LEN]; char display_target_rel[MAX_PATH_LEN]; errno = 0;
                if (realpath(first_target_abs_path, ultimate_resolved_path_abs) != NULL &&
                    get_relative_path(ultimate_resolved_path_abs, data->server_root_abs, display_target_rel, MAX_PATH_LEN)) {
                    if (is_intermediate_link) format_list_item(response_line, MAX_BUFFER_SIZE, entry->d_name, " -->> ", display_target_rel, "\n");
                    else format_list_item(response_line, MAX_BUFFER_SIZE, entry->d_name, " --> ", display_target_rel, "\n");
                    } else {
                        format_list_item(response_line, MAX_BUFFER_SIZE, entry->d_name, " -> ", target_buf, " [unresolved/external]\n");
                    }
            } else {
                format_list_item(response_line, MAX_BUFFER_SIZE, entry->d_name, " -> ", NULL, " [broken link]\n");
            }
        } else {
            format_list_item(response_line, MAX_BUFFER_SIZE, entry->d_name, NULL, NULL, "\n");
        }

        send_current_item_response:
        if (strlen(response_line) > 0) {
            if (send_all(client_sockfd, response_line, strlen(response_line)) == -1) goto list_cleanup_and_return;
        }
        errno = 0;
    }

    list_cleanup_and_return:
    if (closedir(dirp) == -1) perror("closedir in handle_list");
}
