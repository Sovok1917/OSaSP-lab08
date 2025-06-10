/*
 * myclient.c
 * A simple TCP client that connects to a server, sends user input,
 * and prints the server's response. It handles SIGINT for graceful and
 * immediate shutdown using select() for I/O multiplexing.
 */

// Define this macro before any includes to get POSIX function declarations
// like sigaction(). 200809L corresponds to POSIX.1-2008.
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/time.h> // For timeval in setsockopt
#include <signal.h>   // For signal handling

#define BUFFER_SIZE 1024

// --- Global variables for graceful shutdown ---
static volatile sig_atomic_t g_shutdown_flag = 0;
// ---

// Function prototypes
static void process_commands_from_file(FILE *file, int sockfd, char *current_prompt_dir);
static void interactive_mode(int sockfd, char *current_prompt_dir);
static void update_prompt_dir(const char *server_response, char *current_prompt_dir, size_t prompt_dir_size);
static void signal_handler(int signum);

/*
 * main
 * Entry point for the client application.
 */
int main(int argc, char *argv[]);

/*
 * handle_sigint
 * Signal handler for SIGINT (Ctrl+C). Sets a global flag to
 * indicate that the program should terminate gracefully.
 *
 * parameters:
 *   sig - The signal number (unused, required by the signal handler signature).
 *
 * return value:
 *   None.
 */
void handle_sigint(int sig) {
    (void)sig;
    // Set the flag that the main loop checks.
    g_interrupted = 1;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        return 1;
    }

    // --- Setup Signal Handlers ---
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // Do not restart syscalls like fgets()

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction for SIGINT failed");
        return 1;
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction for SIGTERM failed");
        return 1;
    }
    // ---

    const char *server_ip = argv[1];
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: Invalid port number '%s'.\n", argv[2]);
        return 1;
    }

    int sock_fd = -1;
    char *input_buffer = NULL;
    char server_buffer[BUFFER_SIZE];

    input_buffer = malloc(BUFFER_SIZE);
    if (input_buffer == NULL) {
        perror("malloc for input_buffer failed");
        abort();
    }
    input_buffer[0] = '\0';

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    // Do not set SA_RESTART. We want select() to be interrupted by the signal.
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction failed");
        free(input_buffer);
        return 1;
    }

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd == -1) {
        perror("socket creation failed");
        free(input_buffer);
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("inet_pton failed or invalid address");
        close(sock_fd);
        free(input_buffer);
        return 1;
    }

    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("connect failed");
        close(sock_fd);
        free(input_buffer);
        return 1;
    }

    ssize_t bytes_received = recv(sock_fd, server_buffer, BUFFER_SIZE - 1, 0);
    if (bytes_received > 0) {
        server_buffer[bytes_received] = '\0';
        printf("%s", server_buffer);
    } else if (bytes_received == -1 && errno != EINTR) {
        perror("recv for welcome message failed");
    }

    printf("> ");
    fflush(stdout);

    while (!g_interrupted) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds); // Monitor standard input

        // Block here until stdin has data or a signal is caught
        int activity = select(STDIN_FILENO + 1, &read_fds, NULL, NULL, NULL);

    if (g_shutdown_flag) {
        fprintf(stdout, "\nShutdown signal caught. Notifying server...\n");
        char quit_cmd[MAX_CMD_LEN];
        snprintf(quit_cmd, sizeof(quit_cmd), "%s\n", CMD_QUIT);
        if (send_all(sockfd, quit_cmd, strlen(quit_cmd)) == -1) {
            fprintf(stderr, "Warning: Failed to send QUIT command to server during shutdown.\n");
        }
    }

    if (close(sockfd) == -1) {
        perror("close sockfd failed");
    }
    return 0;
}

/*
 * signal_handler
 * Catches SIGINT and SIGTERM to allow for a graceful shutdown.
 * This handler is async-signal-safe.
 *
 * Parameters:
 *   signum: int - The signal number that was caught.
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
 * update_prompt_dir
 * Updates the client's current directory string for the command prompt.
 */
static void update_prompt_dir(const char *server_response, char *current_prompt_dir, size_t prompt_dir_size) {
    if (current_prompt_dir == NULL || prompt_dir_size == 0) return;

    char clean_response[MAX_PATH_LEN];
    strncpy(clean_response, server_response, MAX_PATH_LEN -1);
    clean_response[MAX_PATH_LEN-1] = '\0';
    clean_response[strcspn(clean_response, "\r\n")] = 0;

    if (strlen(clean_response) == 0 || strcmp(clean_response, "/") == 0) {
        if (prompt_dir_size > 0) current_prompt_dir[0] = '\0';
    } else {
        strncpy(current_prompt_dir, clean_response, prompt_dir_size - 1);
        current_prompt_dir[prompt_dir_size - 1] = '\0';
    }
}

/*
 * process_commands_from_file
 * Reads commands from a file, sends them to the server, and prints responses.
 */
