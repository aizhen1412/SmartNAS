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

        void Router::handle_upload(WFHttpTask *server_task)
        {
            auto *req = server_task->get_req();
            auto *resp = server_task->get_resp();

            std::string username = get_authenticated_user(req);
            std::string encoded_filename = "unknown.bin"; // 默认名
            std::string directory = "/";

            // 1. 扫描 Header 获取文件名
            protocol::HttpHeaderCursor cursor(req);
            std::string h_name, h_value;
            while (cursor.next(h_name, h_value))
            {
                if (h_name == "File-Name" || h_name == "file-name")
                    encoded_filename = h_value;
                if (h_name == "Directory" || h_name == "directory")
                    directory = normalize_dir(smartnas::utils::HashUtil::url_decode(h_value));
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
                meta.storage_path = smartnas::config::AppConfig::get_instance().data_path(file_hash + ".bin").string();
                meta.upload_time = std::chrono::system_clock::now().time_since_epoch().count();
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
            std::string username = get_authenticated_user(req);
            if (username.empty())
            {
                resp->set_status_code("401");
                resp->append_output_body("{\"error\":\"Authentication required\"}");
                return;
            }

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

            auto query_value = [&uri](const std::string &name) -> std::string
            {
                const size_t pos = uri.find(name + "=");
                if (pos == std::string::npos)
                    return "";
                std::string value = uri.substr(pos + name.size() + 1);
                const size_t end = value.find('&');
                return end == std::string::npos ? value : value.substr(0, end);
            };

            int total_chunks = 0;
            long long file_size = -1;
            long long chunk_size = -1;
            try
            {
                total_chunks = std::stoi(total_str);
                const std::string file_size_str = query_value("size");
                const std::string chunk_size_str = query_value("chunkSize");
                if (!file_size_str.empty() && !chunk_size_str.empty())
                {
                    file_size = std::stoll(file_size_str);
                    chunk_size = std::stoll(chunk_size_str);
                }
            }
            catch (const std::exception &)
            {
                resp->set_status_code("400");
                resp->append_output_body("{\"error\":\"Invalid chunk count\"}");
                resp->add_header_pair("Content-Type", "application/json");
                return;
            }
            if (!is_sha256_hex(hash_str) || total_chunks < 0 || total_chunks > 1000000 ||
                file_size < -1 || chunk_size == 0 || chunk_size < -1)
            {
                resp->set_status_code("400");
                resp->append_output_body("{\"error\":\"Invalid hash or chunk count\"}");
                resp->add_header_pair("Content-Type", "application/json");
                return;
            }
            if (file_size >= 0 && chunk_size > 0)
            {
                const long long expected_chunks = file_size / chunk_size + (file_size % chunk_size != 0 ? 1 : 0);
                if (expected_chunks != total_chunks)
                {
                    resp->set_status_code("400");
                    resp->append_output_body("{\"error\":\"File size and chunk count do not match\"}");
                    resp->add_header_pair("Content-Type", "application/json");
                    return;
                }
            }
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
                    bool present = core::FileManager::chunk_exists(hash_str, i);
                    if (present && file_size >= 0 && chunk_size > 0)
                    {
                        const long long offset = static_cast<long long>(i) * chunk_size;
                        if (offset >= file_size)
                            present = false;
                        else
                        {
                            const size_t expected_chunk_size = static_cast<size_t>(std::min(chunk_size, file_size - offset));
                            present = core::FileManager::chunk_has_size(hash_str, i, expected_chunk_size);
                        }
                    }
                    if (!present)
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
            std::string username = get_authenticated_user(req);
            if (username.empty())
            {
                resp->set_status_code("401");
                resp->append_output_body("{\"error\":\"Authentication required\"}");
                return;
            }
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

            if (is_sha256_hex(hash) && !index_str.empty())
            {
                int chunk_index = -1;
                try
                {
                    chunk_index = std::stoi(index_str);
                }
                catch (const std::exception &)
                {
                    resp->set_status_code("400");
                    resp->append_output_body("{\"error\":\"Invalid chunk index\"}");
                    resp->add_header_pair("Content-Type", "application/json");
                    return;
                }

                if (chunk_index < 0 || !core::FileManager::save_chunk(hash, chunk_index, body, size))
                {
                    resp->set_status_code("500");
                    resp->append_output_body("{\"error\":\"Failed to save chunk; check var/data permissions and disk space\"}");
                    resp->add_header_pair("Content-Type", "application/json");
                    return;
                }
                resp->append_output_body("{\"status\":\"ok\"}");
                resp->add_header_pair("Content-Type", "application/json");
            }
            else
            {
                resp->set_status_code("400");
                resp->append_output_body("{\"error\":\"Missing file hash or chunk index\"}");
                resp->add_header_pair("Content-Type", "application/json");
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
            std::string h_name, h_value, hash, fname, total_chunks_str, size_str, directory = "/";
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
                if (h_name == "Directory")
                    directory = normalize_dir(utils::HashUtil::url_decode(h_value));
            }
            if (!is_sha256_hex(hash) || total_chunks_str.empty() || size_str.empty())
            {
                resp->set_status_code("400");
                resp->append_output_body("{\"error\":\"Missing merge headers\"}");
                resp->add_header_pair("Content-Type", "application/json");
                return;
            }

            int total_chunks = 0;
            long long expected_size = 0;
            try
            {
                total_chunks = std::stoi(total_chunks_str);
                expected_size = std::stoll(size_str);
            }
            catch (const std::exception &)
            {
                resp->set_status_code("400");
                resp->append_output_body("{\"error\":\"Invalid merge headers\"}");
                resp->add_header_pair("Content-Type", "application/json");
                return;
            }
            if (total_chunks < 0 || total_chunks > 1000000 || expected_size < 0)
            {
                resp->set_status_code("400");
                resp->append_output_body("{\"error\":\"Invalid chunk count or file size\"}");
                resp->add_header_pair("Content-Type", "application/json");
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
                std::string actual_hash;
                size_t actual_size = 0;
                if (!core::FileManager::merge_chunks(hash, total_chunks, final_filename, actual_hash, actual_size))
                {
                    resp->set_status_code("409");
                    resp->append_output_body("{\"error\":\"Failed to merge chunks; a chunk may be missing or storage is unavailable\"}");
                    resp->add_header_pair("Content-Type", "application/json");
                    return;
                }

                if (actual_size != static_cast<size_t>(expected_size))
                {
                    core::FileManager::delete_file(final_filename);
                    resp->set_status_code("400");
                    std::string error = "{\"error\":\"Merged file size mismatch\",\"expected\":" +
                                        std::to_string(expected_size) + ",\"actual\":" + std::to_string(actual_size) + "}";
                    resp->append_output_body(error.c_str(), error.size());
                    resp->add_header_pair("Content-Type", "application/json");
                    return;
                }

                if (actual_hash.empty() || actual_hash != hash)
                {
                    core::FileManager::delete_file(final_filename);
                    resp->set_status_code("400");
                    std::string error = "{\"error\":\"Hash verification failed\",\"expected\":\"" + hash +
                                        "\",\"actual\":\"" + actual_hash + "\"}";
                    resp->append_output_body(error.c_str(), error.size());
                    resp->add_header_pair("Content-Type", "application/json");
                    return;
                }

                core::FileManager::delete_chunks(hash, total_chunks);
                status_msg = "File merged and indexed.";
                should_save_metadata = true;
            }

            if (should_save_metadata)
            {
                core::FileMetadata meta;
                meta.filename = utils::HashUtil::url_decode(fname);
                meta.file_hash = hash;
                meta.file_size = expected_size;
                meta.storage_path = smartnas::config::AppConfig::get_instance().data_path(hash + ".bin").string();
                meta.upload_time = std::chrono::system_clock::now().time_since_epoch().count();
                meta.owner = username;
                meta.directory = directory;

                // 摘要由外部服务通过独立 API 写入，上传路径不依赖额外服务。
                meta.summary = "";
                db.save_file_metadata(meta);
            }

            std::string res_json = "{\"status\":\"success\",\"message\":\"" + status_msg + "\"}";
            resp->add_header_pair("Content-Type", "application/json");
            resp->append_output_body(res_json.c_str(), res_json.size());
        }

    } // namespace api
} // namespace smartnas
