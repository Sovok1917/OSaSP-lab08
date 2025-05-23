/*
 * server.c
 * Implements a multi-threaded TCP server that handles client requests
 * according to a simple protocol.
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
#include <limits.h> // For PATH_MAX, NAME_MAX (usually in limits.h or sys/param.h)
#include <sys/stat.h>
#include <dirent.h> // For opendir, readdir, closedir
#include <libgen.h> // For dirname
#include <stdarg.h> // Required for va_list, va_start, va_end

#include "common.h"
#include "protocol.h"

#ifndef NAME_MAX // Define if not available, common value
#define NAME_MAX 255
#endif

// Structure to pass data to client handler threads
typedef struct client_thread_data_s {
    int client_sockfd;
    char client_ip[INET_ADDRSTRLEN];
    int client_port;
    char server_root_abs[MAX_PATH_LEN];
    char current_wd_abs[MAX_PATH_LEN]; // Client's current working directory (absolute)
} client_thread_data_t;

static char server_root_global[MAX_PATH_LEN]; // Store canonical server root path

// Function prototypes
void *client_handler_thread(void *arg);
void log_event(const char *format, ...);
void handle_cd(client_thread_data_t *data, const char *path_arg, char *response_buffer, size_t response_max_len);
void handle_list(client_thread_data_t *data, int client_sockfd);
char *get_relative_path(const char *abs_path, const char *root_path, char *rel_path_buf, size_t buf_len);
static void format_list_item(char *buffer, size_t buf_size, const char *name, const char *middle, const char *target, const char *suffix);


/*
 * Main function for the server.
 * Initializes the server, listens for connections, and spawns threads for clients.
 */
int main(int argc, char *argv[]) {
    initialize_static_memory();

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <port_no> <dir>\n", argv[0]);
        return 1;
    }

    long port = strtol(argv[1], NULL, 10);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number: %s\n", argv[1]);
        return 1;
    }

    if (realpath(argv[2], server_root_global) == NULL) {
        perror("realpath (server root directory)");
        fprintf(stderr, "Error resolving server root directory: %s\n", argv[2]);
        return 1;
    }
    struct stat root_stat;
    if (stat(server_root_global, &root_stat) != 0 || !S_ISDIR(root_stat.st_mode)) {
        fprintf(stderr, "Server root '%s' is not a valid directory.\n", server_root_global);
        return 1;
    }
    log_event("Server root set to: %s", server_root_global);


    int server_sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    server_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sockfd == -1) {
        perror("socket");
        return 1;
    }

    int optval = 1;
    if (setsockopt(server_sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
        perror("setsockopt SO_REUSEADDR");
        close(server_sockfd);
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons((uint16_t)port);

    if (bind(server_sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        close(server_sockfd);
        return 1;
    }

    if (listen(server_sockfd, 10) == -1) { // Listen queue size 10
        perror("listen");
        close(server_sockfd);
        return 1;
    }

    log_event("Готов. Listening on port %ld", port);

    while (1) {
        int client_sockfd = accept(server_sockfd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_sockfd == -1) {
            if (errno == EINTR) continue;
            perror("accept");
            continue; // Log error and continue accepting
        }

        client_thread_data_t *thread_data = malloc(sizeof(client_thread_data_t));
        if (thread_data == NULL) {
            perror("malloc for thread_data");
            close(client_sockfd);
            log_event("CRITICAL: malloc failed for thread_data. Aborting.");
            abort();
        }

        thread_data->client_sockfd = client_sockfd;
        inet_ntop(AF_INET, &client_addr.sin_addr, thread_data->client_ip, INET_ADDRSTRLEN);
        thread_data->client_port = ntohs(client_addr.sin_port);
        strncpy(thread_data->server_root_abs, server_root_global, MAX_PATH_LEN -1);
        thread_data->server_root_abs[MAX_PATH_LEN-1] = '\0';
        strncpy(thread_data->current_wd_abs, server_root_global, MAX_PATH_LEN -1);
        thread_data->current_wd_abs[MAX_PATH_LEN-1] = '\0';


        log_event("conn_req %s accepted port:%d", thread_data->client_ip, thread_data->client_port);

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_handler_thread, thread_data) != 0) {
            perror("pthread_create");
            free(thread_data);
            close(client_sockfd);
        }
        pthread_detach(tid);
    }

    close(server_sockfd); // Should be unreachable in normal operation
    return 0;
}

