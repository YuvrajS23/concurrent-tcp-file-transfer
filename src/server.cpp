#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kBufferSize = 64U * 1024U;
constexpr std::size_t kMaxRequestLine = 4096U;
constexpr std::size_t kMaxClients = 256U;
constexpr std::uint64_t kMaxUploadBytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr auto kIdleTimeout = std::chrono::seconds(60);

volatile std::sig_atomic_t g_stop = 0;
std::uint64_t g_upload_sequence = 0;

void handle_stop_signal(int) {
    g_stop = 1;
}

enum class ClientState {
    AwaitingRequest,
    SendingGetHeader,
    SendingFile,
    SendingReady,
    ReceivingFile,
    SendingResult,
    SendingError,
};

struct Client {
    int fd = -1;
    ClientState state = ClientState::AwaitingRequest;
    std::string request_buffer;
    std::string pending_output;
    std::size_t pending_offset = 0;
    std::array<char, kBufferSize> file_buffer{};
    std::size_t file_buffer_size = 0;
    std::size_t file_buffer_offset = 0;
    std::ifstream input_file;
    std::ofstream output_file;
    std::filesystem::path temporary_path;
    std::filesystem::path final_path;
    std::uint64_t expected_bytes = 0;
    std::uint64_t transferred_bytes = 0;
    std::chrono::steady_clock::time_point last_activity = std::chrono::steady_clock::now();
};

struct Request {
    enum class Operation { Get, Put } operation;
    std::string filename;
    std::uint64_t size = 0;
};

enum class FlushResult { Drained, WouldBlock, Failed };

void print_usage(const char* program) {
    std::cerr << "Usage: " << program
              << " --port <0-65535> --root <storage-directory> [--bind <address>]\n"
              << "\n"
              << "The service binds to 127.0.0.1 by default. Use --bind 0.0.0.0 only "
                 "on a trusted network.\n";
}

bool parse_port(std::string_view text, std::uint16_t& port) {
    if (text.empty()) {
        return false;
    }

    std::uint64_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
        if (value > std::numeric_limits<std::uint16_t>::max()) {
            return false;
        }
    }
    port = static_cast<std::uint16_t>(value);
    return true;
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

std::optional<Request> parse_request(const std::string& line, std::string& error) {
    std::istringstream stream(line);
    std::string operation;
    std::string filename;
    std::string size_text;
    std::string extra;

    if (!(stream >> operation >> filename)) {
        error = "bad-request";
        return std::nullopt;
    }

    if (!is_safe_filename(filename)) {
        error = "unsafe-filename";
        return std::nullopt;
    }

    if (operation == "GET") {
        if (stream >> extra) {
            error = "bad-request";
            return std::nullopt;
        }
        return Request{Request::Operation::Get, filename, 0};
    }

    if (operation != "PUT" || !(stream >> size_text) || (stream >> extra)) {
        error = "bad-request";
        return std::nullopt;
    }

    std::uint64_t size = 0;
    if (!parse_uint64(size_text, size) || size > kMaxUploadBytes) {
        error = "invalid-size";
        return std::nullopt;
    }

    return Request{Request::Operation::Put, filename, size};
}

bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::optional<int> create_listener(const std::string& bind_address, std::uint16_t port,
                                   std::uint16_t& actual_port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;

    addrinfo* results = nullptr;
    const std::string port_text = std::to_string(port);
    const int lookup_result = ::getaddrinfo(bind_address.c_str(), port_text.c_str(), &hints, &results);
    if (lookup_result != 0) {
        std::cerr << "Error: cannot resolve bind address '" << bind_address
                  << "': " << ::gai_strerror(lookup_result) << '\n';
        return std::nullopt;
    }

    int listener = -1;
    for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
        const int fd = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (fd < 0) {
            continue;
        }

        const int reuse = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        if (::bind(fd, candidate->ai_addr, candidate->ai_addrlen) == 0 &&
            ::listen(fd, SOMAXCONN) == 0 && set_nonblocking(fd)) {
            listener = fd;
            break;
        }
        ::close(fd);
    }
    ::freeaddrinfo(results);

    if (listener < 0) {
        std::cerr << "Error: could not bind/listen on " << bind_address << ':' << port << ": "
                  << std::strerror(errno) << '\n';
        return std::nullopt;
    }

    sockaddr_storage address{};
    socklen_t address_length = sizeof(address);
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_length) != 0) {
        std::cerr << "Error: getsockname failed: " << std::strerror(errno) << '\n';
        ::close(listener);
        return std::nullopt;
    }

    if (address.ss_family == AF_INET) {
        actual_port = ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port);
    } else if (address.ss_family == AF_INET6) {
        actual_port = ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
    } else {
        std::cerr << "Error: unsupported socket family\n";
        ::close(listener);
        return std::nullopt;
    }

    return listener;
}

