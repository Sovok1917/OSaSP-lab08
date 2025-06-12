/*
 * src/server.c
 *
 * This file implements a multi-threaded TCP server. It listens for client
 * connections, spawning a new thread for each one. The server restricts all
 * file operations to a specified root directory ("jail") for security. It
 * processes commands like LIST, CD, and executes server-side scripts requested
 * via the '@' command.
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
#include <ctype.h>  // For isspace

#include "common.h"
#include "protocol.h"

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#define MAX_SCRIPT_DEPTH 5 // Prevents infinite recursion in @ command

typedef struct client_thread_data_s {
    int client_sockfd;
    char client_ip[INET_ADDRSTRLEN];
    int client_port;
    char server_root_abs[MAX_PATH_LEN];
    char current_wd_abs[MAX_PATH_LEN];
    int script_depth; // For tracking nested @ calls
} client_thread_data_t;

// Global variables for handling graceful shutdown.
static volatile sig_atomic_t g_shutdown_flag = 0;
static int g_server_sockfd = -1;
static char server_root_global[MAX_PATH_LEN];

// Function Prototypes
static void *client_handler_thread(void *arg);
static int process_client_command(client_thread_data_t *data, char *command_line);
static void log_event(const char *format, ...);
static void handle_cd(client_thread_data_t *data, const char *path_arg);
static void handle_list(client_thread_data_t *data);
static void handle_at_command(client_thread_data_t *data, const char *filename);
static char *get_relative_path(const char *abs_path, const char *root_path, char *rel_path_buf, size_t buf_len);
static void format_list_item(char *buffer, size_t buf_size, const char *name, const char *middle, const char *target, const char *suffix);
static void signal_handler(int signum);

/*
 * Purpose:
 *   The main entry point for the server application. It initializes the server,
 *   sets up a listening socket on a specified port, and enters an infinite
 *   loop to accept and handle incoming client connections, each in a new thread.
 *
 * Parameters:
 *   argc: The number of command-line arguments.
 *   argv: An array of command-line argument strings. The expected usage is:
 *         ./myserver <port_no> <root_directory>
 *
 * Returns:
 *   0 on successful shutdown, and 1 on error.
 */
int main(int argc, char *argv[]) {
    initialize_static_memory();

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <port_no> <root_directory>\n", argv[0]);
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction for SIGINT failed");
        return 1;
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction for SIGTERM failed");
        return 1;
    }

    char *endptr;
    long port_long = strtol(argv[1], &endptr, 10);
    if (endptr == argv[1] || *endptr != '\0' || port_long <= 0 || port_long > 65535) {
        fprintf(stderr, "Error: Invalid port number '%s'. Must be an integer between 1 and 65535.\n", argv[1]);
        return 1;
    }
    uint16_t port = (uint16_t)port_long;

    if (realpath(argv[2], server_root_global) == NULL) {
        perror("Error resolving server root directory (realpath)");
        return 1;
    }
    struct stat root_stat;
    if (stat(server_root_global, &root_stat) != 0) {
        perror("Error stating server root directory (stat)");
        return 1;
    }
    if (!S_ISDIR(root_stat.st_mode)) {
        fprintf(stderr, "Error: Server root '%s' is not a directory.\n", server_root_global);
        return 1;
    }
    log_event("Server root set to: %s", server_root_global);

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

    struct sockaddr_in server_addr;
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
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client_sockfd = accept(g_server_sockfd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_sockfd == -1) {
            if (errno == EINTR && g_shutdown_flag) break;
            if (errno == EINTR) continue;
            perror("accept failed");
            continue;
        }

        client_thread_data_t *thread_data = malloc(sizeof(client_thread_data_t));
        if (thread_data == NULL) {
            perror("malloc for thread_data failed");
            close(client_sockfd);
            abort();
        }

        thread_data->client_sockfd = client_sockfd;
        inet_ntop(AF_INET, &client_addr.sin_addr, thread_data->client_ip, INET_ADDRSTRLEN);
        thread_data->client_port = ntohs(client_addr.sin_port);
        thread_data->script_depth = 0;

        strncpy(thread_data->server_root_abs, server_root_global, MAX_PATH_LEN);
        strncpy(thread_data->current_wd_abs, server_root_global, MAX_PATH_LEN);

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
 * Purpose:
 *   A signal handler that catches SIGINT and SIGTERM to set a global flag,
 *   allowing the main accept loop to terminate gracefully.
 *
 * Parameters:
 *   signum: The signal number that was caught.
 *
 * Returns:
 *   void
 */
