/*
 * client.c
 * Implements a TCP client that interacts with the server.
 * Supports interactive mode and batch mode from a file.
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
#include <sys/time.h>

#include "common.h"
#include "protocol.h"

void process_commands_from_file(FILE *file, int sockfd, char *current_prompt_dir);
void interactive_mode(int sockfd, char *current_prompt_dir);
void update_prompt_dir(const char *server_response, char *current_prompt_dir, size_t prompt_dir_size);

int main(int argc, char *argv[]) {
    initialize_static_memory();

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <server_address> <port_number> [@command_file]\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    long port = strtol(argv[2], NULL, 10);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number: %s\n", argv[2]);
        return 1;
    }

    FILE *command_file = NULL;
    if (argc == 4) {
        if (argv[3][0] == '@') {
            const char *filename = argv[3] + 1;
            command_file = fopen(filename, "r");
            if (command_file == NULL) {
                perror("fopen (command_file)");
                fprintf(stderr, "Error opening command file: %s\n", filename);
                return 1;
            }
        } else {
            fprintf(stderr, "Invalid command file argument format. Use @filename.\n");
            return 1;
        }
    }

    int sockfd;
    struct sockaddr_in server_addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("socket");
        if (command_file) fclose(command_file);
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        fprintf(stderr, "Invalid server address: %s\n", server_ip);
        close(sockfd);
        if (command_file) fclose(command_file);
        return 1;
    }

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("connect");
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
        perror("setsockopt SO_RCVTIMEO for welcome");
    }

    while ((nbytes = recv_line(sockfd, buffer, MAX_BUFFER_SIZE)) > 0) {
        printf("%s", buffer);
        if (strstr(buffer, "Разработчик:") != NULL) break;
    }
    if (nbytes == -2) { /* timeout */ }
    else if (nbytes <= 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        fprintf(stderr, "Failed to receive welcome message or connection closed.\n");
        close(sockfd);
        if (command_file) fclose(command_file);
        return (nbytes == 0) ? 0 : 1;
    }

    char current_prompt_dir[MAX_PATH_LEN] = ""; // Initial prompt is just ">"

    if (command_file) {
        process_commands_from_file(command_file, sockfd, current_prompt_dir);
        fclose(command_file);
    } else {
        interactive_mode(sockfd, current_prompt_dir);
    }

    close(sockfd);
    return 0;
}

void update_prompt_dir(const char *server_response, char *current_prompt_dir, size_t prompt_dir_size) {
    char clean_response[MAX_PATH_LEN];
    strncpy(clean_response, server_response, MAX_PATH_LEN -1);
    clean_response[MAX_PATH_LEN-1] = '\0';
    clean_response[strcspn(clean_response, "\r\n")] = 0;

    if (strlen(clean_response) == 0 || strcmp(clean_response, "/") == 0) {
        if (prompt_dir_size > 0) { // Fix for -Wformat-zero-length
            current_prompt_dir[0] = '\0';
        }
    } else {
        snprintf(current_prompt_dir, prompt_dir_size, "%s", clean_response);
    }
}


