cmake_minimum_required(VERSION 3.28.3)
project(httpServer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -D_GNU_SOURCE")

# Common include directories
include_directories(http1_x http0_9)

# HTTP/1.0 Server executable
add_executable(http_1_0_server
        http1_x/http_1_0.c
        http1_x/http_1_0.h
        http1_x/parser_http_1_0.c
        http1_x/parser_http_1_0.h)

# HTTP/1.0 Parser test executable
add_executable(test_parser
        http1_x/test_parser.c
        http1_x/parser_http_1_0.c
        http1_x/parser_http_1_0.h)

# HTTP/0.9 Server executable
add_executable(http_0_9_server
        http0_9/http_server.c)

# Byte order test executable
add_executable(hn_byteorder
        http0_9/hn_byteorder_representation.c)

# Simple scratch test executable
add_executable(test_scratch
        test.c)
