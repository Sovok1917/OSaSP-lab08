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
#include <netdb.h>
#include <sys/select.h> // Required for select()

#define BUFFER_SIZE 1024

// Global flag for signal handler to ensure safe termination.
volatile sig_atomic_t g_interrupted = 0;

/*
 * main
 * The main entry point for the client application. It handles command-line
 * arguments, connects to the server, and manages the main I/O loop.
 *
 * parameters:
 *   argc - The number of command-line arguments.
 *   argv - An array of command-line argument strings.
 *
 * return value:
 *   Returns 0 on successful execution, 1 on error.
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

        if (activity < 0) {
            if (errno == EINTR) {
                // Interrupted by our SIGINT handler. The loop will now terminate.
                continue;
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
                break;
            }

            input_buffer[strcspn(input_buffer, "\n")] = '\0';

            if (strcmp(input_buffer, "quit") == 0 || strcmp(input_buffer, "exit") == 0) {
                break;
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
