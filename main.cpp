/*
 * A simple tcp forwarding utility that accepts incoming connections
 * on a local port and forwards them to a pre-configured remote address.
 *
 * Use cmake with -DCMAKE_EXE_LINKER_FLAGS="-static" -DCMAKE_FIND_LIBRARY_SUFFIXES=".a"
 * on linux to produce a static executable.
 *
 * Copyright 2018, Michael Beckemeyer
 */

#include <asio/signal_set.hpp>
#include <asio/ts/net.hpp>

#include <thread>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using asio::io_context;
using namespace asio::ip;

// Forwards data from an incoming tcp socket to the configured destination endpoint.
// Pipes all incoming data from one socke to to the other one, in both directions.
class session : public std::enable_shared_from_this<session>
{
    static const size_t PIPE_SIZE = 128 * 1024;

public:
    session(tcp::socket incoming, const tcp::endpoint& src, const tcp::endpoint& dest)
        : m_src_addr(src)
        , m_dest_addr(dest)
        , m_src_socket(std::move(incoming))
        , m_dest_socket(m_src_socket.get_executor().context())
    {
        m_src_socket.set_option(tcp::no_delay(true));

        m_dest_socket.open(dest.protocol());
        m_dest_socket.set_option(tcp::no_delay(true));
        m_dest_socket.set_option(tcp::socket::keep_alive(true));

        // Pipe src --> dest
        m_src_to_dest.from = &m_src_socket;
        m_src_to_dest.from_addr = &m_src_addr;
        m_src_to_dest.to = &m_dest_socket;
        m_src_to_dest.to_addr = &m_dest_addr;
        m_src_to_dest.buffer.resize(PIPE_SIZE);

        // Pipe dest --> src
        m_dest_to_src.from = &m_dest_socket;
        m_dest_to_src.from_addr = &m_dest_addr;
        m_dest_to_src.to = &m_src_socket;
        m_dest_to_src.to_addr = &m_src_addr;
        m_dest_to_src.buffer.resize(PIPE_SIZE);
    }

    session(const session&) = delete;
    session& operator=(const session&) = delete;

    void start() {
        start_connect();
    }

    void stop() {
        if (!m_stopped) {
            m_stopped = true;
            std::error_code ignored;

            if (m_src_socket.is_open()) {
                m_src_socket.shutdown(tcp::socket::shutdown_both, ignored);
            }
            if (m_dest_socket.is_open()) {
                m_dest_socket.shutdown(tcp::socket::shutdown_both, ignored);
            }

            m_src_socket.close(ignored);
            m_dest_socket.close(ignored);

            std::cout << "Closed session from " << m_src_addr
                      << " (read: " << m_src_to_dest.total << ", write: " << m_dest_to_src.total << ")"
                      << std::endl;
        }
    }

private:
    void start_connect() {
        m_dest_socket.async_connect(m_dest_addr, [self = shared_from_this()](const std::error_code& err) {
            self->on_connect(err);
        });
    }

    void on_connect(const std::error_code& err) {
        if (m_stopped || err == asio::error::operation_aborted) {
            return;
        }

        if (err) {
            std::cout << "Failed to connect to " << m_dest_addr << ": " << err.message() << std::endl;
            stop();
            return;
        }

        start_pipe(m_src_to_dest);
        start_pipe(m_dest_to_src);
    }

private:
    struct pipe {
        pipe(const pipe&) = delete;
        pipe& operator=(const pipe&) = delete;

        pipe() = default;

        tcp::socket* from = nullptr;
        tcp::socket* to = nullptr;
        tcp::endpoint* from_addr = nullptr;
        tcp::endpoint* to_addr = nullptr;
        std::vector<unsigned char> buffer;
        std::uint64_t total = 0;
    };

    // Pipe from socket to socket.
    void start_pipe(pipe& ctx)
    {
        auto callback = [self = shared_from_this(), &ctx](const std::error_code& err, size_t read_size) {
            self->on_pipe_read(ctx, err, read_size);
        };

        ctx.from->async_read_some(asio::buffer(ctx.buffer), std::move(callback));
    }

    // When data was read from the "from" socket of a pipe.
    void on_pipe_read(pipe& ctx, const std::error_code& err, size_t read_size) {
        if (m_stopped || err == asio::error::operation_aborted) {
            return;
        }

        if (err) {
            if (err == asio::error::eof) {
                std::cout << "Socket " << *ctx.from_addr << " end of file, closing session." << std::endl;
            } else {
                std::cout << "Failed to read from socket " << *ctx.from_addr << ": "
                          << err.message() << std::endl;
            }
            stop();
            return;
        }

        auto callback = [self = shared_from_this(), &ctx](const std::error_code& err, size_t write_size) {
            self->on_pipe_write(ctx, err, write_size);
        };

        asio::async_write(*ctx.to, asio::buffer(ctx.buffer, read_size), std::move(callback));
    }

    // When data was written to the "to" socket of a pipe.
    void on_pipe_write(pipe& ctx, const std::error_code& err, size_t write_size) {
        if (m_stopped || err == asio::error::operation_aborted) {
            return;
        }

        if (err) {
            std::cout << "Failed to write to socket " << *ctx.to_addr << ": "
                      << err.message() << std::endl;
            stop();
            return;
        }

        ctx.total += write_size;
        start_pipe(ctx);
    }

private:
    tcp::endpoint m_src_addr;
    tcp::endpoint m_dest_addr;
    tcp::socket m_src_socket;   // Intial incoming connection
    tcp::socket m_dest_socket;  // Proxy destination socket
    pipe m_src_to_dest;
    pipe m_dest_to_src;
    bool m_stopped = false;
};

