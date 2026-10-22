# VSI Core Engine Architecture & Technical Specification
**Project:** VCloud Software Infrastructure (VSI) — Core Edge Engine  
**Author:** Sai (Systems Developer)  
**Approved by:** Architect & Founder, VSI  
**Document Version:** 1.0.0 (Unified Specification)

---

## 1. Executive Vision & System Classification

### 1.1 What We Are Building
The **VSI Core Edge Engine** is a high-performance Layer-7 (Application Layer) HTTP engine written in C. It serves as the primary ingress point and architectural bridge for cloud infrastructure, combining:
1. **Low-Level Socket Lifecycle Management**: Direct POSIX TCP networking with optimized socket options (`SO_REUSEADDR`).
2. **Deterministic HTTP/1.x Framing**: Dual-phase delimited header parsing (`\r\n\r\n`) and explicit payload streaming via `Content-Length`.
3. **Multi-Threaded Concurrency**: Lock-isolated, bounded queue worker pools with sentinel management and zero heap allocations.
4. **Armored Ingress Protection (Slowloris Shield)**: Absorbing slow/hostile mobile client connections on lightweight C buffers before offloading to upstream business logic.
5. **Zero-Copy Static File Delivery**: Direct kernel page-cache-to-NIC DMA streaming using Linux `sendfile()`.

### 1.2 OSI Layer Positioning
The VSI Engine operates strictly at **Layer 7 (Application Layer)**:

```
+-----------------------------------------------------------------------------------------+
| OSI LAYER                     | RESPONSIBILITY                 | VSI CORE ENGINE SCOPE  |
+-------------------------------+--------------------------------+------------------------+
| Layer 7: APPLICATION          | HTTP 1.0/1.1 Protocol Framing, | ★ VSI ENGINE CORE      |
|                               | URI Routing, Header Parsing    |                        |
+-------------------------------+--------------------------------+------------------------+
| Layer 6: PRESENTATION         | TLS / SSL Cryptography         | OpenSSL / Hardware     |
|                               | Compression                    | Acceleration (AES-NI)  |
+-------------------------------+--------------------------------+------------------------+
| Layer 5: SESSION              | Socket lifecycle               | POSIX Sockets API      |
+===============================+================================+========================+
|                                 --- THE KERNEL BOUNDARY ---                             |
+===============================+================================+========================+
| Layer 4: TRANSPORT            | TCP/UDP, 3-Way Handshake,      | Handled by Linux Kernel|
|                               | Retransmission, Flow Control   | TCP Stack (Unmodified) |
+-------------------------------+--------------------------------+------------------------+
| Layer 3: NETWORK              | IP Routing, Packet Addressing  | Linux IP Subsystem     |
+-------------------------------+--------------------------------+------------------------+
| Layer 2: DATA LINK            | Ethernet Frames, MAC Addresses | Network Device Driver  |
+-------------------------------+--------------------------------+------------------------+
| Layer 1: PHYSICAL             | Physical Signals / Fiber / RF  | Network Hardware (NIC) |
+-------------------------------+--------------------------------+------------------------+
```

---

## 2. Why an L7 C Engine Fronts Modern Runtimes (Python/Java)

In production cloud environments (Google, Netflix, Cloudflare), application runtimes such as **Python (Uvicorn / FastAPI)** or **Java (Quarkus / Spring / Netty)** are virtually never exposed directly to the open Internet. The VSI C Engine serves as the vital front-end bridge:

```
                        [ UNTRUSTED INTERNET ]
                                   |
                                   | (Slow 3G/4G/5G mobile clients, Slowloris threats)
                                   v
+-------------------------------------------------------------------------------------+
|                              VSI CORE EDGE ENGINE (C)                               |
|                                                                                     |
|   * Holds thousands of client FDs in lightweight buffers (few KB each)              |
|   * Buffers full HTTP headers & payload before contacting application runtimes      |
|   * Offloads TLS/HTTPS decryption using hardware-accelerated OpenSSL instructions   |
|   * Serves static assets instantly via Linux `sendfile()` (Zero CPU Copies)         |
+-------------------------------------------------------------------------------------+
                                   |
                                   | (Internal Unix Domain Socket: AF_UNIX)
                                   | (~10 microseconds latency = 0.01 ms RAM transfer)
                                   v
+-------------------------------------------------------------------------------------+
|                           INTERNAL APPLICATION RUNTIMES                             |
|                                                                                     |
|         [ Python Uvicorn / ASGI ]                 [ Java Quarkus / Netty ]          |
|      (FastAPI Business Logic / ORM)            (High-Throughput Enterprise APIs)    |
|                                                                                     |
|   * Heavy execution stacks (30-80 MB per       * Protected from connection fatigue  |
|     process, Python GIL / JVM heap)            * Pure execution focus: 2ms per task |
+-------------------------------------------------------------------------------------+
```

