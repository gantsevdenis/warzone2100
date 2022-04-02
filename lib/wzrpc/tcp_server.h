#ifndef _tcp_server_h_
#define _tcp_server_h_
#include "3rdparty/json/json.hpp"

void tcp_start();
void tcp_send_data(const nlohmann::json& data);
void tcp_bind(const std::string&,  void (*)(const nlohmann::json &request));

#endif