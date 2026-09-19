#pragma once
#include <charconv>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace gamebridge::rtc {
// A bounded, strict JSON subset shared by ICE, description and candidate
// schemas. There are deliberately no floats, booleans or implicit conversions:
// none occurs in the public ABI's accepted signaling schemas.
struct Json {
  enum Kind { Object, Array, String, Integer, Null } kind = Null;
  std::map<std::string, Json> object;
  std::vector<Json> array;
  std::string string;
  uint32_t integer{};
};

inline bool ValidUtf8(std::string_view s) {
  for (size_t i = 0; i < s.size();) {
    const auto c = uint8_t(s[i++]);
    if (c < 128)
      continue;
    unsigned count;
    uint32_t code;
    if (c >= 0xc2 && c <= 0xdf) {
      count = 1;
      code = c & 31;
    } else if (c >= 0xe0 && c <= 0xef) {
      count = 2;
      code = c & 15;
    } else if (c >= 0xf0 && c <= 0xf4) {
      count = 3;
      code = c & 7;
    } else
      return false;
    if (s.size() - i < count)
      return false;
    const auto minimum = count == 1 ? 128u : count == 2 ? 2048u : 65536u;
    while (count--) {
      const auto tail = uint8_t(s[i++]);
      if ((tail & 0xc0) != 0x80)
        return false;
      code = (code << 6) | (tail & 63);
    }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
      return false;
  }
  return true;
}

