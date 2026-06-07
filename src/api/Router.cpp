#include "smartnas/api/Router.h"
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

#include <mutex>
#include <thread>
#include <unordered_map>
#include <memory>

namespace
{
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
}
namespace smartnas
{
    namespace api
    {
        const std::string JWT_SECRET = "SMARTNAS_SECRET_KEY_2026_!@#";

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

            // 2. 如果 Header 里没有，尝试从 URI query parameter 里取 ?token=...
            if (token.empty())
            {
                std::string uri = req->get_request_uri();
                size_t token_pos = uri.find("token=");
                if (token_pos != std::string::npos)
                {
                    token = uri.substr(token_pos + 6);
                    size_t amp_pos = token.find("&");
                    if (amp_pos != std::string::npos)
                    {
                        token = token.substr(0, amp_pos);
                    }
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
                                    .allow_algorithm(jwt::algorithm::hs256{JWT_SECRET})
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

            // --- 【全局 CORS 处理开始】 ---
            // 无论什么请求，先给它加上这个头
            resp->add_header_pair("Access-Control-Allow-Origin", "*");
            resp->add_header_pair("Access-Control-Allow-Headers", "Authorization, Content-Type, User, Password, File-Name, File-Hash, Chunk-Index, Total-Chunks, File-Size");
            // 允许的请求方法
            resp->add_header_pair("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            // 如果是浏览器的预检请求，直接返回 200，不需要走后面的逻辑
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
            else if (uri.rfind("/download", 0) == 0 && method == "GET")
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
            else if (uri == "/api/list" && method == "GET")
            {
                handle_list_files(server_task);
            }
            else if (uri.rfind("/api/delete", 0) == 0 && method == "POST")
            {
                handle_delete(server_task);
            }
            else if (uri.rfind("/api/v1/files/search", 0) == 0 && method == "GET")
            {
                handle_search_files(server_task);
            }
            else if (uri == "/api/v1/files/summary" && method == "POST")
            {
                handle_update_file_summary(server_task);
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
                                 .sign(jwt::algorithm::hs256{JWT_SECRET});

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
        void Router::handle_not_found(protocol::HttpResponse *resp)
        {
            resp->set_status_code("404");
            resp->append_output_body("404 Not Found - SmartNAS");
        }
        void Router::handle_upload(WFHttpTask *server_task)
        {
            auto *req = server_task->get_req();
            auto *resp = server_task->get_resp();

            std::string username = get_authenticated_user(req);
            std::string encoded_filename = "unknown.bin"; // 默认名

            // 1. 扫描 Header 获取文件名
            protocol::HttpHeaderCursor cursor(req);
            std::string h_name, h_value;
            while (cursor.next(h_name, h_value))
            {
                if (h_name == "File-Name" || h_name == "file-name")
                    encoded_filename = h_value;
            }

            // 2. 解码文件名
            std::string real_filename = smartnas::utils::HashUtil::url_decode(encoded_filename);

            if (username.empty())
            {
                resp->set_status_code("401");
                resp->append_output_body("{\"error\":\"User identification required\"}");
                return;
            }

            const void *body;
            size_t size;
            req->get_parsed_body(&body, &size);
            std::string file_hash = smartnas::utils::HashUtil::sha256(body, size);
            auto &db = smartnas::db::DatabaseManager::get_instance();

            std::string status_msg;
            bool should_save_metadata = false;

            // 1. 判断是否重复上传（同一用户拥有同一文件）
            if (db.user_has_file(username, file_hash))
            {
                status_msg = "File already exists in your account.";
                should_save_metadata = false; // 用户已经有了，不需要再插入重复的元数据
            }
            // 2. 秒传判断：如果别人传过，服务器已经有了
            else if (db.exists(file_hash))
            {
                // 哈希存在，磁盘已经有这个文件了
                status_msg = "Hit! Seconds-Transfer success.";
                should_save_metadata = true; // 磁盘有了，但还得给当前用户记账
            }
            else
            {
                // 哈希不存在，真实写磁盘
                std::string filename = file_hash + ".bin";
                if (smartnas::core::FileManager::save_file(filename, body, size))
                {
                    status_msg = "New file saved and indexed.";
                    should_save_metadata = true;
                }
                else
                {
                    resp->set_status_code("500");
                    return;
                }
            }

            // 3. 【核心修改】为当前用户存入元数据
            if (should_save_metadata)
            {
                smartnas::core::FileMetadata meta;
                meta.filename = real_filename; // 【核心修改：使用真实文件名】
                meta.file_hash = file_hash;
                meta.file_size = size;
                // 建议：磁盘上的物理文件依然用 hash.bin 命名（为了秒传去重）
                // 但在数据库里记录它的“真名”
                meta.storage_path = "../../var/data/" + file_hash + ".bin";
                meta.upload_time = std::chrono::system_clock::now().time_since_epoch().count();
                meta.owner = username;

                // 摘要由外部服务通过独立 API 写入，上传路径不依赖额外服务。
                meta.summary = "";
                db.save_file_metadata(meta);
            }

            // 4. 返回响应
            std::string res_json = "{\"status\":\"success\",\"message\":\"" + status_msg + "\",\"hash\":\"" + file_hash + "\"}";
            resp->add_header_pair("Content-Type", "application/json");
            resp->add_header_pair("Access-Control-Allow-Origin", "*"); // 记得加跨域
            resp->append_output_body(res_json.c_str(), res_json.size());
        }

        void Router::handle_upload_init(WFHttpTask *task)
        {
            auto *req = task->get_req();
            auto *resp = task->get_resp();
            std::string uri = req->get_request_uri();

            size_t hash_pos = uri.find("hash=");
            size_t total_pos = uri.find("total=");
            if (hash_pos == std::string::npos || total_pos == std::string::npos)
            {
                resp->set_status_code("400");
                return;
            }

            std::string hash_str = uri.substr(hash_pos + 5);
            size_t amp_pos = hash_str.find("&");
            if (amp_pos != std::string::npos)
                hash_str = hash_str.substr(0, amp_pos);

            std::string total_str = uri.substr(total_pos + 6);
            amp_pos = total_str.find("&");
            if (amp_pos != std::string::npos)
                total_str = total_str.substr(0, amp_pos);

            int total_chunks = std::stoi(total_str);
            auto &db = db::DatabaseManager::get_instance();

            if (db.exists(hash_str))
            {
                resp->append_output_body("{\"status\":\"exists\"}");
            }
            else
            {
                std::string missing = "{\"status\":\"missing\",\"missing\":[";
                bool first = true;
                for (int i = 0; i < total_chunks; ++i)
                {
                    if (!core::FileManager::chunk_exists(hash_str, i))
                    {
                        if (!first)
                            missing += ",";
                        missing += std::to_string(i);
                        first = false;
                    }
                }
                missing += "]}";
                resp->append_output_body(missing.c_str(), missing.size());
            }
            resp->add_header_pair("Content-Type", "application/json");
        }

        void Router::handle_upload_chunk(WFHttpTask *task)
        {
            auto *req = task->get_req();
            auto *resp = task->get_resp();
            protocol::HttpHeaderCursor cursor(req);
            std::string h_name, h_value, hash, index_str;
            while (cursor.next(h_name, h_value))
            {
                if (h_name == "File-Hash")
                    hash = h_value;
                if (h_name == "Chunk-Index")
                    index_str = h_value;
            }
            const void *body;
            size_t size;
            req->get_parsed_body(&body, &size);

            if (!hash.empty() && !index_str.empty())
            {
                core::FileManager::save_chunk(hash, std::stoi(index_str), body, size);
                resp->append_output_body("{\"status\":\"ok\"}");
                resp->add_header_pair("Content-Type", "application/json");
            }
            else
            {
                resp->set_status_code("400");
            }
        }

        void Router::handle_upload_merge(WFHttpTask *task)
        {
            auto *req = task->get_req();
            auto *resp = task->get_resp();

            std::string username = get_authenticated_user(req);
            if (username.empty())
            {
                resp->set_status_code("401");
                return;
            }

            protocol::HttpHeaderCursor cursor(req);
            std::string h_name, h_value, hash, fname, total_chunks_str, size_str;
            while (cursor.next(h_name, h_value))
            {
                if (h_name == "File-Hash")
                    hash = h_value;
                if (h_name == "File-Name")
                    fname = h_value;
                if (h_name == "Total-Chunks")
                    total_chunks_str = h_value;
                if (h_name == "File-Size")
                    size_str = h_value;
            }
            if (hash.empty() || total_chunks_str.empty())
            {
                resp->set_status_code("400");
                return;
            }

            auto &db = db::DatabaseManager::get_instance();
            std::string status_msg;
            bool should_save_metadata = false;

            if (db.user_has_file(username, hash))
            {
                status_msg = "File already exists in your account.";
            }
            else if (db.exists(hash))
            {
                status_msg = "Hit! Seconds-Transfer success.";
                should_save_metadata = true;
            }
            else
            {
                std::string final_filename = hash + ".bin";
                if (core::FileManager::merge_chunks(hash, std::stoi(total_chunks_str), final_filename))
                {
                    status_msg = "File merged and indexed.";
                    should_save_metadata = true;
                }
                else
                {
                    resp->set_status_code("500");
                    return;
                }
            }

            if (should_save_metadata)
            {
                core::FileMetadata meta;
                meta.filename = utils::HashUtil::url_decode(fname);
                meta.file_hash = hash;
                meta.file_size = std::stoll(size_str);
                meta.storage_path = "../../var/data/" + hash + ".bin";
                meta.upload_time = std::chrono::system_clock::now().time_since_epoch().count();
                meta.owner = username;

                // 摘要由外部服务通过独立 API 写入，上传路径不依赖额外服务。
                meta.summary = "";
                db.save_file_metadata(meta);
            }

            std::string res_json = "{\"status\":\"success\",\"message\":\"" + status_msg + "\"}";
            resp->add_header_pair("Content-Type", "application/json");
            resp->append_output_body(res_json.c_str(), res_json.size());
        }

        void Router::handle_delete(WFHttpTask *server_task)
        {
            auto *req = server_task->get_req();
            auto *resp = server_task->get_resp();

            std::string username = get_authenticated_user(req);

            if (username.empty())
            {
                resp->set_status_code("401");
                resp->append_output_body("{\"error\":\"User identification required\"}");
                return;
            }

            std::string uri = req->get_request_uri();
            size_t pos = uri.find("hash=");
            if (pos == std::string::npos)
            {
                resp->set_status_code("400");
                resp->append_output_body("{\"error\":\"Missing hash parameter\"}");
                return;
            }
            std::string hash = uri.substr(pos + 5);

            auto &db = smartnas::db::DatabaseManager::get_instance();
            if (db.delete_file_metadata(hash, username))
            {
                // 检查是否还有其他用户引用此文件
                if (db.count_file_references(hash) == 0)
                {
                    smartnas::core::FileManager::delete_file(hash + ".bin");
                }
                resp->set_status_code("200");
                resp->append_output_body("{\"status\":\"success\"}");
            }
            else
            {
                resp->set_status_code("404");
                resp->append_output_body("{\"error\":\"File not found or no permission\"}");
            }
        }

        void Router::handle_home(WFHttpTask *task)
        {
            auto *resp = task->get_resp();

            // 1. 打开网页文件
            std::ifstream file("../web/index.html");
            if (file.is_open())
            {
                // 2. 将文件内容读取到内存缓冲区
                std::stringstream buffer;
                buffer << file.rdbuf();
                std::string content = buffer.str();

                // 3. 设置响应头：极其重要！告诉浏览器这是一个 HTML 网页
                resp->add_header_pair("Content-Type", "text/html; charset=utf-8");

                // 4. 发送网页数据
                resp->append_output_body(content.c_str(), content.size());
            }
            else
            {
                // 如果找不到网页文件，返回报错
                resp->set_status_code("404");
                resp->append_output_body("<html><body><h1>Error 404: UI Source Not Found</h1><p>Please check if web/index.html exists.</p></body></html>");
                resp->add_header_pair("Content-Type", "text/html");
            }
        }
        void Router::handle_list_files(WFHttpTask *task)
        {
            auto *req = task->get_req();
            auto *resp = task->get_resp();

            // 1. 设置响应头
            resp->add_header_pair("Content-Type", "application/json");
            resp->add_header_pair("Access-Control-Allow-Origin", "*");

            // 2. 获取当前用户
            std::string username = get_authenticated_user(req);

            // 如果没传用户，返回空数组或报错
            if (username.empty())
            {
                resp->append_output_body("[]", 2);
                return;
            }

            // 3. 从数据库读取该用户的文件列表
            // 确保你的 DatabaseManager.cpp 里已经实现了 get_user_files
            auto files = db::DatabaseManager::get_instance().get_user_files(username);

            // 4. 手动构造 JSON 字符串
            std::string json = "[";
            for (size_t i = 0; i < files.size(); ++i)
            {
                json += "{";
                json += "\"name\":\"" + files[i].filename + "\",";
                json += "\"hash\":\"" + files[i].file_hash + "\",";

                // 格式化大小
                double kb = files[i].file_size / 1024.0;
                char size_buf[32];
                snprintf(size_buf, sizeof(size_buf), "%.2f KB", kb);

                json += "\"size\":\"" + std::string(size_buf) + "\",";
                json += "\"rawSize\":" + std::to_string(files[i].file_size) + ",";
                json += "\"uploadTime\":" + std::to_string(files[i].upload_time);
                json += "}";
                if (i < files.size() - 1)
                    json += ",";
            }
            json += "]";

            resp->append_output_body(json.c_str(), json.size());
        }

        void Router::handle_download(WFHttpTask *server_task)
        {
            auto *req = server_task->get_req();
            auto *resp = server_task->get_resp();

            std::string uri = req->get_request_uri();
            size_t pos = uri.find("hash=");
            if (pos == std::string::npos)
            {
                resp->set_status_code("400");
                resp->append_output_body("Missing hash parameter");
                return;
            }
            std::string hash = uri.substr(pos + 5);

            size_t amp_pos = hash.find("&");
            if (amp_pos != std::string::npos)
            {
                hash = hash.substr(0, amp_pos);
            }

            smartnas::core::FileMetadata meta;
            if (!smartnas::db::DatabaseManager::get_instance().get_file_metadata(hash, meta))
            {
                resp->set_status_code("404");
                resp->append_output_body("File not found in database");
                return;
            }

            serve_file_with_range(server_task, hash, meta, true);
        }

        void Router::handle_preview(WFHttpTask *server_task)
        {
            auto *req = server_task->get_req();
            auto *resp = server_task->get_resp();

            // 1. 获取当前访问者（鉴权）
            std::string current_user = get_authenticated_user(req);

            // 2. 解析 Hash 参数 (建议封装成 get_query_params 工具函数)
            std::string uri = req->get_request_uri();
            std::string hash = "";
            size_t hash_pos = uri.find("hash=");
            if (hash_pos != std::string::npos)
            {
                hash = uri.substr(hash_pos + 5);
                size_t amp_pos = hash.find("&");
                if (amp_pos != std::string::npos)
                {
                    hash = hash.substr(0, amp_pos);
                }
            }

            if (hash.empty())
            {
                resp->set_status_code("400");
                resp->append_output_body("Error: Missing hash parameter");
                return;
            }

            // 3. 从数据库获取元数据
            smartnas::core::FileMetadata meta;
            if (!smartnas::db::DatabaseManager::get_instance().get_file_metadata(hash, meta))
            {
                resp->set_status_code("404");
                resp->append_output_body("Error: File not found in database");
                return;
            }

            // 4. 【核心安全修复】权限检查：只有主人才能预览
            // 如果你在开发演示阶段想允许匿名预览，可以暂时注释掉这段，但面试时一定要提到！
            if (!smartnas::db::DatabaseManager::get_instance().user_has_file(current_user, hash))
            {
                resp->set_status_code("403"); // Forbidden
                resp->append_output_body("Error: You do not have permission to view this file.");
                return;
            }

            // 5. 交给分块发送函数处理 (is_download = false)
            serve_file_with_range(server_task, hash, meta, false);
        }
        void Router::serve_file_with_range(WFHttpTask *task, const std::string &hash, const smartnas::core::FileMetadata &meta, bool is_download)
        {
            auto *req = task->get_req();
            auto *resp = task->get_resp();
            std::string filename = hash + ".bin";

            size_t total_size = smartnas::core::FileManager::get_file_size(filename);
            if (total_size <= 0)
            {
                resp->set_status_code("404");
                resp->append_output_body("File not found.");
                return;
            }

            size_t start = 0;
            size_t end = total_size - 1;
            bool client_requested_range = false;

            // 1. 稳健解析 Range 头
            protocol::HttpHeaderCursor cursor(req);
            std::string range_val;
            if (cursor.find("Range", range_val) || cursor.find("range", range_val))
            {
                if (range_val.find("bytes=") == 0)
                {
                    std::string r = range_val.substr(6);
                    size_t dash = r.find('-');
                    if (dash != std::string::npos)
                    {
                        try
                        {
                            std::string s_str = r.substr(0, dash);
                            std::string e_str = r.substr(dash + 1);
                            if (!s_str.empty())
                                start = std::stoull(s_str);
                            if (!e_str.empty())
                                end = std::stoull(e_str);
                            client_requested_range = true;
                        }
                        catch (...)
                        {
                            client_requested_range = false;
                        }
                    }
                }
            }

            // 范围合法性检查
            if (start > end || start >= total_size)
            {
                resp->set_status_code("416");
                resp->add_header_pair("Content-Range", "bytes */" + std::to_string(total_size));
                return;
            }
            if (end >= total_size)
                end = total_size - 1;

            // 2. 内存安全处理
            // 只有在浏览器支持断点续传（发了 Range 头）或者文件确实巨大时才截断
            size_t MAX_CHUNK = 8 * 1024 * 1024; // 建议 8MB，兼顾性能和内存
            size_t content_length = end - start + 1;

            // 只有当是分块请求，或者不是下载（比如视频预览）并且确实需要限制的时候，才截断
            // 注意：对于图片预览或正常下载，不要截断
            std::string ext = "";
            size_t dot_pos = meta.filename.rfind('.');
            if (dot_pos != std::string::npos)
                ext = meta.filename.substr(dot_pos + 1);
            for (auto &c : ext)
                c = tolower(c);
            bool is_video = (ext == "mp4" || ext == "webm" || ext == "avi");

            if (!is_download && is_video && content_length > MAX_CHUNK)
            {
                content_length = MAX_CHUNK;
                end = start + content_length - 1;
            }

            std::string real_path = "../../var/data/" + filename;
            auto buf = std::shared_ptr<char[]>(new char[content_length]);

            // --- 核心修复 A：准确设置 Content-Type ---
            std::string content_type = "application/octet-stream";
            if (!is_download)
            {
                if (ext == "jpg" || ext == "jpeg")
                    content_type = "image/jpeg";
                else if (ext == "png")
                    content_type = "image/png";
                else if (ext == "mp4")
                    content_type = "video/mp4";
                else if (ext == "pdf")
                    content_type = "application/pdf";
                else if (ext == "txt")
                    content_type = "text/plain; charset=utf-8";
            }
            resp->add_header_pair("Content-Type", content_type);

            // --- 核心修复 B：状态码与 Range 响应头 ---
            if (client_requested_range || content_length < total_size)
            {
                resp->set_status_code("206");
                std::string cr = "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(total_size);
                resp->add_header_pair("Content-Range", cr);
            }
            else
            {
                resp->set_status_code("200");
            }

            resp->add_header_pair("Content-Length", std::to_string(content_length));
            resp->add_header_pair("Accept-Ranges", "bytes");

            // --- 核心修复 C：Content-Disposition ---
            std::string encoded_name = "";
            for (unsigned char c : meta.filename)
            {
                if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                    encoded_name += c;
                else
                {
                    char c_buf[4];
                    snprintf(c_buf, sizeof(c_buf), "%%%02X", c);
                    encoded_name += c_buf;
                }
            }

            // 预览用 inline，下载用 attachment
            std::string disposition = (is_download ? "attachment" : "inline");
            disposition += "; filename=\"" + encoded_name + "\"";
            resp->add_header_pair("Content-Disposition", disposition);

            // --- 核心修复 D：Workflow 异步 I/O 与零拷贝缓冲区下发 ---
            auto *pread_task = WFTaskFactory::create_pread_task(real_path, buf.get(), content_length, start,
                                                                [task, buf](WFFileIOTask *io_task)
                                                                {
                                                                    long ret = io_task->get_retval();
                                                                    if (ret >= 0)
                                                                    {
                                                                        // zero-copy! 我们不动 buffer，直接让底层发这个地址的内容！
                                                                        task->get_resp()->append_output_body_nocopy(buf.get(), ret);
                                                                    }
                                                                    else
                                                                    {
                                                                        // 异步读取时发现文件损坏或不存在
                                                                        task->get_resp()->set_status_code("500");
                                                                        task->get_resp()->append_output_body("Server I/O Error.");
                                                                    }
                                                                });

            // 绑定生命周期：让 buf 活到 task 发送完毕彻底析构的那一刻
            task->set_callback([buf](WFHttpTask *)
                               {
                                   // Lambda 捕获着 buf 的 scoped pointer，在请求完成后自动被释放
                               });

            // 将磁盘读取任务挂载到当前任务列
            // Router::process 执行完毕并不会立即回复客户端，而是等此 Series 走完才会回复！
            series_of(task)->push_back(pread_task);
        }

        void Router::handle_search_files(WFHttpTask *task)
        {
            auto *req = task->get_req();
            auto *resp = task->get_resp();

            std::string username = get_authenticated_user(req);
            if (username.empty())
            {
                resp->set_status_code("401");
                resp->add_header_pair("Content-Type", "application/json; charset=utf-8");
                resp->append_output_body("{\"error\":\"Authentication required\"}");
                return;
            }

            std::string uri = req->get_request_uri();
            std::string keyword = "";
            size_t pos = uri.find("keyword=");
            if (pos != std::string::npos)
            {
                keyword = uri.substr(pos + 8);
                size_t amp_pos = keyword.find("&");
                if (amp_pos != std::string::npos)
                {
                    keyword = keyword.substr(0, amp_pos);
                }
            }

            keyword = smartnas::utils::HashUtil::url_decode(keyword);
            auto files = smartnas::db::DatabaseManager::get_instance().search_files_by_summary(username, keyword);

            std::string json_result = "[\n";
            for (size_t i = 0; i < files.size(); ++i)
            {
                json_result += "  {\n";
                json_result += "    \"filename\": \"" + escape_json_string(files[i].filename) + "\",\n";
                json_result += "    \"summary\": \"" + escape_json_string(files[i].summary) + "\"\n";
                json_result += "  }";
                if (i < files.size() - 1)
                    json_result += ",";
                json_result += "\n";
            }
            json_result += "]\n";

            resp->add_header_pair("Content-Type", "application/json; charset=utf-8");
            resp->append_output_body(json_result.c_str(), json_result.size());
        }

        void Router::handle_update_file_summary(WFHttpTask *task)
        {
            auto *req = task->get_req();
            auto *resp = task->get_resp();
            resp->add_header_pair("Content-Type", "application/json; charset=utf-8");

            std::string username = get_authenticated_user(req);
            if (username.empty())
            {
                resp->set_status_code("401");
                resp->append_output_body("{\"error\":\"Authentication required\"}");
                return;
            }

            std::string hash;
            protocol::HttpHeaderCursor cursor(req);
            std::string h_name, h_value;
            while (cursor.next(h_name, h_value))
            {
                if (h_name == "File-Hash" || h_name == "file-hash")
                {
                    hash = h_value;
                    break;
                }
            }

            if (hash.empty())
            {
                resp->set_status_code("400");
                resp->append_output_body("{\"error\":\"Missing File-Hash header\"}");
                return;
            }

            const void *body = nullptr;
            size_t size = 0;
            req->get_parsed_body(&body, &size);
            std::string summary;
            if (body != nullptr && size > 0)
            {
                summary.assign(static_cast<const char *>(body), size);
            }

            auto &db = smartnas::db::DatabaseManager::get_instance();
            if (!db.user_has_file(username, hash))
            {
                resp->set_status_code("404");
                resp->append_output_body("{\"error\":\"File not found or no permission\"}");
                return;
            }

            if (!db.update_file_summary(username, hash, summary))
            {
                resp->set_status_code("500");
                resp->append_output_body("{\"error\":\"Failed to update summary\"}");
                return;
            }

            resp->append_output_body("{\"status\":\"success\"}");
        }

    } // namespace api
} // namespace smartnas
