#include "RouterInternal.h"
#include "smartnas/utils/HashUtil.h"
#include "smartnas/config/AppConfig.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <openssl/rand.h>
#include <sstream>
#include <string>

namespace smartnas::api::detail
{
    const std::string &jwt_secret()
    {
        return smartnas::config::AppConfig::get_instance().jwt_secret();
    }
    bool is_sha256_hex(const std::string &value)
    {
        return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char ch)
        {
            return std::isdigit(ch) != 0 || (ch >= 'a' && ch <= 'f');
        });
    }

    std::string escape_json_string(const std::string &value)
    {
        std::string escaped;
        escaped.reserve(value.size() + 16);
        for (char ch : value)
        {
            switch (ch)
            {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(ch);
                break;
            }
        }
        return escaped;
    }

    bool is_json_string_array(const std::string &value)
    {
        size_t pos = 0;
        auto skip_space = [&]()
        {
            while (pos < value.size() && std::isspace(static_cast<unsigned char>(value[pos])))
                ++pos;
        };
        skip_space();
        if (pos >= value.size() || value[pos++] != '[')
            return false;
        skip_space();
        if (pos < value.size() && value[pos] == ']')
        {
            ++pos;
            skip_space();
            return pos == value.size();
        }

        while (pos < value.size())
        {
            if (value[pos++] != '"')
                return false;
            bool closed = false;
            while (pos < value.size())
            {
                unsigned char ch = static_cast<unsigned char>(value[pos++]);
                if (ch == '"')
                {
                    closed = true;
                    break;
                }
                if (ch < 0x20)
                    return false;
                if (ch != '\\')
                    continue;
                if (pos >= value.size())
                    return false;
                char escaped = value[pos++];
                if (escaped == 'u')
                {
                    if (pos + 4 > value.size())
                        return false;
                    for (int i = 0; i < 4; ++i)
                    {
                        if (!std::isxdigit(static_cast<unsigned char>(value[pos++])))
                            return false;
                    }
                }
                else if (std::string("\"\\/bfnrt").find(escaped) == std::string::npos)
                {
                    return false;
                }
            }
            if (!closed)
                return false;
            skip_space();
            if (pos < value.size() && value[pos] == ',')
            {
                ++pos;
                skip_space();
                continue;
            }
            if (pos < value.size() && value[pos] == ']')
            {
                ++pos;
                skip_space();
                return pos == value.size();
            }
            return false;
        }
        return false;
    }

    std::string get_query_value(const std::string &uri, const std::string &key)
    {
        std::string marker = key + "=";
        size_t pos = uri.find(marker);
        if (pos == std::string::npos)
            return "";
        std::string value = uri.substr(pos + marker.size());
        size_t amp = value.find("&");
        if (amp != std::string::npos)
            value = value.substr(0, amp);
        return smartnas::utils::HashUtil::url_decode(value);
    }

    std::string normalize_dir(std::string dir)
    {
        if (dir.empty())
            return "/";
        if (dir[0] != '/')
            dir = "/" + dir;
        while (dir.size() > 1 && dir.back() == '/')
            dir.pop_back();
        if (dir.find("..") != std::string::npos)
            return "/";
        return dir;
    }

    std::string random_token()
    {
        unsigned char bytes[24];
        if (RAND_bytes(bytes, sizeof(bytes)) != 1)
        {
            auto now = std::chrono::system_clock::now().time_since_epoch().count();
            std::string seed = std::to_string(now) + std::to_string(std::rand());
            return smartnas::utils::HashUtil::sha256(seed.c_str(), seed.size()).substr(0, 32);
        }
        static const char *hex = "0123456789abcdef";
        std::string token;
        token.reserve(sizeof(bytes) * 2);
        for (unsigned char byte : bytes)
        {
            token.push_back(hex[byte >> 4]);
            token.push_back(hex[byte & 0x0f]);
        }
        return token;
    }
}
