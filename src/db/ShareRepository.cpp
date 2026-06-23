#include "smartnas/db/DatabaseManager.h"
#include <chrono>
#include <string>

namespace smartnas
{
    namespace db
    {

        bool DatabaseManager::create_share(const std::string &token, const std::string &username, const std::string &hash, long long expires_at)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            long long now = std::chrono::system_clock::now().time_since_epoch().count();
            std::string sql = "INSERT INTO shares (token, file_hash, owner, expires_at, created_time) VALUES (?, ?, ?, ?, ?);";
            sqlite3_stmt *stmt;
            bool success = false;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, username.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 4, expires_at);
                sqlite3_bind_int64(stmt, 5, now);
                success = sqlite3_step(stmt) == SQLITE_DONE;
                sqlite3_finalize(stmt);
            }
            return success;
        }

        bool DatabaseManager::get_share(const std::string &token, core::ShareMetadata &out_share)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            std::string sql = "SELECT s.token, s.file_hash, s.owner, s.expires_at, f.file_name FROM shares s JOIN files f ON s.file_hash = f.file_hash AND s.owner = f.owner WHERE s.token = ? AND f.deleted = 0 LIMIT 1;";
            sqlite3_stmt *stmt;
            bool found = false;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW)
                {
                    out_share.token = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
                    out_share.file_hash = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
                    out_share.owner = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
                    out_share.expires_at = sqlite3_column_int64(stmt, 3);
                    out_share.filename = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
                    found = true;
                }
                sqlite3_finalize(stmt);
            }
            return found;
        }

    } // namespace db
} // namespace smartnas
