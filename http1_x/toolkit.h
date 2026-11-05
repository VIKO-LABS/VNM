#ifndef HTTPSERVER_TOOLKIT_H
#define HTTPSERVER_TOOLKIT_H

#include "parser_http_1_0.h"
#include <stddef.h>
#include <stdint.h>

/**
 * Maps raw HTTP request byte buffer into an HttpRequest struct.
 *
 * @param input_buffer  Source raw HTTP byte buffer
 * @param buffer_size   Number of bytes available in input_buffer
 * @param req           Pointer to the destination HttpRequest struct to populate
 * @return              0 on success, or -1 on parsing/mapping error
 */
int request_mapper(const char *input_buffer, size_t buffer_size, HttpRequest *req);

/**
 * Demaps (serializes) an HttpRequest struct back into a raw HTTP wire-format byte buffer.
 *
 * @param req           Pointer to the source HttpRequest struct
 * @param output_buffer Destination buffer to write serialized HTTP bytes into
 * @param buffer_size   Maximum capacity of output_buffer
 * @return              Total bytes written into output_buffer, or -1 on error/overflow
 */
int request_demapper(const HttpRequest *req, char *output_buffer, size_t buffer_size);

/**
 * Maps raw HTTP response byte buffer into an HttpResponse struct.
 *
 * @param input_buffer  Source raw HTTP response byte buffer
 * @param buffer_size   Number of bytes available in input_buffer
 * @param res           Pointer to the destination HttpResponse struct to populate
 * @return              0 on success, or -1 on parsing/mapping error
 */
int response_mapper(const char *input_buffer, size_t buffer_size, HttpResponse *res);

/**
 * Demaps (serializes) an HttpResponse struct back into a raw HTTP wire-format byte buffer.
 *
 * @param res           Pointer to the source HttpResponse struct
 * @param output_buffer Destination buffer to write serialized HTTP response bytes into
 * @param buffer_size   Maximum capacity of output_buffer
 * @return              Total bytes written into output_buffer, or -1 on error/overflow
 */
int response_demapper(const HttpResponse *res, char *output_buffer, size_t buffer_size);

/**
 * Prints the contents of a buffer up to size bytes to stdout.
 * Safely handles non-printable bytes or null-terminators using binary-safe stdout write.
 *
 * @param buffer Pointer to the raw byte buffer to print
 * @param size   Number of bytes to print
 */
void print_buffer(const char *buffer, size_t size);

/**
 * Inspects and prints a human-readable breakdown of a TLS handshake message
 * (e.g. TLS Client Hello or Server Hello), extracting TLS version, ciphers,
 * and SNI (Server Name Indication) domain name directly from raw wire bytes.
 *
 * @param buffer Raw byte buffer received on the socket
 * @param size   Number of bytes in the buffer
 */
void print_tls_handshake(const char *buffer, size_t size);

#endif // HTTPSERVER_TOOLKIT_H
