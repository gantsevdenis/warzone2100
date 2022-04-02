#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "udp_server.h"
#include "lib/framework/frame.h"
#include "3rdparty/json/json.hpp"

static int RPC_SERVER_FD;
static sockaddr_in* RPC_SERVER_CLIENT_SA = nullptr;

static void udp_send(const void *msg, int len);
void udp_start()
{
    int sock;
    sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    if (sock == -1)
    {
        perror("couldn't create udp server");
        return;
    }
    auto sa = new sockaddr_in;
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons(21000);
    int s = inet_pton(AF_INET, "127.0.0.1", &(sa->sin_addr));
    if (s < 0)
    {
        perror("int_pton");
        return;
    }
    RPC_SERVER_FD = sock;
    RPC_SERVER_CLIENT_SA = sa;
}

void udp_send_data(const nlohmann::json& data)
{
    const std::vector<std::uint8_t> out = nlohmann::json::to_msgpack(data);
    udp_send(out.data(), out.size());
}

static void udp_send(const void *msg, int len)
{
    ASSERT_OR_RETURN(, RPC_SERVER_CLIENT_SA != nullptr, "udp server not initialized");
    ASSERT_OR_RETURN(, RPC_SERVER_FD > 0, "udp socket not initialized");
    ssize_t nbsent = sendto(RPC_SERVER_FD, msg, len, 0, (sockaddr*)RPC_SERVER_CLIENT_SA, sizeof(*RPC_SERVER_CLIENT_SA));
    if (nbsent != len)
    {
        debug(LOG_ERROR, "not everything was sent! Do smth smart");
        perror("");
    }
}