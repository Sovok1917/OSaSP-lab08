/*
 * src/protocol.h
 *
 * This header file defines constants and protocol-specific elements that are
 * shared between the client and server applications. This includes buffer sizes,
 * command strings, and response prefixes to ensure both applications adhere to
 * the same communication standard.
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#define MAX_BUFFER_SIZE 4096
#define MAX_PATH_LEN 4096 // PATH_MAX can be large, using a fixed practical limit for buffers
#define MAX_CMD_LEN 256
#define MAX_ARGS_LEN (MAX_BUFFER_SIZE - MAX_CMD_LEN - 5) // For ECHO command argument

#define SERVER_DEFAULT_WELCOME_MSG "Welcome to the test server 'myserver'"

// Client receive timeout in milliseconds for multi-line responses
#define CLIENT_RECV_TIMEOUT_MS 200

// Command strings
#define CMD_ECHO "ECHO"
#define CMD_QUIT "QUIT"
#define CMD_INFO "INFO"
#define CMD_CD "CD"
#define CMD_LIST "LIST"

// Server responses
#define RESP_BYE "BYE"
#define RESP_ERROR_PREFIX "ERROR: "

#endif // PROTOCOL_H
