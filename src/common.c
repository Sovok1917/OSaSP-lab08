/*
 * common.c
 * Implements shared utility functions for the client and server applications.
 * These include timestamp generation, reliable socket sending, and line-based receiving.
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

#define HAS_REVERSIBLE_CC_MODE 0
#if HAS_REVERSIBLE_CC_MODE
// Example: Code specific to REVERSIBLE_CC_MODE
#endif

/*
 * initialize_static_memory
 * Initializes any global or static memory that requires runtime initialization.
 * This function is called at the beginning of main() in both client and server.
 * Currently, it serves as a placeholder for potential future use.
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
 * get_timestamp
 * Populates the provided buffer with the current timestamp.
 * The format is YYYY.MM.DD-HH:MM:SS.sss.
 *
 * Parameters:
 *   buffer: char* - The buffer where the timestamp string will be stored.
 *   len: size_t - The maximum size of the buffer.
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
        if (len > 15) snprintf(buffer, len, "TIMESTAMP_ERROR");
        else if (len > 0) buffer[0] = '\0';
        return;
    }

    time_t sec = tv.tv_sec;
    tm_info = localtime(&sec); // localtime uses a static buffer, consider localtime_r for heavy threading
    if (tm_info == NULL) {
        perror("localtime in get_timestamp");
        if (len > 15) snprintf(buffer, len, "TIMESTAMP_ERROR");
        else if (len > 0) buffer[0] = '\0';
        return;
    }

    size_t written = strftime(buffer, len, "%Y.%m.%d-%H:%M:%S", tm_info);
    if (written > 0 && written + 5 < len) { // +5 for ".sss\0"
        snprintf(buffer + written, len - written, ".%03ld", (long)tv.tv_usec / 1000);
    } else if (written == 0) {
        if (len > 22) snprintf(buffer, len, "TIMESTAMP_FORMAT_ERROR");
        else if (len > 0) buffer[0] = '\0';
    }
}

/*
 * send_all
 * Reliably sends all data in the buffer over the specified socket.
 * Handles potential partial sends by looping until all bytes are sent or an error occurs.
 *
 * Parameters:
 *   sockfd: int - The socket file descriptor.
 *   buffer: const char* - Pointer to the data to be sent.
 *   length: size_t - The number of bytes to send from the buffer.
 *
 * Returns:
 *   int - 0 on success, -1 on error (perror will be called by this function).
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
            fprintf(stderr, "send_all: send returned 0 (peer closed connection or invalid state)\n");
            return -1;
        }
        total_sent += (size_t)sent_bytes;
    }
    return 0;
}

/*
 * recv_line
 * Receives a line of text (terminated by a newline character '\n') from the socket.
 * The newline character is included in the buffer if space permits. The buffer
 * is always null-terminated. This function reads one byte at a time.
 *
 * Parameters:
 *   sockfd: int - The socket file descriptor.
 *   buffer: char* - The buffer to store the received line.
 *   max_len: size_t - The maximum number of bytes to read into the buffer (including null terminator).
 *                    Must be at least 1 for the null terminator.
 *
 * Returns:
 *   ssize_t - Number of bytes read (excluding null terminator, but including newline if present).
 *             Returns 0 if the connection was closed by the peer before any data for the current line was received.
 *             Returns -1 on a critical socket error (perror will be called by this function).
 *             Returns -2 if a timeout occurred (if SO_RCVTIMEO is set and expired before any byte is received).
 *             If data is partially read then EOF or error, returns bytes read so far.
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
