# HTTP/1.0 Deep Dive: Complete Upgrade Reference (from HTTP/0.9)

This reference document details every architectural, structural, and protocol addition introduced in **HTTP/1.0 (RFC 1945)** compared to **HTTP/0.9**. Use this as your engineering specification before implementing your HTTP/1.0 parser and response engine.

---

## 1. High-Level Comparison Matrix

| Aspect | HTTP/0.9 (1991) | HTTP/1.0 (RFC 1945 - May 1996) |
| :--- | :--- | :--- |
| **Methods** | Only `GET` | `GET`, `HEAD`, `POST` (plus extension methods `PUT`, `DELETE`, etc.) |
| **Request Line** | `GET <path>\r\n` | `<Method> <Request-URI> HTTP/1.0\r\n` |
| **Protocol Versioning** | Absent | Explicit protocol negotiation token (`HTTP/1.0`) |
| **Request Headers** | None | Key-Value metadata lines ended by `\r\n\r\n` |
| **Request Body** | None | Supported (required for `POST`, indicated by `Content-Length`) |
| **Response Status Line** | None | `HTTP/1.0 <Status-Code> <Reason-Phrase>\r\n` |
| **Response Headers** | None | Content-Type, Content-Length, Date, Server, etc. |
| **Media Types (MIME)** | Plain HTML only | Any binary/text payload (HTML, CSS, JPEG, PDF, etc.) via MIME |
| **Caching & Conditionals** | None | `If-Modified-Since` request header & `Last-Modified` response header |
| **Authorization** | None | `Authorization: Basic <base64>` & `401 Unauthorized` challenge |
| **Host Header** | None | Introduced unofficially in late 1.0 (standardized in RFC 1945 Appendix) |
| **Connection Lifecycle** | 1 TCP connection = 1 file (Always close) | Default close (early `Connection: keep-alive` extension introduced) |

---

## 2. Request Structure & Parsing Grammar

### 2.1 HTTP/0.9 Request
A single line with no headers and no protocol token:
```text
GET /index.html\r\n
```

### 2.2 HTTP/1.0 Full Request Structure
An HTTP/1.0 request introduces a 4-part structure:
```text
+-----------------------------------------------------+
| Request-Line (Method + Request-URI + HTTP-Version)  |  CRLF
+-----------------------------------------------------+
| Header Fields (Key: Value)                          |  CRLF
| Header Fields (Key: Value)                          |  CRLF
| ...                                                 |  CRLF
+-----------------------------------------------------+
| [CRLF (Empty Line delimiter: \r\n)]                 |  CRLF
+-----------------------------------------------------+
| [Optional Message Body / Payload]                   |
+-----------------------------------------------------+
```

### 2.3 Concrete Example (HTTP/1.0 GET)
```http
GET /images/logo.png HTTP/1.0\r\n
User-Agent: Mosaic/2.4\r\n
Accept: image/gif, image/jpeg, text/html\r\n
If-Modified-Since: Sun, 06 Nov 1994 08:49:37 GMT\r\n
\r\n
```

### 2.4 Concrete Example (HTTP/1.0 POST)
```http
POST /submit-form HTTP/1.0\r\n
User-Agent: Mozilla/1.0\r\n
Content-Type: application/x-www-form-urlencoded\r\n
Content-Length: 27\r\n
\r\n
username=alice&action=login
```

---

## 3. The Request Line Breakdown

In HTTP/1.0, the request line is strictly parsed as:
```text
Request-Line = Method SP Request-URI SP HTTP-Version CRLF
```

* `SP` = Single space character (ASCII 32).
* `CRLF` = `\r\n` (ASCII 13 followed by ASCII 10).

### 3.1 The Methods
1. **`GET`**: Retrieve the resource identified by the Request-URI.
2. **`HEAD`**: Identical to `GET`, but the server **MUST NOT** return a message body in the response. Only send the status line and headers. Used by clients to check file size (`Content-Length`), existence (`404` vs `200`), or modification dates without downloading large files.
3. **`POST`**: Send an enclosing entity/body to the server for processing (e.g., HTML form submissions). Requires reading `Content-Length` bytes from the client socket after `\r\n\r\n`.

### 3.2 Request-URI
* Can be `abs_path` (e.g., `/index.html?search=c`) when querying an origin server directly.
* Can be `absoluteURI` (e.g., `http://example.com/index.html`) when communicating with forward proxies.

### 3.3 HTTP-Version
* Format: `HTTP/1.0` (case-sensitive).
* If a client does **not** provide a version token (e.g., `GET /index.html\r\n`), the server must identify this as a legacy **HTTP/0.9 Simple-Request** and reply with an HTTP/0.9 Simple-Response (raw body only, no headers).

