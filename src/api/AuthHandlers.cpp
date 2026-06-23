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

            // 打印一下，看看我们抠出来的变量对不对
            std::cout << "[Final Check] User: " << user << ", Pwd: " << pwd << std::endl;

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

            // 调试打印：让你在服务器终端能看到登录尝试
            std::cout << "[Login Attempt] User: " << user << ", Pwd: " << pwd << std::endl;

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
                std::string json_resp = "{\"status\": \"success\", \"token\": \"" + token + "\", \"message\": \"Login Success! Welcome, " + user + "\"}";
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