static void process_commands_from_file(FILE *file, int sockfd, char *current_prompt_dir) {
    char line_buffer[MAX_BUFFER_SIZE];
    char response_buffer[MAX_BUFFER_SIZE];
    ssize_t nbytes_recv;
    int line_count = 0;

    while (!g_shutdown_flag && fgets(line_buffer, sizeof(line_buffer), file) != NULL) {
        line_count++;
        line_buffer[strcspn(line_buffer, "\r\n")] = 0;

        if (strlen(line_buffer) == 0 && !feof(file)) continue;

        if (strlen(current_prompt_dir) == 0) printf("> %s\n", line_buffer);
        else printf("%s> %s\n", current_prompt_dir, line_buffer);
        fflush(stdout);

        if (send_all(sockfd, line_buffer, strlen(line_buffer)) == -1 || send_all(sockfd, "\n", 1) == -1) {
            fprintf(stderr, "Error sending command/newline from file (line %d): %s\n", line_count, line_buffer);
            break;
        }

        char temp_cmd_check[MAX_CMD_LEN];
        sscanf(line_buffer, "%s", temp_cmd_check);
        if (strcmp(temp_cmd_check, CMD_QUIT) == 0) {
            if ((nbytes_recv = recv_line(sockfd, response_buffer, MAX_BUFFER_SIZE)) > 0) {
                printf("%s", response_buffer);
            }
            perror("select failed");
            break;
        }

        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            if (fgets(input_buffer, BUFFER_SIZE, stdin) == NULL) {
                if (feof(stdin) && !g_interrupted) {
                    printf("\nEOF detected. Shutting down.\n");
                } else if (ferror(stdin) && !g_interrupted) {
                    perror("fgets failed");
                }
                first_line_after_cd = 0;
            }
        }
        if (nbytes_recv == 0) {
            fprintf(stderr, "Server closed connection unexpectedly (file processing line %d).\n", line_count);
            break;
        } else if (nbytes_recv == -1) {
            fprintf(stderr, "Error receiving response from server (file processing line %d).\n", line_count);
            break;
        }
        fflush(stdout);
    }
    if (ferror(file)) {
        perror("Error reading from command file");
    }
    if (g_shutdown_flag) {
        fprintf(stderr, "\nShutdown signal received during file processing. Aborting.\n");
    }
}

/*
 * interactive_mode
 * Handles interactive command input from the user via stdin.
 */
static void interactive_mode(int sockfd, char *current_prompt_dir) {
    char command_buffer[MAX_BUFFER_SIZE];
    char response_buffer[MAX_BUFFER_SIZE];
    ssize_t nbytes_recv;

    while (!g_shutdown_flag) {
        if (strlen(current_prompt_dir) == 0) printf("> ");
        else printf("%s> ", current_prompt_dir);
        fflush(stdout);

        if (fgets(command_buffer, sizeof(command_buffer), stdin) == NULL) {
            if (g_shutdown_flag) {
                // Signal was caught, loop will terminate.
                break;
            }
            if (feof(stdin)) {
                printf("\nEOF detected on stdin. Sending QUIT command.\n");
                strncpy(command_buffer, CMD_QUIT, sizeof(command_buffer) -1);
                command_buffer[sizeof(command_buffer)-1] = '\0';
            } else if (errno == EINTR) {
                // Interrupted by a signal, clear error and continue loop
                clearerr(stdin);
                continue;
            } else {
                perror("fgets from stdin failed");
                break;
            }

            input_buffer[strcspn(input_buffer, "\n")] = '\0';

        if (send_all(sockfd, command_buffer, strlen(command_buffer)) == -1 || send_all(sockfd, "\n", 1) == -1) {
            fprintf(stderr, "Error sending command/newline: %s\n", command_buffer);
            break;
        }

        char temp_cmd_check[MAX_CMD_LEN];
        sscanf(command_buffer, "%s", temp_cmd_check);
        if (strcmp(temp_cmd_check, CMD_QUIT) == 0) {
            if ((nbytes_recv = recv_line(sockfd, response_buffer, MAX_BUFFER_SIZE)) > 0) {
                printf("%s", response_buffer);
            }

            if (send(sock_fd, input_buffer, strlen(input_buffer), 0) == -1) {
                if (errno == EPIPE) {
                    fprintf(stderr, "Server closed the connection.\n");
                } else {
                    perror("send failed");
                }
                break;
            }

            printf("> ");
            fflush(stdout);
        }
    }

    if (g_interrupted) {
        printf("\nInterruption detected. Shutting down.\n");
    }

    if (strcmp(input_buffer, "quit") != 0 && strcmp(input_buffer, "exit") != 0) {
        printf("Exiting. Sending QUIT command to server...\n");
        if (send(sock_fd, "QUIT", 4, 0) == -1) {
            if (errno != EPIPE) {
                perror("send QUIT command failed");
            }
        }
    }

    close(sock_fd);
    free(input_buffer);

    return 0;
}
