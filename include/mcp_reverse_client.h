/**
 * @file mcp_reverse_client.h
 * @brief MCP Reverse Proxy Client
 *
 * This file implements a reverse proxy client that allows an MCP server
 * to connect to a remote proxy server instead of listening on a local port.
 * This is useful when the local server is not directly accessible from the internet.
 */

#ifndef MCP_REVERSE_CLIENT_H
#define MCP_REVERSE_CLIENT_H

#include "mcp_server.h"
#include "mcp_message.h"
#include "mcp_logger.h"
#include "httplib.h"

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>

namespace mcp {

/**
 * @class reverse_client
 * @brief Reverse proxy client for MCP servers
 *
 * This class allows an MCP server to operate in reverse mode, connecting
 * to a remote proxy server instead of listening on a local port.
 */
class reverse_client {
public:
    /**
     * @struct configuration
     * @brief Configuration for reverse proxy client
     */
    struct configuration {
        /** Proxy server URL (e.g., "http://proxy.example.com:8080") */
        std::string proxy_url;

        /** Registration endpoint (default: "/register") */
        std::string register_endpoint{"/register"};

        /** Polling endpoint (default: "/poll") */
        std::string poll_endpoint{"/poll"};

        /** Response endpoint (default: "/response") */
        std::string response_endpoint{"/response"};

        /** Long polling timeout in seconds (default: 30) */
        int poll_timeout_seconds{30};

        /** Retry delay on error in seconds (default: 5) */
        int retry_delay_seconds{5};
    };

    /**
     * @brief Constructor
     * @param mcp_server Shared pointer to the MCP server instance
     * @param config Reverse proxy configuration
     */
    reverse_client(std::shared_ptr<server> mcp_server, const configuration& config);

    /**
     * @brief Destructor
     */
    ~reverse_client();

    /**
     * @brief Start the reverse proxy client
     * @param blocking If true, blocks until stopped
     * @return True if started successfully
     */
    bool start(bool blocking = true);

    /**
     * @brief Stop the reverse proxy client
     */
    void stop();

    /**
     * @brief Check if the client is running
     * @return True if running
     */
    bool is_running() const;

    /**
     * @brief Get the session ID assigned by the proxy
     * @return Session ID string
     */
    std::string get_session_id() const;

private:
    // Register with the proxy server
    bool register_with_proxy();

    // Poll for incoming requests
    void poll_loop();

    // Process a single poll request
    bool process_poll();

    // Send response back to proxy
    bool send_response(const std::string& request_id, const json& response);

    // MCP server instance
    std::shared_ptr<server> mcp_server_;

    // Configuration
    configuration config_;

    // HTTP client for proxy communication
    std::unique_ptr<httplib::Client> http_client_;

    // Session ID assigned by proxy
    std::string session_id_;

    // Running flag
    std::atomic<bool> running_{false};

    // Polling thread
    std::unique_ptr<std::thread> poll_thread_;

    // Mutex for thread safety
    mutable std::mutex mutex_;
};

} // namespace mcp

#endif // MCP_REVERSE_CLIENT_H
