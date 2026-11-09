// SPDX-License-Identifier: BSD-3-Clause
//
// Small newline-JSON client for the x52d control socket, shared by the CLI
// and the GTK4 panel.
//
#ifndef X52_CLIENT_HPP
#define X52_CLIENT_HPP

#include <string>

#include <nlohmann/json.hpp>

namespace x52 {

std::string default_socket_path();
bool daemon_is_running();

class Client {
public:
    Client() = default;
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool connect(const std::string& path = default_socket_path(), int timeout_ms = 1500);
    void close();
    bool is_connected() const noexcept { return fd_ >= 0; }
    const std::string& error() const noexcept { return error_; }

    // Sends one request and waits for the matching reply.  Returns false and
    // fills `error` when the daemon is unreachable or reports a failure.
    bool request(const std::string& cmd, const nlohmann::json& args,
                 nlohmann::json& result, std::string& error);

    // Reads the next unsolicited line (events).  Used by `x52ctl watch`.
    bool next_line(std::string& line, std::string& error);

    bool request(const std::string& cmd, nlohmann::json& result, std::string& error) {
        return request(cmd, nlohmann::json::object(), result, error);
    }

private:
    bool read_line(std::string& line);
    int fd_ = -1;
    std::string buffer_;
    std::string error_;
    int next_id_ = 1;
};

}  // namespace x52

#endif  // X52_CLIENT_HPP
