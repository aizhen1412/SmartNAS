#include "smartnas/db/DatabaseManager.h"
#include <chrono>
#include <string>
#include <vector>

namespace smartnas
{
    namespace db
    {

        bool DatabaseManager::create_folder(const std::string &username, const std::string &path)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            long long now = std::chrono::system_clock::now().time_since_epoch().count();
            std::string sql = "INSERT OR IGNORE INTO folders (owner, path, created_time) VALUES (?, ?, ?);";
            sqlite3_stmt *stmt;
            bool success = false;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 3, now);
                success = sqlite3_step(stmt) == SQLITE_DONE;
                sqlite3_finalize(stmt);
            }
            return success;
        }

        bool DatabaseManager::delete_folder(const std::string &username, const std::string &path)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            const std::string child_pattern = path + "/%";
            std::string check_sql =
                "SELECT EXISTS(SELECT 1 FROM folders WHERE owner = ? AND path LIKE ?) "
                "OR EXISTS(SELECT 1 FROM files WHERE owner = ? AND (directory = ? OR directory LIKE ?));";
            sqlite3_stmt *check_stmt;
            if (sqlite3_prepare_v2(db_, check_sql.c_str(), -1, &check_stmt, nullptr) != SQLITE_OK)
                return false;
            sqlite3_bind_text(check_stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(check_stmt, 2, child_pattern.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(check_stmt, 3, username.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(check_stmt, 4, path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(check_stmt, 5, child_pattern.c_str(), -1, SQLITE_TRANSIENT);
            const bool has_contents = sqlite3_step(check_stmt) == SQLITE_ROW && sqlite3_column_int(check_stmt, 0) != 0;
            sqlite3_finalize(check_stmt);
            if (has_contents)
                return false;

            std::string sql = "DELETE FROM folders WHERE owner = ? AND path = ?;";
            sqlite3_stmt *stmt;
            bool success = false;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
                success = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0;
                sqlite3_finalize(stmt);
            }
            return success;
        }

        bool DatabaseManager::move_folder(const std::string &username, const std::string &path, const std::string &new_path)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (path.empty() || new_path.empty() || path == "/" || path == new_path)
                return false;

            const std::string child_pattern = path + "/%";
            if (new_path.rfind(path + "/", 0) == 0)
                return false;

            if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK)
                return false;

            auto update_paths = [&](const char *sql) -> bool
            {
                sqlite3_stmt *stmt = nullptr;
                if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
                    return false;
                sqlite3_bind_text(stmt, 1, new_path.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, username.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 4, path.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 5, child_pattern.c_str(), -1, SQLITE_TRANSIENT);
                const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
                sqlite3_finalize(stmt);
                return ok;
            };

            const bool folders_ok = update_paths(
                "UPDATE folders SET path = ? || substr(path, length(?) + 1) "
                "WHERE owner = ? AND (path = ? OR path LIKE ?);");
            const bool folder_found = folders_ok && sqlite3_changes(db_) > 0;
            const bool files_ok = folder_found && update_paths(
                "UPDATE files SET directory = ? || substr(directory, length(?) + 1) "
                "WHERE owner = ? AND (directory = ? OR directory LIKE ?);");
            if (!files_ok || sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK)
            {
                sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
                return false;
            }
            return true;
        }

        std::vector<smartnas::core::FolderMetadata> DatabaseManager::get_user_folders(const std::string &username)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            std::vector<smartnas::core::FolderMetadata> folders;
            std::string sql = "SELECT path, created_time FROM folders WHERE owner = ? ORDER BY path;";
            sqlite3_stmt *stmt;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
                while (sqlite3_step(stmt) == SQLITE_ROW)
                {
                    smartnas::core::FolderMetadata folder;
                    folder.path = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
                    folder.created_time = sqlite3_column_int64(stmt, 1);
                    folders.push_back(folder);
                }
                sqlite3_finalize(stmt);
            }
            return folders;
        }

    } // namespace db
} // namespace smartnas
