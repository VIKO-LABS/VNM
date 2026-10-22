# URL & Request URI Architecture: HTTP/0.9 vs HTTP/1.x

This document explains the evolution, anatomy, characteristics, and architectural rationale of the URL (Uniform Resource Locator) and Request-URI formats used in **HTTP/0.9** and **HTTP/1.x** (HTTP/1.0 & HTTP/1.1).

---

## 1. High-Level Comparison & Evolution

| Feature | HTTP/0.9 (1991) | HTTP/1.0 (1996 - RFC 1945) | HTTP/1.1 (1997/1999 - RFC 2068/2616/7230) |
| :--- | :--- | :--- | :--- |
| **Request Line Format** | `GET <path>\r\n` | `METHOD <URI> HTTP/1.0\r\n` | `METHOD <URI> HTTP/1.1\r\n` |
| **Allowed URI Format** | Plain document path / ISINDEX query | `abs_path` or `absoluteURI` | `abs_path`, `absoluteURI`, `authority`, `*` |
| **Host Information** | Implicit (from TCP connection only) | Optional via `absoluteURI` (proxy) | Mandatory via `Host:` header or `absoluteURI` |
| **Virtual Hosting** | ❌ Impossible (1 IP = 1 Website) | ⚠️ Partial / Unreliable | ✅ Standardized (Name-based Virtual Hosting) |
| **Query Parameters** | Primitive search string (`?query`) | Key-Value pairs (`?k1=v1&k2=v2`) | Standardized Key-Value pairs & encoding |
| **Fragment (`#anchor`)**| Client-side only (never sent) | Client-side only (never sent) | Client-side only (never sent) |

---

## 2. HTTP/0.9 URL Structure & Request Format

### Request Line Syntax
In the original 1991 specification designed by Tim Berners-Lee, the entire request consisted of a single line:

```text
GET /path/to/resource\r\n
```
*(or for indexed searching)*:
```text
GET /search/catalog?word1+word2\r\n
```

### Breakdown of Sections in HTTP/0.9

```text
  GET    /documents/manual.html    ?query_words
  ──┬──  ────────────┬─────────    ───────┬────
    │                │                    │
  Method         Target Path         Search Query (Optional)
```

1. **Method (`GET`):**
   * **Characteristics:** Only `GET` was supported. There were no headers, status lines, or POST payloads.
   * **Reason:** The web was originally designed as a read-only hypertext browsing system for CERN researchers.

2. **Target Path (`/path/to/resource`):**
   * **Characteristics:** A UNIX-style absolute path representing a document on the server's filesystem.
   * **Reason:** Servers simply mapped `/path/to/file.html` to a local directory (e.g. `/var/www/path/to/file.html`) and streamed back the raw HTML.
   * **Limitation:** The path lacked any context of *which* website was being requested because the domain name was not included.

3. **Search Query (`?word1+word2`):**
   * **Characteristics:** Used with the early HTML `<ISINDEX>` tag. Individual search keywords were separated by `+` characters.
   * **Reason:** Provided basic index lookup inside document catalogs without requiring forms or POST requests.

### Why HTTP/0.9 URL Handling Had Major Flaws
* **No Host Name:** The client resolved the domain to an IP address, opened a TCP socket to port 80, and only sent `/index.html`. The server only knew its own IP, not the hostname the user typed into the browser.
* **The Single-Site Bottleneck:** Because of the lack of a hostname in the URL, every website on the Internet needed its own dedicated IP address. As the web grew rapidly in the mid-1990s, IPv4 address exhaustion became an urgent crisis.

---

## 3. HTTP/1.x URL & Request-URI Structure

With HTTP/1.0 (RFC 1945) and HTTP/1.1 (RFC 2616 / RFC 7230 / RFC 3986), the URL specification was formalized into the complete URI standard.

### Full Canonical URL Breakdown

```text
  https://user:password@www.example.com:8080/api/v1/users?role=admin&sort=asc#summary
  └──┬──┘ └──────┬─────┘ └──────┬──────┘ └──┬─┘ └────┬────┘ └────────┬────────┘ └───┬───┘
   Scheme   User Info        Host          Port     Path           Query         Fragment
            (Authority component)
```

---

### Detailed Section-by-Section Analysis

#### 1. Scheme (`http://` or `https://`)
* **What it is:** Identifies the protocol used to access the resource.
* **Characteristics:** Case-insensitive, followed by `://`.
* **Reason:** Enables browsers and user agents to determine how to communicate with the endpoint (e.g., standard cleartext TCP for `http` on port 80, TLS handshake before HTTP for `https` on port 443, or alternative protocols like `ftp://`, `ws://`).
* **In HTTP Requests:** When talking directly to an origin server, the scheme is stripped by the browser and implied by the connection. When talking to a forward proxy, the scheme is retained.

#### 2. User Information (`user:password@`)
* **What it is:** Optional credentials embedded directly within the authority section.
* **Characteristics:** Followed by an `@` symbol.
* **Reason & History:** Originally intended for automated authentication (borrowed from FTP and Telnet conventions) and HTTP Basic Authentication.
* **Modern Status (⚠️ Deprecated):** RFC 3986 strongly discourages putting plaintext passwords in URLs due to security hazards (URLs leak in browser histories, server access logs, and HTTP `Referer` headers).

