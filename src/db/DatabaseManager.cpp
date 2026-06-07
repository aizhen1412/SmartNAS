#include "smartnas/db/DatabaseManager.h"
#include "smartnas/utils/HashUtil.h"
#include <iostream>

namespace smartnas
{
    namespace db
    {

        DatabaseManager &DatabaseManager::get_instance()
        {
            static DatabaseManager instance;
            return instance;
        }

        DatabaseManager::~DatabaseManager()
        {
            if (db_)
                sqlite3_close(db_);
        }

        bool DatabaseManager::init(const std::string &db_path)
        {
            if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK)
                return false;

            // 创建用户表和文件表
            const char *sql =
                "CREATE TABLE IF NOT EXISTS users ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "username TEXT UNIQUE,"
                "password_hash TEXT);"

                "CREATE TABLE IF NOT EXISTS files ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "file_hash TEXT," // 去掉 UNIQUE，因为不同用户可以拥有同一个哈希的文件
                "file_name TEXT,"
                "file_size INTEGER,"
                "storage_path TEXT,"
                "upload_time INTEGER,"
                "owner TEXT,"
                "summary TEXT);"; // 文件摘要，由外部服务通过 Core API 写入
            char *errMsg = nullptr;
            if (sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg) != SQLITE_OK)
            {
                std::cerr << "SQL error: " << errMsg << std::endl;
                sqlite3_free(errMsg);
                return false;
            }
            return true;
        }

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

        bool DatabaseManager::save_file_metadata(const core::FileMetadata &meta)
        {
            std::lock_guard<std::mutex> lock(mtx_);

            std::string sql = "INSERT INTO files (file_hash, file_name, file_size, storage_path, upload_time, owner, summary) VALUES (?, ?, ?, ?, ?, ?, ?);";

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

        bool DatabaseManager::get_file_metadata(const std::string &hash, core::FileMetadata &out_meta)
        {
            std::lock_guard<std::mutex> lock(mtx_);

            std::string sql = "SELECT file_name, file_size, storage_path, upload_time FROM files WHERE file_hash = ?;";
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

            std::string sql = "SELECT 1 FROM files WHERE file_hash = ? AND owner = ? LIMIT 1;";
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

        std::vector<smartnas::core::FileMetadata> DatabaseManager::get_user_files(const std::string &username)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            std::vector<smartnas::core::FileMetadata> files;

            std::string sql = "SELECT file_hash, file_name, file_size, upload_time, summary FROM files WHERE owner = ?;";
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
                    if (summary_ptr)
                        meta.summary = summary_ptr;
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

            std::string sql = "SELECT file_hash, file_name, file_size, upload_time, owner, summary FROM files WHERE owner = ? AND (summary LIKE ? OR file_name LIKE ?);";
            sqlite3_stmt *stmt;

            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                std::string like_query = "%" + keyword + "%";
                sqlite3_bind_text(stmt, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, like_query.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, like_query.c_str(), -1, SQLITE_TRANSIENT);
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
                    files.push_back(meta);
                }
                sqlite3_finalize(stmt);
            }
            return files;
        }

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
    }
}
