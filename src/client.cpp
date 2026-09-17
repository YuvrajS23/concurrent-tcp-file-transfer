#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr std::size_t kBufferSize = 64U * 1024U;
constexpr std::size_t kMaxResponseLine = 4096U;

struct Options {
    std::string operation;
    std::string host;
    std::string remote_name;
    std::filesystem::path local_path;
    std::uint16_t port = 0;
    std::uint32_t delay_ms = 0;
};

void print_usage(const char* program) {
    std::cerr << "Usage:\n"
              << "  " << program
              << " get --host <host> --port <1-65535> --remote <filename> --local <path> [--delay-ms <ms>]\n"
              << "  " << program
              << " put --host <host> --port <1-65535> --local <path> --remote <filename> [--delay-ms <ms>]\n";
}

bool parse_uint64(std::string_view text, std::uint64_t& value) {
    if (text.empty()) {
        return false;
    }

    value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    return true;
}

bool parse_port(std::string_view text, std::uint16_t& port) {
    std::uint64_t parsed = 0;
    if (!parse_uint64(text, parsed) || parsed == 0 ||
        parsed > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    port = static_cast<std::uint16_t>(parsed);
    return true;
}

bool is_safe_filename(std::string_view filename) {
    if (filename.empty() || filename.size() > 255U || filename == "." || filename == "..") {
        return false;
    }
    for (const char character : filename) {
        const bool allowed = (character >= 'a' && character <= 'z') ||
                             (character >= 'A' && character <= 'Z') ||
                             (character >= '0' && character <= '9') ||
                             character == '.' || character == '_' || character == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

std::optional<Options> parse_options(int argc, char* argv[]) {
    if (argc < 2) {
        return std::nullopt;
    }

    Options options;
    options.operation = argv[1];
    if (options.operation != "get" && options.operation != "put") {
        return std::nullopt;
    }

    bool saw_host = false;
    bool saw_port = false;
    bool saw_remote = false;
    bool saw_local = false;
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if ((argument != "--host" && argument != "--port" && argument != "--remote" &&
             argument != "--local" && argument != "--delay-ms") ||
            index + 1 >= argc) {
            return std::nullopt;
        }

        const std::string value = argv[++index];
        if (argument == "--host") {
            if (saw_host || value.empty()) {
                return std::nullopt;
            }
            options.host = value;
            saw_host = true;
        } else if (argument == "--port") {
            if (saw_port || !parse_port(value, options.port)) {
                return std::nullopt;
            }
            saw_port = true;
        } else if (argument == "--remote") {
            if (saw_remote || !is_safe_filename(value)) {
                return std::nullopt;
            }
            options.remote_name = value;
            saw_remote = true;
        } else if (argument == "--local") {
            if (saw_local || value.empty()) {
                return std::nullopt;
            }
            options.local_path = value;
            saw_local = true;
        } else {
            std::uint64_t delay = 0;
            if (!parse_uint64(value, delay) || delay > 60000U) {
                return std::nullopt;
            }
            options.delay_ms = static_cast<std::uint32_t>(delay);
        }
    }

    if (!saw_host || !saw_port || !saw_remote || !saw_local) {
        return std::nullopt;
    }
    return options;
}

std::optional<int> connect_to_server(const std::string& host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;

    addrinfo* results = nullptr;
    const std::string port_text = std::to_string(port);
    const int lookup_result = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &results);
    if (lookup_result != 0) {
        std::cerr << "Error: cannot resolve " << host << ": " << ::gai_strerror(lookup_result) << '\n';
        return std::nullopt;
    }

    int socket_fd = -1;
    for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
        const int fd = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (::connect(fd, candidate->ai_addr, candidate->ai_addrlen) == 0) {
            socket_fd = fd;
            break;
        }
        ::close(fd);
    }
    ::freeaddrinfo(results);

    if (socket_fd < 0) {
        std::cerr << "Error: cannot connect to " << host << ':' << port << ": " << std::strerror(errno)
                  << '\n';
        return std::nullopt;
    }
    return socket_fd;
}

bool send_all(int fd, const char* data, std::size_t length) {
    std::size_t offset = 0;
    while (offset < length) {
        const ssize_t sent = ::send(fd, data + offset, length - offset, 0);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        std::cerr << "Error: send failed: " << std::strerror(errno) << '\n';
        return false;
    }
    return true;
}

class BufferedSocketReader {
  public:
    explicit BufferedSocketReader(int fd) : fd_(fd) {}

    bool read_line(std::string& line) {
        while (true) {
            const std::size_t newline = buffered_.find('\n');
            if (newline != std::string::npos) {
                line = buffered_.substr(0, newline);
                buffered_.erase(0, newline + 1U);
                return true;
            }
            if (buffered_.size() > kMaxResponseLine) {
                std::cerr << "Error: response line is too long\n";
                return false;
            }

            std::array<char, kBufferSize> buffer{};
            const ssize_t received = ::recv(fd_, buffer.data(), buffer.size(), 0);
            if (received > 0) {
                buffered_.append(buffer.data(), static_cast<std::size_t>(received));
                continue;
            }
            if (received < 0 && errno == EINTR) {
                continue;
            }
            std::cerr << "Error: connection closed before a complete response was received\n";
            return false;
        }
    }