/*
 * Handles communication with a single client in a dedicated thread.
 * arg: Pointer to client_thread_data_t structure.
 */
void *client_handler_thread(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    char buffer[MAX_BUFFER_SIZE];
    char response[MAX_BUFFER_SIZE];
    ssize_t nbytes;

    if (send_all(data->client_sockfd, SERVER_DEFAULT_WELCOME_MSG, strlen(SERVER_DEFAULT_WELCOME_MSG)) == -1) {
        log_event("Error sending welcome message to %s:%d", data->client_ip, data->client_port);
        goto cleanup;
    }

    while ((nbytes = recv_line(data->client_sockfd, buffer, MAX_BUFFER_SIZE)) > 0) {
        buffer[strcspn(buffer, "\r\n")] = 0; // Remove trailing newline/CR
        log_event("Client %s:%d sent: %s", data->client_ip, data->client_port, buffer);

        char command[MAX_CMD_LEN];
        char cmd_arg[MAX_ARGS_LEN];
        memset(command, 0, sizeof(command));
        memset(cmd_arg, 0, sizeof(cmd_arg));

        sscanf(buffer, "%s %[^\n]", command, cmd_arg); // Read first word into command, rest into cmd_arg

        response[0] = '\0';

        if (strcmp(command, CMD_ECHO) == 0) {
            snprintf(response, MAX_BUFFER_SIZE, "%s\n", cmd_arg);
        } else if (strcmp(command, CMD_QUIT) == 0) {
            snprintf(response, MAX_BUFFER_SIZE, "%s\n", RESP_BYE);
            if(send_all(data->client_sockfd, response, strlen(response)) == -1) {
                log_event("Error sending BYE to %s:%d", data->client_ip, data->client_port);
            }
            log_event("Client %s:%d disconnected (QUIT)", data->client_ip, data->client_port);
            break;
        } else if (strcmp(command, CMD_INFO) == 0) {
            strncpy(response, SERVER_DEFAULT_WELCOME_MSG, MAX_BUFFER_SIZE -1);
            response[MAX_BUFFER_SIZE-1] = '\0'; // Ensure null termination
        } else if (strcmp(command, CMD_CD) == 0) {
            handle_cd(data, cmd_arg, response, MAX_BUFFER_SIZE);
        } else if (strcmp(command, CMD_LIST) == 0) {
            handle_list(data, data->client_sockfd);
            continue; // LIST handles its own sending, skip common send_all
        } else {
            snprintf(response, MAX_BUFFER_SIZE, "%sUnknown command: %s\n", RESP_ERROR_PREFIX, command);
        }

        if (strlen(response) > 0) {
            if (send_all(data->client_sockfd, response, strlen(response)) == -1) {
                log_event("Error sending response to %s:%d for command %s", data->client_ip, data->client_port, command);
                break;
            }
        }
    }

    if (nbytes == 0) {
        log_event("Client %s:%d disconnected (EOF)", data->client_ip, data->client_port);
    } else if (nbytes < 0 && nbytes != -2) { // -2 is timeout, not applicable here for server
        log_event("Error receiving from %s:%d: %s", data->client_ip, data->client_port, strerror(errno));
    }

    cleanup:
    close(data->client_sockfd);
    log_event("Connection closed for %s:%d", data->client_ip, data->client_port);
    free(data);
    pthread_exit(NULL);
}

/*
 * Logs an event with a timestamp.
 * format: printf-style format string.
 * ...: Variable arguments for the format string.
 */
