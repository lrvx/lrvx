#include "lrvx-connectors/hyperliquid/hl_signer.h"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#include <lrvx/log/log.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

using socket_t = SOCKET;
constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using socket_t = int;
constexpr socket_t INVALID_SOCK = -1;
#endif

namespace lrvx::hl
{

namespace
{

#ifdef _WIN32
struct WinsockInit
{
  WinsockInit()
  {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
  }
  ~WinsockInit() { WSACleanup(); }
};

static WinsockInit& ensureWinsock()
{
  static WinsockInit init;
  return init;
}

inline void close_socket(socket_t s) { ::closesocket(s); }
#else
inline void close_socket(socket_t s) { ::close(s); }
#endif

static bool send_all(socket_t fd, const void* p, size_t n)
{
  const char* c = static_cast<const char*>(p);
  while (n)
  {
#ifdef _WIN32
    int w = ::send(fd, c, static_cast<int>(n), 0);
#else
    ssize_t w = ::send(fd, c, n, MSG_NOSIGNAL);
#endif
    if (w <= 0)
    {
      return false;
    }
    c += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

static bool recv_all(socket_t fd, void* p, size_t n)
{
  char* c = static_cast<char*>(p);
  while (n)
  {
#ifdef _WIN32
    int r = ::recv(fd, c, static_cast<int>(n), 0);
#else
    ssize_t r = ::recv(fd, c, n, 0);
#endif
    if (r <= 0)
    {
      return false;
    }
    c += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

#ifndef _WIN32
// The signing request carries the raw private key, so the only transport this
// client will use is a Unix socket that no other user on the box can open:
// a filesystem path with an owner and a mode, both checkable before a byte is
// written. Anything else -- loopback TCP above all -- authenticates neither
// end, and whoever binds the port first harvests the key.
//
// The path is checked, not just connected to. lstat rather than stat so a
// symlink pointing somewhere world-writable is refused instead of followed.
// A window remains between the check and the connect; closing it for real
// needs the socket to live in a directory only this user can write, which is
// what the shipped daemon does.
static bool is_private_signer_socket(const char* path)
{
  struct stat st
  {
  };
  if (::lstat(path, &st) != 0)
  {
    return false;
  }
  if (!S_ISSOCK(st.st_mode))
  {
    LRVX_LOG_ERROR("[HL] signer path is not a socket: " << path);
    return false;
  }
  if (st.st_uid != ::geteuid())
  {
    LRVX_LOG_ERROR("[HL] signer socket is owned by another user, refusing: " << path);
    return false;
  }
  if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0)
  {
    LRVX_LOG_ERROR("[HL] signer socket is accessible beyond its owner (mode "
                   << (st.st_mode & 07777) << "), refusing: " << path);
    return false;
  }
  return true;
}

// hl_signerd answers one request at a time, so a sign can queue behind
// another client's request or behind the daemon's own slow first sign. The
// timeout is sized for that queue; the bound is for a daemon that is wedged,
// not for one that is busy.
constexpr int kSignerTimeoutMs = 2000;

static socket_t connect_unix(const char* path, int timeout_ms = kSignerTimeoutMs)
{
  socket_t fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
  {
    return INVALID_SOCK;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
  {
    close_socket(fd);
    return INVALID_SOCK;
  }
  return fd;
}
#endif

}  // namespace

static std::string escape_json(const std::string& s)
{
  std::string o;
  o.reserve(s.size() + 16);
  for (char c : s)
  {
    switch (c)
    {
      case '\"':
        o += "\\\"";
        break;
      case '\\':
        o += "\\\\";
        break;
      case '\b':
        o += "\\b";
        break;
      case '\f':
        o += "\\f";
        break;
      case '\n':
        o += "\\n";
        break;
      case '\r':
        o += "\\r";
        break;
      case '\t':
        o += "\\t";
        break;
      default:
        o += c;
        break;
    }
  }
  return o;
}

static std::string build_request_json(const HlSignParams& p)
{
  std::string j = "{";
  j += "\"action_json\":\"" + escape_json(p.actionJson) + "\"";
  j += ",\"nonce\":" + std::to_string(p.nonceMs);
  j += ",\"is_mainnet\":" + std::string(p.isMainnet ? "true" : "false");
  j += ",\"private_key\":\"" + escape_json(p.privateKeyHex) + "\"";
  j += ",\"active_pool\":";
  if (p.activePoolJson && !p.activePoolJson->empty())
  {
    j += *p.activePoolJson;
  }
  else
  {
    j += "null";
  }
  j += ",\"expires_after\":";
  if (p.expiresAfterMs)
  {
    j += std::to_string(*p.expiresAfterMs);
  }
  else
  {
    j += "null";
  }
  j += "}";
  return j;
}

// Where hl_signerd listens, overridable because /dev/shm is Linux-only.
static constexpr const char* HL_SIGNER_DEFAULT_SOCKET = "/dev/shm/hl_sign.sock";
static constexpr const char* HL_SIGNER_SOCKET_ENV = "LRVX_HL_SIGNER_SOCKET";

// A signature reply is {"r","s","v"} -- a couple of hundred bytes. The length
// header comes from the peer, so it is a request for an allocation, not a
// fact; anything past this ceiling is a protocol violation and is refused
// before a single byte is reserved.
static constexpr uint32_t HL_SIGNER_MAX_RESPONSE = 4096;

std::optional<HlSig> hl_sign_with_sdk(const HlSignParams& p)
{
#ifdef _WIN32
  // No Unix sockets, and loopback TCP is not an acceptable substitute for a
  // transport that carries a private key, so there is nothing to talk to.
  (void)p;
  (void)HL_SIGNER_DEFAULT_SOCKET;
  (void)HL_SIGNER_SOCKET_ENV;
  (void)HL_SIGNER_MAX_RESPONSE;
  LRVX_LOG_ERROR("[HL] no signer transport on this platform: hl_signerd needs a Unix socket");
  return std::nullopt;
#else
  const char* envPath = std::getenv(HL_SIGNER_SOCKET_ENV);
  const std::string path = (envPath && *envPath) ? envPath : HL_SIGNER_DEFAULT_SOCKET;

  if (!is_private_signer_socket(path.c_str()))
  {
    return std::nullopt;
  }

  // Built only once the transport is known to be trustworthy: no reason to
  // have the key in a buffer we might never be allowed to send.
  const std::string req = build_request_json(p);

  socket_t fd = connect_unix(path.c_str(), kSignerTimeoutMs);
  if (fd == INVALID_SOCK)
  {
    LRVX_LOG_ERROR("[HL] connect hl_signerd failed on " << path);
    return std::nullopt;
  }

  uint32_t len = htonl(static_cast<uint32_t>(req.size()));
  if (!send_all(fd, &len, 4) || !send_all(fd, req.data(), req.size()))
  {
    close_socket(fd);
    LRVX_LOG_ERROR("[HL] send req failed");
    return std::nullopt;
  }

  uint32_t rlen_be = 0;
  if (!recv_all(fd, &rlen_be, 4))
  {
    close_socket(fd);
    return std::nullopt;
  }
  const uint32_t rlen = ntohl(rlen_be);
  if (rlen == 0 || rlen > HL_SIGNER_MAX_RESPONSE)
  {
    close_socket(fd);
    LRVX_LOG_ERROR("[HL] signer declared a " << rlen << "-byte response, above the "
                                             << HL_SIGNER_MAX_RESPONSE << "-byte bound");
    return std::nullopt;
  }

  std::string out;
  out.resize(rlen);
  if (!recv_all(fd, out.data(), rlen))
  {
    close_socket(fd);
    return std::nullopt;
  }
  close_socket(fd);

  auto findv = [&](const char* key) -> std::string
  {
    std::string k = std::string("\"") + key + "\":";
    size_t pos = out.find(k);
    if (pos == std::string::npos)
    {
      return {};
    }
    pos += k.size();
    while (pos < out.size() && out[pos] == ' ')
    {
      ++pos;
    }
    if (pos < out.size() && out[pos] == '\"')
    {
      size_t e = out.find('\"', pos + 1);
      if (e == std::string::npos)
      {
        return {};
      }
      return out.substr(pos + 1, e - (pos + 1));
    }
    else
    {
      size_t e = out.find_first_of(",}\n\r ", pos);
      if (e == std::string::npos)
      {
        e = out.size();
      }
      return out.substr(pos, e - pos);
    }
  };

  std::string r = findv("r"), s = findv("s"), v = findv("v");
  if (r.empty() || s.empty() || v.empty())
  {
    LRVX_LOG_ERROR("[HL] signer bad json: " + out);
    return std::nullopt;
  }
  return HlSig{std::move(r), std::move(s), std::atoi(v.c_str())};
#endif
}

}  // namespace lrvx::hl
