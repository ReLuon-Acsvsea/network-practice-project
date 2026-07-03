#include "netx/socket.h"

#include "netx/inet_address.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdexcept>

namespace netx {

namespace {//匿名命名空间，链接器看不到它

int SetNonblockFlag(int fd,bool on){
    
}
}

}
