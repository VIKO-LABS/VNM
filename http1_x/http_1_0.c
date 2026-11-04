//
// Created by vithurshan on 10/22/26.
//

#include "http_1_0.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>

#include "parser_http_1_0.h"
#include <sys/socket.h>
#include <arpa/inet.h>

#define BUFFER_SIZE 65536
#define WORKER_THREAD_POOL_SIZE 5

//worker1,worker2,worker3
//available.pop()
//workermanager.getavailableworker()

pthread_t thread_pool[WORKER_THREAD_POOL_SIZE];

void* handle_user_connection(void* client_fd_arg) {
    int client_fd = (int)(intptr_t)client_fd_arg;
    char recieved_data[BUFFER_SIZE];
    int filled = 0;
    char *header_end = NULL;

    // Phase 1: Read until the header delimiter (\r\n\r\n) is found
    while (filled < BUFFER_SIZE - 1) {
        //linux kernel will get and assembled and store in a buffer (kernel) while it communiate with client. by calling read we just drain the buffere and stored it in the userspace buffer given by us . eventhough any interuption the data will be stored in the kernel buffer
        int r = read(client_fd, recieved_data + filled, BUFFER_SIZE - 1 - filled);
        if (r <= 0) {
            break;
        }
        filled += r; // r gives the copied data from the kernel buffer so it helps to identify the last of recieve data index
        recieved_data[filled] = '\0';

        header_end = strstr(recieved_data, "\r\n\r\n");
        if (header_end != NULL) {
            break;
        }
    }

    // Phase 2 & 3: If headers were found, parse Content-Length and read the body
    if (header_end != NULL) {
        int content_length = 0;
        // Case-insensitive search for Content-Length
        char *cl_ptr = strcasestr(recieved_data, "Content-Length:");
        if (cl_ptr != NULL) {
            content_length = atoi(cl_ptr + 15);
        }
        printf("header content length is %d\n", content_length);

        if (content_length > 0) {
            // Bytes of the body that have already been read into the buffer
            int header_len = (header_end + 4) - recieved_data;
            int body_already_read = filled - header_len;
            int body_remaining = content_length - body_already_read;

            while (body_remaining > 0 && filled < BUFFER_SIZE - 1) { // there is a risk when content remining length exceed the buffer size
                int r = read(client_fd, recieved_data + filled,
                             (body_remaining < BUFFER_SIZE - 1 - filled) ? body_remaining : (BUFFER_SIZE - 1 - filled));
                if (r <= 0) {
                    break;
                }
                filled += r;
                body_remaining -= r;
                recieved_data[filled] = '\0';
            }
        }
    }
    //request header and body recieved and stored in recieved data
    HttpRequest req;
    parse_http_1_0(recieved_data,filled,&req);
    printf("Total bytes received: %d\n", filled);
    printf("request method \n");
    int s = 0;
    while (req.method[s]!='\0') {
        printf("%c",req.method[s]);
        s++;
    }
    printf("\n");

    // Send a valid HTTP/1.0 response
    const char *body = "<h1>Hello from C HTTP Server!</h1>\n";
    char response[1024];
    int resp_len = snprintf(response, sizeof(response),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s", strlen(body), body);

    write(client_fd, response, resp_len);
    printf("before\n");
    sleep(300);
    printf("released\n");
    close(client_fd);
    return NULL;
}

int main() {
    //int argc, char** argv
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0); // when we call this kernel sace will create a struct of socket and sock object to hold ip, port and other meta data | and put it in the process's fd table and return the index to the userspace
    if (sock_fd < 0) {
        perror("socket creation failed");
        return 1;
    }

    int opt = 1;
    setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // by calling this it will set SO_REUSEADDR = 1 which allows to reuse the ort right after a server stopped because generally kernel hold the port for 120 to 60 seconds to drain existing communication and connections

    struct sockaddr_in socket_address = {0};
    socket_address.sin_family = AF_INET;
    socket_address.sin_port = htons(8085);
    socket_address.sin_addr.s_addr = htonl(INADDR_ANY);

    int bind_status = bind(sock_fd, (struct sockaddr*)(&socket_address), sizeof(socket_address));
    // listen() instructs kernel to passively accept handshakes and populate the internal SYN/Accept queues : ie - this call makes the kernel to accept the inbound on the port and create the 2 queues
    int listen_status = listen(sock_fd, 4);
    printf("bind status %d and listen status %d\n", bind_status, listen_status);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    while (true) {
        // accept() pops a completed TCP connection from kernel Accept queue; if empty, thread sleeps (0% CPU) until NIC IRQ wakes it
        int client_fd = accept(sock_fd, NULL, NULL);
        if (client_fd < 0) {
            perror("accept error");
            continue;
        }

        pthread_t temp_worker;
        pthread_create(&temp_worker, &attr, handle_user_connection, (void*)(intptr_t)client_fd);
    }

    pthread_attr_destroy(&attr);
    close(sock_fd);
    return 0;
}
