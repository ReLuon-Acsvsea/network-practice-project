#include "netx/http_server.h"
#include "netx/event_loop.h"

#include <iostream>

using namespace netx;
// wrk -t8 -c100 -d5s http://127.0.0.1:8080/
int main(int argc, char* argv[]) {
  try {
    int io_threads = std::atoi(argv[1]);
    if (io_threads < 0) {
        std::cerr << "Error: io_threads must be a non-negative integer." << std::endl;
        return EXIT_FAILURE;
    }
    EventLoop loop;
    InetAddress addr(8080);
    HttpServer server(&loop, addr, /*io_threads=*/io_threads, /*reuse_port=*/false);
    server.GetStatic("/", "hello from netx\n", "text/plain");
    server.Post("/echo", [](const HttpRequest& req, HttpResponse* resp) {
      std::cout << "/echo page" << std::endl;
      resp->set_status(200, "OK");
      resp->set_header("Content-Type", "text/plain");
      resp->set_body(req.body());
    });
    server.Start();
    loop.Loop();
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << std::endl;
  }
}


