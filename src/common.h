/*
 * src/common.h
 *
 * This header file declares shared utility functions and structures used by both
 * the client and server applications. It provides a common interface for tasks
 * such as reliable socket I/O and timestamp generation.
 */
#ifndef COMMON_H
#define COMMON_H

#include <sys/types.h> // For ssize_t
#include <stddef.h>    // For size_t

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
void get_timestamp(char *buffer, size_t len);

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
int send_all(int sockfd, const char *buffer, size_t length);

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
ssize_t recv_line(int sockfd, char *buffer, size_t max_len);

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
void initialize_static_memory(void);

#endif // COMMON_H
