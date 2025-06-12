Simple Client-Server Application

This project implements a multi-threaded server and a client that communicate
over TCP sockets using a simple text-based protocol.

Features:
- Server supports ECHO, QUIT, INFO, CD, LIST, and script execution.
- Server is multi-threaded, handling each client in a separate thread.
- Client can run in interactive mode or trigger server-side script execution.
- Server operations are restricted to a specified root directory.
- LIST command shows directories, files, and resolves symbolic links.

Build Instructions:
The project uses a Makefile.
- To build in debug mode (default):
  make
  or
  make MODE=debug

- To build in release mode:
  make MODE=release

- To clean build artifacts:
  make clean

Executables will be placed in the 'build/' directory.

Running the Server:
./build/myserver <port_number> <root_directory_path>
Example:
./build/myserver 8080 /tmp/server_root

The server will log its activity to standard output.
Ensure the <root_directory_path> exists and is accessible.

Running the Client:
./build/myclient <server_address> <port_number> [@batch_file_on_server]
Example (interactive):
./build/myclient 127.0.0.1 8080

Example (executing a script on the server from the command line):
./build/myclient 127.0.0.1 8080 @commands.txt

Client commands:
  ECHO <text>          - Server echoes back <text>.
  QUIT                 - Disconnects from the server.
  INFO                 - Displays server information.
  CD <directory_name>  - Changes current directory on the server.
  LIST                 - Lists contents of the current server directory.
  LCD <directory>      - (Client-side) Changes the client's Local Current Directory.
  @<filename>          - (Server-side) Commands the server to execute a script file
                         located in its current working directory.

Client prompt '>' will change to '<current_dir_on_server)>' after a successful CD.
