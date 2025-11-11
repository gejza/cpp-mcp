/**
 * @file proxy_client_example.cpp
 * @brief Example of MCP client connecting through proxy to reverse server
 *
 * This example demonstrates how a client can connect to an MCP server
 * that is running in reverse proxy mode.
 *
 * Usage:
 *   ./proxy_client_example [proxy_url]
 *
 * Example:
 *   ./proxy_client_example http://proxy.example.com:9000
 */

#include "mcp_message.h"
#include "mcp_logger.h"
#include "json.hpp"
#include "httplib.h"

#include <iostream>
#include <memory>

using namespace mcp;
using json = nlohmann::ordered_json;

class simple_proxy_client {
public:
    simple_proxy_client(const std::string& proxy_url) {
        // Parse URL
        size_t scheme_end = proxy_url.find("://");
        if (scheme_end != std::string::npos) {
            size_t host_start = scheme_end + 3;
            size_t port_start = proxy_url.find(":", host_start);

            std::string host;
            int port = 80;

            if (port_start != std::string::npos) {
                host = proxy_url.substr(host_start, port_start - host_start);
                port = std::stoi(proxy_url.substr(port_start + 1));
            } else {
                host = proxy_url.substr(host_start);
            }

            client_ = std::make_unique<httplib::Client>(host, port);
            client_->set_read_timeout(65); // Longer than server timeout
        }
    }

    json send_request(const std::string& method, const json& params = json::object()) {
        request req = request::create(method, params);

        auto res = client_->Post("/message", req.to_json().dump(), "application/json");

        if (!res) {
            throw mcp_exception(error_code::internal_error, "Failed to connect to proxy");
        }

        if (res->status != 200) {
            throw mcp_exception(error_code::internal_error,
                "Proxy returned error: " + std::to_string(res->status));
        }

        return json::parse(res->body);
    }

private:
    std::unique_ptr<httplib::Client> client_;
};

int main(int argc, char* argv[]) {
    std::string proxy_url = "http://localhost:9000";

    if (argc > 1) {
        proxy_url = argv[1];
    }

    LOG_INFO("=== MCP Proxy Client Example ===");
    LOG_INFO("Proxy URL: ", proxy_url);

    try {
        simple_proxy_client client(proxy_url);

        // Initialize
        LOG_INFO("\n1. Initializing connection...");
        json init_params = {
            {"protocolVersion", MCP_VERSION},
            {"clientInfo", {
                {"name", "Proxy Client Example"},
                {"version", "1.0.0"}
            }},
            {"capabilities", json::object()}
        };

        json init_response = client.send_request("initialize", init_params);

        if (init_response.contains("error")) {
            LOG_ERROR("Initialization failed: ", init_response["error"]["message"]);
            return 1;
        }

        LOG_INFO("Initialization successful!");
        LOG_INFO("Server: ", init_response["result"]["serverInfo"]["name"],
                 " v", init_response["result"]["serverInfo"]["version"]);

        // Send initialized notification
        client.send_request("notifications/initialized", json::object());

        // List available tools
        LOG_INFO("\n2. Listing available tools...");
        json tools_response = client.send_request("tools/list");

        if (tools_response.contains("result") && tools_response["result"].contains("tools")) {
            LOG_INFO("Available tools:");
            for (const auto& tool : tools_response["result"]["tools"]) {
                LOG_INFO("  - ", tool["name"], ": ", tool["description"]);
            }
        }

        // Call get_time tool
        LOG_INFO("\n3. Calling 'get_time' tool...");
        json time_params = {
            {"name", "get_time"},
            {"arguments", json::object()}
        };
        json time_response = client.send_request("tools/call", time_params);

        if (time_response.contains("result")) {
            LOG_INFO("Result: ", time_response["result"]["content"][0]["text"]);
        }

        // Call calculator tool
        LOG_INFO("\n4. Calling 'calculator' tool (add 15 + 27)...");
        json calc_params = {
            {"name", "calculator"},
            {"arguments", {
                {"operation", "add"},
                {"a", 15},
                {"b", 27}
            }}
        };
        json calc_response = client.send_request("tools/call", calc_params);

        if (calc_response.contains("result")) {
            LOG_INFO("Result: ", calc_response["result"]["content"][0]["text"]);
        }

        // Call echo tool
        LOG_INFO("\n5. Calling 'echo' tool...");
        json echo_params = {
            {"name", "echo"},
            {"arguments", {
                {"text", "Hello from reverse proxy!"},
                {"uppercase", true}
            }}
        };
        json echo_response = client.send_request("tools/call", echo_params);

        if (echo_response.contains("result")) {
            LOG_INFO("Result: ", echo_response["result"]["content"][0]["text"]);
        }

        // Ping
        LOG_INFO("\n6. Sending ping...");
        json ping_response = client.send_request("ping");
        LOG_INFO("Ping successful!");

        LOG_INFO("\n=== All tests completed successfully! ===");

    } catch (const mcp_exception& e) {
        LOG_ERROR("MCP error: ", e.what());
        return 1;
    } catch (const std::exception& e) {
        LOG_ERROR("Error: ", e.what());
        return 1;
    }

    return 0;
}
