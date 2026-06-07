#pragma once
#include <string>
#include <chrono>

namespace smartnas
{
    namespace core
    {

        struct FileMetadata
        {
            std::string filename;     // 原始文件名
            std::string file_hash;    // SHA-256 指纹
            size_t file_size;         // 文件大小
            std::string storage_path; // 在服务器上的实际物理路径
            long long upload_time;    // 上传时间戳
        };

    } // namespace core
} // namespace smartnas