---

## 4. Response Structure & Parsing Grammar

### 4.1 HTTP/0.9 Response
HTTP/0.9 had **no status line and no headers**. If a file existed, raw HTML was dumped. If it didn't exist, the server either sent an HTML error message or abruptly closed the socket.

### 4.2 HTTP/1.0 Full Response Structure
```text
+-----------------------------------------------------+
| Status-Line (HTTP-Version + Status-Code + Reason)   |  CRLF
+-----------------------------------------------------+
| Response Headers (Key: Value)                       |  CRLF
| Response Headers (Key: Value)                       |  CRLF
| ...                                                 |  CRLF
+-----------------------------------------------------+
| CRLF (Empty Line delimiter: \r\n)                   |  CRLF
+-----------------------------------------------------+
| [Entity Body / File Data]                           |
+-----------------------------------------------------+
```

### 4.3 The Status-Line
```text
Status-Line = HTTP-Version SP Status-Code SP Reason-Phrase CRLF
```
Example: `HTTP/1.0 200 OK\r\n`

#### Core HTTP/1.0 Status Codes:
* **`2xx` (Success):**
  * `200 OK`: Request succeeded; entity body contains resource data.
  * `201 Created`: Resource successfully created (primarily responses to POST/PUT).
  * `202 Accepted`: Request accepted for processing, but processing not completed.
  * `204 No Content`: Action succeeded, but no body returned.
* **`3xx` (Redirection):**
  * `301 Moved Permanently`: Resource relocated permanently to URI in `Location:` header.
  * `302 Moved Temporarily`: Resource temporarily at a different URI (`Location:` header).
  * `304 Not Modified`: Cached copy on client is still fresh (in response to `If-Modified-Since`). **Must not include a body.**
* **`4xx` (Client Errors):**
  * `400 Bad Request`: Malformed syntax in request.
  * `401 Unauthorized`: Authentication required (`WWW-Authenticate` header sent).
  * `403 Forbidden`: Server understood request, but refuses to authorize access.
  * `404 Not Found`: No resource found matching the path.
* **`5xx` (Server Errors):**
  * `500 Internal Server Error`: Server caught an unexpected error/bug.
  * `501 Not Implemented`: Unsupported method or functionality (e.g., server only supports GET/HEAD).
  * `502 Bad Gateway`: Proxy/gateway received invalid upstream response.
  * `503 Service Unavailable`: Server overloaded or down for maintenance.

---

## 5. Comprehensive Guide to HTTP/1.0 Headers (RFC 1945)

Headers in HTTP/1.0 are case-insensitive keys followed by a colon `:`, optional linear whitespace, the value, and `\r\n`. RFC 1945 groups headers into 4 distinct classes:
1. **General Headers** (apply to both request and response messages)
2. **Request Headers** (give client context / constraints to the server)
3. **Response Headers** (give server context / operational details to the client)
4. **Entity Headers** (describe the payload / body data)

Additionally, several ubiquitous **Extension Headers** emerged in late HTTP/1.0 implementations.

---

### 5.1 General Headers (Messages on Both Ends)

| Header Key | Direction | Purpose & Behavior | Example Format |
| :--- | :--- | :--- | :--- |
| **`Date`** | Request / Response | Indicates the exact date and time the message was originated. Essential for caches to calculate freshness. | `Date: Sun, 06 Nov 1994 08:49:37 GMT` |
| **`Pragma`** | Request / Response | Used for implementation-specific directives. In HTTP/1.0, its only standard directive is `no-cache`, which instructs all intermediate caches and servers to fetch a fresh copy. | `Pragma: no-cache` |

---

### 5.2 Request Headers (Client to Server)

| Header Key | Purpose & Behavior | Implementation Details & Example |
| :--- | :--- | :--- |
| **`User-Agent`** | Informs the server about the client browser, operating system, and version. | Used by servers for statistical analytics or tailored content delivery.<br>`User-Agent: Mozilla/2.0 (compatible; MSIE 3.0; Windows 95)` |
| **`From`** | Contains an Internet email address for the human user operating the requesting agent. | Commonly used by search engine crawlers (robots) so webmasters could contact the bot operator if the crawler misbehaved.<br>`From: webmaster@example.com` |
| **`Referer`** *(sic)* | Contains the URI of the resource from which the current request URI was obtained (the referrer link clicked). | Used for analytics, tracking source links, and cache optimization.<br>`Referer: http://www.example.com/index.html` |
| **`Authorization`** | Supplies user credentials for accessing protected resources. | Used with HTTP Basic Authentication. Value is `Basic ` followed by `base64(username:password)`.<br>`Authorization: Basic YWxhZGRpbjpvcGVuc2VzYW1l` |
| **`If-Modified-Since`** | Enables **Conditional GET**. Informs the server to only send the file body if it has been updated after the specified timestamp. | If file is unmodified, server returns `304 Not Modified` with zero body bytes.<br>`If-Modified-Since: Sun, 06 Nov 1994 08:49:37 GMT` |
| **`Accept`** | Content negotiation: Tells the server which MIME types the client understands or prefers. | Allows client to prefer images or specific formats.<br>`Accept: text/html, image/gif, image/jpeg, */*` |
| **`Accept-Charset`** | Specifies character sets acceptable for the response. | `Accept-Charset: iso-8859-1, utf-8` |
| **`Accept-Encoding`** | Specifies content-encoding/compression schemes acceptable to the client. | Enables transparent compression.<br>`Accept-Encoding: gzip, compress` |
| **`Accept-Language`** | Preferred natural languages for localized content. | `Accept-Language: en-US, en, fr` |

