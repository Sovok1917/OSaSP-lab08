/*
 * src/client.c
 *
 * This file implements the TCP client application. It connects to the server,
 * handles user input for interactive sessions, processes client-side commands
 * like LCD, and sends all other commands to the server for execution, including
 * server-side script requests using the '@' syntax.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/time.h> // For timeval in setsockopt
#include <signal.h>   // For signal handling

#include "common.h"
#include "protocol.h"

// Global flag for handling graceful shutdown on signals.
static volatile sig_atomic_t g_shutdown_flag = 0;

// Function Prototypes
static void interactive_mode(int sockfd, char *current_prompt_dir);
static void update_prompt_dir(const char *server_response, char *current_prompt_dir, size_t prompt_dir_size);
static void signal_handler(int signum);

/*
 * Purpose:
 *   The main entry point for the client application. It parses command-line
 *   arguments, establishes a connection to the server, and enters either an
 *   interactive loop or a non-interactive mode to execute a single command
 *   (like a server-side script) and then exit.
 *
 * Parameters:
 *   argc: The number of command-line arguments.
 *   argv: An array of command-line argument strings. The expected usage is:
 *         ./myclient <server_address> <port_number> [@batch_file_on_server]
 *
 * Returns:
 *   0 on successful completion, and 1 on error.
 */
int main(int argc, char *argv[]) {
    initialize_static_memory();

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <server_address> <port_number> [@batch_file_on_server]\n", argv[0]);
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

    const char *server_ip = argv[1];
    char *endptr;
    long port_long = strtol(argv[2], &endptr, 10);
    if (endptr == argv[2] || *endptr != '\0' || port_long <= 0 || port_long > 65535) {
        fprintf(stderr, "Error: Invalid port number '%s'. Must be an integer between 1 and 65535.\n", argv[2]);
        return 1;
    }
    uint16_t port = (uint16_t)port_long;

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("socket creation failed");
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("inet_pton failed for server address");
        close(sockfd);
        return 1;
    }

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("connect to server failed");
        close(sockfd);
        return 1;
    }

    char buffer[MAX_BUFFER_SIZE];
    ssize_t nbytes;

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = CLIENT_RECV_TIMEOUT_MS * 1000;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) == -1) {
        perror("setsockopt SO_RCVTIMEO for welcome message failed");
    }

    int welcome_received_complete = 0;
    while ((nbytes = recv_line(sockfd, buffer, MAX_BUFFER_SIZE)) > 0) {
        printf("%s", buffer);
        if (strstr(buffer, "Developer:") != NULL) {
            welcome_received_complete = 1;
            break;
        }
    }
    if (!welcome_received_complete && nbytes <= 0 && nbytes != -2) {
        fprintf(stderr, "Failed to receive complete welcome message or connection closed prematurely.\n");
        close(sockfd);
        return (nbytes == 0) ? 0 : 1;
    }

    char current_prompt_dir[MAX_PATH_LEN] = "";

    if (argc == 4) { // Non-interactive mode
        const char* command_arg = argv[3];
        if (command_arg[0] != '@') {
            fprintf(stderr, "Error: Invalid fourth argument. Must be of the form @filename\n");
            close(sockfd);
            return 1;
        }
        printf("> %s\n", command_arg);
        char command_to_send[MAX_BUFFER_SIZE];
        snprintf(command_to_send, sizeof(command_to_send), "%s\n", command_arg);
        if (send_all(sockfd, command_to_send, strlen(command_to_send)) == -1) {
            fprintf(stderr, "Error sending command to server.\n");
        } else {
            while ((nbytes = recv_line(sockfd, buffer, MAX_BUFFER_SIZE)) > 0) {
                printf("%s", buffer);
            }
        }
    } else { // Interactive mode
        interactive_mode(sockfd, current_prompt_dir);
    }

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
 * Purpose:
 *   A signal handler that catches SIGINT and SIGTERM to set a global flag,
 *   allowing the main loops to terminate gracefully.
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
 *   Updates the client's command prompt string based on the server's response
 *   to a successful CD command.
 *
 * Parameters:
 *   server_response: The raw response line from the server.
 *   current_prompt_dir: The buffer to update with the new directory name for the prompt.
 *   prompt_dir_size: The size of the current_prompt_dir buffer.
 *
 * Returns:
 *   void
 */
static void update_prompt_dir(const char *server_response, char *current_prompt_dir, size_t prompt_dir_size) {
    if (current_prompt_dir == NULL || prompt_dir_size == 0) return;

    char clean_response[MAX_PATH_LEN];
    snprintf(clean_response, sizeof(clean_response), "%s", server_response);
    clean_response[strcspn(clean_response, "\r\n")] = 0;

    if (strlen(clean_response) == 0 || strcmp(clean_response, "/") == 0) {
        if (prompt_dir_size > 0) current_prompt_dir[0] = '\0';
    } else {
        snprintf(current_prompt_dir, prompt_dir_size, "%s", clean_response);
    }
}

/*
 * Purpose:
 *   Manages the main interactive loop for the client. It displays a prompt,
 *   reads user input, handles client-side commands (like LCD), and sends all
 *   other commands to the server for processing.
 *
 * Parameters:
 *   sockfd: The file descriptor for the connected server socket.
 *   current_prompt_dir: A buffer containing the string for the current server directory prompt.
 *
 * Returns:
 *   void
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
            if (g_shutdown_flag) break;
            if (feof(stdin)) {
                printf("\nEOF detected on stdin. Sending QUIT command.\n");
                snprintf(command_buffer, sizeof(command_buffer), "%s", CMD_QUIT);
            } else if (errno == EINTR) {
                clearerr(stdin);
                continue;
            } else {
                perror("fgets from stdin failed");
                break;
            }
        }

        command_buffer[strcspn(command_buffer, "\r\n")] = 0;
        if (strlen(command_buffer) == 0 && !feof(stdin)) continue;

        if (strncmp(command_buffer, "LCD ", 4) == 0) {
            const char *path = command_buffer + 4;
            if (strlen(path) == 0) {
                fprintf(stderr, "Usage: LCD <local_directory>\n");
            } else {
                if (chdir(path) == -1) {
                    perror("LCD command failed");
                } else {
                    char cwd_buf[MAX_PATH_LEN];
                    if (getcwd(cwd_buf, sizeof(cwd_buf)) != NULL) {
                        printf("Local directory changed to: %s\n", cwd_buf);
                    } else {
                        perror("getcwd failed after LCD");
                    }
                }
            }
            continue;
        }

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
            break;
        }

        int is_cd_command = (strcmp(temp_cmd_check, CMD_CD) == 0);
        int first_line_after_cd = 1;

        while ((nbytes_recv = recv_line(sockfd, response_buffer, MAX_BUFFER_SIZE)) > 0) {
            printf("%s", response_buffer);
            if (is_cd_command && first_line_after_cd) {
                if (strncmp(response_buffer, RESP_ERROR_PREFIX, strlen(RESP_ERROR_PREFIX)) != 0) {
                    update_prompt_dir(response_buffer, current_prompt_dir, MAX_PATH_LEN);
                }
                first_line_after_cd = 0;
            }
        }

        if (nbytes_recv == 0) {
            fprintf(stderr, "\nServer closed connection unexpectedly.\n");
            break;
        } else if (nbytes_recv == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "\nError receiving response from server.\n");
            break;
        }
        fflush(stdout);
        if (feof(stdin)) break;
    }
}
