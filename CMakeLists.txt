cmake_minimum_required(VERSION 3.28.3)
project(httpServer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -D_GNU_SOURCE")

# Common include directories
include_directories(src http1_x CoreEngine services/forwardProxy utils)

# VSI Core Engine executable
add_executable(vsi_ngine
        src/main.c
        CoreEngine/transport/vsi_transport.c
        CoreEngine/transport/vsi_transport.h
        CoreEngine/config/vsi_config.c
        CoreEngine/config/vsi_config.h
        services/forwardProxy/forwardProxy.c
        services/forwardProxy/forwardProxy.h
        utils/toolkit.c
        utils/toolkit.h
        http1_x/http1_codec.c
        http1_x/http1_codec.h
        http1_x/parser_http_1_0.c
        http1_x/parser_http_1_0.h
        http1_x/outboundEngine.c
        http1_x/outboundEngine.h
        http1_x/WorkerManager.c
        http1_x/WorkerManager.h)

target_include_directories(vsi_ngine PRIVATE
        ${CMAKE_SOURCE_DIR}
        src
        http1_x
        CoreEngine
        services/forwardProxy
        utils)

# Simple scratch test executable
#add_executable(test_scratch
#        test.c)
