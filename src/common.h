/*
 * common.h
 * Declares shared utility functions and structures for client and server.
 */
#ifndef COMMON_H
#define COMMON_H

#include <sys/types.h> // For ssize_t
#include <stddef.h>    // For size_t

/*
 * Gets the current timestamp in YYYY.MM.DD-HH:MM:SS.sss format.
 * buffer: The buffer to store the timestamp.
 * len: The length of the buffer.
 */
void get_timestamp(char *buffer, size_t len);

/*
 * Sends all data in the buffer over the socket.
 * sockfd: The socket file descriptor.
 * buffer: The data to send.
 * length: The number of bytes to send.
 * Returns: 0 on success, -1 on error.
 */
int send_all(int sockfd, const char *buffer, size_t length);

/*
 * Receives a line of text (up to newline) from the socket.
 * The newline character is included in the buffer if space permits.
 * The buffer is null-terminated.
 * sockfd: The socket file descriptor.
 * buffer: The buffer to store the received line.
 * max_len: The maximum number of bytes to read into buffer (including null terminator).
 * Returns: Number of bytes read (excluding null terminator, but including newline if present),
 *          0 if connection closed, -1 on error.
 *          -2 on timeout (if SO_RCVTIMEO is set and expires).
 */
ssize_t recv_line(int sockfd, char *buffer, size_t max_len);

/*
 * Initializes static memory. Currently a placeholder.
 */
void initialize_static_memory(void);

#endif // COMMON_H
