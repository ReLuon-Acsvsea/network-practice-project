#include "netx/inet_address.h"

#include <arpa/inet.h>
#include <cstring>

namespace netx {

InetAddress::InetAddress()
{
    std::memset(&addr_,0,sizeof(addr_));
    addr_.sin_family = AF_INET;
}

InetAddress::InetAddress(uint16_t port):InetAddress(){
    addr_.sin_addr.s_addr = htonl(INADDR_ANY);
    addr_.sin_port = htons(port);
}

InetAddress::InetAddress(std::string_view ip,uint16_t port):InetAddress(){
    ::inet_pton(AF_INET,std::string(ip).c_str(),&addr_.sin_addr);
    addr_.sin_port = htons(port);
}

const struct sockaddr* InetAddress::sockaddr() const{
    return reinterpret_cast<const struct sockaddr*>(&addr_);
}





}