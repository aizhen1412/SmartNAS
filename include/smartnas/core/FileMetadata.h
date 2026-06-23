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
            std::string tags = "[]"; // JSON 字符串数组，由 Agent 生成
            std::string directory = "/";
            int deleted = 0;
            long long deleted_time = 0;
        };

        struct FolderMetadata
        {
            std::string path;
            long long created_time;
        };

        struct ShareMetadata
        {
            std::string token;
            std::string file_hash;
            std::string owner;
            long long expires_at;
            std::string filename;
        };

    } // namespace core
} // namespace smartnas
