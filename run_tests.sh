#!/bin/bash

SERVER_PORT=8888
SERVER_ROOT="/tmp/my_server_root_script"
SERVER_EXEC="./build/myserver"
CLIENT_EXEC="./build/myclient"
SERVER_LOG="/tmp/server_script.log"
CLIENT_OUTPUT="/tmp/client_script_output.log"
COMMAND_FILE="/tmp/script_commands.txt"

# Function to clean up
cleanup() {
    echo "Cleaning up..."
    killall myserver 2>/dev/null
    rm -f "$SERVER_LOG" "$CLIENT_OUTPUT" "$COMMAND_FILE"
    rm -rf "$SERVER_ROOT"
    echo "Cleanup complete."
}

# Trap EXIT signal to ensure cleanup
trap cleanup EXIT

echo "Preparing test environment..."
mkdir -p "$SERVER_ROOT/dir_A" "$SERVER_ROOT/dir_B/sub_B"
echo "root_file.txt content" > "$SERVER_ROOT/root_file.txt"
echo "file_A.txt content" > "$SERVER_ROOT/dir_A/file_A.txt"
ln -s "$SERVER_ROOT/root_file.txt" "$SERVER_ROOT/link_to_root"
ln -s dir_A "$SERVER_ROOT/link_to_dir_A"

echo "Starting server in background..."
$SERVER_EXEC "$SERVER_PORT" "$SERVER_ROOT" > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
sleep 2 # Give server time to start

# Check if server started
if ! ps -p $SERVER_PID > /dev/null; then
    echo "ERROR: Server failed to start. Check $SERVER_LOG"
    exit 1
fi
echo "Server started (PID: $SERVER_PID)."

echo "Creating command file..."
cat > "$COMMAND_FILE" << EOF
INFO
ECHO Scripted Test Message!
LIST
CD dir_A
LIST
CD ..
LIST
CD link_to_dir_A
LIST
CD /
CD non_existent
CD /dir_B/sub_B
LIST
QUIT
EOF

echo "Running client with command file..."
$CLIENT_EXEC 127.0.0.1 "$SERVER_PORT" @"$COMMAND_FILE" > "$CLIENT_OUTPUT" 2>&1

echo "Client finished. Waiting for server to process..."
sleep 1 # Give server time to log final messages

echo "Stopping server..."
kill $SERVER_PID
wait $SERVER_PID 2>/dev/null # Wait for server to exit

echo "----------------------------------------"
echo "CLIENT OUTPUT:"
echo "----------------------------------------"
cat "$CLIENT_OUTPUT"
echo "----------------------------------------"
echo "SERVER LOG:"
echo "----------------------------------------"
cat "$SERVER_LOG"
echo "----------------------------------------"

# Basic checks (can be expanded significantly)
echo "Performing basic checks on client output..."
if grep -q "Scripted Test Message!" "$CLIENT_OUTPUT"; then
    echo "PASS: ECHO command seems to work."
else
    echo "FAIL: ECHO command output not found."
fi

if grep -q "root_file.txt" "$CLIENT_OUTPUT" && grep -q "dir_A/" "$CLIENT_OUTPUT"; then
    echo "PASS: Initial LIST command seems to work."
else
    echo "FAIL: Initial LIST command output missing expected items."
fi

# Check for CD to dir_A and its content
# This requires knowing the exact output format of LIST and CD
# For a more robust check, you'd parse the output more carefully.
# Example: Check if after "CD dir_A", "file_A.txt" is listed.
# This is a simplified check:
if grep -A 5 "CD dir_A" "$CLIENT_OUTPUT" | grep -q "file_A.txt"; then
     echo "PASS: CD to dir_A and subsequent LIST seems to work."
else
     echo "FAIL: CD to dir_A or its LIST output is incorrect."
fi


if grep -q "BYE" "$CLIENT_OUTPUT"; then
    echo "PASS: QUIT command seems to work."
else
    echo "FAIL: QUIT command output 'BYE' not found."
fi

echo "----------------------------------------"
echo "Test script finished. Review logs for details."