### The 5 Architectural Justifications:
1. **The Slowloris & Slow Client Shield**:
   * A mobile client on a weak signal may take 5–10 seconds to send a 500-byte POST request.
   * If connected directly to Python or Java, heavy worker threads are held hostage doing zero CPU work, rapidly exhausting the application thread pool.
   * The VSI C Engine buffers the request asynchronously. Only when `\r\n\r\n` and all `Content-Length` bytes are in memory does it hand off the request over an internal Unix socket (`AF_UNIX`) in **10 microseconds (0.01 ms)**.
2. **Zero-Copy Static File Delivery**:
   * Application runtimes require copying disk files into user-space heap buffers, triggering JVM garbage collection or Python memory fragmentation.
   * The VSI Engine uses the Linux `sendfile()` syscall, piping disk blocks directly to network buffers via DMA with zero CPU copies.
3. **Connection Concurrency Footprint**:
   * 10,000 idle TCP connections in Python would require tens of gigabytes of RAM.
   * In the VSI C Engine, each connection is an integer file descriptor (`int client_fd`), easily managing tens of thousands of idle connections in under 50 MB of RAM.
4. **Hardware TLS Cryptography**:
   * Offloads TLS handshakes and cryptographic computations (AES-GCM / ChaCha20) to C/assembly with AES-NI instructions, shielding the app runtimes from crypto compute spikes.
5. **Zero-Downtime Hot Deployments**:
   * When upgrading Python or Java backend instances, the VSI Engine queues incoming traffic during restarts, preventing connection-refused errors for end users.

---

## 3. Core Engine Mechanics & Implementation

### 3.1 Two-Phase HTTP Request Ingestion
Because TCP is a continuous byte-stream protocol without application-level boundaries, the engine executes a deterministic two-phase read algorithm:

```
[ Incoming Socket Bytes ]
           |
           v
+----------------------------------------------------+
| PHASE 1: Scan for Header Delimiter (`\r\n\r\n`)     |
| Keep reading until CRLF CRLF marks header boundary |
+----------------------------------------------------+
           |
           v
+----------------------------------------------------+
| PHASE 2: Inspect Headers for `Content-Length`      |
+----------------------------------------------------+
           |
           +--------------------------+
           |                          |
   (No Body / GET / HEAD)    (Body Present / POST / PUT)
           |                          |
           v                          v
    [ Request Ready ]      +-------------------------------------------+
                           | PHASE 3: Stream Exact Body Bytes          |
                           | `remaining = Content-Length - pre_read`   |
                           | Read until `remaining == 0`               |
                           +-------------------------------------------+
                                      |
                                      v
                               [ Request Ready ]
```

```c
// Delimited Header Read Phase
while (filled < BUFFER_SIZE - 1) {
    int r = read(client_fd, recieved_data + filled, BUFFER_SIZE - 1 - filled);
    if (r <= 0) break;
    filled += r;
    recieved_data[filled] = '\0';
    header_end = strstr(recieved_data, "\r\n\r\n");
    if (header_end != NULL) break;
}

// Fixed-Length Payload Read Phase
if (header_end != NULL) {
    int content_length = 0;
    char *cl_ptr = strcasestr(recieved_data, "Content-Length:");
    if (cl_ptr != NULL) content_length = atoi(cl_ptr + 15);

    if (content_length > 0) {
        int header_len = (header_end + 4) - recieved_data;
        int body_already_read = filled - header_len;
        int body_remaining = content_length - body_already_read;

        while (body_remaining > 0 && filled < BUFFER_SIZE - 1) {
            int r = read(client_fd, recieved_data + filled,
                         (body_remaining < BUFFER_SIZE - 1 - filled) ? body_remaining : (BUFFER_SIZE - 1 - filled));
            if (r <= 0) break;
            filled += r;
            body_remaining -= r;
            recieved_data[filled] = '\0';
        }
    }
}
```

### 3.2 Threading & Resource Lifecycle: Detached Threads vs Pools

