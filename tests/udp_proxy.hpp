#pragma once
#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
using Socket = SOCKET;
inline void closeSocket(Socket s) {
    closesocket(s);
}
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
inline void closeSocket(Socket s) {
    close(s);
}
#endif

// Real bidirectional UDP impairment, not a simulation of the application protocol.
class UdpProxy {
    Socket socket_;
    std::atomic<bool> stop_{};
    std::thread worker_;

  public:
    UdpProxy(uint16_t port, uint16_t destination) : socket_(::socket(AF_INET, SOCK_DGRAM, 0)) {
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = htons(port);
        if (bind(socket_, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0) {
            closeSocket(socket_);
            throw std::runtime_error("Cannot bind impairment proxy");
        }
#ifdef _WIN32
        u_long enabled = 1;
        ioctlsocket(socket_, FIONBIO, &enabled);
#else
        fcntl(socket_, F_SETFL, fcntl(socket_, F_GETFL, 0) | O_NONBLOCK);
#endif
        worker_ = std::thread([this, destination, port] {
            using Clock = std::chrono::steady_clock;
            struct Pending {
                Clock::time_point when;
                sockaddr_in target;
                std::vector<char> data;
            };
            std::vector<Pending> queue;
            sockaddr_in server{};
            server.sin_family = AF_INET;
            server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            server.sin_port = htons(destination);
            sockaddr_in client{};
            bool known = false;
            std::mt19937 random(port);
            std::uniform_int_distribution<int> loss(0, 99), jitter(-12, 12);
            while (!stop_) {
                char data[65536];
                sockaddr_in from{};
#ifdef _WIN32
                int size = sizeof(from);
#else
                socklen_t size = sizeof(from);
#endif
                int n = int(recvfrom(socket_, data, sizeof(data), 0,
                                     reinterpret_cast<sockaddr *>(&from), &size));
                if (n > 0) {
                    bool reply = from.sin_port == server.sin_port;
                    if (!reply) {
                        client = from;
                        known = true;
                    }
                    if (known && loss(random) >= 2)
                        queue.push_back(
                            {Clock::now() + std::chrono::milliseconds(50 + jitter(random)),
                             reply ? client : server, std::vector<char>(data, data + n)});
                }
                auto now = Clock::now();
                for (auto it = queue.begin(); it != queue.end();)
                    if (it->when <= now) {
                        sendto(socket_, it->data.data(), int(it->data.size()), 0,
                               reinterpret_cast<sockaddr *>(&it->target), sizeof(it->target));
                        it = queue.erase(it);
                    } else
                        ++it;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    ~UdpProxy() {
        stop_ = true;
        worker_.join();
        closeSocket(socket_);
    }
    UdpProxy(const UdpProxy &) = delete;
};
