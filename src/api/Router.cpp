#include "smartnas/api/Router.h"
#include "smartnas/core/FileManager.h"
#include "smartnas/utils/HashUtil.h"
#include "smartnas/db/DatabaseManager.h"
#include <chrono>
#include <iostream>
#include <string>

namespace smartnas
{
    namespace api
    {

        void Router::process(WFHttpTask *server_task)
        {
            auto *req = server_task->get_req();
            auto *resp = server_task->get_resp();
            std::string uri = req->get_request_uri();
            std::string method = req->get_method();

            if (uri == "/ping" || uri == "/")
            {
                handle_ping(req, resp);
            }
            else if (uri == "/upload" && method == "POST")
            {
                handle_upload(server_task);
            }
            // 【修改点】使用 rfind 来检查 uri 是否以 "/download" 开头
            // rfind(str, 0) == 0 的意思是：在第 0 个位置找到了字符串 str
            else if (uri.rfind("/download", 0) == 0 && method == "GET")
            {
                handle_download(server_task);
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
        void Router::handle_upload(WFHttpTask *server_task)
        {
            auto *req = server_task->get_req();
            auto *resp = server_task->get_resp();

            const void *body;
            size_t size;
            req->get_parsed_body(&body, &size);

            // 1. 计算哈希
            std::string file_hash = smartnas::utils::HashUtil::sha256(body, size);

            // 2. 获取数据库单例
            auto &db = smartnas::db::DatabaseManager::get_instance();

            std::string status_msg;

            // 3. 核心逻辑：秒传判断
            if (db.exists(file_hash))
            {
                status_msg = "Hit! File exists (Seconds-Transfer success).";
                std::cout << "[API Router] 秒传触发: " << file_hash << std::endl;
            }
            else
            {
                // 哈希不存在，真实写磁盘
                std::string filename = file_hash + ".bin";
                if (smartnas::core::FileManager::save_file(filename, body, size))
                {
                    // 写盘成功后，存入数据库
                    smartnas::core::FileMetadata meta;
                    meta.filename = "client_uploaded_file"; // 暂时固定，以后从Header取
                    meta.file_hash = file_hash;
                    meta.file_size = size;
                    meta.storage_path = "../var/data/" + filename;
                    meta.upload_time = std::chrono::system_clock::now().time_since_epoch().count();

                    db.save_metadata(meta);
                    status_msg = "New file saved and indexed.";
                }
                else
                {
                    resp->set_status_code("500");
                    return;
                }
            }

            // 4. 返回 JSON 结果
            std::string res_json = "{\"status\":\"success\",\"message\":\"" + status_msg + "\",\"hash\":\"" + file_hash + "\"}";
            resp->add_header_pair("Content-Type", "application/json");
            resp->append_output_body(res_json.c_str(), res_json.size());
        }
        void Router::handle_download(WFHttpTask *server_task)
        {
            auto *req = server_task->get_req();
            auto *resp = server_task->get_resp();

            // 1. 获取 URL 里的参数。Workflow 的方式很简单：
            // 用户请求：/download?hash=abcdef...
            std::string uri = req->get_request_uri();
            size_t pos = uri.find("hash=");
            if (pos == std::string::npos)
            {
                resp->set_status_code("400");
                resp->append_output_body("Missing hash parameter");
                return;
            }
            std::string hash = uri.substr(pos + 5); // 截取 hash= 之后的内容

            // 2. 从“账本”里查查有没有这个文件
            smartnas::core::FileMetadata meta;
            if (!smartnas::db::DatabaseManager::get_instance().get_metadata(hash, meta))
            {
                resp->set_status_code("404");
                resp->append_output_body("File not found in database");
                return;
            }

            // 3. 从硬盘读取文件
            std::string file_content;
            // 我们之前存的时候是用的 hash + ".bin" 命名的
            if (smartnas::core::FileManager::load_file(hash + ".bin", file_content))
            {
                // 4. 将文件内容塞进 Response
                resp->append_output_body(file_content.c_str(), file_content.size());

                // 设置下载 Header，告诉浏览器这是一个文件，建议保存的文件名是原始名
                std::string header_val = "attachment; filename=\"" + meta.filename + "\"";
                resp->add_header_pair("Content-Disposition", header_val);
                resp->add_header_pair("Content-Type", "application/octet-stream");
            }
            else
            {
                resp->set_status_code("500");
                resp->append_output_body("Disk read error");
            }
        }

    } // namespace api
} // namespace smartnas
