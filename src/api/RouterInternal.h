#pragma once
#include <string>

namespace protocol
{
    class HttpResponse;
}

namespace smartnas::api::detail
{
    const std::string &jwt_secret();

    bool is_sha256_hex(const std::string &value);
    std::string escape_json_string(const std::string &value);
    bool is_json_string_array(const std::string &value);
    std::string get_query_value(const std::string &uri, const std::string &key);
    void send_json(protocol::HttpResponse *response, const std::string &body, const std::string &status = "200");
    void send_json_error(protocol::HttpResponse *response, const std::string &message, const std::string &status);
    std::string normalize_dir(std::string dir);
    std::string random_token();
}
