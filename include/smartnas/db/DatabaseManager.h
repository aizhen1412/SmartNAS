#pragma once
#include <sqlite3.h>
#include <string>
#include <mutex>
#include <vector>
#include "smartnas/core/FileMetadata.h"
#include "smartnas/core/User.h"

namespace smartnas
{
    namespace db
    {

        class DatabaseManager
        {
        public:
            static DatabaseManager &get_instance();
            ~DatabaseManager();

            // 初始化：创建表结构
            bool init(const std::string &db_path);

            // --- 用户相关 ---
            bool register_user(const std::string &username, const std::string &password);
            bool authenticate_user(const std::string &username, const std::string &password);

            // --- 文件相关 (重写之前的方法) ---
            bool save_file_metadata(const core::FileMetadata &meta);
            bool update_file_summary(const std::string &owner, const std::string &hash, const std::string &summary);
            bool get_file_metadata(const std::string &hash, core::FileMetadata &out_meta);
            bool get_user_file_metadata(const std::string &username, const std::string &hash, core::FileMetadata &out_meta);
            bool delete_file_metadata(const std::string &hash, const std::string &username);
            bool soft_delete_file_metadata(const std::string &hash, const std::string &username);
            bool restore_file_metadata(const std::string &hash, const std::string &username);
            bool rename_file(const std::string &username, const std::string &hash, const std::string &new_name);
            bool move_file(const std::string &username, const std::string &hash, const std::string &directory);
            int count_file_references(const std::string &hash);
            bool exists(const std::string &hash);
            bool user_has_file(const std::string &username, const std::string &hash);
            // 根据用户名获取该用户的所有文件
            std::vector<core::FileMetadata> get_user_files(const std::string &username, const std::string &directory = "/", bool include_deleted = false);
            std::vector<core::FileMetadata> get_deleted_files(const std::string &username);
            long long get_user_storage_usage(const std::string &username);
            // 根据自然语言关键词在摘要中搜索可能的文件
            std::vector<core::FileMetadata> search_files_by_summary(const std::string &owner, const std::string &keyword);
            bool create_folder(const std::string &username, const std::string &path);
            bool delete_folder(const std::string &username, const std::string &path);
            std::vector<core::FolderMetadata> get_user_folders(const std::string &username);
            bool create_share(const std::string &token, const std::string &username, const std::string &hash, long long expires_at);
            bool get_share(const std::string &token, core::ShareMetadata &out_share);

        private:
            DatabaseManager() : db_(nullptr) {}
            sqlite3 *db_;
            std::mutex mtx_;
        };

    } // namespace db
} // namespace smartnas
