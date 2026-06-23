#include "smartnas/core/FileManager.h"
#include "smartnas/config/AppConfig.h"
#include <fstream> // C++ 文件流库
#include <iostream>
#include <cstdio>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>
#include <vector>
#include <array>
#include <iomanip>
#include <sstream>
#include <openssl/evp.h>

namespace smartnas
{
    namespace core
    {

        bool FileManager::save_file(const std::string &filename, const void *data, size_t size)
        {
            // 存储目录由统一配置加载，并在启动时解析为绝对路径。
            std::string filepath = smartnas::config::AppConfig::get_instance().data_path(filename).string();

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
            std::string filepath = smartnas::config::AppConfig::get_instance().data_path(filename).string();
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
            std::string filepath = smartnas::config::AppConfig::get_instance().data_path(filename).string();
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
            std::error_code error;
            std::filesystem::create_directories(smartnas::config::AppConfig::get_instance().data_dir(), error);
            if (error)
            {
                std::cerr << "[FileManager] 无法创建数据目录: " << error.message() << std::endl;
                return false;
            }

            std::string filepath = smartnas::config::AppConfig::get_instance().data_path(hash + "_chunk_" + std::to_string(chunk_index)).string();
            std::ofstream outfile(filepath, std::ios::binary | std::ios::trunc);
            if (!outfile.is_open())
            {
                std::cerr << "[FileManager] 无法写入分片: " << filepath << std::endl;
                return false;
            }
            outfile.write(static_cast<const char *>(data), size);
            outfile.close();
            if (!outfile)
            {
                std::cerr << "[FileManager] 分片写入不完整: " << filepath << std::endl;
                return false;
            }
            return true;
        }

        bool FileManager::chunk_exists(const std::string &hash, int chunk_index)
        {
            std::string filepath = smartnas::config::AppConfig::get_instance().data_path(hash + "_chunk_" + std::to_string(chunk_index)).string();
            std::ifstream infile(filepath);
            return infile.good();
        }

        bool FileManager::chunk_has_size(const std::string &hash, int chunk_index, size_t expected_size)
        {
            const std::string filepath = smartnas::config::AppConfig::get_instance().data_path(hash + "_chunk_" + std::to_string(chunk_index)).string();
            std::error_code error;
            const auto size = std::filesystem::file_size(filepath, error);
            return !error && size == expected_size;
        }

        bool FileManager::merge_chunks(const std::string &hash, int total_chunks, const std::string &final_filename,
                                       std::string &merged_hash, size_t &merged_size)
        {
            std::string final_path = smartnas::config::AppConfig::get_instance().data_path(final_filename).string();
            std::ofstream outfile(final_path, std::ios::binary | std::ios::trunc);
            if (!outfile.is_open())
            {
                std::cerr << "[FileManager] 无法创建合并文件: " << final_path << std::endl;
                return false;
            }

            EVP_MD_CTX *digest = EVP_MD_CTX_new();
            if (!digest || EVP_DigestInit_ex(digest, EVP_sha256(), nullptr) != 1)
            {
                EVP_MD_CTX_free(digest);
                outfile.close();
                std::remove(final_path.c_str());
                return false;
            }

            merged_size = 0;
            std::array<char, 1024 * 1024> buffer;
            for (int i = 0; i < total_chunks; ++i)
            {
                const std::string filepath = smartnas::config::AppConfig::get_instance().data_path(hash + "_chunk_" + std::to_string(i)).string();
                std::ifstream infile(filepath, std::ios::binary);
                if (!infile.is_open())
                {
                    std::cerr << "[FileManager] 缺少分片: " << filepath << std::endl;
                    EVP_MD_CTX_free(digest);
                    outfile.close();
                    std::remove(final_path.c_str());
                    return false;
                }

                while (infile)
                {
                    infile.read(buffer.data(), buffer.size());
                    const std::streamsize count = infile.gcount();
                    if (count <= 0)
                        continue;
                    outfile.write(buffer.data(), count);
                    if (!outfile || EVP_DigestUpdate(digest, buffer.data(), static_cast<size_t>(count)) != 1)
                    {
                        std::cerr << "[FileManager] 合并写入或校验失败: " << final_path << std::endl;
                        EVP_MD_CTX_free(digest);
                        outfile.close();
                        std::remove(final_path.c_str());
                        return false;
                    }
                    merged_size += static_cast<size_t>(count);
                }
                if (!infile.eof())
                {
                    EVP_MD_CTX_free(digest);
                    outfile.close();
                    std::remove(final_path.c_str());
                    return false;
                }
            }

            outfile.close();
            if (!outfile)
            {
                std::cerr << "[FileManager] 合并文件落盘失败: " << final_path << std::endl;
                EVP_MD_CTX_free(digest);
                std::remove(final_path.c_str());
                return false;
            }

            unsigned char hash_bytes[EVP_MAX_MD_SIZE];
            unsigned int hash_length = 0;
            if (EVP_DigestFinal_ex(digest, hash_bytes, &hash_length) != 1)
            {
                EVP_MD_CTX_free(digest);
                std::remove(final_path.c_str());
                return false;
            }
            EVP_MD_CTX_free(digest);

            std::ostringstream result;
            for (unsigned int i = 0; i < hash_length; ++i)
                result << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash_bytes[i]);
            merged_hash = result.str();
            return true;
        }

        void FileManager::delete_chunks(const std::string &hash, int total_chunks)
        {
            for (int i = 0; i < total_chunks; ++i)
            {
                std::string filepath = smartnas::config::AppConfig::get_instance().data_path(hash + "_chunk_" + std::to_string(i)).string();
                std::remove(filepath.c_str());
            }
        }

        size_t FileManager::get_file_size(const std::string &filename)
        {
            std::string filepath = smartnas::config::AppConfig::get_instance().data_path(filename).string();
            struct stat stat_buf;
            if (stat(filepath.c_str(), &stat_buf) == 0)
            {
                return stat_buf.st_size;
            }
            return 0;
        }

        bool FileManager::pread_file(const std::string &filename, size_t offset, size_t length, std::string &out_data)
        {
            std::string filepath = smartnas::config::AppConfig::get_instance().data_path(filename).string();
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
