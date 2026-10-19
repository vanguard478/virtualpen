// TCP transport: fallback for when accessory mode is unavailable. Run
// `adb reverse tcp:PORT tcp:PORT` so the app's connection to its own
// localhost lands here. Adds roughly a millisecond over AOA.
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "link.h"

namespace vp {

void runTcp(int port, const std::function<void()> &onConnect, const DataFn &onData,
            const std::function<void()> &onDisconnect) {
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
        onConnect();
        while (!g_stop) {
            pollfd cp{c, POLLIN, 0};
            if (poll(&cp, 1, 200) == 0) continue;
            ssize_t n = recv(c, buf, sizeof(buf), 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            onData(buf, size_t(n));
        }
        close(c);
        onDisconnect();
        fprintf(stderr, "tablet disconnected\n");
    }
    close(srv);
}

} // namespace vp