---

### 5.3 Response Headers (Server to Client)

| Header Key | Purpose & Behavior | Implementation Details & Example |
| :--- | :--- | :--- |
| **`Server`** | Contains information about the software used by the origin server handling the request. | Analogous to `User-Agent`, but for the server.<br>`Server: Apache/1.1 NCSA/1.4 MyCustomCServer/1.0` |
| **`Location`** | Defines the exact URL to which a client should redirect. | Required with `301 Moved Permanently` and `302 Moved Temporarily` responses.<br>`Location: http://www.example.com/newpath/index.html` |
| **`WWW-Authenticate`** | Sent with a `401 Unauthorized` response to challenge the client for authentication. | Specifies the authentication scheme (`Basic`) and the security `realm`.<br>`WWW-Authenticate: Basic realm="Restricted Admin Area"` |
| **`Retry-After`** | Used with `503 Service Unavailable` to indicate how long the service is expected to be unavailable to the requesting client. | Can be an integer (seconds) or an HTTP-date.<br>`Retry-After: 120` |

---

### 5.4 Entity Headers (Describing the Payload / Body)

Entity headers define metadata for the request body (in `POST`) or the response body (in `GET`/`POST`).

| Header Key | Purpose & Behavior | Implementation Details & Example |
| :--- | :--- | :--- |
| **`Content-Type`** | Specifies the MIME media type of the transmitted body. Critical for browsers to decide how to render the payload. | Without this, browsers cannot distinguish HTML from raw plain text or binary images.<br>`Content-Type: text/html; charset=iso-8859-1`<br>`Content-Type: image/png` |
| **`Content-Length`** | The exact size of the message body in octets (bytes). | Crucial for the client/browser to know how many bytes to read before expecting EOF or updating download progress.<br>`Content-Length: 3482` |
| **`Last-Modified`** | The date and time at which the origin server believes the resource was last modified. | Used by clients in future requests with `If-Modified-Since` for cache validation.<br>`Last-Modified: Wed, 15 Nov 1995 04:58:08 GMT` |
| **`Expires`** | Gives the date/time after which the response is considered stale by caches. | Tells proxies and browsers that cached data can be reused without querying server until this date.<br>`Expires: Thu, 01 Dec 1994 16:00:00 GMT` |
| **`Content-Encoding`** | Indicates what additional content coding (e.g., compression) has been applied to the entity. | Tells the client how to decompress the body.<br>`Content-Encoding: x-gzip` |
| **`Content-Language`** | Describes the natural language(s) of the intended audience for the enclosed entity. | `Content-Language: en` |
| **`Allow`** | Lists the set of HTTP methods supported by the target resource. | Sent by the server when responding with `405 Method Not Allowed`.<br>`Allow: GET, HEAD` |

---

### 5.5 Essential HTTP/1.0 Extension Headers (Widely Implemented)

Although RFC 1945 formally cataloged the baseline protocol, practical web implementations in 1995–1996 added several headers that became de-facto standards:

| Header Key | Type | Purpose & Behavior | Example Format |
| :--- | :--- | :--- | :--- |
| **`Host`** | Request | Identifies the internet host and port number of the resource being requested. Enabled **Name-based Virtual Hosting** before HTTP/1.1. | `Host: www.example.com:8080` |
| **`Connection`** | General | Controls whether the network connection stays open after the current transaction finishes. | `Connection: keep-alive`<br>`Connection: close` |
| **`Keep-Alive`** | General | Configures parameters for persistent connections (timeout in seconds and maximum requests). | `Keep-Alive: timeout=5, max=100` |
| **`Set-Cookie`** | Response | Sends a state tracking token (cookie) from the server to be stored by the browser (Netscape standard). | `Set-Cookie: session_id=xyz123; path=/` |
| **`Cookie`** | Request | Returns stored cookies back to the origin server with subsequent requests. | `Cookie: session_id=xyz123` |

