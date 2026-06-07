#pragma once
#include "smartnas/core/FileMetadata.h"
#include <unordered_map>
#include <mutex>

namespace smartnas
{
    namespace db
    {

        class DatabaseManager
        {
        public:
            // 单例模式：保证整个程序只有一个“账本”
            static DatabaseManager &get_instance();

            // 检查哈希是否已存在 (秒传判断)
            bool exists(const std::string &hash);

            // 记录一笔新账
            void save_metadata(const smartnas::core::FileMetadata &meta);

            // 根据哈希查找元数据 (下载时用)
            bool get_metadata(const std::string &hash, smartnas::core::FileMetadata &out_meta);

        private:
            DatabaseManager() = default;
            // 使用哈希表模拟数据库，Key 是 SHA-256
            std::unordered_map<std::string, smartnas::core::FileMetadata> mock_db_;
            std::mutex mtx_; // 保证线程安全
        };

    } // namespace db
} // namespace smartnas