void remove_temporary_file(Client& client) {
    if (!client.temporary_path.empty()) {
        std::error_code error;
        std::filesystem::remove(client.temporary_path, error);
        client.temporary_path.clear();
    }
}

void close_client(Client& client) {
    client.input_file.close();
    client.output_file.close();
    remove_temporary_file(client);
    if (client.fd >= 0) {
        ::close(client.fd);
        client.fd = -1;
    }
}

void queue_error(Client& client, const std::string& reason) {
    client.input_file.close();
    client.output_file.close();
    remove_temporary_file(client);
    client.pending_output = "ERR " + reason + "\n";
    client.pending_offset = 0;
    client.state = ClientState::SendingError;
}

void note_activity(Client& client) {
    client.last_activity = std::chrono::steady_clock::now();
}

FlushResult flush_pending_output(Client& client) {
    while (client.pending_offset < client.pending_output.size()) {
        const char* data = client.pending_output.data() + client.pending_offset;
        const std::size_t remaining = client.pending_output.size() - client.pending_offset;
        const ssize_t sent = ::send(client.fd, data, remaining, 0);
        if (sent > 0) {
            client.pending_offset += static_cast<std::size_t>(sent);
            note_activity(client);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return FlushResult::WouldBlock;
        }
        return FlushResult::Failed;
    }

    client.pending_output.clear();
    client.pending_offset = 0;
    return FlushResult::Drained;
}

bool complete_upload(Client& client) {
    client.output_file.close();
    if (!client.output_file) {
        queue_error(client, "storage-failure");
        return true;
    }

    std::error_code error;
    std::filesystem::rename(client.temporary_path, client.final_path, error);
    if (error) {
        queue_error(client, "storage-failure");
        return true;
    }

    client.temporary_path.clear();
    client.pending_output = "OK " + std::to_string(client.expected_bytes) + "\n";
    client.pending_offset = 0;
    client.state = ClientState::SendingResult;
    return true;
}

bool start_request(Client& client, const std::filesystem::path& storage_root, const Request& request) {
    const std::filesystem::path target = storage_root / request.filename;

    if (request.operation == Request::Operation::Get) {
        std::error_code metadata_error;
        if (!std::filesystem::exists(target, metadata_error) || metadata_error ||
            !std::filesystem::is_regular_file(target, metadata_error) || metadata_error ||
            std::filesystem::is_symlink(target, metadata_error)) {
            queue_error(client, "not-found");
            return true;
        }

        client.input_file.open(target, std::ios::binary | std::ios::ate);
        if (!client.input_file) {
            queue_error(client, "not-readable");
            return true;
        }
        const std::streampos end_position = client.input_file.tellg();
        if (end_position < 0) {
            queue_error(client, "not-readable");
            return true;
        }
        client.expected_bytes = static_cast<std::uint64_t>(end_position);
        client.transferred_bytes = 0;
        client.input_file.seekg(0, std::ios::beg);
        if (!client.input_file) {
            queue_error(client, "not-readable");
            return true;
        }

        client.pending_output = "OK " + std::to_string(client.expected_bytes) + "\n";
        client.pending_offset = 0;
        client.state = ClientState::SendingGetHeader;
        return true;
    }

    client.expected_bytes = request.size;
    client.transferred_bytes = 0;
    client.final_path = target;
    const std::uint64_t sequence = ++g_upload_sequence;
    client.temporary_path = storage_root /
                            (".upload-" + std::to_string(client.fd) + "-" +
                             std::to_string(sequence) + ".part");
    client.output_file.open(client.temporary_path, std::ios::binary | std::ios::trunc);
    if (!client.output_file) {
        queue_error(client, "storage-failure");
        return true;
    }

    client.pending_output = "READY\n";
    client.pending_offset = 0;
    client.state = ClientState::SendingReady;
    return true;
}

