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

        void Router::handle_delete(WFHttpTask *server_task)
        {
            auto *req = server_task->get_req();
            auto *resp = server_task->get_resp();

            std::string username = get_authenticated_user(req);

            if (username.empty())
            {
                send_json_error(resp, "User identification required", "401");
                return;
            }

            std::string hash = get_query_value(req->get_request_uri(), "hash");
            if (hash.empty())
            {
                send_json_error(resp, "Missing hash parameter", "400");
                return;
            }

            auto &db = smartnas::db::DatabaseManager::get_instance();
            if (db.soft_delete_file_metadata(hash, username))
            {
                send_json(resp, "{\"status\":\"success\"}");
            }
            else
            {
                send_json_error(resp, "File not found or no permission", "404");
            }
        }

        void Router::handle_restore(WFHttpTask *server_task)
        {
            auto *req = server_task->get_req();
            auto *resp = server_task->get_resp();
            std::string username = get_authenticated_user(req);
            if (username.empty())
            {
                send_json_error(resp, "User identification required", "401");
                return;
            }
            std::string hash = get_query_value(req->get_request_uri(), "hash");
            if (hash.empty())
            {
                send_json_error(resp, "Missing hash parameter", "400");
                return;
            }
            if (db::DatabaseManager::get_instance().restore_file_metadata(hash, username))
                send_json(resp, "{\"status\":\"success\"}");
            else
            {
                send_json_error(resp, "File not found", "404");
            }
        }

        void Router::handle_purge(WFHttpTask *server_task)
        {
            auto *req = server_task->get_req();
            auto *resp = server_task->get_resp();
            std::string username = get_authenticated_user(req);
            if (username.empty())
            {
                send_json_error(resp, "User identification required", "401");
                return;
            }
            std::string hash = get_query_value(req->get_request_uri(), "hash");
            if (hash.empty())
            {
                send_json_error(resp, "Missing hash parameter", "400");
                return;
            }
            auto &db = db::DatabaseManager::get_instance();
            if (db.delete_file_metadata(hash, username))
            {
                if (db.count_file_references(hash) == 0)
                    core::FileManager::delete_file(hash + ".bin");
                send_json(resp, "{\"status\":\"success\"}");
            }
            else
            {
                send_json_error(resp, "File not found", "404");
            }
        }

        void Router::handle_rename_file(WFHttpTask *task)
        {
            auto *req = task->get_req();
            auto *resp = task->get_resp();
            std::string username = get_authenticated_user(req);
            if (username.empty())
            {
                send_json_error(resp, "User identification required", "401");
                return;
            }
            std::string hash = get_query_value(req->get_request_uri(), "hash");
            std::string name = get_query_value(req->get_request_uri(), "name");
            if (hash.empty() || name.empty() || name.find("/") != std::string::npos)
            {
                send_json_error(resp, "Invalid hash or name", "400");
                return;
            }
            if (db::DatabaseManager::get_instance().rename_file(username, hash, name))
                send_json(resp, "{\"status\":\"success\"}");
            else
            {
                send_json_error(resp, "File not found", "404");
            }
        }

        void Router::handle_move_file(WFHttpTask *task)
        {
            auto *req = task->get_req();
            auto *resp = task->get_resp();
            std::string username = get_authenticated_user(req);
            if (username.empty())
            {
                send_json_error(resp, "User identification required", "401");
                return;
            }
            std::string hash = get_query_value(req->get_request_uri(), "hash");
            std::string directory = normalize_dir(get_query_value(req->get_request_uri(), "dir"));
            if (hash.empty())
            {
                send_json_error(resp, "Missing hash parameter", "400");
                return;
            }
            db::DatabaseManager::get_instance().create_folder(username, directory);
            if (db::DatabaseManager::get_instance().move_file(username, hash, directory))
                send_json(resp, "{\"status\":\"success\"}");
            else
            {
                send_json_error(resp, "File not found", "404");
            }
        }

        void Router::handle_stats(WFHttpTask *task)
        {
            auto *req = task->get_req();
            auto *resp = task->get_resp();
            std::string username = get_authenticated_user(req);
            if (username.empty())
            {
                resp->set_status_code("401");
                return;
            }
            auto files = db::DatabaseManager::get_instance().get_user_files(username, "/");
            auto deleted = db::DatabaseManager::get_instance().get_deleted_files(username);
            long long usage = db::DatabaseManager::get_instance().get_user_storage_usage(username);
            std::string json = "{\"usage\":" + std::to_string(usage) + ",\"rootFiles\":" + std::to_string(files.size()) + ",\"deletedFiles\":" + std::to_string(deleted.size()) + "}";
            resp->add_header_pair("Content-Type", "application/json");
            resp->append_output_body(json);
        }

    } // namespace api
} // namespace smartnas