// Listens for incoming a tcp connections and starts a new session for every new socket.
class server : public std::enable_shared_from_this<server>
{
public:
    server(const tcp::endpoint& listen_addr, std::string remote_host, uint16_t remote_port, io_context& io)
        : m_listen_addr(listen_addr)
        , m_dest_host(std::move(remote_host))
        , m_dest_port(remote_port)
        , m_listener(io)
    {
        m_listener.open(m_listen_addr.protocol());
        m_listener.set_option(tcp::acceptor::reuse_address(true));
        m_listener.bind(m_listen_addr);
        m_listener.listen();
    }

    server(const server&) = delete;
    server& operator=(const server&) = delete;

    void start() {
        if (!m_running) {
            m_running = true;
            start_accept();
        }
    }

    void stop() {
        if (m_running) {
            std::error_code ignore;
            m_listener.close(ignore);
            m_running = false;
        }
    }

private:
    void start_accept() {
        m_listener.async_accept(m_peer_addr, [self = shared_from_this()](const std::error_code& err, tcp::socket peer) {
            self->on_accept(std::move(err), std::move(peer));
        });
    }

    void on_accept(const std::error_code& err, tcp::socket peer) {
        if (!m_running || err == asio::error::operation_aborted) {
            return;
        }

        if (err) {
            std::cout << "Failed to accept a connection: " << err.message() << std::endl;
            stop();
            return;
        }

        std::cout << "Accepted a new socket from " << m_peer_addr << std::endl;

        try {
            start_resolve(std::move(peer), m_peer_addr);
        } catch (const std::exception& e) {
            std::cerr << "Failed to start host name resolve: " << e.what() << std::endl;
        }

        start_accept();
    }

private:
    struct lookup_ctx : enable_shared_from_this<lookup_ctx> {
        lookup_ctx(tcp::socket peer, const tcp::endpoint& peer_addr)
            : peer(std::move(peer))
            , peer_addr(peer_addr)
            , resolver(this->peer.get_executor().context())
        {}

        tcp::socket peer;
        tcp::endpoint peer_addr;
        tcp::resolver resolver;
    };

    void start_resolve(tcp::socket peer, const tcp::endpoint& peer_addr) {
        std::shared_ptr<lookup_ctx> lookup = std::make_shared<lookup_ctx>(std::move(peer), peer_addr);
        lookup->resolver.async_resolve(m_dest_host, "",
            [self = shared_from_this(), lookup](const std::error_code& err, tcp::resolver::results_type results) {
                self->on_resolve(*lookup, err, std::move(results));
            }
        );
    }

    void on_resolve(lookup_ctx& ctx, const std::error_code& err, tcp::resolver::results_type results) {
        if (!m_running || err == asio::error::operation_aborted) {
            return;
        }

        if (err) {
            std::cout << "Failed to resolve " << m_dest_host << ": " << err.message() << std::endl;
            return;
        }

        if (results.empty()) {
            std::cout << "No addresses found for host " << m_dest_host << std::endl;
            return;
        }

        address addr = static_cast<tcp::endpoint>(*results.begin()).address();
        std::cout << "Resolved " << m_dest_host << " to " << addr << std::endl;

        try {
            std::make_shared<session>(std::move(ctx.peer), ctx.peer_addr, tcp::endpoint(addr, m_dest_port))->start();
        } catch (const std::exception& e) {
            std::cout << "Failed to create new session for " << m_peer_addr << ": " << e.what() << std::endl;
        }
    }


private:
    tcp::endpoint m_listen_addr;
    std::string m_dest_host;
    uint16_t m_dest_port;
    tcp::endpoint m_peer_addr;
    tcp::acceptor m_listener;
    bool m_running = false;
};

// Thrown on SIGINT, SIGTERM
struct force_shutdown {};

int start_server(tcp::endpoint listen_addr, const std::string& remote_host, uint16_t remote_port, asio::io_context& io) {
    // Start listening on the local socket.
    std::shared_ptr<server> srv;
    try {
        srv = std::make_shared<server>(listen_addr, remote_host, remote_port, io);
        srv->start();
    } catch (const std::exception& e) {
        std::cout << "Failed to start server: " << e.what() << std::endl;
        return 1;
    }

    // Wait for incoming signals.
    std::shared_ptr<asio::signal_set> signals;
    try {
        signals = std::make_shared<asio::signal_set>(io);
        signals->add(SIGINT);
        signals->add(SIGTERM);
        signals->async_wait([signals](const std::error_code& err, int signal) {
            if (err == asio::error::operation_aborted)
                return;

            if (err) {
                std::cout << "Failed to wait for signals: " << err.message() << std::endl;
                return;
            }

            (void) signal;
            throw force_shutdown();
        });
    } catch (const std::exception& e) {
        std::cout << "Failed to setup signal handlers: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

int main(int argc, char** argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() != 3) {
        std::cout << "Usage: " << argv[0] << " <LOCAL_PORT> <DEST_HOST> <DEST_PORT>" << std::endl;
        return 1;
    }

    tcp::endpoint listen_addr(address_v4::any(), std::stoi(args[0]));
    std::string remote_host = args[1];
    uint16_t remote_port = std::stoi(args[2]);

    std::cout << "Setting up forwarding from local port " << listen_addr.port()
              << " to destination " << remote_host << ":" << remote_port << std::endl;

    asio::io_context io;
    if (int ret = start_server(listen_addr, remote_host, remote_port, io); ret != 0) {
        return ret;
    }

    while (1) {
        try {
            io.run();
            break;
        } catch (force_shutdown) {
            std::cout << "Shutting down." << std::endl;
            break;
        } catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << std::endl;
            io.restart();
        }
    }

    return 0;
}
