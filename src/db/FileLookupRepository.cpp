#include "smartnas/db/DatabaseManager.h"
#include <string>

namespace smartnas
{
    namespace db
    {

        bool DatabaseManager::get_file_metadata(const std::string &hash, core::FileMetadata &out_meta)
        {
            std::lock_guard<std::mutex> lock(mtx_);

            std::string sql = "SELECT file_name, file_size, storage_path, upload_time, owner, summary, tags, directory, deleted, deleted_time FROM files WHERE file_hash = ? LIMIT 1;";
            sqlite3_stmt *stmt;
            bool found = false;

            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW)
                {
                    // 从查询结果中提取字段
                    out_meta.filename = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
                    out_meta.file_size = sqlite3_column_int64(stmt, 1);
                    out_meta.storage_path = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
                    out_meta.upload_time = sqlite3_column_int64(stmt, 3);
                    const char *owner_ptr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
                    const char *summary_ptr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
                    const char *tags_ptr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6));
                    const char *dir_ptr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7));
                    if (owner_ptr)
                        out_meta.owner = owner_ptr;
                    if (summary_ptr)
                        out_meta.summary = summary_ptr;
                    if (tags_ptr)
                        out_meta.tags = tags_ptr;
                    out_meta.directory = dir_ptr ? dir_ptr : "/";
                    out_meta.deleted = sqlite3_column_int(stmt, 8);
                    out_meta.deleted_time = sqlite3_column_int64(stmt, 9);
                    out_meta.file_hash = hash;
                    found = true;
                }
                sqlite3_finalize(stmt);
            }
            return found;
        }

        bool DatabaseManager::get_user_file_metadata(const std::string &username, const std::string &hash, core::FileMetadata &out_meta)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            std::string sql = "SELECT file_name, file_size, storage_path, upload_time, owner, summary, tags, directory, deleted, deleted_time FROM files WHERE file_hash = ? AND owner = ? LIMIT 1;";
            sqlite3_stmt *stmt;
            bool found = false;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW)
                {
                    out_meta.filename = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
                    out_meta.file_size = sqlite3_column_int64(stmt, 1);
                    out_meta.storage_path = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
                    out_meta.upload_time = sqlite3_column_int64(stmt, 3);
                    out_meta.owner = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
                    const char *summary_ptr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
                    const char *tags_ptr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6));
                    const char *dir_ptr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7));
                    if (summary_ptr)
                        out_meta.summary = summary_ptr;
                    if (tags_ptr)
                        out_meta.tags = tags_ptr;
                    out_meta.directory = dir_ptr ? dir_ptr : "/";
                    out_meta.deleted = sqlite3_column_int(stmt, 8);
                    out_meta.deleted_time = sqlite3_column_int64(stmt, 9);
                    out_meta.file_hash = hash;
                    found = true;
                }
                sqlite3_finalize(stmt);
            }
            return found;
        }

        bool DatabaseManager::exists(const std::string &hash)
        {
            std::lock_guard<std::mutex> lock(mtx_);

            std::string sql = "SELECT 1 FROM files WHERE file_hash = ? LIMIT 1;";
            sqlite3_stmt *stmt;
            bool found = false;

            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW)
                {
                    found = true;
                }
                sqlite3_finalize(stmt);
            }
            return found;
        }

        bool DatabaseManager::user_has_file(const std::string &username, const std::string &hash)
        {
            std::lock_guard<std::mutex> lock(mtx_);

            std::string sql = "SELECT 1 FROM files WHERE file_hash = ? AND owner = ? AND deleted = 0 LIMIT 1;";
            sqlite3_stmt *stmt;
            bool found = false;

            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW)
                {
                    found = true;
                }
                sqlite3_finalize(stmt);
            }
            return found;
        }

        int DatabaseManager::count_file_references(const std::string &hash)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            std::string sql = "SELECT COUNT(*) FROM files WHERE file_hash = ?;";
            sqlite3_stmt *stmt;
            int count = 0;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW)
                {
                    count = sqlite3_column_int(stmt, 0);
                }
                sqlite3_finalize(stmt);
            }
            return count;
        }

    } // namespace db
} // namespace smartnas
