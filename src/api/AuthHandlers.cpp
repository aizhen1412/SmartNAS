#include "smartnas/api/Router.h"
#include "RouterInternal.h"
#include "smartnas/core/FileManager.h"
#include "smartnas/utils/HashUtil.h"
#include "smartnas/db/DatabaseManager.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFTaskFactory.h"
#include <jwt-cpp/jwt.h>
#include <chrono>
#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <map>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <memory>
#include <regex>

namespace
{
    std::string json_string_field(const std::string &body, const std::string &key)
    {
        const std::regex pattern("\"" + key + "\"\\s*:\\s*\"((?:\\\\.|[^\"\\\\])*)\"");
        std::smatch match;
        if (!std::regex_search(body, match, pattern))
            return "";
        std::string value = match[1].str();
        std::string out;
        out.reserve(value.size());
        for (size_t i = 0; i < value.size(); ++i)
        {
            if (value[i] == '\\' && i + 1 < value.size())
            {
                const char escaped = value[++i];
                if (escaped == 'n')
                    out.push_back('\n');
                else if (escaped == 'r')
                    out.push_back('\r');
                else if (escaped == 't')
                    out.push_back('\t');
                else
                    out.push_back(escaped);
            }
            else
            {
                out.push_back(value[i]);
            }
        }
        return out;
    }

    void fill_credentials_from_body(protocol::HttpRequest *req, std::string &user, std::string &pwd)
    {
        const void *body = nullptr;
        size_t size = 0;
        req->get_parsed_body(&body, &size);
        if (!body || size == 0)
            return;
        const std::string payload(static_cast<const char *>(body), size);
        if (user.empty())
            user = json_string_field(payload, "user");
        if (pwd.empty())
            pwd = json_string_field(payload, "password");
    }
}

namespace smartnas
{
    namespace api
    {
        using namespace detail;

        void Router::handle_register(WFHttpTask *task)
        {
            auto *req = task->get_req();
            auto *resp = task->get_resp();

            std::string user = "";
            std::string pwd = "";

            // --- 【直接利用扫描逻辑获取数据】 ---
            protocol::HttpHeaderCursor cursor(req);
            std::string h_name, h_value;
            while (cursor.next(h_name, h_value))
            {
                // 我们在扫描过程中，顺便把 User 和 Password 存下来
                // 使用 if 判断不区分大小写
                if (h_name == "User" || h_name == "user")
                {
                    user = h_value;
                }
                else if (h_name == "Password" || h_name == "password")
                {
                    pwd = h_value;
                }
            }
            fill_credentials_from_body(req, user, pwd);

            std::cout << "[Register Attempt] User: " << user << std::endl;

            // 判断逻辑
            if (user.empty() || pwd.empty())
            {
                resp->set_status_code("400");
                resp->append_output_body("Error: Missing User or Password in Header.");
                return;
            }

            // 调用数据库
            if (db::DatabaseManager::get_instance().register_user(user, pwd))
            {
                resp->append_output_body("Register success for: " + user);
            }
            else
            {
                resp->set_status_code("409");
                resp->append_output_body("Register failed: User exists or Database error.");
            }
        }

        void Router::handle_login(WFHttpTask *task)
        {
            auto *req = task->get_req();
            auto *resp = task->get_resp();

            // 统一添加响应头（虽然 process 里加了，这里显式加一下 Content-Type 保证网页显示）
            resp->add_header_pair("Content-Type", "text/plain; charset=utf-8");

            std::string user = "";
            std::string pwd = "";

            // --- 【升级为：一次遍历扫描逻辑】 ---
            protocol::HttpHeaderCursor cursor(req);
            std::string h_name, h_value;
            while (cursor.next(h_name, h_value))
            {
                // 建议：比较时直接对比大小写，或者做简单的转换
                if (h_name == "User" || h_name == "user")
                {
                    user = h_value;
                }
                else if (h_name == "Password" || h_name == "password")
                {
                    pwd = h_value;
                }
            }
            fill_credentials_from_body(req, user, pwd);

            std::cout << "[Login Attempt] User: " << user << std::endl;

            if (user.empty() || pwd.empty())
            {
                resp->set_status_code("400");
                resp->append_output_body("Error: Credentials required in Header.");
                return;
            }

            // 调用数据库鉴权
            if (db::DatabaseManager::get_instance().authenticate_user(user, pwd))
            {
                auto token = jwt::create()
                                 .set_issuer("smartnas")
                                 .set_type("JWS")
                                 .set_payload_claim("user", jwt::claim(std::string(user)))
                                 .set_expires_at(std::chrono::system_clock::now() + std::chrono::hours{24})
                                 .sign(jwt::algorithm::hs256{jwt_secret()});

                resp->set_status_code("200");
                std::string json_resp = "{\"status\": \"success\", \"token\": \"" + token + "\", \"message\": \"Login Success! Welcome, " + escape_json_string(user) + "\"}";
                resp->add_header_pair("Content-Type", "application/json; charset=utf-8");
                resp->append_output_body(json_resp.c_str(), json_resp.size());
            }
            else
            {
                // 401 代表鉴权失败
                resp->set_status_code("401");
                resp->append_output_body("Login Failed: Invalid username or password.");
            }
        }

    } // namespace api
} // namespace smartnas
