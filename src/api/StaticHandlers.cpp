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

        void Router::handle_home(WFHttpTask *task)
        {
            auto *resp = task->get_resp();

            // 1. 打开网页文件
            std::ifstream file(smartnas::config::AppConfig::get_instance().web_path("index.html"));
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

        void Router::handle_runtime_config(WFHttpTask *task)
        {
            auto *resp = task->get_resp();
            const auto &config = smartnas::config::AppConfig::get_instance();
            const std::string json =
                "{\"agentPort\":" + std::to_string(config.agent_port()) +
                ",\"uploadChunkSize\":" + std::to_string(config.upload_chunk_size()) +
                ",\"uploadConcurrency\":" + std::to_string(config.upload_concurrency()) +
                ",\"webCryptoLimit\":" + std::to_string(config.web_crypto_limit()) + "}";
            resp->add_header_pair("Content-Type", "application/json; charset=utf-8");
            resp->append_output_body(json.c_str(), json.size());
        }

        void Router::handle_hash_wasm_script(WFHttpTask *task)
        {
            auto *resp = task->get_resp();
            std::ifstream file(smartnas::config::AppConfig::get_instance().web_path("vendor/hash-wasm-sha256.umd.min.js"), std::ios::binary);
            if (!file.is_open())
            {
                resp->set_status_code("404");
                resp->append_output_body("Vendor script not found");
                return;
            }

            std::stringstream buffer;
            buffer << file.rdbuf();
            const std::string content = buffer.str();
            resp->add_header_pair("Content-Type", "application/javascript; charset=utf-8");
            resp->add_header_pair("Cache-Control", "public, max-age=31536000, immutable");
            resp->append_output_body(content.c_str(), content.size());
        }

        void Router::handle_static_asset(WFHttpTask *task)
        {
            auto *req = task->get_req();
            auto *resp = task->get_resp();
            const std::string uri = req->get_request_uri();
            const auto &config = smartnas::config::AppConfig::get_instance();
            const std::map<std::string, std::pair<std::string, std::string>> assets = {
                {"/assets/css/app.css", {config.web_path("css/app.css").string(), "text/css; charset=utf-8"}},
                {"/assets/js/state.js", {config.web_path("js/state.js").string(), "application/javascript; charset=utf-8"}},
                {"/assets/js/core.js", {config.web_path("js/core.js").string(), "application/javascript; charset=utf-8"}},
                {"/assets/js/files.js", {config.web_path("js/files.js").string(), "application/javascript; charset=utf-8"}},
                {"/assets/js/upload.js", {config.web_path("js/upload.js").string(), "application/javascript; charset=utf-8"}},
                {"/assets/js/operations.js", {config.web_path("js/operations.js").string(), "application/javascript; charset=utf-8"}},
                {"/assets/js/file-qa.js", {config.web_path("js/file-qa.js").string(), "application/javascript; charset=utf-8"}},
                {"/assets/js/chat.js", {config.web_path("js/chat.js").string(), "application/javascript; charset=utf-8"}},
                {"/assets/js/bootstrap.js", {config.web_path("js/bootstrap.js").string(), "application/javascript; charset=utf-8"}},
            };
            const auto asset = assets.find(uri);
            if (asset == assets.end())
            {
                resp->set_status_code("404");
                resp->append_output_body("Asset not found");
                return;
            }

            std::ifstream file(asset->second.first, std::ios::binary);
            if (!file.is_open())
            {
                resp->set_status_code("404");
                resp->append_output_body("Asset file unavailable");
                return;
            }
            std::stringstream buffer;
            buffer << file.rdbuf();
            const std::string content = buffer.str();
            resp->add_header_pair("Content-Type", asset->second.second);
            resp->add_header_pair("Cache-Control", "no-cache");
            resp->append_output_body(content.c_str(), content.size());
        }

    } // namespace api
} // namespace smartnas
