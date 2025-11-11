/**
 * @file proxy_server.cpp
 * @brief Simple proxy server for reverse MCP connections
 *
 * This server acts as a public-facing proxy that accepts:
 * 1. MCP servers connecting in reverse mode (via /register and /poll)
 * 2. MCP clients connecting normally (via /sse and /message)
 *
 * It routes requests from clients to the appropriate MCP server.
 */

#include "mcp_logger.h"
#include "json.hpp"
#include "httplib.h"

#include <map>
#include <mutex>
#include <queue>
#include <memory>
#include <random>
#include <sstream>
#include <chrono>

using json = nlohmann::ordered_json;

// Request waiting to be processed by MCP server
struct pending_request {
    std::string request_id;
    json request_data;
    std::promise<json> response_promise;
    std::chrono::steady_clock::time_point timestamp;
};

// Session representing a connected MCP server
struct server_session {
    std::string session_id;
    std::queue<std::shared_ptr<pending_request>> pending_requests;
    std::map<std::string, std::shared_ptr<pending_request>> in_flight_requests;
    std::mutex mutex;
    std::chrono::steady_clock::time_point last_activity;
};

// Global state
std::map<std::string, std::shared_ptr<server_session>> sessions;
std::mutex sessions_mutex;

// Generate random ID
std::string generate_id() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);

    std::stringstream ss;
    ss << std::hex;
    for (int i = 0; i < 16; ++i) {
        ss << dis(gen);
    }
    return ss.str();
}

