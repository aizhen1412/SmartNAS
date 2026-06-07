#include "smartnas/core/FileManager.h"
#include <fstream> // C++ 文件流库
#include <iostream>
#include <cstdio>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace smartnas
{
    namespace core
    {

        bool FileManager::save_file(const std::string &filename, const void *data, size_t size)
        {
            // 1. 拼接最终的存储路径。
            // 注意：这里的路径是相对路径。因为我们在 build 目录启动程序，
            // 所以用 "../../var/data/" 指向你根目录的 var/data 文件夹。
            std::string filepath = "../../var/data/" + filename;

            // 2. 创建文件输出流对象 (std::ofstream)
            // std::ios::binary 告诉操作系统：这是二进制文件，不要对换行符做任何瞎篡改！
            // std::ios::trunc 表示如果文件已经存在，就清空覆盖它。
            std::ofstream outfile(filepath, std::ios::binary | std::ios::trunc);

            // 3. 检查文件是否成功打开（比如目录不存在或没有写入权限时会失败）
            if (!outfile.is_open())
            {
                std::cerr << "[FileManager] 错误: 无法打开文件进行写入 -> " << filepath << std::endl;
                return false;
            }

            // 4. 将内存中的数据一次性写到磁盘
            // static_cast 是一种安全的类型转换，将 void* 转为 char* 让 fstream 认识
            outfile.write(static_cast<const char *>(data), size);

            // 5. 关闭文件流，释放系统资源
            outfile.close();

            std::cout << "[FileManager] 成功保存文件: " << filepath << " (" << size << " bytes)" << std::endl;
            return true;
        }
        bool FileManager::load_file(const std::string &filename, std::string &out_data)
        {
            std::string filepath = "../../var/data/" + filename;
            // 以二进制模式打开文件
            std::ifstream infile(filepath, std::ios::binary);
            if (!infile.is_open())
                return false;

            // 将文件指针移到末尾，获取文件大小
            infile.seekg(0, std::ios::end);
            size_t size = infile.tellg();
            out_data.resize(size); // 预分配内存

            // 回到开头，读取内容
            infile.seekg(0, std::ios::beg);
            infile.read(&out_data[0], size);
            infile.close();
            return true;
        }

        bool FileManager::delete_file(const std::string &filename)
        {
            std::string filepath = "../../var/data/" + filename;
            if (std::remove(filepath.c_str()) != 0)
            {
                std::cerr << "[FileManager] 错误: 无法删除文件 -> " << filepath << std::endl;
                return false;
            }
            std::cout << "[FileManager] 成功删除文件: " << filepath << std::endl;
            return true;
        }

        bool FileManager::save_chunk(const std::string &hash, int chunk_index, const void *data, size_t size)
        {
            std::string filepath = "../../var/data/" + hash + "_chunk_" + std::to_string(chunk_index);
            std::ofstream outfile(filepath, std::ios::binary | std::ios::trunc);
            if (!outfile.is_open())
                return false;
            outfile.write(static_cast<const char *>(data), size);
            return true;
        }

        bool FileManager::chunk_exists(const std::string &hash, int chunk_index)
        {
            std::string filepath = "../../var/data/" + hash + "_chunk_" + std::to_string(chunk_index);
            std::ifstream infile(filepath);
            return infile.good();
        }

        bool FileManager::merge_chunks(const std::string &hash, int total_chunks, const std::string &final_filename)
        {
            std::string final_path = "../../var/data/" + final_filename;
            std::ofstream outfile(final_path, std::ios::binary | std::ios::trunc);
            if (!outfile.is_open())
                return false;

            for (int i = 0; i < total_chunks; ++i)
            {
                std::string filepath = "../../var/data/" + hash + "_chunk_" + std::to_string(i);
                std::ifstream infile(filepath, std::ios::binary);
                if (!infile.is_open())
                    return false; // 缺少分片
                outfile << infile.rdbuf();
                infile.close();
                std::remove(filepath.c_str());
            }
            return true;
        }

        size_t FileManager::get_file_size(const std::string &filename)
        {
            std::string filepath = "../../var/data/" + filename;
            struct stat stat_buf;
            if (stat(filepath.c_str(), &stat_buf) == 0)
            {
                return stat_buf.st_size;
            }
            return 0;
        }

        bool FileManager::pread_file(const std::string &filename, size_t offset, size_t length, std::string &out_data)
        {
            std::string filepath = "../../var/data/" + filename;
            int fd = open(filepath.c_str(), O_RDONLY);
            if (fd < 0)
                return false;

            out_data.resize(length);
            ssize_t bytes_read = pread(fd, &out_data[0], length, offset);
            close(fd);

            if (bytes_read < 0)
            {
                out_data.clear();
                return false;
            }
            out_data.resize(bytes_read);
            return true;
        }

    } // namespace core
} // namespace smartnas