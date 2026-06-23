#include "smartnas/db/DatabaseManager.h"
#include "smartnas/utils/HashUtil.h"
#include <string>

namespace smartnas
{
    namespace db
    {

        bool DatabaseManager::register_user(const std::string &username, const std::string &password)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            std::string hash = utils::HashUtil::sha256(password.c_str(), password.size());

            std::string sql = "INSERT INTO users (username, password_hash) VALUES (?, ?);";
            sqlite3_stmt *stmt;
            bool success = false;

            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_DONE)
                {
                    success = true;
                }
                sqlite3_finalize(stmt);
            }
            return success;
        }

        bool DatabaseManager::authenticate_user(const std::string &username, const std::string &password)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            std::string hash = utils::HashUtil::sha256(password.c_str(), password.size());

            std::string sql = "SELECT * FROM users WHERE username=? AND password_hash=?;";
            sqlite3_stmt *stmt;
            bool success = false;

            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW)
                {
                    success = true;
                }
                sqlite3_finalize(stmt);
            }
            return success;
        }

    } // namespace db
} // namespace smartnas