int main(int argc, char* argv[]) {
    int port = 9000;

    if (argc > 1) {
        port = std::atoi(argv[1]);
    }

    LOG_INFO("Starting MCP Proxy Server on port ", port);

    httplib::Server proxy;

    // CORS setup
    proxy.Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
    });

    // Register endpoint - MCP servers register here
    proxy.Post("/register", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Content-Type", "application/json");

        try {
            json request = json::parse(req.body);

            // Generate session ID
            std::string session_id = generate_id();

            // Create session
            auto session = std::make_shared<server_session>();
            session->session_id = session_id;
            session->last_activity = std::chrono::steady_clock::now();

            {
                std::lock_guard<std::mutex> lock(sessions_mutex);
                sessions[session_id] = session;
            }

            LOG_INFO("MCP server registered with session_id: ", session_id);

            json response = {
                {"session_id", session_id},
                {"poll_endpoint", "/poll"},
                {"response_endpoint", "/response"}
            };

            res.status = 200;
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            LOG_ERROR("Registration error: ", e.what());
            res.status = 400;
            res.set_content("{\"error\":\"Invalid request\"}", "application/json");
        }
    });

    // Poll endpoint - MCP servers poll for requests here
    proxy.Get("/poll", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Content-Type", "application/json");

        auto it = req.params.find("session_id");
        if (it == req.params.end()) {
            res.status = 400;
            res.set_content("{\"error\":\"Missing session_id\"}", "application/json");
            return;
        }

        std::string session_id = it->second;
        std::shared_ptr<server_session> session;

        {
            std::lock_guard<std::mutex> lock(sessions_mutex);
            auto sess_it = sessions.find(session_id);
            if (sess_it == sessions.end()) {
                res.status = 404;
                res.set_content("{\"error\":\"Session not found\"}", "application/json");
                return;
            }
            session = sess_it->second;
        }

        // Wait for a request (simple timeout-based long polling)
        auto start = std::chrono::steady_clock::now();
        auto timeout = std::chrono::seconds(30);

        while (std::chrono::steady_clock::now() - start < timeout) {
            std::shared_ptr<pending_request> request;

            {
                std::lock_guard<std::mutex> lock(session->mutex);
                if (!session->pending_requests.empty()) {
                    request = session->pending_requests.front();
                    session->pending_requests.pop();
                    session->in_flight_requests[request->request_id] = request;
                    session->last_activity = std::chrono::steady_clock::now();
                    break;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::shared_ptr<pending_request> request;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (!session->pending_requests.empty()) {
                request = session->pending_requests.front();
                session->pending_requests.pop();
                session->in_flight_requests[request->request_id] = request;
            }
        }

        if (request) {
            json response = {
                {"request_id", request->request_id},
                {"request", request->request_data}
            };

            res.status = 200;
            res.set_content(response.dump(), "application/json");
        } else {
            // No requests available
            res.status = 204; // No Content
        }
    });

    // Response endpoint - MCP servers send responses here
    proxy.Post("/response", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Content-Type", "application/json");

        auto it = req.params.find("session_id");
        if (it == req.params.end()) {
            res.status = 400;
            res.set_content("{\"error\":\"Missing session_id\"}", "application/json");
            return;
        }

        std::string session_id = it->second;
        std::shared_ptr<server_session> session;

        {
            std::lock_guard<std::mutex> lock(sessions_mutex);
            auto sess_it = sessions.find(session_id);
            if (sess_it == sessions.end()) {
                res.status = 404;
                res.set_content("{\"error\":\"Session not found\"}", "application/json");
                return;
            }
            session = sess_it->second;
        }

        try {
            json response_data = json::parse(req.body);

            if (!response_data.contains("request_id") || !response_data.contains("response")) {
                res.status = 400;
                res.set_content("{\"error\":\"Invalid response format\"}", "application/json");
                return;
            }

            std::string request_id = response_data["request_id"];
            json response_json = response_data["response"];

            // Find the pending request and fulfill the promise
            std::shared_ptr<pending_request> request;
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                auto req_it = session->in_flight_requests.find(request_id);
                if (req_it != session->in_flight_requests.end()) {
                    request = req_it->second;
                    session->in_flight_requests.erase(req_it);
                    session->last_activity = std::chrono::steady_clock::now();
                }
            }

            if (request) {
                request->response_promise.set_value(response_json);
                LOG_INFO("Response received for request: ", request_id);
            } else {
                LOG_WARNING("Response for unknown request: ", request_id);
            }

            res.status = 200;
            res.set_content("{\"status\":\"ok\"}", "application/json");
        } catch (const std::exception& e) {
            LOG_ERROR("Response error: ", e.what());
            res.status = 400;
            res.set_content("{\"error\":\"Invalid response\"}", "application/json");
        }
    });

    // Message endpoint - MCP clients send requests here
    proxy.Post("/message", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Content-Type", "application/json");

        // For simplicity, route to the first available session
        // In production, you'd want session management for clients
        std::shared_ptr<server_session> session;

        {
            std::lock_guard<std::mutex> lock(sessions_mutex);
            if (sessions.empty()) {
                res.status = 503;
                res.set_content("{\"error\":\"No MCP servers available\"}", "application/json");
                return;
            }
            session = sessions.begin()->second;
        }

        try {
            json request_data = json::parse(req.body);

            // Create pending request
            auto request = std::make_shared<pending_request>();
            request->request_id = generate_id();
            request->request_data = request_data;
            request->timestamp = std::chrono::steady_clock::now();

            auto response_future = request->response_promise.get_future();

            {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->pending_requests.push(request);
            }

            LOG_INFO("Request queued: ", request->request_id);

            // Wait for response (with timeout)
            auto status = response_future.wait_for(std::chrono::seconds(60));

            if (status == std::future_status::ready) {
                json response = response_future.get();
                res.status = 200;
                res.set_content(response.dump(), "application/json");
            } else {
                res.status = 504;
                res.set_content("{\"error\":\"Request timeout\"}", "application/json");
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Message error: ", e.what());
            res.status = 400;
            res.set_content("{\"error\":\"Invalid request\"}", "application/json");
        }
    });

    // Start server
    LOG_INFO("Proxy server listening on 0.0.0.0:", port);
    proxy.listen("0.0.0.0", port);

    return 0;
}
