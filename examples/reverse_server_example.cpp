/**
 * @file reverse_server_example.cpp
 * @brief Example of MCP server running in reverse proxy mode
 *
 * This example demonstrates how to run an MCP server that connects
 * to a remote proxy server instead of listening on a local port.
 *
 * Usage:
 *   ./reverse_server_example [proxy_url]
 *
 * Example:
 *   ./reverse_server_example http://proxy.example.com:9000
 */

#include "mcp_server.h"
#include "mcp_reverse_client.h"
#include "mcp_tool.h"
#include "mcp_logger.h"

#include <iostream>
#include <memory>
#include <chrono>
#include <ctime>

using namespace mcp;

int main(int argc, char* argv[]) {
    std::string proxy_url = "http://localhost:9000";

    if (argc > 1) {
        proxy_url = argv[1];
    }

    LOG_INFO("=== MCP Reverse Server Example ===");
    LOG_INFO("Proxy URL: ", proxy_url);

    try {
        // 1. Create and configure MCP server (but DON'T start it in listen mode)
        server::configuration server_config;
        server_config.name = "Reverse MCP Server";
        server_config.version = "1.0.0";

        auto mcp_server = std::make_shared<server>(server_config);

        // 2. Set server capabilities
        json capabilities = {
            {"tools", json::object()}
        };
        mcp_server->set_capabilities(capabilities);

        // 3. Register some tools

        // Tool 1: Get current time
        tool time_tool = tool_builder("get_time")
            .with_description("Get the current system time")
            .build();

        mcp_server->register_tool(time_tool, [](const json& args, const std::string& session_id) -> json {
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            std::string time_str = std::ctime(&time_t);
            time_str.pop_back(); // Remove newline

            LOG_INFO("Tool 'get_time' called from session: ", session_id);

            return json::array({
                {
                    {"type", "text"},
                    {"text", "Current time: " + time_str}
                }
            });
        });

        // Tool 2: Calculator
        tool calc_tool = tool_builder("calculator")
            .with_description("Perform basic arithmetic operations")
            .with_string_param("operation", "Operation: add, subtract, multiply, divide", true)
            .with_number_param("a", "First number", true)
            .with_number_param("b", "Second number", true)
            .build();

        mcp_server->register_tool(calc_tool, [](const json& args, const std::string& session_id) -> json {
            std::string operation = args["operation"];
            double a = args["a"];
            double b = args["b"];
            double result = 0.0;

            LOG_INFO("Tool 'calculator' called: ", operation, "(", a, ", ", b, ") from session: ", session_id);

            if (operation == "add") {
                result = a + b;
            } else if (operation == "subtract") {
                result = a - b;
            } else if (operation == "multiply") {
                result = a * b;
            } else if (operation == "divide") {
                if (b == 0) {
                    throw mcp_exception(error_code::invalid_params, "Division by zero");
                }
                result = a / b;
            } else {
                throw mcp_exception(error_code::invalid_params, "Unknown operation: " + operation);
            }

            return json::array({
                {
                    {"type", "text"},
                    {"text", "Result: " + std::to_string(result)}
                }
            });
        });

        // Tool 3: Echo
        tool echo_tool = tool_builder("echo")
            .with_description("Echo back the provided text")
            .with_string_param("text", "Text to echo", true)
            .with_boolean_param("uppercase", "Convert to uppercase", false)
            .build();

        mcp_server->register_tool(echo_tool, [](const json& args, const std::string& session_id) -> json {
            std::string text = args["text"];
            bool uppercase = args.contains("uppercase") ? args["uppercase"].get<bool>() : false;

            LOG_INFO("Tool 'echo' called from session: ", session_id);

            if (uppercase) {
                for (char& c : text) {
                    c = std::toupper(c);
                }
            }

            return json::array({
                {
                    {"type", "text"},
                    {"text", text}
                }
            });
        });

        LOG_INFO("Registered ", mcp_server->get_tools().size(), " tools");

        // 4. Create reverse proxy client configuration
        reverse_client::configuration reverse_config;
        reverse_config.proxy_url = proxy_url;
        reverse_config.poll_timeout_seconds = 30;
        reverse_config.retry_delay_seconds = 5;

        // 5. Create and start reverse client
        auto reverse = std::make_shared<reverse_client>(mcp_server, reverse_config);

        LOG_INFO("Starting reverse proxy client...");
        LOG_INFO("The server will connect to the proxy and wait for requests");
        LOG_INFO("Press Ctrl+C to stop");

        // Start in blocking mode
        if (!reverse->start(true)) {
            LOG_ERROR("Failed to start reverse client");
            return 1;
        }

    } catch (const mcp_exception& e) {
        LOG_ERROR("MCP error: ", e.what());
        return 1;
    } catch (const std::exception& e) {
        LOG_ERROR("Error: ", e.what());
        return 1;
    }

    return 0;
}
