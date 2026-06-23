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
            if (db.soft_delete_file_metadata(hash, username))
            {
                resp->set_status_code("200");
                resp->append_output_body("{\"status\":\"success\"}");
            }
            else
            {
                resp->set_status_code("404");
                resp->append_output_body("{\"error\":\"File not found or no permission\"}");
            }
        }

        void Router::handle_restore(WFHttpTask *server_task)
        {
            auto *req = server_task->get_req();
            auto *resp = server_task->get_resp();
            std::string username = get_authenticated_user(req);
            if (username.empty())
            {
                resp->set_status_code("401");
                return;
            }
            std::string hash = get_query_value(req->get_request_uri(), "hash");
            if (hash.empty())
            {
                resp->set_status_code("400");
                return;
            }
            if (db::DatabaseManager::get_instance().restore_file_metadata(hash, username))
                resp->append_output_body("{\"status\":\"success\"}");
            else
            {
                resp->set_status_code("404");
                resp->append_output_body("{\"error\":\"File not found\"}");
            }
        }

        void Router::handle_purge(WFHttpTask *server_task)
        {
            auto *req = server_task->get_req();
            auto *resp = server_task->get_resp();
            std::string username = get_authenticated_user(req);
            if (username.empty())
            {
                resp->set_status_code("401");
                return;
            }
            std::string hash = get_query_value(req->get_request_uri(), "hash");
            if (hash.empty())
            {
                resp->set_status_code("400");
                return;
            }
            auto &db = db::DatabaseManager::get_instance();
            if (db.delete_file_metadata(hash, username))
            {
                if (db.count_file_references(hash) == 0)
                    core::FileManager::delete_file(hash + ".bin");
                resp->append_output_body("{\"status\":\"success\"}");
            }
            else
            {
                resp->set_status_code("404");
                resp->append_output_body("{\"error\":\"File not found\"}");
            }
        }

        void Router::handle_rename_file(WFHttpTask *task)
        {
            auto *req = task->get_req();
            auto *resp = task->get_resp();
            std::string username = get_authenticated_user(req);
            if (username.empty())
            {
                resp->set_status_code("401");
                return;
            }
            std::string hash = get_query_value(req->get_request_uri(), "hash");
            std::string name = get_query_value(req->get_request_uri(), "name");
            if (hash.empty() || name.empty() || name.find("/") != std::string::npos)
            {
                resp->set_status_code("400");
                resp->append_output_body("{\"error\":\"Invalid hash or name\"}");
                return;
            }
            if (db::DatabaseManager::get_instance().rename_file(username, hash, name))
                resp->append_output_body("{\"status\":\"success\"}");
            else
            {
                resp->set_status_code("404");
                resp->append_output_body("{\"error\":\"File not found\"}");
            }
        }

        void Router::handle_move_file(WFHttpTask *task)
        {
            auto *req = task->get_req();
            auto *resp = task->get_resp();
            std::string username = get_authenticated_user(req);
            if (username.empty())
            {
                resp->set_status_code("401");
                return;
            }
            std::string hash = get_query_value(req->get_request_uri(), "hash");
            std::string directory = normalize_dir(get_query_value(req->get_request_uri(), "dir"));
            if (hash.empty())
            {
                resp->set_status_code("400");
                return;
            }
            db::DatabaseManager::get_instance().create_folder(username, directory);
            if (db::DatabaseManager::get_instance().move_file(username, hash, directory))
                resp->append_output_body("{\"status\":\"success\"}");
            else
            {
                resp->set_status_code("404");
                resp->append_output_body("{\"error\":\"File not found\"}");
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