bool handle_request_readable(Client& client, const std::filesystem::path& storage_root) {
    std::array<char, kBufferSize> buffer{};
    while (true) {
        const ssize_t received = ::recv(client.fd, buffer.data(), buffer.size(), 0);
        if (received > 0) {
            note_activity(client);
            client.request_buffer.append(buffer.data(), static_cast<std::size_t>(received));
            const std::size_t newline = client.request_buffer.find('\n');
            if (newline == std::string::npos) {
                if (client.request_buffer.size() > kMaxRequestLine) {
                    queue_error(client, "request-too-long");
                }
                return true;
            }

            if (newline + 1U != client.request_buffer.size() || newline > kMaxRequestLine) {
                queue_error(client, "bad-request");
                return true;
            }

            std::string error;
            const auto request = parse_request(client.request_buffer.substr(0, newline), error);
            client.request_buffer.clear();
            if (!request) {
                queue_error(client, error);
                return true;
            }
            return start_request(client, storage_root, *request);
        }
        if (received == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        return false;
    }
}

bool handle_upload_readable(Client& client) {
    std::array<char, kBufferSize> buffer{};
    while (true) {
        const ssize_t received = ::recv(client.fd, buffer.data(), buffer.size(), 0);
        if (received > 0) {
            note_activity(client);
            const std::uint64_t remaining = client.expected_bytes - client.transferred_bytes;
            const std::size_t accepted = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(received)));
            client.output_file.write(buffer.data(), static_cast<std::streamsize>(accepted));
            if (!client.output_file) {
                queue_error(client, "storage-failure");
                return true;
            }
            client.transferred_bytes += accepted;

            if (accepted != static_cast<std::size_t>(received)) {
                queue_error(client, "protocol-error");
                return true;
            }
            if (client.transferred_bytes == client.expected_bytes) {
                return complete_upload(client);
            }
            return true;
        }
        if (received == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        return false;
    }
}

bool handle_file_writable(Client& client) {
    if (client.file_buffer_offset == client.file_buffer_size) {
        if (client.transferred_bytes == client.expected_bytes) {
            return false;
        }

        const std::uint64_t remaining = client.expected_bytes - client.transferred_bytes;
        const std::size_t wanted = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(client.file_buffer.size())));
        client.input_file.read(client.file_buffer.data(), static_cast<std::streamsize>(wanted));
        const std::streamsize read_count = client.input_file.gcount();
        if (read_count <= 0) {
            return false;
        }
        client.file_buffer_size = static_cast<std::size_t>(read_count);
        client.file_buffer_offset = 0;
    }

    const char* data = client.file_buffer.data() + client.file_buffer_offset;
    const std::size_t remaining = client.file_buffer_size - client.file_buffer_offset;
    const ssize_t sent = ::send(client.fd, data, remaining, 0);
    if (sent > 0) {
        const std::size_t count = static_cast<std::size_t>(sent);
        client.file_buffer_offset += count;
        client.transferred_bytes += count;
        note_activity(client);
        return true;
    }
    if (sent < 0 && errno == EINTR) {
        return true;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return true;
    }
    return false;
}

bool handle_writable(Client& client) {
    switch (client.state) {
        case ClientState::SendingGetHeader: {
            const FlushResult result = flush_pending_output(client);
            if (result == FlushResult::Failed) {
                return false;
            }
            if (result == FlushResult::Drained) {
                if (client.expected_bytes == 0) {
                    return false;
                }
                client.state = ClientState::SendingFile;
            }
            return true;
        }
        case ClientState::SendingReady: {
            const FlushResult result = flush_pending_output(client);
            if (result == FlushResult::Failed) {
                return false;
            }
            if (result == FlushResult::Drained) {
                if (client.expected_bytes == 0) {
                    return complete_upload(client);
                }
                client.state = ClientState::ReceivingFile;
            }
            return true;
        }
        case ClientState::SendingResult:
        case ClientState::SendingError:
            return flush_pending_output(client) != FlushResult::Failed && !client.pending_output.empty();
        case ClientState::SendingFile:
            return handle_file_writable(client);
        case ClientState::AwaitingRequest:
        case ClientState::ReceivingFile:
            return true;
    }
    return false;
}

short requested_events(const Client& client) {
    switch (client.state) {
        case ClientState::AwaitingRequest:
        case ClientState::ReceivingFile:
            return POLLIN;
        case ClientState::SendingGetHeader:
        case ClientState::SendingFile:
        case ClientState::SendingReady:
        case ClientState::SendingResult:
        case ClientState::SendingError:
            return POLLOUT;
    }
    return 0;
}

