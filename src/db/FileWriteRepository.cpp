#include "smartnas/db/DatabaseManager.h"
#include <iostream>
#include <string>

namespace smartnas
{
    namespace db
    {

        bool DatabaseManager::save_file_metadata(const core::FileMetadata &meta)
        {
            std::lock_guard<std::mutex> lock(mtx_);

            std::string sql = "INSERT INTO files (file_hash, file_name, file_size, storage_path, upload_time, owner, summary, tags, directory, deleted, deleted_time) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 0);";

            sqlite3_stmt *stmt;
            bool success = false;

            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, meta.file_hash.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, meta.filename.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 3, meta.file_size);
                sqlite3_bind_text(stmt, 4, meta.storage_path.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 5, meta.upload_time);
                sqlite3_bind_text(stmt, 6, meta.owner.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 7, meta.summary.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 8, meta.tags.empty() ? "[]" : meta.tags.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 9, meta.directory.empty() ? "/" : meta.directory.c_str(), -1, SQLITE_TRANSIENT);

                if (sqlite3_step(stmt) == SQLITE_DONE)
                {
                    success = true;
                }
                else
                {
                    std::cerr << "Save Metadata Error: " << sqlite3_errmsg(db_) << std::endl;
                }
                sqlite3_finalize(stmt);
            }
            else
            {
                std::cerr << "Prepare Statement Error: " << sqlite3_errmsg(db_) << std::endl;
            }
            return success;
        }

        bool DatabaseManager::update_file_summary(const std::string &owner, const std::string &hash, const std::string &summary)
        {
            std::lock_guard<std::mutex> lock(mtx_);

            std::string sql = "UPDATE files SET summary = ? WHERE owner = ? AND file_hash = ?;";
            sqlite3_stmt *stmt;
            bool success = false;

            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, summary.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, owner.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, hash.c_str(), -1, SQLITE_TRANSIENT);

                if (sqlite3_step(stmt) == SQLITE_DONE)
                {
                    success = sqlite3_changes(db_) > 0;
                }
                else
                {
                    std::cerr << "Update Summary Error: " << sqlite3_errmsg(db_) << std::endl;
                }
                sqlite3_finalize(stmt);
            }
            else
            {
                std::cerr << "Prepare Summary Update Error: " << sqlite3_errmsg(db_) << std::endl;
            }
            return success;
        }

        bool DatabaseManager::update_file_tags(const std::string &owner, const std::string &hash, const std::string &tags)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            const char *sql = "UPDATE files SET tags = ? WHERE owner = ? AND file_hash = ?;";
            sqlite3_stmt *stmt;
            bool success = false;
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, tags.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, owner.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, hash.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_DONE)
                    success = sqlite3_changes(db_) > 0;
                sqlite3_finalize(stmt);
            }
            return success;
        }

    } // namespace db
} // namespace smartnas