#### 3. Host / Domain (`www.example.com` or IP)
* **What it is:** Identifies the network host providing the service. Can be a registered domain name, an IPv4 address (`192.168.1.1`), or an IPv6 address enclosed in square brackets (`[::1]`).
* **Characteristics:** Case-insensitive. Resolved via DNS to an IP address.
* **Reason (The Virtual Hosting Solution):**
  * In HTTP/1.1, the `Host:` header was made **mandatory**:
    ```http
    GET /index.html HTTP/1.1
    Host: www.example.com
    ```
  * This solved the HTTP/0.9 flaw: a single physical server with a single IP address can now host thousands of distinct websites (Virtual Hosting). The web server checks the `Host` header to pick the correct website root.

#### 4. Port (`:80`, `:8080`, `:443`)
* **What it is:** The TCP port number the server is listening on.
* **Characteristics:** Decimal number from 1 to 65535, preceded by `:`.
* **Reason:** Allows multiple network services to run concurrently on the same IP address. If omitted, standard defaults are inferred based on the scheme (`http` -> 80, `https` -> 443).

#### 5. Path (`/api/v1/users`)
* **What it is:** A hierarchical sequence of path segments separated by `/`.
* **Characteristics:**
  * Must begin with a leading slash `/` on HTTP servers.
  * Case-sensitive on most servers (depends on the operating system / routing layer).
  * Supports percent-encoding (`%20` for space, `%2F` for `/`).
* **Reason:** Identifies the specific resource or logical endpoint. In static servers (like HTTP/0.9 and basic HTTP/1.0), it corresponds to a filesystem directory structure. In modern servers, it maps to application routes and controllers.

#### 6. Query String (`?role=admin&sort=asc`)
* **What it is:** Non-hierarchical data passed to the resource, beginning with `?`.
* **Characteristics:**
  * Typically formatted as `key=value` pairs separated by `&` (or historically `;`).
  * Characters outside standard ASCII/alphanumeric are percent-encoded (`?name=John%20Doe`).
* **Reason:** Allows clients to pass parameters, filters, pagination, or state to the server without altering the hierarchical path of the resource.
* **HTTP/1.x vs 0.9:** While HTTP/0.9 treated everything after `?` as simple search keywords, HTTP/1.0+ standardized structured parameter passing for form submissions and dynamic applications.

#### 7. Fragment / Anchor (`#summary`)
* **What it is:** An identifier referencing a sub-section within the primary resource, preceded by `#`.
* **Crucial Rule:** **The Fragment is NEVER sent to the HTTP server.**
* **Reason:**
  * It is purely client-side. The browser strips `#summary` before constructing the HTTP request line.
  * Once the document is received and rendered, the browser scrolls to the HTML element with `id="summary"`.
  * In modern Single Page Applications (SPAs), fragments or hash-routing (`/#/dashboard`) allow client-side routing without triggering new HTTP server requests.

---

## 4. How HTTP/1.x Formats the URI in Actual Network Packets

In HTTP/1.0 and HTTP/1.1, the Request-URI appears in one of four distinct forms depending on the target:

### 1. Origin Form (Standard Direct Request)
Used when a client connects directly to the web server:
```http
GET /articles/today.html?category=tech HTTP/1.1
Host: news.example.com
```
* The path and query are sent as the target URI (`abs_path`).
* The domain name is transferred in the `Host` header.

### 2. Absolute Form (Proxy Request)
Used when the client connects through an intermediate HTTP Forward Proxy:
```http
GET http://news.example.com/articles/today.html?category=tech HTTP/1.1
Host: news.example.com
```
* **Reason:** The proxy needs to know the destination server to connect to on behalf of the client.

### 3. Authority Form (Tunneling)
Used only with the `CONNECT` method for establishing HTTPS tunneling through proxies:
```http
CONNECT news.example.com:443 HTTP/1.1
Host: news.example.com:443
```
* Contains only the `host:port` pair.

### 4. Asterisk Form (Server Health / Capabilities)
Used only with `OPTIONS` to query server-wide capabilities:
```http
OPTIONS * HTTP/1.1
Host: example.com
```

---

## 5. Summary Checklist for Your C Parser Implementation

When parsing incoming requests in C across stages:

1. **For HTTP/0.9:**
   * Scan for `GET `.
   * Read until `\r` or `\n`.
   * Treat everything between `GET ` and end-of-line as the resource path (optionally splitting on `?`).
   * Ignore headers because none exist.

2. **For HTTP/1.0 & HTTP/1.1:**
   * Extract Method, Request-URI, and Version separated by spaces: `METHOD <Request-URI> HTTP/<version>\r\n`.
   * If Request-URI starts with `/`: Parse as `abs_path` + optional `?query`.
   * If Request-URI starts with `http://`: Parse as `absoluteURI` (extract host, port, path, and query).
   * Continue parsing the stream line-by-line (`\r\n`) to read headers until reaching the empty line (`\r\n\r\n`).
   * In HTTP/1.1, inspect the parsed headers to verify that a `Host:` header exists (return `400 Bad Request` if missing).
