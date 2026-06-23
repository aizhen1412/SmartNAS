#pragma once
#include "workflow/WFHttpServer.h"
#include "smartnas/core/FileMetadata.h"

namespace smartnas
{
    namespace api
    {

        class Router
        {
        public:
            static void process(WFHttpTask *server_task);

        private:
            static void handle_ping(protocol::HttpRequest *req, protocol::HttpResponse *resp);
            static void handle_upload(WFHttpTask *server_task);
            static void handle_upload_init(WFHttpTask *task);
            static void handle_upload_chunk(WFHttpTask *task);
            static void handle_upload_merge(WFHttpTask *task);
            static void handle_download(WFHttpTask *server_task);
            static void handle_preview(WFHttpTask *server_task);

            static std::string get_authenticated_user(protocol::HttpRequest *req);
            static void serve_file_with_range(WFHttpTask *task, const std::string &hash, const smartnas::core::FileMetadata &meta, bool is_download);
            static void handle_not_found(protocol::HttpResponse *resp);
            static void handle_register(WFHttpTask *task);
            static void handle_login(WFHttpTask *task);
            static void handle_list_files(WFHttpTask *task);
            static void handle_list_all_files(WFHttpTask *task);
            static void handle_current_user(WFHttpTask *task);
            static void handle_delete(WFHttpTask *task);
            static void handle_restore(WFHttpTask *task);
            static void handle_purge(WFHttpTask *task);
            static void handle_home(WFHttpTask *task);
            static void handle_runtime_config(WFHttpTask *task);
            static void handle_static_asset(WFHttpTask *task);
            static void handle_hash_wasm_script(WFHttpTask *task);
            static void handle_search_files(WFHttpTask *task);
            static void handle_update_file_summary(WFHttpTask *task);
            static void handle_update_file_tags(WFHttpTask *task);
            static void handle_rename_file(WFHttpTask *task);
            static void handle_move_file(WFHttpTask *task);
            static void handle_folders(WFHttpTask *task);
            static void handle_create_folder(WFHttpTask *task);
            static void handle_rename_folder(WFHttpTask *task);
            static void handle_move_folder(WFHttpTask *task);
            static void handle_delete_folder(WFHttpTask *task);
            static void handle_stats(WFHttpTask *task);
            static void handle_create_share(WFHttpTask *task);
            static void handle_share_download(WFHttpTask *task);
        };

    } // namespace api
} // namespace smartnas
