// TCP transport: fallback for when accessory mode is unavailable. Run
// `adb reverse tcp:PORT tcp:PORT` so the app's connection to its own
// localhost lands here. Adds roughly a millisecond over AOA.
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "link.h"

namespace vp {

namespace {

class TcpDownlink : public Downlink {
public:
    explicit TcpDownlink(int fd) : fd_(fd) {}

    bool send(const uint8_t *data, size_t len, bool droppable) override {
        std::lock_guard<std::mutex> lock(m_);
        if (dead_) return false;
        if (droppable) {
            // Skip the frame rather than wait when the socket buffer is full.
            pollfd p{fd_, POLLOUT, 0};
            if (poll(&p, 1, 0) <= 0) return false;
        }
        while (len > 0) {
            ssize_t n = ::send(fd_, data, len, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) continue;
                dead_ = true;
                return false;
            }
            data += n;
            len -= size_t(n);
        }
        return true;
    }

private:
    std::mutex m_;
    int fd_;
    bool dead_ = false;
};

} // namespace

void runTcp(int port, const LinkEvents &ev) {
    int srv = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || listen(srv, 1) != 0) {
        fprintf(stderr, "cannot listen on 127.0.0.1:%d: %s\n", port, strerror(errno));
        close(srv);
        return;
    }
    fprintf(stderr, "listening on 127.0.0.1:%d (run: adb reverse tcp:%d tcp:%d)\n", port, port, port);

    uint8_t buf[4096];
    while (!g_stop) {
        pollfd pfd{srv, POLLIN, 0};
        if (poll(&pfd, 1, 200) <= 0) continue;
        int c = accept4(srv, nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0) continue;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        fprintf(stderr, "tablet connected over tcp\n");
        {
            TcpDownlink downlink(c);
            ev.up(&downlink);
            while (!g_stop) {
                pollfd cp{c, POLLIN, 0};
                if (poll(&cp, 1, 200) == 0) continue;
                ssize_t n = recv(c, buf, sizeof(buf), 0);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                ev.data(buf, size_t(n));
            }
            ev.down();
        }
        close(c);
        fprintf(stderr, "tablet disconnected\n");
    }
    close(srv);
}

} // namespace vp
