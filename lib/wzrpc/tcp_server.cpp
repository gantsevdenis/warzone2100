#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "tcp_server.h"

#include "lib/framework/frame.h"
#include "lib/framework/wzapp.h"
#include "3rdparty/json/json.hpp"
#include "wzrpc.h"
static int RPC_SERVER_FD;
static int RPC_CLIENT_FD;
static bool QUIT = false;
static bool IS_CONNECTED = false;
static int _accept_and_recv_thread(void *);
static void dispatch(const nlohmann::json &request);

using callback_t = void (*)(const nlohmann::json &request);
static std::unordered_map<std::string, callback_t> callbacks;
// TODO: add correct socket shutdow!
static void send_data(const void *data, const size_t len);
void tcp_start()
{
    const int port = 8080;
    int sockfd = socket(AF_INET,SOCK_STREAM, 0);
    struct sockaddr_in sa; // IPv4
 	memset(&sa,0,sizeof(sa));
    static int resuse = 1;
    sa.sin_family      = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port        = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &(sa.sin_addr));
    int res = bind(sockfd, (sockaddr*) &sa, sizeof(sa));
    if (res < 0) 
    {
        perror("couldn't bind TCP socket");
        exit(1);
    }
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&resuse, sizeof(resuse)) < 0)
    {
        perror("couldn't set SO_REUSEADDR");
        exit(1);
    }
    if (listen(sockfd, 1) < 0)
    {
        perror("couldnt start listen TCP socket");
        exit(1);
    }
    RPC_SERVER_FD = sockfd;   
    auto th = wzThreadCreate(_accept_and_recv_thread, nullptr);
    wzThreadStart(th);
    debug(LOG_INFO, "started rpc server");
}


void tcp_send_data(const nlohmann::json& data)
{
    std::vector<uint8_t> buff = nlohmann::json::to_msgpack(data);
    auto network_size = htonl(buff.size());
    // send len prefixed data..
    send_data(&network_size, sizeof(network_size));
    send_data(buff.data(), buff.size());
}

void tcp_bind(const std::string &name, callback_t fp)
{
    callbacks.insert(std::pair<std::string, callback_t>(name, fp));
}

static void send_data(const void *data, const size_t len)
{
    ASSERT_OR_RETURN(, RPC_CLIENT_FD != 0, "client disconnected?");
    ASSERT_OR_RETURN(, len < UINT32_MAX, "not sending this msg.. something is wrong");
    size_t nb = write(RPC_CLIENT_FD, data, len);
    if (nb == -1)
    {
        perror("problem writing data:");
    }
    if (nb != len)
    {
        debug(LOG_WARNING, "didnt write everything!! Fixme %lu!=%lu", len, nb);
    }
}

static int _accept_and_recv_thread(void * _)
{
    const int recvsize = 1024;
    uint8_t recvbuf[recvsize];
//    auto self = static_cast<TcpServer*>(data);
    struct sockaddr_storage client_addr;
    socklen_t addr_size;
    while (!QUIT)
    {
        debug(LOG_INFO, "waiting for peer to join on duplex channel...");
        addr_size = sizeof (client_addr);
        RPC_CLIENT_FD =  accept(RPC_SERVER_FD, (struct sockaddr *)&client_addr, &addr_size);
        IS_CONNECTED = true;
        debug(LOG_INFO, "peer joined");
        while(1)
        {
            size_t nb = recv(RPC_CLIENT_FD, recvbuf, recvsize, 0);
            if (nb == 0)
            {
                fprintf(stderr, "peer closed connection\n");
                if (errno)
                {
                    perror("Error:");
                }
                int res = close(RPC_CLIENT_FD);
                if (res == -1) 
                {
                    perror("Error closing client socket:");
                }
                IS_CONNECTED = false;
                break;
            }
            
            // strict: true
            // allow_exceptions: false
            auto j = nlohmann::json::from_msgpack(recvbuf, recvbuf+nb, true, false);
            // parsing error
            if (!j.is_discarded())
            {
                dispatch(j);
            } else
            {
                wzrpc::sendError(wzrpc::RET_STATUS::RET_JSON_UNPARSABLE);
                //nlohmann::json error = nlohmann::json::object({ {"error", (int) wzrpc::RET_STATUS::RET_UNPARSABLE} });
                //tcp_send_data(error);
            }
        
        }
    }
    return 0;
}

static void dispatch(const nlohmann::json &request)
{
    const auto call = request.at("call").get_ref<const std::string&>();
    debug(LOG_INFO, "recved call %s", call.c_str());
    auto it = callbacks.find(call);
    if ( it == callbacks.end())
    {
        // return some meaningful error msg
        debug(LOG_INFO, "function call '%s' is not bound", call.c_str());
        wzrpc::sendError(wzrpc::RET_STATUS::RET_ERR_CALL_NOT_FOUND, call);
        //nlohmann::json error = nlohmann::json::object({ {"error", (int) wzrpc::RET_STATUS::RET_ERR_CALL_NOT_FOUND} });
        //tcp_send_data(error);
        return;
    }
    try
    {
        (*it).second(request);
    } catch (nlohmann::json::type_error& e)
    {
        debug(LOG_ERROR, "json error: %s %i", e.what(), e.id);
        wzrpc::sendError(wzrpc::RET_STATUS::RET_JSON_TYPE_ERROR);
    } catch (nlohmann::json::other_error& e)
    {
        debug(LOG_ERROR, "json error: %s %i", e.what(), e.id);
        wzrpc::sendError(wzrpc::RET_STATUS::RET_JSON_OTHER_ERROR);
    }
}


