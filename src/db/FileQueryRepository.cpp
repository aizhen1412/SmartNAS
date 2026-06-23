#include "smartnas/db/DatabaseManager.h"
#include <string>
#include <vector>

namespace smartnas
{
    namespace db
    {

        std::vector<smartnas::core::FileMetadata> DatabaseManager::get_user_files(const std::string &username, const std::string &directory, bool include_deleted)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            std::vector<smartnas::core::FileMetadata> files;

            std::string sql = include_deleted
                                  ? "SELECT file_hash, file_name, file_size, upload_time, summary, tags, directory, deleted, deleted_time FROM files WHERE owner = ? AND directory = ?;"
                                  : "SELECT file_hash, file_name, file_size, upload_time, summary, tags, directory, deleted, deleted_time FROM files WHERE owner = ? AND directory = ? AND deleted = 0;";
            sqlite3_stmt *stmt;

            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, directory.c_str(), -1, SQLITE_TRANSIENT);
                while (sqlite3_step(stmt) == SQLITE_ROW)
                {
                    smartnas::core::FileMetadata meta;
                    meta.file_hash = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
                    meta.filename = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
                    meta.file_size = sqlite3_column_int64(stmt, 2);
                    meta.upload_time = sqlite3_column_int64(stmt, 3);
                    const char *summary_ptr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
                    if (summary_ptr)
                        meta.summary = summary_ptr;
                    const char *tags_ptr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
                    if (tags_ptr)
                        meta.tags = tags_ptr;
                    const char *dir_ptr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6));
                    meta.directory = dir_ptr ? dir_ptr : "/";
                    meta.deleted = sqlite3_column_int(stmt, 7);
                    meta.deleted_time = sqlite3_column_int64(stmt, 8);
                    files.push_back(meta);
                }
                sqlite3_finalize(stmt);
            }
            return files;
        }

        std::vector<smartnas::core::FileMetadata> DatabaseManager::get_all_user_files(const std::string &username, bool include_deleted)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            std::vector<smartnas::core::FileMetadata> files;
            const char *sql = include_deleted
                                  ? "SELECT file_hash, file_name, file_size, upload_time, summary, tags, directory, deleted, deleted_time FROM files WHERE owner = ?;"
                                  : "SELECT file_hash, file_name, file_size, upload_time, summary, tags, directory, deleted, deleted_time FROM files WHERE owner = ? AND deleted = 0;";
            sqlite3_stmt *stmt;
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
                while (sqlite3_step(stmt) == SQLITE_ROW)
                {
                    smartnas::core::FileMetadata meta;
                    meta.file_hash = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
                    meta.filename = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
                    meta.file_size = sqlite3_column_int64(stmt, 2);
                    meta.upload_time = sqlite3_column_int64(stmt, 3);
                    const char *summary = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
                    const char *tags = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
                    const char *directory = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6));
                    meta.summary = summary ? summary : "";
                    meta.tags = tags ? tags : "[]";
                    meta.directory = directory ? directory : "/";
                    meta.deleted = sqlite3_column_int(stmt, 7);
                    meta.deleted_time = sqlite3_column_int64(stmt, 8);
                    files.push_back(meta);
                }
                sqlite3_finalize(stmt);
            }
            return files;
        }

        std::vector<smartnas::core::FileMetadata> DatabaseManager::get_deleted_files(const std::string &username)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            std::vector<smartnas::core::FileMetadata> files;
            std::string sql = "SELECT file_hash, file_name, file_size, upload_time, summary, tags, directory, deleted, deleted_time FROM files WHERE owner = ? AND deleted = 1;";
            sqlite3_stmt *stmt;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
                while (sqlite3_step(stmt) == SQLITE_ROW)
                {
                    smartnas::core::FileMetadata meta;
                    meta.file_hash = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
                    meta.filename = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
                    meta.file_size = sqlite3_column_int64(stmt, 2);
                    meta.upload_time = sqlite3_column_int64(stmt, 3);
                    const char *summary_ptr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
                    const char *tags_ptr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
                    const char *dir_ptr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6));
                    if (summary_ptr)
                        meta.summary = summary_ptr;
                    if (tags_ptr)
                        meta.tags = tags_ptr;
                    meta.directory = dir_ptr ? dir_ptr : "/";
                    meta.deleted = sqlite3_column_int(stmt, 7);
                    meta.deleted_time = sqlite3_column_int64(stmt, 8);
                    files.push_back(meta);
                }
                sqlite3_finalize(stmt);
            }
            return files;
        }

        std::vector<smartnas::core::FileMetadata> DatabaseManager::search_files_by_summary(const std::string &owner, const std::string &keyword)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            std::vector<smartnas::core::FileMetadata> files;

            std::string sql = "SELECT file_hash, file_name, file_size, upload_time, owner, summary, tags FROM files WHERE owner = ? AND deleted = 0 AND (summary LIKE ? OR file_name LIKE ? OR tags LIKE ?);";
            sqlite3_stmt *stmt;

            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                std::string like_query = "%" + keyword + "%";
                sqlite3_bind_text(stmt, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, like_query.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, like_query.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 4, like_query.c_str(), -1, SQLITE_TRANSIENT);
                while (sqlite3_step(stmt) == SQLITE_ROW)
                {
                    smartnas::core::FileMetadata meta;
                    meta.file_hash = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
                    meta.filename = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
                    meta.file_size = sqlite3_column_int64(stmt, 2);
                    meta.upload_time = sqlite3_column_int64(stmt, 3);
                    meta.owner = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
                    const char *summary_ptr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
                    if (summary_ptr)
                        meta.summary = summary_ptr;
                    const char *tags_ptr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6));
                    if (tags_ptr)
                        meta.tags = tags_ptr;
                    files.push_back(meta);
                }
                sqlite3_finalize(stmt);
            }
            return files;
        }

        long long DatabaseManager::get_user_storage_usage(const std::string &username)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            std::string sql = "SELECT COALESCE(SUM(file_size), 0) FROM files WHERE owner = ? AND deleted = 0;";
            sqlite3_stmt *stmt;
            long long usage = 0;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW)
                    usage = sqlite3_column_int64(stmt, 0);
                sqlite3_finalize(stmt);
            }
            return usage;
        }

    } // namespace db
} // namespace smartnas
