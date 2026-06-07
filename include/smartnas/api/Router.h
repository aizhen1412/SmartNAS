#pragma once
#include "workflow/WFHttpServer.h"

// 使用命名空间防止代码冲突
namespace smartnas
{
    namespace api
    {

        class Router
        {
        public:
            // 核心入口：用来替换我们之前写在 main.cpp 里的 process_http_request
            static void process(WFHttpTask *server_task);

        private:
            // 具体的业务处理函数：比如处理 /ping 接口
            static void handle_ping(protocol::HttpRequest *req, protocol::HttpResponse *resp);

            // 【新增】处理文件上传请求
            static void handle_upload(WFHttpTask *server_task);
            static void handle_download(WFHttpTask *server_task);
            static void handle_not_found(protocol::HttpResponse *resp);
        };

    } // namespace api
} // namespace smartnas