class JsonParser {
public:
  explicit JsonParser(std::string_view text) : text_(text) {}
  std::optional<Json> Parse() {
    if (text_.size() > 262144 || !ValidUtf8(text_))
      return {};
    auto value = Value(0);
    White();
    return value && offset_ == text_.size() ? value : std::nullopt;
  }

private:
  void White() {
    while (offset_ < text_.size() &&
           (text_[offset_] == ' ' || text_[offset_] == '\t' ||
            text_[offset_] == '\n' || text_[offset_] == '\r'))
      ++offset_;
  }
  bool Eat(char c) {
    White();
    if (offset_ < text_.size() && text_[offset_] == c) {
      ++offset_;
      return true;
    }
    return false;
  }
  std::optional<uint32_t> Hex() {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
      if (offset_ == text_.size())
        return {};
      const auto c = text_[offset_++];
      unsigned n;
      if (c >= '0' && c <= '9')
        n = c - '0';
      else if (c >= 'a' && c <= 'f')
        n = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
        n = c - 'A' + 10;
      else
        return {};
      value = value * 16 + n;
    }
    return value;
  }
  std::optional<std::string> String() {
    if (!Eat('"'))
      return {};
    std::string value;
    while (offset_ < text_.size()) {
      const auto c = uint8_t(text_[offset_++]);
      if (c == '"')
        return value;
      if (c < 32)
        return {};
      if (c != '\\') {
        value += char(c);
        continue;
      }
      if (offset_ == text_.size())
        return {};
      const auto e = text_[offset_++];
      switch (e) {
      case '"':
      case '\\':
      case '/':
        value += e;
        break;
      case 'b':
        value += '\b';
        break;
      case 'f':
        value += '\f';
        break;
      case 'n':
        value += '\n';
        break;
      case 'r':
        value += '\r';
        break;
      case 't':
        value += '\t';
        break;
      case 'u': {
        auto cp = Hex();
        if (!cp)
          return {};
        if (*cp >= 0xd800 && *cp <= 0xdbff) {
          if (offset_ + 2 > text_.size() || text_.substr(offset_, 2) != "\\u")
            return {};
          offset_ += 2;
          auto low = Hex();
          if (!low || *low < 0xdc00 || *low > 0xdfff)
            return {};
          *cp = 0x10000 + ((*cp - 0xd800) << 10) + *low - 0xdc00;
        } else if (*cp >= 0xdc00 && *cp <= 0xdfff)
          return {};
        if (*cp < 128)
          value += char(*cp);
        else if (*cp < 2048) {
          value += char(0xc0 | (*cp >> 6));
          value += char(0x80 | (*cp & 63));
        } else if (*cp < 65536) {
          value += char(0xe0 | (*cp >> 12));
          value += char(0x80 | ((*cp >> 6) & 63));
          value += char(0x80 | (*cp & 63));
        } else {
          value += char(0xf0 | (*cp >> 18));
          value += char(0x80 | ((*cp >> 12) & 63));
          value += char(0x80 | ((*cp >> 6) & 63));
          value += char(0x80 | (*cp & 63));
        }
        break;
      }
      default:
        return {};
      }
    }
    return {};
  }
  std::optional<Json> Value(unsigned depth) {
    White();
    if (depth > 8 || offset_ == text_.size())
      return {};
    Json value;
    if (text_[offset_] == '{') {
      ++offset_;
      value.kind = Json::Object;
      if (Eat('}'))
        return value;
      do {
        auto key = String();
        if (!key || !Eat(':') || value.object.contains(*key))
          return {};
        auto child = Value(depth + 1);
        if (!child)
          return {};
        value.object.emplace(std::move(*key), std::move(*child));
      } while (Eat(','));
      return Eat('}') ? std::optional(std::move(value)) : std::nullopt;
    }
    if (text_[offset_] == '[') {
      ++offset_;
      value.kind = Json::Array;
      if (Eat(']'))
        return value;
      do {
        auto child = Value(depth + 1);
        if (!child || value.array.size() >= 256)
          return {};
        value.array.push_back(std::move(*child));
      } while (Eat(','));
      return Eat(']') ? std::optional(std::move(value)) : std::nullopt;
    }
    if (text_[offset_] == '"') {
      auto s = String();
      if (!s)
        return {};
      value.kind = Json::String;
      value.string = std::move(*s);
      return value;
    }
    if (text_.substr(offset_, 4) == "null") {
      offset_ += 4;
      return value;
    }
    const auto start = offset_;
    while (offset_ < text_.size() && text_[offset_] >= '0' &&
           text_[offset_] <= '9')
      ++offset_;
    if (start == offset_ || (offset_ - start > 1 && text_[start] == '0'))
      return {};
    auto result = std::from_chars(text_.data() + start, text_.data() + offset_,
                                  value.integer);
    if (result.ec != std::errc())
      return {};
    value.kind = Json::Integer;
    return value;
  }
  std::string_view text_;
  size_t offset_{};
};
inline std::optional<Json> ParseJson(std::string_view text) {
  return JsonParser(text).Parse();
}
inline std::string JsonString(std::string_view s) {
  std::string out = "\"";
  constexpr char hex[] = "0123456789abcdef";
  for (uint8_t c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += char(c);
    } else if (c < 32) {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    } else
      out += char(c);
  }
  return out + '"';
}
struct IceServer {
  std::vector<std::string> urls;
  std::string username, credential;
};
inline bool ValidIceUrl(std::string_view uri, bool &turn) {
  auto colon = uri.find(':');
  if (colon == uri.npos)
    return false;
  const auto scheme = uri.substr(0, colon);
  turn = scheme == "turn" || scheme == "turns";
  if (!turn && scheme != "stun" && scheme != "stuns")
    return false;
  auto address = uri.substr(colon + 1);
  if (address.empty() ||
      address.find_first_of("/#@%\\ \t\r\n") != address.npos ||
      address.find('\0') != address.npos)
    return false;
  auto query = address.find('?');
  if (query != address.npos) {
    const auto transport = address.substr(query);
    if (!turn ||
        (transport != "?transport=udp" && transport != "?transport=tcp") ||
        (scheme == "turns" && transport != "?transport=tcp"))
      return false;
    address = address.substr(0, query);
  }
  std::string_view port;
  if (address.starts_with('[')) {
    auto end = address.find(']');
    if (end == address.npos || end < 3 ||
        address.substr(1, end - 1).find(':') == address.npos)
      return false;
    IN6_ADDR ipv6{};
    const auto host = std::string(address.substr(1, end - 1));
    if (InetPtonA(AF_INET6, host.c_str(), &ipv6) != 1)
      return false;
    for (auto c : address.substr(1, end - 1))
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F') || c == ':' || c == '.'))
        return false;
    if (end + 1 != address.size()) {
      if (address[end + 1] != ':')
        return false;
      port = address.substr(end + 2);
      if (port.empty())
        return false;
    }
  } else {
    auto separator = address.find(':');
    auto host = address.substr(0, separator);
    if (host.empty())
      return false;
    for (auto c : host)
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-'))
        return false;
    if (separator != address.npos) {
      port = address.substr(separator + 1);
      if (port.empty())
        return false;
    }
  }
  if (!port.empty()) {
    unsigned n;
    auto parsed = std::from_chars(port.data(), port.data() + port.size(), n);
    if (parsed.ec != std::errc() || parsed.ptr != port.data() + port.size() ||
        !n || n > 65535)
      return false;
  }
  return true;
}
inline std::optional<std::vector<IceServer>> ParseIce(std::string_view text) {
  auto json = ParseJson(text);
  if (!json || json->kind != Json::Object || json->object.size() > 1)
    return {};
  std::vector<IceServer> result;
  if (json->object.empty())
    return result;
  if (!json->object.contains("iceServers"))
    return {};
  const auto &servers = json->object.at("iceServers");
  if (servers.kind != Json::Array || servers.array.size() > 32)
    return {};
  std::set<std::string> unique;
  for (const auto &entry : servers.array) {
    if (entry.kind != Json::Object || !entry.object.contains("urls"))
      return {};
    IceServer server;
    for (const auto &[key, value] : entry.object) {
      if (key == "urls") {
        if (value.kind == Json::String)
          server.urls.push_back(value.string);
        else if (value.kind == Json::Array) {
          for (const auto &url : value.array) {
            if (url.kind != Json::String)
              return {};
            server.urls.push_back(url.string);
          }
        } else
          return {};
      } else if ((key == "username" || key == "credential") &&
                 value.kind == Json::String &&
                 value.string.find('\0') == value.string.npos) {
        (key == "username" ? server.username : server.credential) =
            value.string;
      } else
        return {};
    }
    if (server.urls.empty() || server.urls.size() > 32)
      return {};
    for (const auto &url : server.urls) {
      bool turn;
      if (!ValidIceUrl(url, turn) || !unique.insert(url).second ||
          (turn && (server.username.empty() || server.credential.empty())))
        return {};
    }
    result.push_back(std::move(server));
  }
  return result;
}
} // namespace gamebridge::rtc
