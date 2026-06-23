#include "smartnas/db/DatabaseManager.h"
#include <chrono>
#include <iostream>
#include <string>

namespace smartnas
{
    namespace db
    {

        bool DatabaseManager::delete_file_metadata(const std::string &hash, const std::string &username)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            std::string sql = "DELETE FROM files WHERE file_hash = ? AND owner = ?;";
            sqlite3_stmt *stmt;
            bool success = false;

            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_DONE)
                {
                    success = sqlite3_changes(db_) > 0;
                }
                else
                {
                    std::cerr << "SQL error during delete: " << sqlite3_errmsg(db_) << std::endl;
                }
                sqlite3_finalize(stmt);
            }
            return success;
        }

        bool DatabaseManager::soft_delete_file_metadata(const std::string &hash, const std::string &username)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            long long now = std::chrono::system_clock::now().time_since_epoch().count();
            std::string sql = "UPDATE files SET deleted = 1, deleted_time = ? WHERE file_hash = ? AND owner = ? AND deleted = 0;";
            sqlite3_stmt *stmt;
            bool success = false;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_int64(stmt, 1, now);
                sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, username.c_str(), -1, SQLITE_TRANSIENT);
                success = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0;
                sqlite3_finalize(stmt);
            }
            return success;
        }

        bool DatabaseManager::restore_file_metadata(const std::string &hash, const std::string &username)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            std::string sql = "UPDATE files SET deleted = 0, deleted_time = 0 WHERE file_hash = ? AND owner = ? AND deleted = 1;";
            sqlite3_stmt *stmt;
            bool success = false;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
                success = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0;
                sqlite3_finalize(stmt);
            }
            return success;
        }

        bool DatabaseManager::rename_file(const std::string &username, const std::string &hash, const std::string &new_name)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            std::string sql = "UPDATE files SET file_name = ? WHERE file_hash = ? AND owner = ? AND deleted = 0;";
            sqlite3_stmt *stmt;
            bool success = false;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, new_name.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, username.c_str(), -1, SQLITE_TRANSIENT);
                success = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0;
                sqlite3_finalize(stmt);
            }
            return success;
        }

        bool DatabaseManager::move_file(const std::string &username, const std::string &hash, const std::string &directory)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            std::string sql = "UPDATE files SET directory = ? WHERE file_hash = ? AND owner = ? AND deleted = 0;";
            sqlite3_stmt *stmt;
            bool success = false;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, directory.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, username.c_str(), -1, SQLITE_TRANSIENT);
                success = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0;
                sqlite3_finalize(stmt);
            }
            return success;
        }

    } // namespace db
} // namespace smartnas