static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        g_shutdown_flag = 1;
    }
}

/*
 * Purpose:
 *   The main function for each client thread. It handles all communication with
 *   a single connected client, reading commands and dispatching them for processing
 *   until the client disconnects or sends a QUIT command.
 *
 * Parameters:
 *   arg: A void pointer to a dynamically allocated client_thread_data_t struct
 *        containing connection-specific information.
 *
 * Returns:
 *   A void pointer (always NULL).
 */
static void *client_handler_thread(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    char buffer[MAX_BUFFER_SIZE];
    ssize_t nbytes;

    if (send_all(data->client_sockfd, SERVER_DEFAULT_WELCOME_MSG, strlen(SERVER_DEFAULT_WELCOME_MSG)) == -1) {
        log_event("Error sending welcome message to %s:%d.", data->client_ip, data->client_port);
        goto cleanup;
    }

    while ((nbytes = recv_line(data->client_sockfd, buffer, MAX_BUFFER_SIZE)) > 0) {
        buffer[strcspn(buffer, "\r\n")] = 0;
        log_event("Client %s:%d sent command: '%s'", data->client_ip, data->client_port, buffer);

        if (process_client_command(data, buffer) != 0) {
            break;
        }
    }

    if (nbytes == 0) {
        log_event("Client %s:%d disconnected (received EOF).", data->client_ip, data->client_port);
    } else if (nbytes == -1) {
        log_event("Error receiving data from %s:%d.", data->client_ip, data->client_port);
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
 * Purpose:
 *   Acts as a central dispatcher for all client commands. It parses the command
 *   line, identifies the primary command, and calls the appropriate handler function.
 *
 * Parameters:
 *   data: A pointer to the client's thread-specific data structure.
 *   command_line: The raw command string received from the client.
 *
 * Returns:
 *   0 to continue the client session, or 1 to terminate it (e.g., on QUIT).
 */
static int process_client_command(client_thread_data_t *data, char *command_line) {
    char command[MAX_CMD_LEN];
    char cmd_arg[MAX_ARGS_LEN];
    char response[MAX_BUFFER_SIZE];

    char *cmd_start = command_line;
    while (*cmd_start && isspace((unsigned char)*cmd_start)) {
        cmd_start++;
    }

    if (*cmd_start == '@') {
        const char *filename = cmd_start + 1;
        while (*filename && isspace((unsigned char)*filename)) {
            filename++;
        }
        handle_at_command(data, filename);
        return 0;
    }

    memset(command, 0, sizeof(command));
    memset(cmd_arg, 0, sizeof(cmd_arg));
    response[0] = '\0';

    sscanf(cmd_start, "%s %[^\n]", command, cmd_arg);

    if (strcmp(command, CMD_ECHO) == 0) {
        snprintf(response, sizeof(response), "%s\n", cmd_arg);
    } else if (strcmp(command, CMD_QUIT) == 0) {
        snprintf(response, sizeof(response), "%s\n", RESP_BYE);
        send_all(data->client_sockfd, response, strlen(response));
        return 1;
    } else if (strcmp(command, CMD_INFO) == 0) {
        snprintf(response, sizeof(response), "%s", SERVER_DEFAULT_WELCOME_MSG);
    } else if (strcmp(command, CMD_CD) == 0) {
        handle_cd(data, cmd_arg);
        return 0;
    } else if (strcmp(command, CMD_LIST) == 0) {
        handle_list(data);
        return 0;
    } else {
        if (strlen(command) > 0) {
            snprintf(response, sizeof(response), "%sUnknown command: %s\n", RESP_ERROR_PREFIX, command);
        }
    }

    if (strlen(response) > 0) {
        if (send_all(data->client_sockfd, response, strlen(response)) == -1) {
            return 1;
        }
    }
    return 0;
}

/*
 * Purpose:
 *   Prints a formatted log message to standard output, prefixed with a timestamp.
 *   This function is variadic, accepting arguments like printf.
 *
 * Parameters:
 *   format: A printf-style format string for the log message.
 *   ...: A variable number of arguments corresponding to the format string.
 *
 * Returns:
 *   void
 */
static void log_event(const char *format, ...) {
    char timestamp[64];
    char log_buffer[MAX_BUFFER_SIZE + 128];
    va_list args;

    get_timestamp(timestamp, sizeof(timestamp));

    va_start(args, format);
    int prefix_len = snprintf(log_buffer, sizeof(log_buffer), "%s ", timestamp);
    if (prefix_len > 0 && (size_t)prefix_len < sizeof(log_buffer)) {
        vsnprintf(log_buffer + prefix_len, sizeof(log_buffer) - prefix_len, format, args);
    }
    va_end(args);

    printf("%s\n", log_buffer);
    fflush(stdout);
}

/*
 * Purpose:
 *   Calculates a client-facing relative path from an absolute server path,
 *   based on the server's chroot-like root directory.
 *
 * Parameters:
 *   abs_path: The absolute path on the server's filesystem.
 *   root_path: The absolute path of the server's root jail.
 *   rel_path_buf: The buffer to store the resulting relative path.
 *   buf_len: The size of the rel_path_buf.
 *
 * Returns:
 *   A pointer to rel_path_buf on success, or NULL on failure.
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

    const char *path_after_root = (strcmp(root_path, "/") == 0) ? abs_path : (abs_path + root_len);

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
 * Purpose:
 *   Handles the CD (Change Directory) command. It resolves the requested path,
 *   performs security checks to prevent escaping the root jail, and updates
 *   the client's current working directory if successful.
 *
 * Parameters:
 *   data: A pointer to the client's thread-specific data structure.
 *   path_arg: The directory path argument provided by the client.
 *
 * Returns:
 *   void
 */
static void handle_cd(client_thread_data_t *data, const char *path_arg) {
    char response_buffer[MAX_BUFFER_SIZE];
    response_buffer[0] = '\0';

    if (path_arg == NULL || strlen(path_arg) == 0) {
        snprintf(response_buffer, sizeof(response_buffer), "%sCD: Missing argument\n", RESP_ERROR_PREFIX);
        send_all(data->client_sockfd, response_buffer, strlen(response_buffer));
        return;
    }

    char target_path_trial[MAX_PATH_LEN];
    if (path_arg[0] == '/') {
        snprintf(target_path_trial, sizeof(target_path_trial), "%s%s", data->server_root_abs, (strcmp(path_arg, "/") == 0) ? "" : path_arg);
    } else {
        if (strlen(data->current_wd_abs) + 1 + strlen(path_arg) >= sizeof(target_path_trial)) {
            snprintf(response_buffer, sizeof(response_buffer), "%sCD: Resulting path is too long\n", RESP_ERROR_PREFIX);
            send_all(data->client_sockfd, response_buffer, strlen(response_buffer));
            return;
        }
        strcpy(target_path_trial, data->current_wd_abs);
        strcat(target_path_trial, "/");
        strcat(target_path_trial, path_arg);
    }

    char resolved_path[MAX_PATH_LEN];
    if (realpath(target_path_trial, resolved_path) == NULL) {
        snprintf(response_buffer, sizeof(response_buffer), "%sCD: Invalid path: %s\n", RESP_ERROR_PREFIX, path_arg);
    } else {
        struct stat st;
        if (stat(resolved_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            snprintf(response_buffer, sizeof(response_buffer), "%sCD: Not a directory: %s\n", RESP_ERROR_PREFIX, path_arg);
        } else if (strncmp(resolved_path, data->server_root_abs, strlen(data->server_root_abs)) != 0) {
            snprintf(response_buffer, sizeof(response_buffer), "%sCD: Operation not permitted\n", RESP_ERROR_PREFIX);
        } else {
            strncpy(data->current_wd_abs, resolved_path, sizeof(data->current_wd_abs) - 1);
            data->current_wd_abs[sizeof(data->current_wd_abs) - 1] = '\0';
            char rel_path[MAX_PATH_LEN];
            if (get_relative_path(data->current_wd_abs, data->server_root_abs, rel_path, sizeof(rel_path))) {
                snprintf(response_buffer, sizeof(response_buffer), "%s\n", (strcmp(rel_path, "/") == 0) ? "/" : rel_path + 1);
            } else {
                snprintf(response_buffer, sizeof(response_buffer), "%sCD: Error determining relative path\n", RESP_ERROR_PREFIX);
            }
        }
    }
    send_all(data->client_sockfd, response_buffer, strlen(response_buffer));
}

/*
 * Purpose:
 *   Handles the server-side script execution triggered by the '@' command. It
 *   validates the script path, opens the file, and processes each line as a
 *   new command, with protection against infinite recursion.
 *
 * Parameters:
 *   data: A pointer to the client's thread-specific data structure.
 *   filename: The name of the script file to execute.
 *
 * Returns:
 *   void
 */
static void handle_at_command(client_thread_data_t *data, const char *filename) {
    char response_buffer[MAX_BUFFER_SIZE];

    if (filename == NULL || strlen(filename) == 0) {
        snprintf(response_buffer, sizeof(response_buffer), "%s@: Missing filename\n", RESP_ERROR_PREFIX);
        send_all(data->client_sockfd, response_buffer, strlen(response_buffer));
        return;
    }

    if (data->script_depth >= MAX_SCRIPT_DEPTH) {
        snprintf(response_buffer, sizeof(response_buffer), "%s@: Maximum script recursion depth (%d) exceeded\n", RESP_ERROR_PREFIX, MAX_SCRIPT_DEPTH);
        send_all(data->client_sockfd, response_buffer, strlen(response_buffer));
        return;
    }

    char script_path_trial[MAX_PATH_LEN];
    if (strlen(data->current_wd_abs) + 1 + strlen(filename) >= sizeof(script_path_trial)) {
        snprintf(response_buffer, sizeof(response_buffer), "%s@: Resulting script path is too long\n", RESP_ERROR_PREFIX);
        send_all(data->client_sockfd, response_buffer, strlen(response_buffer));
        return;
    }
    strcpy(script_path_trial, data->current_wd_abs);
    strcat(script_path_trial, "/");
    strcat(script_path_trial, filename);

    char resolved_path[MAX_PATH_LEN];
    if (realpath(script_path_trial, resolved_path) == NULL) {
        snprintf(response_buffer, sizeof(response_buffer), "%s@: Script not found: %s\n", RESP_ERROR_PREFIX, filename);
        send_all(data->client_sockfd, response_buffer, strlen(response_buffer));
        return;
    }

    if (strncmp(resolved_path, data->server_root_abs, strlen(data->server_root_abs)) != 0) {
        snprintf(response_buffer, sizeof(response_buffer), "%s@: Access to script denied: %s\n", RESP_ERROR_PREFIX, filename);
        send_all(data->client_sockfd, response_buffer, strlen(response_buffer));
        return;
    }

    FILE *script_file = fopen(resolved_path, "r");
    if (script_file == NULL) {
        snprintf(response_buffer, sizeof(response_buffer), "%s@: Cannot open script '%s': %s\n", RESP_ERROR_PREFIX, filename, strerror(errno));
        send_all(data->client_sockfd, response_buffer, strlen(response_buffer));
        return;
    }

    data->script_depth++;
    log_event("Client %s:%d starting script '%s' (depth %d)", data->client_ip, data->client_port, filename, data->script_depth);

    char line_buffer[MAX_BUFFER_SIZE];
    while (fgets(line_buffer, sizeof(line_buffer), script_file) != NULL) {
        line_buffer[strcspn(line_buffer, "\r\n")] = 0;
        if (strlen(line_buffer) == 0) continue;

        snprintf(response_buffer, sizeof(response_buffer), "script> %.4080s\n", line_buffer);
        if (send_all(data->client_sockfd, response_buffer, strlen(response_buffer)) == -1) break;

        if (process_client_command(data, line_buffer) != 0) break;
    }

    if (ferror(script_file)) perror("Error reading from script file");
    fclose(script_file);

    log_event("Client %s:%d finished script '%s' (depth %d)", data->client_ip, data->client_port, filename, data->script_depth);
    data->script_depth--;
}

/*
 * Purpose:
 *   Safely constructs a single line of output for the LIST command.
 *
 * Parameters:
 *   buffer: The destination buffer for the formatted string.
 *   buf_size: The size of the destination buffer.
 *   name: The name of the file or directory.
 *   middle: An optional string to place after the name (e.g., " -> ").
 *   target: An optional target for symlinks.
 *   suffix: An optional suffix (e.g., "/" for directories or "\n").
 *
 * Returns:
 *   void
 */
static void format_list_item(char *buffer, size_t buf_size, const char *name, const char *middle, const char *target, const char *suffix) {
    if (buffer == NULL || buf_size == 0) return;
    buffer[0] = '\0';
    size_t current_pos = 0;
    int written;

    written = snprintf(buffer + current_pos, buf_size - current_pos, "%s", name);
    if (written < 0 || (size_t)written >= (buf_size - current_pos)) return;
    current_pos += written;

    if (middle) {
        written = snprintf(buffer + current_pos, buf_size - current_pos, "%s", middle);
        if (written < 0 || (size_t)written >= (buf_size - current_pos)) return;
        current_pos += written;
    }
    if (target) {
        written = snprintf(buffer + current_pos, buf_size - current_pos, "%s", target);
        if (written < 0 || (size_t)written >= (buf_size - current_pos)) return;
        current_pos += written;
    }
    if (suffix) {
        written = snprintf(buffer + current_pos, buf_size - current_pos, "%s", suffix);
        if (written < 0 || (size_t)written >= (buf_size - current_pos)) return;
    }
}

/*
 * Purpose:
 *   Handles the LIST command by reading the contents of the client's current
 *   directory on the server, formatting each entry, and sending it to the client.
 *
 * Parameters:
 *   data: A pointer to the client's thread-specific data structure.
 *
 * Returns:
 *   void
 */
static void handle_list(client_thread_data_t *data) {
    char response_line[MAX_BUFFER_SIZE];
    DIR *dirp = opendir(data->current_wd_abs);
    if (dirp == NULL) {
        snprintf(response_line, sizeof(response_line), "%sLIST: Cannot open directory: %s\n", RESP_ERROR_PREFIX, strerror(errno));
        send_all(data->client_sockfd, response_line, strlen(response_line));
        return;
    }

    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dirp)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char item_path_abs[MAX_PATH_LEN];
        if (strlen(data->current_wd_abs) + 1 + strlen(entry->d_name) >= sizeof(item_path_abs)) {
            log_event("Path too long for item, skipping: %s/%s", data->current_wd_abs, entry->d_name);
            continue;
        }
        strcpy(item_path_abs, data->current_wd_abs);
        strcat(item_path_abs, "/");
        strcat(item_path_abs, entry->d_name);

        struct stat st;
        if (lstat(item_path_abs, &st) == -1) continue;

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

        if (send_all(data->client_sockfd, response_line, strlen(response_line)) == -1) break;
        errno = 0;
    }

    if (errno != 0 && entry == NULL) {
        snprintf(response_line, sizeof(response_line), "%sLIST: Error reading directory: %s\n", RESP_ERROR_PREFIX, strerror(errno));
        send_all(data->client_sockfd, response_line, strlen(response_line));
    }

    if (closedir(dirp) == -1) {
        perror("closedir in handle_list");
    }
}
