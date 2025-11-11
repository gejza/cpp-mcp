# MCP Reverse Proxy Mode

This document describes how to use the MCP library in reverse proxy mode, which allows an MCP server to connect to a remote proxy instead of listening on a local port.

## Why Reverse Proxy Mode?

In some scenarios, your MCP server might be:
- Behind a firewall or NAT
- On a network where incoming connections are blocked
- Not directly accessible from the internet

Reverse proxy mode solves this by having the MCP server **connect out** to a publicly accessible proxy server, which then routes client requests back to your server through this established connection.

## Architecture

```
┌─────────────────┐         ┌──────────────┐         ┌──────────────────┐
│  MCP Client     │◄───────►│ Proxy Server │◄───────►│  MCP Server      │
│  (User/Claude)  │  HTTP   │  (Public)    │  Poll   │  (Your Machine)  │
└─────────────────┘         └──────────────┘         └──────────────────┘
                                    ▲
                                    │
                                    └─ Long polling connection
                                       initiated by MCP server
```

## Components

### 1. Proxy Server (`proxy_server.cpp`)

The proxy server runs on a publicly accessible machine and provides these endpoints:

- `POST /register` - MCP servers register here and get a session ID
- `GET /poll?session_id=X` - MCP servers poll for incoming requests (long polling, 30s timeout)
- `POST /response?session_id=X` - MCP servers send responses back
- `POST /message` - MCP clients send requests here

### 2. Reverse Client (`mcp_reverse_client.h/cpp`)

A wrapper class that:
- Registers your MCP server with the proxy
- Continuously polls for incoming requests
- Routes requests to your MCP server's tool handlers
- Sends responses back through the proxy

### 3. MCP Server (Your Application)

Your normal MCP server code, but:
- **DON'T** call `server.start()` - it won't listen locally
- Instead, wrap it with `reverse_client` and call `reverse_client.start()`

## Minimal Patch Applied

Only **3 new public methods** were added to `mcp_server.h`:

```cpp
// Allow external processing of requests
json process_request_public(const request& req, const std::string& session_id);

// Session management for reverse mode
bool is_session_initialized_public(const std::string& session_id) const;
void set_session_initialized_public(const std::string& session_id, bool initialized);
```

These methods simply delegate to the existing private methods, exposing the request processing logic for reverse mode.

## Usage Example

### Step 1: Start the Proxy Server

On a publicly accessible machine:

```bash
./proxy_server 9000
```

This starts the proxy listening on port 9000.

### Step 2: Start Your MCP Server in Reverse Mode

On your local machine (even behind NAT/firewall):

```cpp
#include "mcp_server.h"
#include "mcp_reverse_client.h"
#include "mcp_tool.h"

// 1. Create MCP server (but DON'T start it)
auto mcp_server = std::make_shared<mcp::server>(config);

// 2. Register your tools as normal
tool my_tool = tool_builder("my_tool")
    .with_description("Does something useful")
    .build();

mcp_server->register_tool(my_tool, [](const json& args, const std::string& session_id) {
    // Your tool logic here
    return json::array({{"type", "text"}, {"text", "Result"}});
});

// 3. Create reverse client config
mcp::reverse_client::configuration reverse_config;
reverse_config.proxy_url = "http://your-proxy.example.com:9000";

// 4. Create and start reverse client
auto reverse = std::make_shared<mcp::reverse_client>(mcp_server, reverse_config);
reverse->start(true); // Blocking mode
```

Or use the example:

```bash
./reverse_server_example http://your-proxy.example.com:9000
```

### Step 3: Connect Clients to the Proxy

Clients connect to the proxy server as if it were the MCP server:

```bash
./proxy_client_example http://your-proxy.example.com:9000
```

## Protocol Details

### Registration Flow

1. **MCP Server → Proxy**: `POST /register`
   ```json
   {
     "type": "mcp_server",
     "timestamp": 1234567890
   }
   ```

2. **Proxy → MCP Server**: Response with session ID
   ```json
   {
     "session_id": "abc123...",
     "poll_endpoint": "/poll",
     "response_endpoint": "/response"
   }
   ```

### Request/Response Flow

1. **Client → Proxy**: `POST /message`
   ```json
   {
     "jsonrpc": "2.0",
     "id": 1,
     "method": "tools/call",
     "params": {...}
   }
   ```

2. **Proxy**: Queues request and waits for MCP server to poll

3. **MCP Server → Proxy**: `GET /poll?session_id=abc123`

