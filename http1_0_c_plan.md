# Plan: Upgrading to an HTTP/1.0 Server in C

Upgrading from HTTP/0.9 to HTTP/1.0 involves a major shift: the introduction of **Headers** and **Status Codes**. Your server must now describe *what* it is sending before it sends the actual file.

## Architecture Overview
The TCP lifecycle remains mostly the same, but the Request and Response formats change significantly:
1. **HTTP/1.0 Request:** `GET /index.html HTTP/1.0\r\nHost: localhost\r\n\r\n`
2. **HTTP/1.0 Response:** Needs a Status Line + Headers + `\r\n\r\n` + Body.

---

## Step-by-Step Implementation Plan

### Step 1: Upgrading the Parser State Machine
Your current state machine stops after the path. You must expand it to handle the HTTP version and headers.
*   **Action:** Expand your enum: `METHOD -> PATH -> PROTOCOL -> HEADERS -> BODY (optional) -> ACCEPTED`.
*   **Action:** When parsing, look for the first space after the path to capture the Protocol (e.g., `HTTP/1.0`).
*   **Action:** Headers are key-value pairs separated by `\r\n`. The entire request header block ends when you receive a blank line (a double `\r\n\r\n`).

### Step 2: Implementing the Status Line
You can no longer just send the file or a plain text "404 file not found". Every response must begin with a status line.
*   **Action:** If the file is found, format a string: `HTTP/1.0 200 OK\r\n`.
*   **Action:** If the file is missing, format a string: `HTTP/1.0 404 Not Found\r\n`.
*   **Action:** If the method isn't GET, you can now return `HTTP/1.0 405 Method Not Allowed\r\n`.

### Step 3: Adding Response Headers
After the status line, you must tell the client about the payload.
*   **Action:** Send `Content-Length: <file_size>\r\n`. (This allows the browser to show a loading bar).
*   **Action:** Send `Content-Type: <mime_type>\r\n`. (You will need a helper function that looks at the file extension of the requested path. `.html` -> `text/html`, `.css` -> `text/css`, `.jpg` -> `image/jpeg`).
*   **Action:** Crucially, signal the end of the headers by sending an empty line: `\r\n`.

### Step 4: Sending the Payload (The Body)
Once the double `\r\n\r\n` is sent, the browser expects the raw file data.
*   **Action:** Just like your HTTP/0.9 code, read the file into your buffer and `write()` it to the client socket immediately following the headers.

### Step 5: Connection Management
*   **Action:** In HTTP/1.0, the default behavior is still to close the connection after the response is completely sent. Keep your `close(client_fd)` exactly where it is. *(Note: HTTP/1.1 introduces Keep-Alive, which requires keeping the socket open, but stick to HTTP/1.0 behavior for now).*

---

## Testing Your Server
Because you are returning standard HTTP/1.0 headers, modern tools will now work natively without special flags.

**Using curl:**
```bash
# Standard curl will now parse your headers correctly
curl -v http://localhost:8082/index.html
```

**Using a Browser:**
You can finally open `http://localhost:8082/index.html` in Chrome, Firefox, or Safari, and it will render correctly because of the `Content-Type` headers!