---

## 6. Conditional Requests & Caching (The Major Efficiency Leap)

In HTTP/0.9, clients re-downloaded the full document on every visit. HTTP/1.0 introduced cache validation:

```mermaid
sequenceDiagram
    autonumber
    actor Client as Browser (Client)
    participant Server as HTTP/1.0 Server

    Note over Client,Server: First Visit: Full Download
    Client->>Server: GET /style.css HTTP/1.0
    Server-->>Client: HTTP/1.0 200 OK<br/>Content-Length: 450<br/>Last-Modified: Mon, 10 Oct 2026 12:00:00 GMT<br/><br/>[450 bytes CSS Data]
    Note over Client: Browser stores CSS file & timestamp

    Note over Client,Server: Second Visit: Conditional Request
    Client->>Server: GET /style.css HTTP/1.0<br/>If-Modified-Since: Mon, 10 Oct 2026 12:00:00 GMT
    alt File has NOT changed
        Server-->>Client: HTTP/1.0 304 Not Modified<br/>Date: Mon, 12 Oct 2026 14:00:00 GMT<br/><br/>(No Body!)
        Note over Client: Browser loads copy from local cache
    else File HAS changed
        Server-->>Client: HTTP/1.0 200 OK<br/>Content-Length: 520<br/>Last-Modified: Mon, 12 Oct 2026 13:50:00 GMT<br/><br/>[520 bytes New Data]
    end
```

### How to Implement in C:
1. When serving a file, query its filesystem metadata using `stat()` (`st_mtime`).
2. Format `st_mtime` into standard HTTP-date (`strftime`) and emit `Last-Modified`.
3. When parsing requests, look for `If-Modified-Since`. Compare the client timestamp against `st_mtime`.
4. If `st_mtime <= client_timestamp`, return status `304 Not Modified`, emit headers, and **do not** write any file bytes to the socket.

---

## 7. The `Host` Header & Virtual Hosting in HTTP/1.0

* **In Official RFC 1945:** `Host` was not part of the initial core HTTP/1.0 standard because HTTP/1.0 was documenting practices already in place between 1991 and 1995.
* **The Problem:** The rapid expansion of domains led to IPv4 exhaustion. Hosting 100 domains required 100 IP addresses.
* **The Adoption:** Before HTTP/1.1 was finalized, web browsers (Netscape, Internet Explorer) and servers (Apache, Nginx predecessors) unofficially backported `Host: <hostname>` into HTTP/1.0 requests.
* **Server Rule:** An HTTP/1.0 server should inspect the `Host:` header if present to route to the correct virtual host folder, but must fall back to a default host if it is missing.

---

## 8. Connection Management

* **HTTP/0.9:** Always closes the TCP socket immediately after transmitting the file body.
* **Standard HTTP/1.0:** The default connection behavior is **non-persistent** (Connection closes after every single response).
* **The `Connection: keep-alive` Extension:**
  * Because opening and tearing down TCP handshakes (SYN, SYN-ACK, ACK) for every image on an HTML page caused massive latency, late HTTP/1.0 implementations introduced:
    ```http
    Connection: keep-alive
    ```
  * If a client sent `Connection: keep-alive` AND the server supported it, the server would reply with `Connection: keep-alive` and keep `client_fd` open to read the next request.
  * *Notice:* In HTTP/1.0, Keep-Alive requires strict `Content-Length` headers so the client knows where one response ends and the next begins.

---

## 9. Architectural Requirements for Your C HTTP/1.0 Server

When writing your C code to support HTTP/1.0:

1. **Parser State Machine Expansion:**
   * Transition from: `METHOD -> PATH -> DONE`
   * To: `METHOD -> URI -> VERSION -> HEADER_KEY -> HEADER_VALUE -> ... -> DOUBLE_CRLF -> BODY`.
2. **Backward Compatibility:**
   * If the first line is `GET /path\r\n` without `HTTP/1.0`, your parser must fallback cleanly to HTTP/0.9 mode (no status line, no headers).
3. **MIME Type Mapping:**
   * Create an extension-to-MIME lookup table (`.html` -> `text/html`, `.txt` -> `text/plain`, `.jpg` -> `image/jpeg`).
4. **Header Buffer Formatting:**
   * Prepare a dedicated buffer for headers formatted with `snprintf()`. Write the header block first, then stream the file body bytes.
5. **Method Dispatcher:**
   * Route `GET` to serve file contents.
   * Route `HEAD` to execute the exact same headers logic, but skip the file `write()` loop.
   * Return `501 Not Implemented` for unsupported methods (`POST`, `PUT`, etc.).
