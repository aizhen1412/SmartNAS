#include "smartnas/api/Router.h"
#include "RouterInternal.h"
#include "smartnas/core/FileManager.h"
#include "smartnas/utils/HashUtil.h"
#include "smartnas/db/DatabaseManager.h"
#include "smartnas/config/AppConfig.h"
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

            std::string uri = req->get_request_uri();
            std::string directory = normalize_dir(get_query_value(uri, "dir"));
            bool deleted = get_query_value(uri, "deleted") == "1";

            // 3. 从数据库读取该用户的文件列表
            // 确保你的 DatabaseManager.cpp 里已经实现了 get_user_files
            auto files = deleted ? db::DatabaseManager::get_instance().get_deleted_files(username)
                                 : db::DatabaseManager::get_instance().get_user_files(username, directory);
            auto folders = db::DatabaseManager::get_instance().get_user_folders(username);

            // 4. 手动构造 JSON 字符串
            std::string json = "{\"directory\":\"" + escape_json_string(directory) + "\",\"folders\":[";
            bool first_folder = true;
            if (!deleted)
            {
                std::string prefix = directory == "/" ? "/" : directory + "/";
                for (const auto &folder : folders)
                {
                    if (folder.path == directory)
                        continue;
                    if (folder.path.rfind(prefix, 0) != 0)
                        continue;
                    std::string rest = folder.path.substr(prefix.size());
                    if (rest.empty() || rest.find("/") != std::string::npos)
                        continue;
                    if (!first_folder)
                        json += ",";
                    json += "{\"name\":\"" + escape_json_string(rest) + "\",\"path\":\"" + escape_json_string(folder.path) + "\",\"createdTime\":" + std::to_string(folder.created_time) + "}";
                    first_folder = false;
                }
            }
            json += "],\"files\":[";
            for (size_t i = 0; i < files.size(); ++i)
            {
                json += "{";
                json += "\"name\":\"" + escape_json_string(files[i].filename) + "\",";
                json += "\"hash\":\"" + escape_json_string(files[i].file_hash) + "\",";

                // 格式化大小
                double kb = files[i].file_size / 1024.0;
                char size_buf[32];
                snprintf(size_buf, sizeof(size_buf), "%.2f KB", kb);

                json += "\"size\":\"" + std::string(size_buf) + "\",";
                json += "\"rawSize\":" + std::to_string(files[i].file_size) + ",";
                json += "\"uploadTime\":" + std::to_string(files[i].upload_time) + ",";
                json += "\"summary\":\"" + escape_json_string(files[i].summary) + "\",";
                json += "\"tags\":" + (files[i].tags.empty() ? "[]" : files[i].tags) + ",";
                json += "\"directory\":\"" + escape_json_string(files[i].directory) + "\",";
                json += "\"deleted\":" + std::to_string(files[i].deleted) + ",";
                json += "\"deletedTime\":" + std::to_string(files[i].deleted_time);
                json += "}";
                if (i < files.size() - 1)
                    json += ",";
            }
            json += "]}";

            resp->append_output_body(json.c_str(), json.size());
        }

        void Router::handle_current_user(WFHttpTask *task)
        {
            auto *resp = task->get_resp();
            std::string username = get_authenticated_user(task->get_req());
            resp->add_header_pair("Content-Type", "application/json; charset=utf-8");
            if (username.empty())
            {
                resp->set_status_code("401");
                resp->append_output_body("{\"error\":\"Authentication required\"}");
                return;
            }
            std::string json = "{\"username\":\"" + escape_json_string(username) + "\"}";
            resp->append_output_body(json.c_str(), json.size());
        }

        void Router::handle_list_all_files(WFHttpTask *task)
        {
            auto *resp = task->get_resp();
            std::string username = get_authenticated_user(task->get_req());
            resp->add_header_pair("Content-Type", "application/json; charset=utf-8");
            if (username.empty())
            {
                resp->set_status_code("401");
                resp->append_output_body("{\"error\":\"Authentication required\"}");
                return;
            }

            auto files = db::DatabaseManager::get_instance().get_all_user_files(username, false);
            std::string json = "{\"files\":[";
            for (size_t i = 0; i < files.size(); ++i)
            {
                if (i)
                    json += ",";
                json += "{\"name\":\"" + escape_json_string(files[i].filename) + "\",";
                json += "\"hash\":\"" + escape_json_string(files[i].file_hash) + "\",";
                json += "\"summary\":\"" + escape_json_string(files[i].summary) + "\",";
                json += "\"tags\":" + (files[i].tags.empty() ? "[]" : files[i].tags) + ",";
                json += "\"directory\":\"" + escape_json_string(files[i].directory) + "\"}";
            }
            json += "]}";
            resp->append_output_body(json.c_str(), json.size());
        }

        void Router::handle_download(WFHttpTask *server_task)
        {
            auto *req = server_task->get_req();
            auto *resp = server_task->get_resp();

            std::string current_user = get_authenticated_user(req);
            if (current_user.empty())
            {
                resp->set_status_code("401");
                resp->append_output_body("Authentication required");
                return;
            }

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
            if (!smartnas::db::DatabaseManager::get_instance().get_user_file_metadata(current_user, hash, meta))
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
            if (!smartnas::db::DatabaseManager::get_instance().get_user_file_metadata(current_user, hash, meta))
            {
                resp->set_status_code("404");
                resp->append_output_body("Error: File not found in database");
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

            std::string real_path = smartnas::config::AppConfig::get_instance().data_path(filename).string();
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

    } // namespace api
} // namespace smartnas
