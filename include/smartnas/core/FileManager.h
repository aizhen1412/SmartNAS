#pragma once
#include <string>

namespace smartnas
{
    namespace core
    {

        class FileManager
        {
        public:
            // 静态方法：负责将内存中的二进制数据写入到指定的磁盘路径
            // 返回值：bool 代表是否写入成功
            // 参数 filename: 文件名 (例如 "test.txt")
            // 参数 data: 内存中数据的起始指针
            // 参数 size: 数据的大小 (字节)
            static bool save_file(const std::string &filename, const void *data, size_t size);
            static bool load_file(const std::string &filename, std::string &out_data);
            static bool delete_file(const std::string &filename);

            // 分片上传相关
            static bool save_chunk(const std::string &hash, int chunk_index, const void *data, size_t size);
            static bool chunk_exists(const std::string &hash, int chunk_index);
            static bool merge_chunks(const std::string &hash, int total_chunks, const std::string &final_filename);
            static void delete_chunks(const std::string &hash, int total_chunks);

            // 零拷贝 / 范围读取支持
            static bool pread_file(const std::string &filename, size_t offset, size_t length, std::string &out_data);
            static size_t get_file_size(const std::string &filename);
        };

    } // namespace core
} // namespace smartnas
