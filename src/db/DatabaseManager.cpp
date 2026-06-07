#include "smartnas/db/DatabaseManager.h"

namespace smartnas
{
    namespace db
    {

        DatabaseManager &DatabaseManager::get_instance()
        {
            static DatabaseManager instance;
            return instance;
        }

        bool DatabaseManager::exists(const std::string &hash)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            return mock_db_.find(hash) != mock_db_.end();
        }

        void DatabaseManager::save_metadata(const smartnas::core::FileMetadata &meta)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            mock_db_[meta.file_hash] = meta;
        }

        bool DatabaseManager::get_metadata(const std::string &hash, smartnas::core::FileMetadata &out_meta)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (mock_db_.find(hash) != mock_db_.end())
            {
                out_meta = mock_db_[hash];
                return true;
            }
            return false;
        }

    } // namespace db
} // namespace smartnas