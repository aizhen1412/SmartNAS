#include "smartnas/db/DatabaseManager.h"
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
                "summary TEXT,"
                "tags TEXT DEFAULT '[]',"
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
                "ALTER TABLE files ADD COLUMN deleted_time INTEGER DEFAULT 0;",
                "ALTER TABLE files ADD COLUMN tags TEXT DEFAULT '[]';"};
            for (const char *migration : migrations)
            {
                char *migErr = nullptr;
                sqlite3_exec(db_, migration, nullptr, nullptr, &migErr);
                if (migErr)
                    sqlite3_free(migErr);
            }
            return true;
        }

    } // namespace db
} // namespace smartnas
