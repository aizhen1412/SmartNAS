#include "smartnas/api/Router.h"
#include "RouterInternal.h"
#include "workflow/HttpUtil.h"
#include <jwt-cpp/jwt.h>
#include <iostream>
#include <regex>
#include <string>

namespace
{
    bool is_allowed_origin(const std::string &origin)
    {
        static const std::regex allowed(
            R"(^https?://(localhost|127\.0\.0\.1|\[::1\]|10\.\d{1,3}\.\d{1,3}\.\d{1,3}|192\.168\.\d{1,3}\.\d{1,3}|172\.(1[6-9]|2\d|3[0-1])\.\d{1,3}\.\d{1,3})(:\d+)?$)");
        return std::regex_match(origin, allowed);
    }
}

namespace smartnas
{
    namespace api
    {
        using namespace detail;

        std::string Router::get_authenticated_user(protocol::HttpRequest *req)
        {
            std::string token = "";

            // 1. 优先尝试从 Header 中获取 Bearer Token
            protocol::HttpHeaderCursor cursor(req);
            std::string h_name, h_value;
            while (cursor.next(h_name, h_value))
            {
                if (h_name == "Authorization" || h_name == "authorization")
                {
                    if (h_value.find("Bearer ") == 0)
                    {
                        token = h_value.substr(7);
                    }
                    break;
                }
            }

            if (token.empty())
            {
                return ""; // 没有 Token
            }

            try
            {
                auto decoded = jwt::decode(token);
                auto verifier = jwt::verify()
                                    .allow_algorithm(jwt::algorithm::hs256{jwt_secret()})
                                    .with_issuer("smartnas");
                verifier.verify(decoded);

                return decoded.get_payload_claim("user").as_string();
            }
            catch (const std::exception &e)
            {
                std::cerr << "JWT Verification Failed: " << e.what() << std::endl;
                return ""; // Token 无效或过期
            }
        }

        void Router::process(WFHttpTask *server_task)
        {
            auto *req = server_task->get_req();
            auto *resp = server_task->get_resp();

            protocol::HttpHeaderCursor cors_cursor(req);
            std::string cors_name, cors_value, origin;
            while (cors_cursor.next(cors_name, cors_value))
            {
                if (cors_name == "Origin" || cors_name == "origin")
                {
                    origin = cors_value;
                    break;
                }
            }
            if (!origin.empty() && is_allowed_origin(origin))
            {
                resp->add_header_pair("Access-Control-Allow-Origin", origin);
                resp->add_header_pair("Vary", "Origin");
            }
            resp->add_header_pair("Access-Control-Allow-Headers", "Authorization, Content-Type, User, Password, File-Name, File-Hash, Chunk-Index, Total-Chunks, File-Size");
            resp->add_header_pair("Access-Control-Allow-Methods", "GET, POST, HEAD, OPTIONS");
            std::string method = req->get_method();
            if (method == "OPTIONS")
            {
                resp->set_status_code("200");
                return;
            }
            // --- 【全局 CORS 处理结束】 ---

            // 原有的路由分发逻辑保持不变
            std::string uri = req->get_request_uri();
            if (uri == "/" || uri == "/index.html")
            {
                handle_home(server_task);
            }
            else if (uri == "/api/config" && method == "GET")
            {
                handle_runtime_config(server_task);
            }
            else if (uri.rfind("/assets/", 0) == 0 && method == "GET")
            {
                handle_static_asset(server_task);
            }
            else if (uri == "/vendor/hash-wasm-sha256.umd.min.js" && method == "GET")
            {
                handle_hash_wasm_script(server_task);
            }
            else if (uri == "/ping")
            {
                handle_ping(req, resp);
            }
            else if (uri == "/upload" && method == "POST")
            {
                handle_upload(server_task);
            }
            else if (uri.rfind("/api/upload/init", 0) == 0 && method == "GET")
            {
                handle_upload_init(server_task);
            }
            else if (uri == "/api/upload/chunk" && method == "POST")
            {
                handle_upload_chunk(server_task);
            }
            else if (uri == "/api/upload/merge" && method == "POST")
            {
                handle_upload_merge(server_task);
            }
            else if (uri.rfind("/download", 0) == 0 && (method == "GET" || method == "HEAD"))
            {
                handle_download(server_task);
            }
            else if (uri.rfind("/api/preview", 0) == 0 && method == "GET")
            {
                handle_preview(server_task);
            }
            else if (uri == "/register" && method == "POST")
            {
                handle_register(server_task);
            }
            else if (uri == "/login" && method == "POST")
            {
                handle_login(server_task);
            }
            else if (uri.rfind("/api/list", 0) == 0 && method == "GET")
            {
                handle_list_files(server_task);
            }
            else if (uri == "/api/v1/files/all" && method == "GET")
            {
                handle_list_all_files(server_task);
            }
            else if (uri == "/api/v1/me" && method == "GET")
            {
                handle_current_user(server_task);
            }
            else if (uri.rfind("/api/delete", 0) == 0 && method == "POST")
            {
                handle_delete(server_task);
            }
            else if (uri.rfind("/api/restore", 0) == 0 && method == "POST")
            {
                handle_restore(server_task);
            }
            else if (uri.rfind("/api/purge", 0) == 0 && method == "POST")
            {
                handle_purge(server_task);
            }
            else if (uri.rfind("/api/rename", 0) == 0 && method == "POST")
            {
                handle_rename_file(server_task);
            }
            else if (uri.rfind("/api/move", 0) == 0 && method == "POST")
            {
                handle_move_file(server_task);
            }
            else if (uri.rfind("/api/folders/rename", 0) == 0 && method == "POST")
            {
                handle_rename_folder(server_task);
            }
            else if (uri.rfind("/api/folders/move", 0) == 0 && method == "POST")
            {
                handle_move_folder(server_task);
            }
            else if (uri.rfind("/api/folders/delete", 0) == 0 && method == "POST")
            {
                handle_delete_folder(server_task);
            }
            else if (uri.rfind("/api/folders", 0) == 0 && method == "GET")
            {
                handle_folders(server_task);
            }
            else if (uri.rfind("/api/folders", 0) == 0 && method == "POST")
            {
                handle_create_folder(server_task);
            }
            else if (uri.rfind("/api/stats", 0) == 0 && method == "GET")
            {
                handle_stats(server_task);
            }
            else if (uri.rfind("/api/share/create", 0) == 0 && method == "POST")
            {
                handle_create_share(server_task);
            }
            else if (uri.rfind("/share/download", 0) == 0 && method == "GET")
            {
                handle_share_download(server_task);
            }
            else if (uri.rfind("/api/v1/files/search", 0) == 0 && method == "GET")
            {
                handle_search_files(server_task);
            }
            else if (uri == "/api/v1/files/summary" && method == "POST")
            {
                handle_update_file_summary(server_task);
            }
            else if (uri == "/api/v1/files/tags" && method == "POST")
            {
                handle_update_file_tags(server_task);
            }
            else
            {
                handle_not_found(resp);
            }
        }

        void Router::handle_ping(protocol::HttpRequest *req, protocol::HttpResponse *resp)
        {
            std::string html = "{\"status\": \"success\", \"message\": \"SmartNAS API is running!\"}";
            resp->append_output_body(html.c_str(), html.size());
            // 这次我们返回 JSON 格式，这是现代后端接口的标准
            resp->add_header_pair("Content-Type", "application/json; charset=utf-8");
        }

        void Router::handle_not_found(protocol::HttpResponse *resp)
        {
            resp->set_status_code("404");
            resp->append_output_body("404 Not Found - SmartNAS");
        }

    } // namespace api
} // namespace smartnas