bool handle_client_event(Client& client, short revents, const std::filesystem::path& storage_root) {
    if (revents & (POLLERR | POLLNVAL)) {
        return false;
    }

    if ((revents & POLLIN) != 0) {
        const bool ok = client.state == ClientState::AwaitingRequest
                            ? handle_request_readable(client, storage_root)
                            : client.state == ClientState::ReceivingFile ? handle_upload_readable(client)
                                                                           : true;
        if (!ok) {
            return false;
        }
    }

    const bool client_needs_write = client.state != ClientState::AwaitingRequest &&
                                    client.state != ClientState::ReceivingFile;
    if (((revents & POLLOUT) != 0 || ((revents & POLLHUP) != 0 && client_needs_write)) &&
        !handle_writable(client)) {
        return false;
    }

    return (revents & POLLHUP) == 0 ||
           (client.state != ClientState::AwaitingRequest && client.state != ClientState::ReceivingFile);
}

void accept_connections(int listener, std::vector<Client>& clients) {
    while (true) {
        sockaddr_storage peer{};
        socklen_t peer_length = sizeof(peer);
        const int fd = ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_length);
        if (fd >= 0) {
            if (clients.size() >= kMaxClients) {
                ::close(fd);
                continue;
            }
            if (!set_nonblocking(fd)) {
                ::close(fd);
                continue;
            }
            Client client;
            client.fd = fd;
            clients.push_back(std::move(client));
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        std::cerr << "Warning: accept failed: " << std::strerror(errno) << '\n';
        return;
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string bind_address = "127.0.0.1";
    std::string root_argument;
    std::uint16_t requested_port = 0;
    bool saw_port = false;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if ((argument == "--port" || argument == "--root" || argument == "--bind") && index + 1 < argc) {
            const std::string value = argv[++index];
            if (argument == "--port") {
                if (!parse_port(value, requested_port)) {
                    std::cerr << "Error: --port must be an integer from 0 to 65535\n";
                    return 2;
                }
                saw_port = true;
            } else if (argument == "--root") {
                root_argument = value;
            } else {
                bind_address = value;
            }
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    if (!saw_port || root_argument.empty()) {
        print_usage(argv[0]);
        return 2;
    }

    std::error_code directory_error;
    std::filesystem::create_directories(root_argument, directory_error);
    if (directory_error || !std::filesystem::is_directory(root_argument, directory_error)) {
        std::cerr << "Error: cannot use storage directory '" << root_argument << "'\n";
        return 2;
    }
    const std::filesystem::path storage_root = std::filesystem::weakly_canonical(root_argument, directory_error);
    if (directory_error) {
        std::cerr << "Error: cannot resolve storage directory '" << root_argument << "'\n";
        return 2;
    }

    std::uint16_t actual_port = 0;
    const auto listener = create_listener(bind_address, requested_port, actual_port);
    if (!listener) {
        return 2;
    }

    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, handle_stop_signal);
    std::signal(SIGTERM, handle_stop_signal);

    std::cout << "Listening on " << bind_address << ':' << actual_port << " with storage root "
              << storage_root.string() << std::endl;

    std::vector<Client> clients;
    while (g_stop == 0) {
        const auto now = std::chrono::steady_clock::now();
        for (Client& client : clients) {
            if (now - client.last_activity > kIdleTimeout) {
                close_client(client);
            }
        }
        clients.erase(std::remove_if(clients.begin(), clients.end(), [](const Client& client) {
                          return client.fd < 0;
                      }),
                      clients.end());

        std::vector<pollfd> poll_fds;
        poll_fds.reserve(clients.size() + 1U);
        poll_fds.push_back(pollfd{*listener, POLLIN, 0});
        for (const Client& client : clients) {
            poll_fds.push_back(pollfd{client.fd, requested_events(client), 0});
        }

        const int result = ::poll(poll_fds.data(), static_cast<nfds_t>(poll_fds.size()), 1000);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "Error: poll failed: " << std::strerror(errno) << '\n';
            break;
        }
        if (result == 0) {
            continue;
        }

        const std::size_t existing_client_count = clients.size();
        for (std::size_t index = 0; index < existing_client_count; ++index) {
            if (poll_fds[index + 1U].revents == 0) {
                continue;
            }
            if (!handle_client_event(clients[index], poll_fds[index + 1U].revents, storage_root)) {
                close_client(clients[index]);
            }
        }

        clients.erase(std::remove_if(clients.begin(), clients.end(), [](const Client& client) {
                          return client.fd < 0;
                      }),
                      clients.end());

        if ((poll_fds[0].revents & POLLIN) != 0) {
            accept_connections(*listener, clients);
        }
    }

    for (Client& client : clients) {
        close_client(client);
    }
    ::close(*listener);
    return 0;
}
