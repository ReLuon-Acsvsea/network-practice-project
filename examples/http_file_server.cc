#include "netx/event_loop.h"
#include "netx/http_server.h"

#include <filesystem>
#include <fstream>
#include <iostream>

using namespace netx;

namespace {

std::string sanitize_filename(const std::string &name) {
  // 仅保留文件名部分，去掉路径分隔符，防止目录穿越
  std::string base = name;
  for (char &ch : base) {
    if (ch == '/' || ch == '\\')
      ch = '_';
  }
  // 避免 ".." 等特殊名字
  if (base == "." || base == ".." || base.empty()) {
    return std::string();
  }
  return base;
}

bool read_file_all(const std::filesystem::path &p, std::string *out) {
  std::ifstream ifs(p, std::ios::binary);
  if (!ifs)
    return false;
  ifs.seekg(0, std::ios::end);
  std::streamsize size = ifs.tellg();
  if (size < 0)
    size = 0;
  ifs.seekg(0, std::ios::beg);
  out->resize(static_cast<size_t>(size));
  ifs.read(out->data(), size);
  return static_cast<std::streamsize>(ifs.gcount()) == size;
}

bool write_file_all(const std::filesystem::path &p, const std::string &data) {
  std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
  if (!ofs)
    return false;
  ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(ofs);
}

const std::string *
find_header(const std::unordered_map<std::string, std::string> &h,
            const std::string &key) {
  auto it = h.find(key);
  if (it == h.end())
    return nullptr;
  return &it->second;
}

} // namespace

int main(int argc, char *argv[]) {
  try {
    int io_threads = 1;
    if (argc >= 2) {
      io_threads = std::atoi(argv[1]);
      if (io_threads < 0)
        io_threads = 0;
    }
    std::filesystem::path root = "/tmp/netx_files";
    if (argc >= 3) {
      root = argv[2];
    }
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
      std::cerr << "Failed to create dir: " << root << ", ec=" << ec.message()
                << std::endl;
      return EXIT_FAILURE;
    }

    EventLoop loop;
    InetAddress addr(8080);
    HttpServer server(&loop, addr, /*io_threads=*/io_threads,
                      /*reuse_port=*/true);

    server.GetStatic("/", "netx file server\n", "text/plain");

    // 文件上传：POST /upload, 头部 X-Filename 指定文件名，body 为文件内容
    server.Post("/upload", [root](const HttpRequest &req, HttpResponse *resp) {
      const auto &hdrs = req.headers();
      const std::string *hn = find_header(hdrs, "X-Filename");
      if (!hn) {
        resp->set_status(400, "Bad Request");
        resp->set_header("Content-Type", "text/plain");
        resp->set_body("missing X-Filename header\n");
        return;
      }
      std::string fname = sanitize_filename(*hn);
      if (fname.empty()) {
        resp->set_status(400, "Bad Request");
        resp->set_header("Content-Type", "text/plain");
        resp->set_body("invalid filename\n");
        return;
      }
      std::filesystem::path path = root / fname;
      if (!write_file_all(path, req.body())) {
        resp->set_status(500, "Internal Server Error");
        resp->set_header("Content-Type", "text/plain");
        resp->set_body("write failed\n");
        return;
      }
      resp->set_status(200, "OK");
      resp->set_header("Content-Type", "text/plain");
      resp->set_body("ok\n");
    });

    // 文件下载：GET /download, 头部 X-Filename 指定文件名
    server.Get("/download", [root](const HttpRequest &req, HttpResponse *resp) {
      const auto &hdrs = req.headers();
      const std::string *hn = find_header(hdrs, "X-Filename");
      if (!hn) {
        resp->set_status(400, "Bad Request");
        resp->set_header("Content-Type", "text/plain");
        resp->set_body("missing X-Filename header\n");
        return;
      }
      std::string fname = sanitize_filename(*hn);
      if (fname.empty()) {
        resp->set_status(400, "Bad Request");
        resp->set_header("Content-Type", "text/plain");
        resp->set_body("invalid filename\n");
        return;
      }
      std::filesystem::path path = root / fname;
      std::string data;
      if (!read_file_all(path, &data)) {
        resp->set_status(404, "Not Found");
        resp->set_header("Content-Type", "text/plain");
        resp->set_body("not found\n");
        return;
      }
      resp->set_status(200, "OK");
      resp->set_header("Content-Type", "application/octet-stream");
      resp->set_body(std::move(data));
    });

    server.Start();
    loop.Loop();
  } catch (const std::exception &ex) {
    std::cerr << "error: " << ex.what() << std::endl;
  }
}