4. **Proxy → MCP Server**: Returns queued request
   ```json
   {
     "request_id": "req789",
     "request": {
       "jsonrpc": "2.0",
       "id": 1,
       "method": "tools/call",
       "params": {...}
     }
   }
   ```

5. **MCP Server**: Processes request using registered handlers

6. **MCP Server → Proxy**: `POST /response?session_id=abc123`
   ```json
   {
     "request_id": "req789",
     "response": {
       "jsonrpc": "2.0",
       "id": 1,
       "result": {...}
     }
   }
   ```

7. **Proxy → Client**: Returns response to waiting client

## Configuration Options

### Reverse Client Configuration

```cpp
mcp::reverse_client::configuration config;
config.proxy_url = "http://proxy.example.com:9000";  // Required
config.register_endpoint = "/register";               // Default: "/register"
config.poll_endpoint = "/poll";                       // Default: "/poll"
config.response_endpoint = "/response";               // Default: "/response"
config.poll_timeout_seconds = 30;                     // Default: 30
config.retry_delay_seconds = 5;                       // Default: 5
```

## Building

The reverse proxy components are included in the main CMake build:

```bash
cmake -B build
cmake --build build --config Release
```

This builds:
- `libmcp.a` (with reverse client support)
- `proxy_server` (the public proxy)
- `reverse_server_example` (example MCP server in reverse mode)
- `proxy_client_example` (example client connecting through proxy)

## Testing Locally

You can test the entire setup on localhost:

```bash
# Terminal 1: Start proxy
./build/examples/proxy_server 9000

# Terminal 2: Start reverse MCP server
./build/examples/reverse_server_example http://localhost:9000

# Terminal 3: Run client
./build/examples/proxy_client_example http://localhost:9000
```

Expected output in Terminal 3:
```
[INFO] === MCP Proxy Client Example ===
[INFO] Proxy URL: http://localhost:9000
[INFO]
1. Initializing connection...
[INFO] Initialization successful!
[INFO] Server: Reverse MCP Server v1.0.0
[INFO]
2. Listing available tools...
[INFO] Available tools:
[INFO]   - get_time: Get the current system time
[INFO]   - calculator: Perform basic arithmetic operations
[INFO]   - echo: Echo back the provided text
[INFO]
3. Calling 'get_time' tool...
[INFO] Result: Current time: Tue Nov 11 12:34:56 2025
[INFO]
4. Calling 'calculator' tool (add 15 + 27)...
[INFO] Result: Result: 42.000000
[INFO]
5. Calling 'echo' tool...
[INFO] Result: HELLO FROM REVERSE PROXY!
[INFO]
6. Sending ping...
[INFO] Ping successful!
[INFO]
=== All tests completed successfully! ===
```

## Production Deployment

### Proxy Server

Deploy the proxy server on a cloud instance with a public IP:

```bash
# Install dependencies
sudo apt-get install build-essential cmake

# Build
cmake -B build
cmake --build build --config Release

# Run with proper port
./build/examples/proxy_server 443  # Or use a reverse proxy like nginx
```

### Security Considerations

The current implementation is a **proof of concept**. For production:

1. **Add Authentication**: Require API keys for server registration
2. **Add SSL/TLS**: Use HTTPS for all communication
3. **Add Rate Limiting**: Prevent abuse
4. **Add Session Timeouts**: Clean up inactive sessions
5. **Add Request Validation**: Validate all JSON-RPC requests
6. **Add Monitoring**: Log all requests/responses for debugging

## Advantages vs. Alternatives

### vs. ngrok/Cloudflare Tunnel
- ✅ No third-party dependency
- ✅ Full control over proxy logic
- ✅ Can customize routing/load balancing
- ❌ Need to maintain proxy server

### vs. VPN
- ✅ Simpler setup
- ✅ Works through corporate firewalls
- ❌ Less secure (without additional hardening)

### vs. Regular MCP Server
- ✅ Works behind NAT/firewall
- ✅ No port forwarding needed
- ❌ Slightly higher latency (polling overhead)
- ❌ Additional complexity

## Troubleshooting

### "Failed to register with proxy server"
- Check that proxy_url is correct
- Verify proxy server is running
- Check network connectivity: `curl http://proxy.example.com:9000/register`

### "No MCP servers available"
- Ensure reverse server has successfully registered
- Check proxy server logs for registration confirmation
- Verify session hasn't timed out

### "Request timeout"
- Increase `poll_timeout_seconds` in reverse client config
- Check that MCP server is processing requests
- Verify network stability

## License

Same as the main MCP library (MIT License).
