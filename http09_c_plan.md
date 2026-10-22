# Plan: Building an HTTP/0.9 Server in C

Building an HTTP/0.9 server in C is a great way to learn network programming (socket APIs). Because HTTP/0.9 has no headers or status codes, we can focus entirely on the TCP connection lifecycle and basic string matching.

## Prerequisites
You will be using the POSIX socket API. The key headers you will need are:
```c
#include <stdio.h>      // Standard I/O (printf, perror)
#include <stdlib.h>     // Standard library (exit)
#include <string.h>     // String manipulation (strncmp, strtok)
#include <unistd.h>     // POSIX API (close, read, write)
#include <sys/socket.h> // Core socket functions
#include <netinet/in.h> // Internet address family (sockaddr_in)
```

## Architecture Overview
An HTTP/0.9 server follows this basic lifecycle:
1. Create a socket.
2. Bind the socket to a port.
3. Listen for incoming connections.
4. **Loop:** Accept a connection -> Read the request -> Send the file -> Close the connection.

---

## Step-by-Step Implementation Plan

### Step 1: Create the Server Socket
The server needs a "welcome socket" to receive incoming TCP connection requests.
*   **Action:** Call `socket(AF_INET, SOCK_STREAM, 0)`.
    *   `AF_INET` specifies IPv4.
    *   `SOCK_STREAM` specifies TCP (a reliable, connection-oriented stream).
*   **Pro-Tip:** Use `setsockopt()` with `SO_REUSEADDR` immediately after creating the socket. This prevents the annoying "Address already in use" error when you restart your server frequently during testing.

### Step 2: Bind the Socket to a Port
You need to assign a port (e.g., 8080) and an IP address to your socket.
*   **Action:** Create a `struct sockaddr_in` structure.
    *   Set family to `AF_INET`.
    *   Set port to `htons(8080)` (Host to Network Short - ensures correct byte order).
    *   Set IP to `INADDR_ANY` (listens on all available network interfaces).
*   **Action:** Call `bind()` to attach this address structure to your server socket.

### Step 3: Start Listening
Tell the OS that this socket should accept incoming connections.
*   **Action:** Call `listen(server_fd, backlog)`.
    *   The `backlog` (e.g., 10) is the queue size for pending connections that haven't been `accept()`ed yet.

### Step 4: The Accept Loop
Your server should run continuously, waiting for clients (browsers/curl) to connect.
*   **Action:** Enter an infinite `while(1)` loop.
*   **Action:** Inside the loop, call `accept(server_fd, ...)`
    *   `accept()` **blocks** execution until a client connects.
    *   When a client connects, it returns a **new socket file descriptor** (`client_fd`). 
    *   *Note: Your `server_fd` keeps listening, while you use `client_fd` to talk to the specific client.*

### Step 5: Read and Parse the HTTP/0.9 Request
HTTP/0.9 is simple: the client will send a string exactly like this: `GET /filename.html\r\n`.
*   **Action:** Call `read(client_fd, buffer, buffer_size)` to read the client's message into a char array.
*   **Action:** Parse the string.
    1.  Check if it starts with `GET `. If not, drop the connection (HTTP/0.9 only supports GET).
    2.  Extract the requested path (e.g., `/index.html`) which sits between "GET " and the carriage return `\r` or newline `\n`.
    3.  If the path is exactly `/`, default to serving a file like `index.html`.

### Step 6: Serve the File
Unlike modern HTTP, you do **not** send headers like `HTTP/1.1 200 OK` or `Content-Type`. You just send the raw bytes of the file.
*   **Action:** Use `fopen()` to open the requested file locally.
*   **Error Handling:** If the file doesn't exist, you can't even send a 404 status code! In HTTP/0.9, you typically just send an HTML string saying "File not found" and close the connection.
*   **Action:** Read the file chunk by chunk (using `fread()`) into a buffer.
*   **Action:** Send each chunk to the client using `write(client_fd, buffer, bytes_read)`. Keep doing this until you hit the end of the file (EOF).

### Step 7: Close the Connection
This is the defining characteristic of HTTP/0.9: the end of the response is signaled by the server closing the TCP connection.
*   **Action:** Close the file pointer with `fclose()`.
*   **Action:** Close the client socket with `close(client_fd)`.
*   **Loop:** The loop goes back to Step 4 and waits for the next `accept()`.

---

## Testing Your Server
Since modern browsers expect HTTP/1.1 or higher, testing HTTP/0.9 can be slightly tricky.

**Using netcat (telnet):**
```bash
# Connect to your server
nc localhost 8080
# Manually type the request and hit enter:
GET /index.html
```

**Using curl:**
```bash
# Force curl to use HTTP/0.9
curl --http0.9 http://localhost:8080/index.html
```
