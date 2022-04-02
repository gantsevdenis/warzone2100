#ifndef _wz_udp_server_h_
#define _wz_udp_server_h_
#include "3rdparty/json/json.hpp"


void udp_start();
void udp_send_data(const nlohmann::json& data);

#endif