void process_commands_from_file(FILE *file, int sockfd, char *current_prompt_dir) {
    char line_buffer[MAX_BUFFER_SIZE];
    char response_buffer[MAX_BUFFER_SIZE];
    ssize_t nbytes;
    int line_count = 0;

    while (fgets(line_buffer, sizeof(line_buffer), file) != NULL) {
        line_count++;
        line_buffer[strcspn(line_buffer, "\r\n")] = 0;

        if (strlen(line_buffer) == 0) continue;

        if (strlen(current_prompt_dir) == 0) {
            printf("> %s\n", line_buffer);
        } else {
            printf("%s> %s\n", current_prompt_dir, line_buffer);
        }
        fflush(stdout);

        // Fix for -Wformat-truncation: send command and newline separately
        if (send_all(sockfd, line_buffer, strlen(line_buffer)) == -1) {
            fprintf(stderr, "Error sending command from file (line %d): %s\n", line_count, line_buffer);
            break;
        }
        if (send_all(sockfd, "\n", 1) == -1) { // Send newline
            fprintf(stderr, "Error sending newline from file (line %d)\n", line_count);
            break;
        }


        char temp_cmd_check[MAX_CMD_LEN];
        sscanf(line_buffer, "%s", temp_cmd_check);
        if (strcmp(temp_cmd_check, CMD_QUIT) == 0) {
            if ((nbytes = recv_line(sockfd, response_buffer, MAX_BUFFER_SIZE)) > 0) {
                printf("%s", response_buffer);
            }
            break;
        }

        int is_cd_command = (strncmp(line_buffer, CMD_CD, strlen(CMD_CD)) == 0);
        int first_line_after_cd = 1;

        while ((nbytes = recv_line(sockfd, response_buffer, MAX_BUFFER_SIZE)) > 0) {
            printf("%s", response_buffer);
            if (is_cd_command && first_line_after_cd) {
                if (strncmp(response_buffer, RESP_ERROR_PREFIX, strlen(RESP_ERROR_PREFIX)) != 0) {
                    update_prompt_dir(response_buffer, current_prompt_dir, MAX_PATH_LEN);
                }
                first_line_after_cd = 0;
            }
        }
        if (nbytes == 0) {
            fprintf(stderr, "Server closed connection unexpectedly (file processing line %d).\n", line_count);
            break;
        } else if (nbytes == -1) {
            fprintf(stderr, "Error receiving response from server (file processing line %d).\n", line_count);
            break;
        }
        fflush(stdout);
    }
}

void interactive_mode(int sockfd, char *current_prompt_dir) {
    char command_buffer[MAX_BUFFER_SIZE];
    char response_buffer[MAX_BUFFER_SIZE];
    ssize_t nbytes;

    while (1) {
        if (strlen(current_prompt_dir) == 0) {
            printf("> ");
        } else {
            printf("%s> ", current_prompt_dir);
        }
        fflush(stdout);

        if (fgets(command_buffer, sizeof(command_buffer), stdin) == NULL) {
            if (feof(stdin)) {
                printf("\nEOF detected on stdin. Sending QUIT.\n");
                strncpy(command_buffer, CMD_QUIT, MAX_BUFFER_SIZE -1); // Prepare QUIT command
                command_buffer[MAX_BUFFER_SIZE-1] = '\0';
            } else {
                perror("fgets stdin");
                break;
            }
        }

        command_buffer[strcspn(command_buffer, "\r\n")] = 0;
        if (strlen(command_buffer) == 0 && !feof(stdin)) continue; // Skip empty input unless it's from EOF->QUIT

        // Fix for -Wformat-truncation: send command and newline separately
        if (send_all(sockfd, command_buffer, strlen(command_buffer)) == -1) {
            fprintf(stderr, "Error sending command: %s\n", command_buffer);
            break;
        }
        if (send_all(sockfd, "\n", 1) == -1) { // Send newline
            fprintf(stderr, "Error sending newline for command: %s\n", command_buffer);
            break;
        }


        char temp_cmd_check[MAX_CMD_LEN];
        sscanf(command_buffer, "%s", temp_cmd_check);

        if (strcmp(temp_cmd_check, CMD_QUIT) == 0) {
            if ((nbytes = recv_line(sockfd, response_buffer, MAX_BUFFER_SIZE)) > 0) {
                printf("%s", response_buffer);
            }
            break;
        }

        int is_cd_command = (strcmp(temp_cmd_check, CMD_CD) == 0);
        int first_line_after_cd = 1;

        while ((nbytes = recv_line(sockfd, response_buffer, MAX_BUFFER_SIZE)) > 0) {
            printf("%s", response_buffer);
            if (is_cd_command && first_line_after_cd) {
                if (strncmp(response_buffer, RESP_ERROR_PREFIX, strlen(RESP_ERROR_PREFIX)) != 0) {
                    update_prompt_dir(response_buffer, current_prompt_dir, MAX_PATH_LEN);
                }
                first_line_after_cd = 0;
            }
        }

        if (nbytes == 0) {
            fprintf(stderr, "\nServer closed connection unexpectedly.\n");
            break;
        } else if (nbytes == -1) {
            fprintf(stderr, "\nError receiving response from server.\n");
            break;
        }
        fflush(stdout);
    }
}
