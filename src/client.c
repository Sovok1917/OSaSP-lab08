/*
 * client.c
 * Implements a TCP client that interacts with a server using a simple text-based protocol.
 * It supports interactive command input from the user and batch processing of commands
 * from a specified file.
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

#include "common.h"
#include "protocol.h"

// Function prototypes
static void process_commands_from_file(FILE *file, int sockfd, char *current_prompt_dir);
static void interactive_mode(int sockfd, char *current_prompt_dir);
static void update_prompt_dir(const char *server_response, char *current_prompt_dir, size_t prompt_dir_size);

/*
 * main
 * Entry point for the client application.
 * Parses command-line arguments for server address, port number, and an optional
 * command file. Connects to the server and enters command processing mode
 * (either interactive or batch).
 *
 * Parameters:
 *   argc: int - The number of command-line arguments.
 *   argv: char*[] - Array of command-line argument strings.
 *                   Expected: ./myclient <server_address> <port_number> [@command_file]
 *
 * Returns:
 *   int - 0 on successful completion, 1 on error.
 */
int main(int argc, char *argv[]) {
    initialize_static_memory();

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <server_address> <port_number> [@command_file]\n", argv[0]);
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

    FILE *command_file = NULL;
    if (argc == 4) {
        if (argv[3][0] == '@') {
            const char *filename = argv[3] + 1;
            if (strlen(filename) == 0) {
                fprintf(stderr, "Error: Command file name is missing after '@'.\n");
                return 1;
            }
            command_file = fopen(filename, "r");
            if (command_file == NULL) {
                perror("Error opening command file");
                fprintf(stderr, "Failed to open: %s\n", filename);
                return 1;
            }
        } else {
            fprintf(stderr, "Error: Invalid command file argument format. Use @filename.\n");
            return 1;
        }
    }

    int sockfd;
    struct sockaddr_in server_addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("socket creation failed");
        if (command_file) fclose(command_file);
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("inet_pton failed for server address");
        fprintf(stderr, "Invalid server address: %s\n", server_ip);
        close(sockfd);
        if (command_file) fclose(command_file);
        return 1;
    }

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("connect to server failed");
        close(sockfd);
        if (command_file) fclose(command_file);
        return 1;
    }

    char buffer[MAX_BUFFER_SIZE];
    ssize_t nbytes;

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = CLIENT_RECV_TIMEOUT_MS * 1000;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) == -1) {
        perror("setsockopt SO_RCVTIMEO for welcome message failed");
        // Non-fatal, proceed without timeout for welcome, or with default timeout
    }

    // Receive and print multi-line welcome message
    // Heuristic: welcome message ends when a line contains "Developer:"
    int welcome_received_complete = 0;
    while ((nbytes = recv_line(sockfd, buffer, MAX_BUFFER_SIZE)) > 0) {
        printf("%s", buffer); // buffer includes newline
        if (strstr(buffer, "Developer:") != NULL) {
            welcome_received_complete = 1;
            break;
        }
    }
    if (!welcome_received_complete && nbytes <= 0 && nbytes != -2) { // -2 is timeout
        fprintf(stderr, "Failed to receive complete welcome message or connection closed prematurely.\n");
        close(sockfd);
        if (command_file) fclose(command_file);
        return (nbytes == 0) ? 0 : 1; // Success if clean EOF, error otherwise
    }
    // If nbytes == -2 (timeout), assume welcome message ended.

    char current_prompt_dir[MAX_PATH_LEN] = ""; // For root, prompt is just ">"

    if (command_file) {
        process_commands_from_file(command_file, sockfd, current_prompt_dir);
        if (fclose(command_file) == EOF) {
            perror("fclose command_file failed");
        }
    } else {
        interactive_mode(sockfd, current_prompt_dir);
    }

    if (close(sockfd) == -1) {
        perror("close sockfd failed");
    }
    return 0;
}

/*
 * update_prompt_dir
 * Updates the client's current directory string for the command prompt based
 * on the server's response to a CD command.
 *
 * Parameters:
 *   server_response: const char* - The line received from the server (new relative path).
 *   current_prompt_dir: char* - Buffer to store the directory string for the prompt.
 *   prompt_dir_size: size_t - Size of the current_prompt_dir buffer.
 *
 * Returns:
 *   void
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
        snprintf(current_prompt_dir, prompt_dir_size, "%s", clean_response);
    }
}

/*
 * process_commands_from_file
 * Reads commands from an opened file stream, sends them to the server,
 * and prints server responses.
 *
 * Parameters:
 *   file: FILE* - The opened file stream containing commands.
 *   sockfd: int - The connected socket file descriptor to the server.
 *   current_prompt_dir: char* - String representing the current directory for the prompt.
 *
 * Returns:
 *   void
 */
static void process_commands_from_file(FILE *file, int sockfd, char *current_prompt_dir) {
    char line_buffer[MAX_BUFFER_SIZE];
    char response_buffer[MAX_BUFFER_SIZE];
    ssize_t nbytes_recv;
    int line_count = 0;

    while (fgets(line_buffer, sizeof(line_buffer), file) != NULL) {
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
        sscanf(line_buffer, "%s", temp_cmd_check); // Get first word for QUIT check
        if (strcmp(temp_cmd_check, CMD_QUIT) == 0) {
            if ((nbytes_recv = recv_line(sockfd, response_buffer, MAX_BUFFER_SIZE)) > 0) {
                printf("%s", response_buffer);
            }
            break;
        }

        int is_cd_command = (strncmp(line_buffer, CMD_CD, strlen(CMD_CD)) == 0);
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
            fprintf(stderr, "Server closed connection unexpectedly (file processing line %d).\n", line_count);
            break;
        } else if (nbytes_recv == -1) {
            fprintf(stderr, "Error receiving response from server (file processing line %d).\n", line_count);
            break;
        } // nbytes_recv == -2 (timeout) means server finished sending for this command.
        fflush(stdout);
    }
    if (ferror(file)) {
        perror("Error reading from command file");
    }
}

/*
 * interactive_mode
 * Handles interactive command input from the user via stdin, sends commands
 * to the server, and prints server responses.
 *
 * Parameters:
 *   sockfd: int - The connected socket file descriptor to the server.
 *   current_prompt_dir: char* - String representing the current directory for the prompt.
 *
 * Returns:
 *   void
 */
static void interactive_mode(int sockfd, char *current_prompt_dir) {
    char command_buffer[MAX_BUFFER_SIZE];
    char response_buffer[MAX_BUFFER_SIZE];
    ssize_t nbytes_recv;

    while (1) {
        if (strlen(current_prompt_dir) == 0) printf("> ");
        else printf("%s> ", current_prompt_dir);
        fflush(stdout);

        if (fgets(command_buffer, sizeof(command_buffer), stdin) == NULL) {
            if (feof(stdin)) {
                printf("\nEOF detected on stdin. Sending QUIT command.\n");
                strncpy(command_buffer, CMD_QUIT, MAX_BUFFER_SIZE -1);
                command_buffer[MAX_BUFFER_SIZE-1] = '\0';
                // Fall through to send QUIT
            } else {
                perror("fgets from stdin failed");
                break;
            }
        }

        command_buffer[strcspn(command_buffer, "\r\n")] = 0;
        if (strlen(command_buffer) == 0 && !feof(stdin)) continue;


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
        } else if (nbytes_recv == -1) {
            fprintf(stderr, "\nError receiving response from server.\n");
            break;
        } // nbytes_recv == -2 (timeout) means server finished sending.
        fflush(stdout);
        if (feof(stdin)) break; // Exit loop if QUIT was sent due to EOF
    }
}