void log_event(const char *format, ...) {
    char timestamp[64];
    char log_buffer[MAX_BUFFER_SIZE + 128];
    va_list args;

    get_timestamp(timestamp, sizeof(timestamp));

    va_start(args, format);
    int prefix_len = snprintf(log_buffer, sizeof(log_buffer), "%s ", timestamp);
    if (prefix_len < 0 || (size_t)prefix_len >= sizeof(log_buffer)) {
        fprintf(stderr, "Error formatting log prefix.\n");
        va_end(args);
        return;
    }
    vsnprintf(log_buffer + prefix_len, sizeof(log_buffer) - (size_t)prefix_len, format, args);
    va_end(args);

    printf("%s\n", log_buffer);
    fflush(stdout);
}

/*
 * Converts an absolute path to a path relative to the server root.
 * abs_path: The absolute path to convert.
 * root_path: The absolute path of the server root.
 * rel_path_buf: Buffer to store the resulting relative path.
 * buf_len: Size of rel_path_buf.
 * Returns: Pointer to rel_path_buf on success, or NULL on error.
 * The relative path will start with "/" if it's not the root itself.
 */
char *get_relative_path(const char *abs_path, const char *root_path, char *rel_path_buf, size_t buf_len) {
    if (buf_len == 0) return NULL;
    rel_path_buf[0] = '\0'; // Initialize to empty string

    size_t root_len = strlen(root_path);
    if (strncmp(abs_path, root_path, root_len) != 0) {
        return NULL; // Not within root
    }

    if (strlen(abs_path) == root_len) { // It is the root itself
        if (buf_len < 2) return NULL; // Need space for "/" and "\0"
        strcpy(rel_path_buf, "/");
    } else {
        // Must be root_path + "/" + something_else OR root_path is "/"
        if (abs_path[root_len] != '/' && strcmp(root_path, "/") != 0) {
            return NULL; // e.g. root="/foo", abs_path="/foobar"
        }

        const char *rel_start = abs_path + root_len;
        // If root_path is "/", rel_start points after the initial '/'. We need to prepend '/' to it.
        // If root_path is "/foo", rel_start points to "/bar" or "bar" (if abs_path was /foo/bar or /foobar).
        // We want the output to always start with '/' relative to the root.

        if (strcmp(root_path, "/") == 0) { // Root is "/", rel_start is like "bar" or "/bar"
            if (*rel_start == '/') { // abs_path was "//bar" (unlikely) or "/bar"
                if (strlen(rel_start) + 1 > buf_len) return NULL;
                strcpy(rel_path_buf, rel_start);
            } else { // abs_path was "/bar", rel_start is "bar"
                if (1 + strlen(rel_start) + 1 > buf_len) return NULL; // For '/' + "bar" + '\0'
                rel_path_buf[0] = '/';
                strcpy(rel_path_buf + 1, rel_start);
            }
        } else { // Root is something like "/foo", rel_start is like "/bar"
            if (strlen(rel_start) + 1 > buf_len) return NULL;
            strcpy(rel_path_buf, rel_start); // rel_start should already begin with '/'
        }
        // Ensure it's at least "/" if it became empty (e.g. abs_path was "/foo/" and root "/foo")
        if (rel_path_buf[0] == '\0') {
            if (buf_len < 2) return NULL;
            strcpy(rel_path_buf, "/");
        }
    }
    return rel_path_buf;
}

/*
 * Handles the CD command.
 * data: Client thread data.
 * path_arg: The directory argument from the CD command.
 * response_buffer: Buffer to store the server's response.
 * response_max_len: Maximum length of the response buffer.
 */
