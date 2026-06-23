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

        void Router::handle_create_share(WFHttpTask *task)
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
            int hours = 24;
            std::string hours_str = get_query_value(req->get_request_uri(), "hours");
            if (!hours_str.empty())
                hours = std::max(1, std::min(720, std::stoi(hours_str)));
            if (hash.empty() || !db::DatabaseManager::get_instance().user_has_file(username, hash))
            {
                resp->set_status_code("404");
                resp->append_output_body("{\"error\":\"File not found\"}");
                return;
            }
            std::string token = random_token();
            long long expires = (std::chrono::system_clock::now() + std::chrono::hours(hours)).time_since_epoch().count();
            if (!db::DatabaseManager::get_instance().create_share(token, username, hash, expires))
            {
                resp->set_status_code("500");
                return;
            }
            std::string json = "{\"status\":\"success\",\"token\":\"" + token + "\",\"url\":\"/share/download?token=" + token + "\",\"expiresAt\":" + std::to_string(expires) + "}";
            resp->add_header_pair("Content-Type", "application/json");
            resp->append_output_body(json);
        }

        void Router::handle_share_download(WFHttpTask *task)
        {
            auto *req = task->get_req();
            auto *resp = task->get_resp();
            std::string token = get_query_value(req->get_request_uri(), "token");
            core::ShareMetadata share;
            if (token.empty() || !db::DatabaseManager::get_instance().get_share(token, share))
            {
                resp->set_status_code("404");
                resp->append_output_body("Share not found");
                return;
            }
            long long now = std::chrono::system_clock::now().time_since_epoch().count();
            if (share.expires_at < now)
            {
                resp->set_status_code("410");
                resp->append_output_body("Share expired");
                return;
            }
            core::FileMetadata meta;
            if (!db::DatabaseManager::get_instance().get_user_file_metadata(share.owner, share.file_hash, meta))
            {
                resp->set_status_code("404");
                return;
            }
            serve_file_with_range(task, share.file_hash, meta, true);
        }

    } // namespace api
} // namespace smartnas

