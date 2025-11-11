/**
 * @file mcp_reverse_client.cpp
 * @brief Implementation of MCP Reverse Proxy Client
 */

#include "mcp_reverse_client.h"
#include <random>
#include <sstream>
#include <iomanip>

namespace mcp {

reverse_client::reverse_client(std::shared_ptr<server> mcp_server, const configuration& config)
    : mcp_server_(mcp_server)
    , config_(config)
{
    // Parse proxy URL to create HTTP client
    // Expected format: http://host:port or https://host:port
    std::string scheme, host;
    int port = 80;

    size_t scheme_end = config_.proxy_url.find("://");
    if (scheme_end != std::string::npos) {
        scheme = config_.proxy_url.substr(0, scheme_end);
        size_t host_start = scheme_end + 3;
        size_t port_start = config_.proxy_url.find(":", host_start);

        if (port_start != std::string::npos) {
            host = config_.proxy_url.substr(host_start, port_start - host_start);
            port = std::stoi(config_.proxy_url.substr(port_start + 1));
        } else {
            host = config_.proxy_url.substr(host_start);
            port = (scheme == "https") ? 443 : 80;
        }
    } else {
        throw mcp_exception(error_code::invalid_params, "Invalid proxy URL format");
    }

    LOG_INFO("Creating HTTP client for proxy: ", scheme, "://", host, ":", port);
    http_client_ = std::make_unique<httplib::Client>(host, port);
    http_client_->set_read_timeout(config_.poll_timeout_seconds + 5);
}

reverse_client::~reverse_client() {
    stop();
}

bool reverse_client::start(bool blocking) {
    if (running_) {
        LOG_WARNING("Reverse client already running");
        return true;
    }

    LOG_INFO("Starting reverse proxy client");

    // Register with proxy
    if (!register_with_proxy()) {
        LOG_ERROR("Failed to register with proxy server");
        return false;
    }

    LOG_INFO("Successfully registered with proxy, session_id: ", session_id_);

    running_ = true;

    if (blocking) {
        // Run polling loop in current thread
        poll_loop();
        return true;
    } else {
        // Start polling thread
        poll_thread_ = std::make_unique<std::thread>([this]() {
            poll_loop();
        });
        return true;
    }
}

void reverse_client::stop() {
    if (!running_) {
        return;
    }

    LOG_INFO("Stopping reverse proxy client");
    running_ = false;

    // Wait for poll thread to finish
    if (poll_thread_ && poll_thread_->joinable()) {
        poll_thread_->join();
    }

    LOG_INFO("Reverse proxy client stopped");
}

bool reverse_client::is_running() const {
    return running_;
}

std::string reverse_client::get_session_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_id_;
}

bool reverse_client::register_with_proxy() {
    LOG_INFO("Registering with proxy server at: ", config_.proxy_url, config_.register_endpoint);

    // Prepare registration request
    json reg_request = {
        {"type", "mcp_server"},
        {"timestamp", std::chrono::system_clock::now().time_since_epoch().count()}
    };

    auto res = http_client_->Post(config_.register_endpoint.c_str(),
                                   reg_request.dump(),
                                   "application/json");

    if (!res) {
        LOG_ERROR("Failed to connect to proxy server");
        return false;
    }

    if (res->status != 200) {
        LOG_ERROR("Proxy server returned error: ", res->status);
        return false;
    }

    try {
        json response = json::parse(res->body);
        if (!response.contains("session_id")) {
            LOG_ERROR("Proxy response missing session_id");
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        session_id_ = response["session_id"].get<std::string>();
        return true;
    } catch (const json::exception& e) {
        LOG_ERROR("Failed to parse proxy response: ", e.what());
        return false;
    }
}

void reverse_client::poll_loop() {
    LOG_INFO("Starting poll loop");

    while (running_) {
        try {
            if (!process_poll()) {
                // Poll failed, wait before retrying
                LOG_WARNING("Poll failed, waiting ", config_.retry_delay_seconds, " seconds before retry");
                std::this_thread::sleep_for(std::chrono::seconds(config_.retry_delay_seconds));
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Exception in poll loop: ", e.what());
            std::this_thread::sleep_for(std::chrono::seconds(config_.retry_delay_seconds));
        }
    }

    LOG_INFO("Poll loop ended");
}

bool reverse_client::process_poll() {
    std::string poll_url = config_.poll_endpoint + "?session_id=" + session_id_;

    auto res = http_client_->Get(poll_url.c_str());

    if (!res) {
        LOG_ERROR("Failed to poll proxy server");
        return false;
    }

    // 204 No Content means no requests available (timeout)
    if (res->status == 204) {
        return true; // This is normal, just continue polling
    }

    if (res->status != 200) {
        LOG_ERROR("Proxy server returned error: ", res->status);
        return false;
    }

    try {
        json poll_response = json::parse(res->body);

        if (!poll_response.contains("request_id") || !poll_response.contains("request")) {
            LOG_ERROR("Invalid poll response format");
            return false;
        }

        std::string request_id = poll_response["request_id"].get<std::string>();
        json request_json = poll_response["request"];

        LOG_INFO("Received request from proxy: ", request_id);

        // Parse MCP request
        request mcp_req;
        mcp_req.jsonrpc = request_json["jsonrpc"].get<std::string>();
        if (request_json.contains("id") && !request_json["id"].is_null()) {
            mcp_req.id = request_json["id"];
        }
        mcp_req.method = request_json["method"].get<std::string>();
        if (request_json.contains("params")) {
            mcp_req.params = request_json["params"];
        }

        // Process request using MCP server
        json response_json = mcp_server_->process_request_public(mcp_req, session_id_);

        // Send response back to proxy
        return send_response(request_id, response_json);

    } catch (const json::exception& e) {
        LOG_ERROR("Failed to parse poll response: ", e.what());
        return false;
    } catch (const std::exception& e) {
        LOG_ERROR("Error processing request: ", e.what());
        return false;
    }
}

bool reverse_client::send_response(const std::string& request_id, const json& response) {
    std::string response_url = config_.response_endpoint + "?session_id=" + session_id_;

    json response_body = {
        {"request_id", request_id},
        {"response", response}
    };

    auto res = http_client_->Post(response_url.c_str(),
                                   response_body.dump(),
                                   "application/json");

    if (!res || res->status != 200) {
        LOG_ERROR("Failed to send response to proxy");
        return false;
    }

    LOG_INFO("Response sent successfully for request: ", request_id);
    return true;
}

} // namespace mcp
