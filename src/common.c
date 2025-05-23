/*
 * common.c
 * Implements shared utility functions for client and server.
 */
#define _POSIX_C_SOURCE 200809L // For strnlen if not implicitly available, dprintf
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>

// Define HAS_REVERSIBLE_CC_MODE if you have such a feature, otherwise 0
#define HAS_REVERSIBLE_CC_MODE 0
#if HAS_REVERSIBLE_CC_MODE
// Code specific to REVERSIBLE_CC_MODE
#endif


void initialize_static_memory(void) {
    // If there were any global static variables needing runtime initialization,
    // they would be initialized here. For example, if not using C99+ initializers
    // or needing re-initialization.
}

void get_timestamp(char *buffer, size_t len) {
    if (buffer == NULL || len == 0) {
        return;
    }
    struct timeval tv;
    struct tm *tm_info;

    if (gettimeofday(&tv, NULL) == -1) {
        perror("gettimeofday");
        snprintf(buffer, len, "TIMESTAMP_ERROR");
        return;
    }

    time_t sec = tv.tv_sec;
    tm_info = localtime(&sec);
    if (tm_info == NULL) {
        perror("localtime");
        snprintf(buffer, len, "TIMESTAMP_ERROR");
        return;
    }

    size_t written = strftime(buffer, len, "%Y.%m.%d-%H:%M:%S", tm_info);
    if (written > 0 && written + 4 < len) { // +4 for .sss and null terminator
        snprintf(buffer + written, len - written, ".%03ld", (long)tv.tv_usec / 1000);
    } else if (written == 0) {
        snprintf(buffer, len, "TIMESTAMP_FORMAT_ERROR");
    }
}

int send_all(int sockfd, const char *buffer, size_t length) {
    size_t total_sent = 0;
    while (total_sent < length) {
        ssize_t sent_bytes = send(sockfd, buffer + total_sent, length - total_sent, 0);
        if (sent_bytes == -1) {
            if (errno == EINTR) continue; // Interrupted by signal, try again
            perror("send");
            return -1;
        }
        if (sent_bytes == 0) { // Should not happen with TCP unless length is 0
            fprintf(stderr, "send returned 0 unexpectedly\n");
            return -1;
        }
        total_sent += (size_t)sent_bytes;
    }
    return 0;
}

ssize_t recv_line(int sockfd, char *buffer, size_t max_len) {
    if (buffer == NULL || max_len == 0) {
        return -1;
    }
    size_t current_len = 0;
    char ch;
    ssize_t nbytes;

    while (current_len < max_len - 1) {
        nbytes = recv(sockfd, &ch, 1, 0);
        if (nbytes == 1) {
            buffer[current_len++] = ch;
            if (ch == '\n') {
                break;
            }
        } else if (nbytes == 0) { // Connection closed
            if (current_len == 0) { // No data received before close
                return 0;
            }
            break; // Partial line received before close
        } else { // Error
            if (errno == EINTR) {
                continue; // Interrupted by signal
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // This indicates a timeout if SO_RCVTIMEO is set
                return -2; // Special return code for timeout
            }
            perror("recv");
            return -1;
        }
    }
    buffer[current_len] = '\0';
    return (ssize_t)current_len;
}
