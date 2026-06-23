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
                json_result += "    \"summary\": \"" + escape_json_string(files[i].summary) + "\",\n";
                json_result += "    \"tags\": " + (files[i].tags.empty() ? "[]" : files[i].tags) + "\n";
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

        void Router::handle_update_file_tags(WFHttpTask *task)
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

            const void *body = nullptr;
            size_t size = 0;
            req->get_parsed_body(&body, &size);
            std::string tags = body && size ? std::string(static_cast<const char *>(body), size) : "[]";
            if (hash.empty() || tags.size() > 4096 || !is_json_string_array(tags))
            {
                resp->set_status_code("400");
                resp->append_output_body("{\"error\":\"Missing File-Hash header or invalid tags JSON\"}");
                return;
            }

            auto &db = smartnas::db::DatabaseManager::get_instance();
            if (!db.user_has_file(username, hash))
            {
                resp->set_status_code("404");
                resp->append_output_body("{\"error\":\"File not found or no permission\"}");
                return;
            }
            if (!db.update_file_tags(username, hash, tags))
            {
                resp->set_status_code("500");
                resp->append_output_body("{\"error\":\"Failed to update tags\"}");
                return;
            }
            resp->append_output_body("{\"status\":\"success\"}");
        }

    } // namespace api
} // namespace smartnas

