#include "smartnas/core/FileManager.h"
#include <fstream> // C++ 文件流库
#include <iostream>

namespace smartnas
{
    namespace core
    {

        bool FileManager::save_file(const std::string &filename, const void *data, size_t size)
        {
            // 1. 拼接最终的存储路径。
            // 注意：这里的路径是相对路径。因为我们在 build 目录启动程序，
            // 所以用 "../var/data/" 指向你根目录的 var/data 文件夹。
            std::string filepath = "../var/data/" + filename;

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
            std::string filepath = "../var/data/" + filename;
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

    } // namespace core
} // namespace smartnas