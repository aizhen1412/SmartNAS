#pragma once
#include <string>
#include <chrono>

namespace smartnas
{
    namespace core
    {

        // include/smartnas/core/FileMetadata.h
        struct FileMetadata
        {
            std::string filename;
            std::string file_hash;
            size_t file_size;
            std::string storage_path;
            long long upload_time;
            std::string owner;   // 【新增】
            std::string summary; // 文件摘要，由外部服务通过 Core API 写入
        };

    } // namespace core
} // namespace smartnas
