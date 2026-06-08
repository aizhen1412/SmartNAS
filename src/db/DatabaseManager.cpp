#include "smartnas/db/DatabaseManager.h"
#include "smartnas/utils/HashUtil.h"
#include <iostream>
#include <chrono>

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
                "summary TEXT,"
                "directory TEXT DEFAULT '/',"
                "deleted INTEGER DEFAULT 0,"
                "deleted_time INTEGER DEFAULT 0);"

                "CREATE TABLE IF NOT EXISTS folders ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "owner TEXT,"
                "path TEXT,"
                "created_time INTEGER,"
                "UNIQUE(owner, path));"

                "CREATE TABLE IF NOT EXISTS shares ("
                "token TEXT PRIMARY KEY,"
                "file_hash TEXT,"
                "owner TEXT,"
                "expires_at INTEGER,"
                "created_time INTEGER);";
            char *errMsg = nullptr;
            if (sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg) != SQLITE_OK)
            {
                std::cerr << "SQL error: " << errMsg << std::endl;
                sqlite3_free(errMsg);
                return false;
            }
            const char *migrations[] = {
                "ALTER TABLE files ADD COLUMN directory TEXT DEFAULT '/';",
                "ALTER TABLE files ADD COLUMN deleted INTEGER DEFAULT 0;",
                "ALTER TABLE files ADD COLUMN deleted_time INTEGER DEFAULT 0;"};
            for (const char *migration : migrations)
            {
                char *migErr = nullptr;
                sqlite3_exec(db_, migration, nullptr, nullptr, &migErr);
                if (migErr)
                    sqlite3_free(migErr);
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

            std::string sql = "INSERT INTO files (file_hash, file_name, file_size, storage_path, upload_time, owner, summary, directory, deleted, deleted_time) VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, 0);";

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
                sqlite3_bind_text(stmt, 8, meta.directory.empty() ? "/" : meta.directory.c_str(), -1, SQLITE_TRANSIENT);

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

            std::string sql = "SELECT file_name, file_size, storage_path, upload_time, owner, summary, directory, deleted, deleted_time FROM files WHERE file_hash = ? LIMIT 1;";
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
                    const char *dir_ptr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6));
                    if (owner_ptr)
                        out_meta.owner = owner_ptr;
                    if (summary_ptr)
                        out_meta.summary = summary_ptr;
                    out_meta.directory = dir_ptr ? dir_ptr : "/";
                    out_meta.deleted = sqlite3_column_int(stmt, 7);
                    out_meta.deleted_time = sqlite3_column_int64(stmt, 8);
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
            std::string sql = "SELECT file_name, file_size, storage_path, upload_time, owner, summary, directory, deleted, deleted_time FROM files WHERE file_hash = ? AND owner = ? LIMIT 1;";
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
                    const char *dir_ptr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6));
                    if (summary_ptr)
                        out_meta.summary = summary_ptr;
                    out_meta.directory = dir_ptr ? dir_ptr : "/";
                    out_meta.deleted = sqlite3_column_int(stmt, 7);
                    out_meta.deleted_time = sqlite3_column_int64(stmt, 8);
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

        std::vector<smartnas::core::FileMetadata> DatabaseManager::get_user_files(const std::string &username, const std::string &directory, bool include_deleted)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            std::vector<smartnas::core::FileMetadata> files;

            std::string sql = include_deleted
                                  ? "SELECT file_hash, file_name, file_size, upload_time, summary, directory, deleted, deleted_time FROM files WHERE owner = ? AND directory = ?;"
                                  : "SELECT file_hash, file_name, file_size, upload_time, summary, directory, deleted, deleted_time FROM files WHERE owner = ? AND directory = ? AND deleted = 0;";
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
                    const char *dir_ptr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
                    meta.directory = dir_ptr ? dir_ptr : "/";
                    meta.deleted = sqlite3_column_int(stmt, 6);
                    meta.deleted_time = sqlite3_column_int64(stmt, 7);
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
            std::string sql = "SELECT file_hash, file_name, file_size, upload_time, summary, directory, deleted, deleted_time FROM files WHERE owner = ? AND deleted = 1;";
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
                    const char *dir_ptr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
                    if (summary_ptr)
                        meta.summary = summary_ptr;
                    meta.directory = dir_ptr ? dir_ptr : "/";
                    meta.deleted = sqlite3_column_int(stmt, 6);
                    meta.deleted_time = sqlite3_column_int64(stmt, 7);
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

            std::string sql = "SELECT file_hash, file_name, file_size, upload_time, owner, summary FROM files WHERE owner = ? AND deleted = 0 AND (summary LIKE ? OR file_name LIKE ?);";
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
            std::string sql = "DELETE FROM folders WHERE owner = ? AND path = ?;";
            sqlite3_stmt *stmt;
            bool success = false;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
                success = sqlite3_step(stmt) == SQLITE_DONE;
                sqlite3_finalize(stmt);
            }
            return success;
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