void handle_cd(client_thread_data_t *data, const char *path_arg, char *response_buffer, size_t response_max_len) {
    if (path_arg == NULL || strlen(path_arg) == 0) {
        snprintf(response_buffer, response_max_len, "%sCD: Missing directory argument\n", RESP_ERROR_PREFIX);
        return;
    }

    char target_path_trial[MAX_PATH_LEN];
    size_t len_root = strlen(data->server_root_abs);
    size_t len_cwd = strlen(data->current_wd_abs);
    size_t len_arg = strlen(path_arg);

    if (path_arg[0] == '/') { // Absolute path from server root
        const char *actual_path_segment = path_arg;
        // If server_root_abs is "/" and path_arg is "/", actual_path_segment should be "" to form "/"
        if (strcmp(data->server_root_abs, "/") == 0 && strcmp(path_arg, "/") == 0) {
            actual_path_segment = "";
            len_arg = 0; // Effectively
        }
        // If server_root_abs is "/foo" and path_arg is "/bar", target is "/foo/bar"
        // len_root + (len_arg if path_arg doesn't start with / relative to root, or len_arg-1 if it does)
        // Simpler: len_root for root, actual_path_segment for the rest.
        if (len_root + strlen(actual_path_segment) + 1 > MAX_PATH_LEN) {
            snprintf(response_buffer, response_max_len, "%sCD: Resulting path too long (abs)\n", RESP_ERROR_PREFIX);
            return;
        }
        snprintf(target_path_trial, MAX_PATH_LEN, "%s%s", data->server_root_abs, actual_path_segment);
    } else { // Relative path
        if (len_cwd + 1 + len_arg + 1 > MAX_PATH_LEN) { // cwd + '/' + arg + '\0'
            snprintf(response_buffer, response_max_len, "%sCD: Resulting path too long (rel)\n", RESP_ERROR_PREFIX);
            return;
        }
        strcpy(target_path_trial, data->current_wd_abs);
        // Append '/' if current_wd_abs is not "/" and does not already end with '/'
        if (strcmp(data->current_wd_abs, "/") != 0) {
            if (len_cwd > 0 && data->current_wd_abs[len_cwd - 1] != '/') {
                strcat(target_path_trial, "/");
            }
        } else if (len_cwd == 1 && data->current_wd_abs[0] == '/') {
            // current_wd_abs is "/", target_path_trial is "/". strcat will append arg.
        }
        strcat(target_path_trial, path_arg);
    }

    char resolved_path[MAX_PATH_LEN];
    if (realpath(target_path_trial, resolved_path) == NULL) {
        response_buffer[0] = '\0'; // Silently ignore
        return;
    }

    struct stat st;
    if (stat(resolved_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        response_buffer[0] = '\0'; // Silently ignore
        return;
    }

    // Check if resolved_path is within server_root_abs
    size_t current_root_len = strlen(data->server_root_abs); // Use current_root_len for clarity
    size_t resolved_len = strlen(resolved_path);

    if (resolved_len < current_root_len || strncmp(resolved_path, data->server_root_abs, current_root_len) != 0) {
        response_buffer[0] = '\0'; // Silently ignore (outside root)
        return;
    }
    // Further check: if resolved_path is /foo and root is /foo/bar, this is also outside.
    // Or if resolved_path is /foobar and root is /foo.
    if (resolved_len > current_root_len && resolved_path[current_root_len] != '/') {
        // This check is for cases like root="/tmp/root" and resolved_path="/tmp/roota"
        // It's only invalid if server_root_abs is not simply "/"
        if (strcmp(data->server_root_abs, "/") != 0) {
            response_buffer[0] = '\0'; // Silently ignore
            return;
        }
    }

    strncpy(data->current_wd_abs, resolved_path, MAX_PATH_LEN -1);
    data->current_wd_abs[MAX_PATH_LEN-1] = '\0';

    char rel_path_for_client[MAX_PATH_LEN];
    if (get_relative_path(data->current_wd_abs, data->server_root_abs, rel_path_for_client, MAX_PATH_LEN)) {
        char display_path[MAX_PATH_LEN];
        if (strcmp(rel_path_for_client, "/") == 0) {
            if (MAX_PATH_LEN > 1) strcpy(display_path, "/"); else display_path[0] = '\0';
        } else if (rel_path_for_client[0] == '/' && strlen(rel_path_for_client) > 1) {
            // rel_path_for_client is like "/dir", display "dir"
            strncpy(display_path, rel_path_for_client + 1, MAX_PATH_LEN -1);
            display_path[MAX_PATH_LEN-1] = '\0';
        } else { // Should not happen if get_relative_path is correct (always returns / or /something)
            strncpy(display_path, rel_path_for_client, MAX_PATH_LEN -1);
            display_path[MAX_PATH_LEN-1] = '\0';
        }

        if (strlen(display_path) + 2 <= response_max_len) { // +1 for \n, +1 for \0
            snprintf(response_buffer, response_max_len, "%s\n", display_path);
        } else if (response_max_len >= 2) {
            snprintf(response_buffer, response_max_len, "%.*s\n", (int)(response_max_len - 2), display_path);
        } else {
            if (response_max_len > 0) response_buffer[0] = '\0';
        }
    } else {
        snprintf(response_buffer, response_max_len, "%sCD: Internal error resolving relative path\n", RESP_ERROR_PREFIX);
    }
}

/*
 * Helper function to format list items safely.
 * name: name of the directory entry.
 * middle: string like " -> " or " -->> " or NULL.
 * target: target path for symlinks, or NULL.
 * suffix: string like "/" for directories, or "\n".
 */
static void format_list_item(char *buffer, size_t buf_size, const char *name, const char *middle, const char *target, const char *suffix) {
    char truncated_name[NAME_MAX + 1];
    strncpy(truncated_name, name, NAME_MAX);
    truncated_name[NAME_MAX] = '\0';

    size_t name_len = strlen(truncated_name);
    size_t middle_len = middle ? strlen(middle) : 0;
    size_t suffix_len = suffix ? strlen(suffix) : 0; // suffix includes newline
    size_t fixed_len = name_len + middle_len + suffix_len;

    // Check if buffer is too small even for fixed parts + null terminator
    if (buf_size <= fixed_len) { // Note: using <= fixed_len because suffix includes \n, snprintf adds \0
        if (buf_size > 0) buffer[0] = '\0';
        // Optionally, log an error or return a specific "too short" marker
        // For now, just make it an empty string if it's too small.
        // A more robust way would be to try to fit at least the name.
        if (buf_size > name_len + suffix_len) { // Try to fit name and suffix
            snprintf(buffer, buf_size, "%s%s", truncated_name, suffix ? suffix : "");
        } else if (buf_size > 0) {
            buffer[0] = '\0';
        }
        return;
    }

    int available_for_target = 0;
    if (target && middle) { // Only calculate if target is involved
        available_for_target = buf_size - 1 - fixed_len; // -1 for null terminator
        if(available_for_target < 0) available_for_target = 0;
    }


    if (target && middle) {
        snprintf(buffer, buf_size, "%s%s%.*s%s", truncated_name, middle, available_for_target, target, suffix ? suffix : "");
    } else if (middle) { // Should not happen if target is NULL but middle is not
        snprintf(buffer, buf_size, "%s%s%s", truncated_name, middle, suffix ? suffix : "");
    } else { // name + suffix (e.g. file: "name" + "\n", or dir: "name" + "/\n")
        snprintf(buffer, buf_size, "%s%s", truncated_name, suffix ? suffix : "");
    }
}

/*
 * Handles the LIST command. Sends directory listing to the client.
 * data: Client thread data.
 * client_sockfd: The client's socket file descriptor.
 */
void handle_list(client_thread_data_t *data, int client_sockfd) {
    DIR *dirp;
    struct dirent *entry;
    char item_path_abs[MAX_PATH_LEN];
    char response_line[MAX_BUFFER_SIZE];

    dirp = opendir(data->current_wd_abs);
    if (dirp == NULL) {
        char err_msg_strerror[128];
        strncpy(err_msg_strerror, strerror(errno), sizeof(err_msg_strerror) - 1);
        err_msg_strerror[sizeof(err_msg_strerror) - 1] = '\0';

        int fixed_len_err = strlen(RESP_ERROR_PREFIX) + strlen("LIST: Cannot open directory ") + strlen(": ") + strlen("\n");
        int max_len_for_cwd_err = MAX_BUFFER_SIZE - 1 - fixed_len_err - strlen(err_msg_strerror);
        if (max_len_for_cwd_err < 0) max_len_for_cwd_err = 0;

        snprintf(response_line, MAX_BUFFER_SIZE, "%sLIST: Cannot open directory %.*s: %s\n",
                 RESP_ERROR_PREFIX, max_len_for_cwd_err, data->current_wd_abs, err_msg_strerror);
        send_all(client_sockfd, response_line, strlen(response_line));
        return;
    }

    errno = 0; // Clear errno before starting to read the directory
    while (1) {
        entry = readdir(dirp);
        if (entry == NULL) {
            if (errno != 0) { // An error occurred during readdir
                char err_msg_readdir[128];
                strncpy(err_msg_readdir, strerror(errno), sizeof(err_msg_readdir)-1);
                err_msg_readdir[sizeof(err_msg_readdir)-1] = '\0';
                // Avoid using snprintf if MAX_BUFFER_SIZE is very small
                if (MAX_BUFFER_SIZE > (strlen(RESP_ERROR_PREFIX) + strlen("LIST: Error reading directory contents: ") + strlen(err_msg_readdir) + 2)) {
                    snprintf(response_line, MAX_BUFFER_SIZE, "%sLIST: Error reading directory contents: %s\n", RESP_ERROR_PREFIX, err_msg_readdir);
                    if (send_all(client_sockfd, response_line, strlen(response_line)) == -1) {
                        // Error sending error message
                    }
                }
            }
            break; // Exit the loop (either clean EOF or error handled)
        }

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            errno = 0;
            continue;
        }

        size_t len_cwd_list = strlen(data->current_wd_abs);
        size_t len_entry_name = strlen(entry->d_name);
        size_t required_len;

        if (strcmp(data->current_wd_abs, "/") == 0) {
            required_len = 1 + len_entry_name + 1; // "/name\0"
        } else {
            required_len = len_cwd_list + 1 + len_entry_name + 1; // "cwd/name\0"
        }

        if (required_len > sizeof(item_path_abs)) {
            format_list_item(response_line, MAX_BUFFER_SIZE, entry->d_name, NULL, NULL, " [path too long to stat]\n");
            if (send_all(client_sockfd, response_line, strlen(response_line)) == -1) { goto list_cleanup_and_return; }
            errno = 0; continue;
        }

        if (strcmp(data->current_wd_abs, "/") == 0) {
            item_path_abs[0] = '/';
            strcpy(item_path_abs + 1, entry->d_name);
        } else {
            strcpy(item_path_abs, data->current_wd_abs);
            strcat(item_path_abs, "/");
            strcat(item_path_abs, entry->d_name);
        }

        struct stat st;
        if (lstat(item_path_abs, &st) == -1) {
            errno = 0;
            continue;
        }

        response_line[0] = '\0';

        if (S_ISDIR(st.st_mode)) {
            format_list_item(response_line, MAX_BUFFER_SIZE, entry->d_name, NULL, NULL, "/\n");
        } else if (S_ISLNK(st.st_mode)) {
            char target_buf[MAX_PATH_LEN]; // Direct target content
            ssize_t readlink_len = readlink(item_path_abs, target_buf, sizeof(target_buf) - 1);

            if (readlink_len != -1) {
                target_buf[readlink_len] = '\0';

                char first_target_abs_path[MAX_PATH_LEN]; // Absolute path of the direct target

                if (target_buf[0] == '/') {
                    strncpy(first_target_abs_path, target_buf, MAX_PATH_LEN -1);
                    first_target_abs_path[MAX_PATH_LEN-1] = '\0';
                } else {
                    char item_path_abs_copy_for_dirname[MAX_PATH_LEN];
                    strncpy(item_path_abs_copy_for_dirname, item_path_abs, MAX_PATH_LEN-1);
                    item_path_abs_copy_for_dirname[MAX_PATH_LEN-1] = '\0';
                    char *link_dir = dirname(item_path_abs_copy_for_dirname);

                    if (!link_dir) {
                        format_list_item(response_line, MAX_BUFFER_SIZE, entry->d_name, " -> ", target_buf, " [error resolving link dir]\n");
                        goto send_current_item_response;
                    }
                    size_t len_link_dir = strlen(link_dir);
                    size_t len_target_direct = strlen(target_buf);
                    if (len_link_dir + 1 + len_target_direct + 1 > MAX_PATH_LEN) {
                        format_list_item(response_line, MAX_BUFFER_SIZE, entry->d_name, " -> ", target_buf, " [resolved path too long]\n");
                        goto send_current_item_response;
                    }
                    strcpy(first_target_abs_path, link_dir);
                    if (strcmp(link_dir, "/") != 0) { // Avoid "//" if link_dir is "/"
                        if (len_link_dir > 0 && link_dir[len_link_dir - 1] != '/') {
                            strcat(first_target_abs_path, "/");
                        }
                    }
                    // Only append if first_target_abs_path is not "/" or target_buf is not empty
                    // and avoid double slashes if first_target_abs_path is "/"
                    if (strcmp(first_target_abs_path, "/") == 0 && target_buf[0] == '/') {
                        // if first_target is "/" and target_buf is "/foo", use target_buf
                        strncpy(first_target_abs_path, target_buf, MAX_PATH_LEN -1);
                        first_target_abs_path[MAX_PATH_LEN-1] = '\0';
                    } else if (strcmp(first_target_abs_path, "/") == 0 && target_buf[0] != '/') {
                        strcat(first_target_abs_path, target_buf); // results in "/target_buf"
                    } else if (strcmp(first_target_abs_path, "/") != 0) {
                        strcat(first_target_abs_path, target_buf);
                    }
                }

                struct stat first_target_st;
                int is_intermediate_link = 0;
                if (lstat(first_target_abs_path, &first_target_st) == 0 && S_ISLNK(first_target_st.st_mode)) {
                    is_intermediate_link = 1;
                }

                char ultimate_resolved_path_abs[MAX_PATH_LEN];
                char display_target_rel[MAX_PATH_LEN];

                if (realpath(first_target_abs_path, ultimate_resolved_path_abs) != NULL &&
                    get_relative_path(ultimate_resolved_path_abs, data->server_root_abs, display_target_rel, MAX_PATH_LEN)) {
                    if (is_intermediate_link) {
                        format_list_item(response_line, MAX_BUFFER_SIZE, entry->d_name, " -->> ", display_target_rel, "\n");
                    } else {
                        format_list_item(response_line, MAX_BUFFER_SIZE, entry->d_name, " --> ", display_target_rel, "\n");
                    }
                    } else {
                        format_list_item(response_line, MAX_BUFFER_SIZE, entry->d_name, " -> ", target_buf, " [unresolved/external]\n");
                    }
            } else { // readlink failed
                format_list_item(response_line, MAX_BUFFER_SIZE, entry->d_name, " -> ", NULL, " [broken link]\n");
            }
        } else {
            format_list_item(response_line, MAX_BUFFER_SIZE, entry->d_name, NULL, NULL, "\n");
        }

        send_current_item_response:
        if (strlen(response_line) > 0) {
            if (send_all(client_sockfd, response_line, strlen(response_line)) == -1) {
                goto list_cleanup_and_return;
            }
        }
        errno = 0; // Reset errno before the next call to readdir
    } // End of while(1) loop

    list_cleanup_and_return:
    if (closedir(dirp) == -1) {
        perror("closedir");
    }
}
