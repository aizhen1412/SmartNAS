#pragma once
#include <string>

namespace smartnas::api::detail
{
    const std::string &jwt_secret();

    bool is_sha256_hex(const std::string &value);
    std::string escape_json_string(const std::string &value);
    bool is_json_string_array(const std::string &value);
    std::string get_query_value(const std::string &uri, const std::string &key);
    std::string normalize_dir(std::string dir);
    std::string random_token();
}
