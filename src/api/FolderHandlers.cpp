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

        void Router::handle_folders(WFHttpTask *task)
        {
            auto *req = task->get_req();
            auto *resp = task->get_resp();
            std::string username = get_authenticated_user(req);
            if (username.empty())
            {
                resp->set_status_code("401");
                return;
            }
            auto folders = db::DatabaseManager::get_instance().get_user_folders(username);
            std::string json = "[";
            for (size_t i = 0; i < folders.size(); ++i)
            {
                json += "{\"path\":\"" + escape_json_string(folders[i].path) + "\",\"createdTime\":" + std::to_string(folders[i].created_time) + "}";
                if (i + 1 < folders.size())
                    json += ",";
            }
            json += "]";
            resp->add_header_pair("Content-Type", "application/json");
            resp->append_output_body(json);
        }

        void Router::handle_create_folder(WFHttpTask *task)
        {
            auto *req = task->get_req();
            auto *resp = task->get_resp();
            std::string username = get_authenticated_user(req);
            if (username.empty())
            {
                resp->set_status_code("401");
                return;
            }
            std::string path = normalize_dir(get_query_value(req->get_request_uri(), "path"));
            if (path == "/")
            {
                resp->set_status_code("400");
                resp->append_output_body("{\"error\":\"Cannot create root\"}");
                return;
            }
            if (db::DatabaseManager::get_instance().create_folder(username, path))
                resp->append_output_body("{\"status\":\"success\"}");
            else
            {
                resp->set_status_code("500");
                resp->append_output_body("{\"error\":\"Failed to create folder\"}");
            }
        }

        void Router::handle_rename_folder(WFHttpTask *task)
        {
            auto *req = task->get_req();
            auto *resp = task->get_resp();
            const std::string username = get_authenticated_user(req);
            const std::string path = normalize_dir(get_query_value(req->get_request_uri(), "path"));
            const std::string name = get_query_value(req->get_request_uri(), "name");
            if (username.empty() || path == "/" || name.empty() || name.find('/') != std::string::npos || name == "." || name == "..")
            {
                resp->set_status_code(username.empty() ? "401" : "400");
                resp->append_output_body("{\"error\":\"Invalid folder or name\"}");
                return;
            }
            const size_t slash = path.find_last_of('/');
            const std::string parent = slash == 0 ? "" : path.substr(0, slash);
            const std::string new_path = parent + "/" + name;
            if (db::DatabaseManager::get_instance().move_folder(username, path, new_path))
                resp->append_output_body("{\"status\":\"success\"}");
            else
            {
                resp->set_status_code("409");
                resp->append_output_body("{\"error\":\"Folder rename failed or target already exists\"}");
            }
        }

        void Router::handle_move_folder(WFHttpTask *task)
        {
            auto *req = task->get_req();
            auto *resp = task->get_resp();
            const std::string username = get_authenticated_user(req);
            const std::string path = normalize_dir(get_query_value(req->get_request_uri(), "path"));
            const std::string directory = normalize_dir(get_query_value(req->get_request_uri(), "dir"));
            if (username.empty() || path == "/" || directory.rfind(path + "/", 0) == 0)
            {
                resp->set_status_code(username.empty() ? "401" : "400");
                resp->append_output_body("{\"error\":\"Invalid source or target folder\"}");
                return;
            }
            const std::string name = path.substr(path.find_last_of('/') + 1);
            const std::string new_path = directory == "/" ? "/" + name : directory + "/" + name;
            if (directory != "/")
                db::DatabaseManager::get_instance().create_folder(username, directory);
            if (db::DatabaseManager::get_instance().move_folder(username, path, new_path))
                resp->append_output_body("{\"status\":\"success\"}");
            else
            {
                resp->set_status_code("409");
                resp->append_output_body("{\"error\":\"Folder move failed or target already exists\"}");
            }
        }

        void Router::handle_delete_folder(WFHttpTask *task)
        {
            auto *req = task->get_req();
            auto *resp = task->get_resp();
            const std::string username = get_authenticated_user(req);
            const std::string path = normalize_dir(get_query_value(req->get_request_uri(), "path"));
            if (username.empty() || path == "/")
            {
                resp->set_status_code(username.empty() ? "401" : "400");
                resp->append_output_body("{\"error\":\"Invalid folder\"}");
                return;
            }
            if (db::DatabaseManager::get_instance().delete_folder(username, path))
                resp->append_output_body("{\"status\":\"success\"}");
            else
            {
                resp->set_status_code("409");
                resp->append_output_body("{\"error\":\"Folder is not empty or does not exist\"}");
            }
        }

    } // namespace api
} // namespace smartnas