    bool copy_exactly(std::ofstream& destination, std::uint64_t bytes, std::uint32_t delay_ms) {
        std::uint64_t remaining = bytes;
        while (remaining > 0) {
            if (buffered_.empty()) {
                std::array<char, kBufferSize> buffer{};
                const ssize_t received = ::recv(fd_, buffer.data(), buffer.size(), 0);
                if (received > 0) {
                    buffered_.append(buffer.data(), static_cast<std::size_t>(received));
                } else if (received < 0 && errno == EINTR) {
                    continue;
                } else {
                    std::cerr << "Error: connection closed before the transfer completed\n";
                    return false;
                }
            }

            const std::size_t count = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buffered_.size())));
            destination.write(buffered_.data(), static_cast<std::streamsize>(count));
            if (!destination) {
                std::cerr << "Error: failed to write local file\n";
                return false;
            }
            buffered_.erase(0, count);
            remaining -= count;
            if (delay_ms != 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }
        }
        return true;
    }

  private:
    int fd_;
    std::string buffered_;
};

bool response_is_error(const std::string& response) {
    if (response.rfind("ERR ", 0) == 0) {
        std::cerr << "Server error: " << response.substr(4) << '\n';
        return true;
    }
    return false;
}

bool parse_ok_size(const std::string& response, std::uint64_t& size) {
    constexpr std::string_view prefix = "OK ";
    if (response.rfind(prefix, 0) != 0 || !parse_uint64(response.substr(prefix.size()), size)) {
        std::cerr << "Error: invalid server response: " << response << '\n';
        return false;
    }
    return true;
}

bool run_get(int fd, const Options& options) {
    const std::string request = "GET " + options.remote_name + "\n";
    if (!send_all(fd, request.data(), request.size())) {
        return false;
    }

    BufferedSocketReader reader(fd);
    std::string response;
    if (!reader.read_line(response) || response_is_error(response)) {
        return false;
    }

    std::uint64_t expected_bytes = 0;
    if (!parse_ok_size(response, expected_bytes)) {
        return false;
    }

    std::error_code error;
    if (std::filesystem::exists(options.local_path, error) || error) {
        std::cerr << "Error: local destination already exists or cannot be inspected: "
                  << options.local_path.string() << '\n';
        return false;
    }
    const std::filesystem::path parent = options.local_path.parent_path();
    if (!parent.empty() && !std::filesystem::is_directory(parent, error)) {
        std::cerr << "Error: local destination directory does not exist: " << parent.string() << '\n';
        return false;
    }

    std::filesystem::path temporary_path = options.local_path;
    temporary_path += ".part";
    if (std::filesystem::exists(temporary_path, error) || error) {
        std::cerr << "Error: temporary destination already exists or cannot be inspected: "
                  << temporary_path.string() << '\n';
        return false;
    }

    std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "Error: cannot open " << temporary_path.string() << " for writing\n";
        return false;
    }

    const bool copied = reader.copy_exactly(output, expected_bytes, options.delay_ms);
    output.close();
    if (!copied || !output) {
        std::filesystem::remove(temporary_path, error);
        return false;
    }

    std::filesystem::rename(temporary_path, options.local_path, error);
    if (error) {
        std::cerr << "Error: cannot finalize local file: " << error.message() << '\n';
        std::filesystem::remove(temporary_path, error);
        return false;
    }

    std::cout << "Downloaded " << expected_bytes << " bytes to " << options.local_path.string() << '\n';
    return true;
}

bool run_put(int fd, const Options& options) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(options.local_path, error) || error) {
        std::cerr << "Error: local source is not a regular readable file: " << options.local_path.string() << '\n';
        return false;
    }
    const std::uint64_t size = std::filesystem::file_size(options.local_path, error);
    if (error) {
        std::cerr << "Error: cannot determine local file size: " << error.message() << '\n';
        return false;
    }

    std::ifstream input(options.local_path, std::ios::binary);
    if (!input) {
        std::cerr << "Error: cannot open " << options.local_path.string() << " for reading\n";
        return false;
    }

    const std::string request = "PUT " + options.remote_name + " " + std::to_string(size) + "\n";
    if (!send_all(fd, request.data(), request.size())) {
        return false;
    }

    BufferedSocketReader reader(fd);
    std::string response;
    if (!reader.read_line(response) || response_is_error(response)) {
        return false;
    }
    if (response != "READY") {
        std::cerr << "Error: invalid server response: " << response << '\n';
        return false;
    }

    std::array<char, kBufferSize> buffer{};
    std::uint64_t sent_bytes = 0;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count <= 0) {
            break;
        }
        if (!send_all(fd, buffer.data(), static_cast<std::size_t>(count))) {
            return false;
        }
        sent_bytes += static_cast<std::uint64_t>(count);
        if (options.delay_ms != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options.delay_ms));
        }
    }
    if (input.bad() || sent_bytes != size) {
        std::cerr << "Error: failed to read the complete local file\n";
        return false;
    }

    if (!reader.read_line(response) || response_is_error(response)) {
        return false;
    }
    std::uint64_t confirmed_bytes = 0;
    if (!parse_ok_size(response, confirmed_bytes) || confirmed_bytes != size) {
        std::cerr << "Error: server reported an unexpected byte count\n";
        return false;
    }

    std::cout << "Uploaded " << size << " bytes as " << options.remote_name << '\n';
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    const auto options = parse_options(argc, argv);
    if (!options) {
        print_usage(argv[0]);
        return 2;
    }

    std::signal(SIGPIPE, SIG_IGN);
    const auto socket_fd = connect_to_server(options->host, options->port);
    if (!socket_fd) {
        return 2;
    }

    const bool success = options->operation == "get" ? run_get(*socket_fd, *options)
                                                        : run_put(*socket_fd, *options);
    ::close(*socket_fd);
    return success ? 0 : 1;
}