#### Memory Footprint (`VIRT` vs `RES`)
In Linux `htop`:
* **`VIRT` (Virtual Memory)**: Shows reserved address space (~6 GB with multiple threads). By default, Linux glibc reserves an **8 MB stack per thread**. This is not actual RAM consumption.
* **`RES` (Resident Memory)**: The **actual physical RAM** used by the engine (typically 5–50 MB).

#### Thread Management Rules:
* **The Detach Contract (`pthread_detach`)**:
  * Default POSIX threads are joinable. If a thread terminates and `pthread_join()` is not called, it becomes a **Zombie Thread**; its 8 MB stack and kernel thread descriptor remain permanently pinned in memory, leading to `EAGAIN` thread exhaustion.
  * Setting `PTHREAD_CREATE_DETACHED` via `pthread_attr_t` instructs the Linux kernel to immediately reclaim all resources upon thread exit with zero memory leakage.

---

## 4. Concurrency Architecture: Bounded Two-Lock Ring Buffer

To manage worker assignments without dynamic heap thrashing (`malloc`/`free`), the engine uses a cache-friendly, array-based circular queue with independent head and tail mutexes.

### 4.1 Data Structure & Sentinel Boundary
* **Capacity**: `MAX_WORKERS + 1` (a dedicated sentinel slot).
* **Empty Condition**: `head == tail`
* **Full Condition**: `(tail + 1) % QUEUE_CAPACITY == head`
* **Zero Contention**: Push operations lock exclusively on `tail_lock`, while Pop operations lock exclusively on `head_lock`.

```c
#define MAX_WORKERS 64
#define QUEUE_CAPACITY (MAX_WORKERS + 1)

typedef struct {
    int buffer[QUEUE_CAPACITY];
    int head; // Read index
    int tail; // Write index
    pthread_mutex_t head_lock;
    pthread_mutex_t tail_lock;
} WorkerQueue;
```

### 4.2 Push / Pop Operations
```c
bool worker_queue_push(WorkerQueue *q, int worker_index) {
    pthread_mutex_lock(&q->tail_lock);
    int next_tail = (q->tail + 1) % QUEUE_CAPACITY;
    if (next_tail == q->head) {
        pthread_mutex_unlock(&q->tail_lock);
        return false; // Queue Full
    }
    q->buffer[q->tail] = worker_index;
    q->tail = next_tail;
    pthread_mutex_unlock(&q->tail_lock);
    return true;
}

int worker_queue_pop(WorkerQueue *q) {
    pthread_mutex_lock(&q->head_lock);
    if (q->head == q->tail) {
        pthread_mutex_unlock(&q->head_lock);
        return -1; // Queue Empty
    }
    int worker_index = q->buffer[q->head];
    q->head = (q->head + 1) % QUEUE_CAPACITY;
    pthread_mutex_unlock(&q->head_lock);
    return worker_index;
}
```

---

## 5. Build System & Module Organization

The project is governed by a unified multi-target CMake configuration ([CMakeLists.txt](file:///home/vithurshan/cloud/PD/httpServer/CMakeLists.txt)):

```
httpServer/
├── CMakeLists.txt              # Unified build with strict flags (-Wall -Wextra -D_GNU_SOURCE)
├── docs/
│   └── VSI_CORE_ENGINE_SPEC.md # This unified technical specification
├── http1_x/
│   ├── http_1_0.c              # Core server loop, socket binding, detached worker dispatch
│   ├── http_1_0.h              # Server handler definitions
│   ├── parser_http_1_0.c       # HTTP/1.0 protocol state machine
│   ├── parser_http_1_0.h       # HTTP request structures and constants
│   ├── WorkerManager.c         # Bounded ring-buffer queue implementation
│   ├── WorkerManager.h         # Concurrency structures and API
│   └── test_parser.c           # Protocol parser unit test harness
└── http0_9/                    # Legacy experimental HTTP 0.9 implementation
```

---

## 6. Development Principles for VSI Engineers
1. **Preserve Documentation & Memory Integrity**: Never allocate on the heap when a bounded array or static ring buffer solves the problem.
2. **Explicit Delimiters**: Always respect TCP streaming semantics—never assume a single `read()` delivers a complete HTTP frame.
3. **Clean Warning-Free Builds**: Every source module must compile cleanly under `-Wall -Wextra -D_GNU_SOURCE` with zero warnings.
