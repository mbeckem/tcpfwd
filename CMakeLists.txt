cmake_minimum_required(VERSION 3.2)

project(tcpfwd)

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

add_executable(tcpfwd
    "main.cpp"
)
target_link_libraries(tcpfwd PUBLIC Threads::Threads)
target_include_directories(tcpfwd SYSTEM PUBLIC "thirdparty/asio-1.12.1/include/")
target_compile_options(tcpfwd PUBLIC "-std=c++17")
target_compile_definitions(tcpfwd PUBLIC ASIO_STANDALONE ASIO_NO_DEPRECATED)
