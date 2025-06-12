/*
 * src/common.c
 *
 * This file implements the shared utility functions declared in common.h. These
 * functions provide common functionality required by both the client and server,
 * such as reliable socket I/O and timestamp generation.
 */
#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>

/*
 * Purpose:
 *   Initializes any static memory that requires runtime setup. This function
 *   is intended to be called at the start of main() to ensure a clean state.
 *
 * Parameters:
 *   None
 *
 * Returns:
 *   void
 */
void initialize_static_memory(void) {
    // No static memory requiring special runtime initialization in this project.
}

/*
 * Purpose:
 *   Populates a buffer with the current timestamp in YYYY.MM.DD-HH:MM:SS.sss format.
 *
 * Parameters:
 *   buffer: A pointer to the character buffer where the timestamp string will be stored.
 *   len: The size of the provided buffer.
 *
 * Returns:
 *   void
 */
void get_timestamp(char *buffer, size_t len) {
    if (buffer == NULL || len == 0) {
        return;
    }
    struct timeval tv;
    struct tm *tm_info;

    if (gettimeofday(&tv, NULL) == -1) {
        perror("gettimeofday in get_timestamp");
        snprintf(buffer, len, "TIMESTAMP_ERROR");
        return;
    }

    time_t sec = tv.tv_sec;
    tm_info = localtime(&sec);
    if (tm_info == NULL) {
        perror("localtime in get_timestamp");
        snprintf(buffer, len, "TIMESTAMP_ERROR");
        return;
    }

    size_t written = strftime(buffer, len, "%Y.%m.%d-%H:%M:%S", tm_info);
    if (written > 0 && written + 5 < len) { // +5 for ".sss\0"
        snprintf(buffer + written, len - written, ".%03ld", (long)tv.tv_usec / 1000);
    }
}

/*
 * Purpose:
 *   Reliably sends a specified number of bytes from a buffer over a socket,
 *   handling potential partial sends.
 *
 * Parameters:
 *   sockfd: The file descriptor of the socket to send data to.
 *   buffer: A pointer to the data buffer to be sent.
 *   length: The number of bytes to send from the buffer.
 *
 * Returns:
 *   0 on success, or -1 on error.
 */
int send_all(int sockfd, const char *buffer, size_t length) {
    if (buffer == NULL) {
        fprintf(stderr, "send_all: buffer is NULL\n");
        return -1;
    }
    size_t total_sent = 0;
    while (total_sent < length) {
        ssize_t sent_bytes = send(sockfd, buffer + total_sent, length - total_sent, 0);
        if (sent_bytes == -1) {
            if (errno == EINTR) continue;
            perror("send in send_all");
            return -1;
        }
        if (sent_bytes == 0) {
            fprintf(stderr, "send_all: send returned 0 (peer closed connection)\n");
            return -1;
        }
        total_sent += (size_t)sent_bytes;
    }
    return 0;
}

/*
 * Purpose:
 *   Receives a line of text (terminated by '\n') from a socket. The function
 *   handles reading byte-by-byte and ensures the resulting buffer is null-terminated.
 *
 * Parameters:
 *   sockfd: The file descriptor of the socket to receive data from.
 *   buffer: A pointer to the buffer where the received line will be stored.
 *   max_len: The maximum size of the buffer, including the null terminator.
 *
 * Returns:
 *   The number of bytes read (including the newline, if present) on success.
 *   0 if the connection was closed by the peer.
 *   -1 on a critical socket error.
 *   -2 if a timeout occurred (if SO_RCVTIMEO is set).
 */
ssize_t recv_line(int sockfd, char *buffer, size_t max_len) {
    if (buffer == NULL || max_len == 0) {
        fprintf(stderr, "recv_line: buffer is NULL or max_len is 0\n");
        return -1;
    }
    if (max_len == 1) {
        buffer[0] = '\0';
        return 0;
    }

    size_t current_len = 0;
    char ch;
    ssize_t nbytes;

    while (current_len < max_len - 1) {
        nbytes = recv(sockfd, &ch, 1, 0);
        if (nbytes == 1) {
            buffer[current_len++] = ch;
            if (ch == '\n') break;
        } else if (nbytes == 0) { // Connection closed
            break;
        } else { // Error (nbytes == -1)
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) { // Timeout
                if (current_len == 0) {
                    buffer[0] = '\0';
                    return -2;
                }
                break; // Timeout after partial read
            }
            perror("recv in recv_line");
            buffer[current_len] = '\0';
            return -1;
        }
    }
    buffer[current_len] = '\0';
    return (ssize_t)current_len;
}
