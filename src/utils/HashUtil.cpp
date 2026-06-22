#include "smartnas/utils/HashUtil.h"
#include <openssl/evp.h> // 【修改点】改用 evp.h
#include <iomanip>
#include <sstream>
#include <array>
#include <fstream>

namespace smartnas
{
    namespace utils
    {

        std::string HashUtil::sha256(const void *data, size_t size)
        {
            // 1. 定义输出缓冲区和长度变量
            unsigned char hash[EVP_MAX_MD_SIZE];
            unsigned int hash_len = 0;

            // 2. 创建并初始化上下文 (Context)
            // 这是现代 OpenSSL 的标准做法，使用对象管理
            EVP_MD_CTX *ctx = EVP_MD_CTX_new();

            // 3. 执行计算三步走：初始化 -> 喂数据 -> 结束并获取结果
            // 使用 EVP_sha256() 指定算法
            EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
            EVP_DigestUpdate(ctx, data, size);
            EVP_DigestFinal_ex(ctx, hash, &hash_len);

            // 4. 释放资源 (重要：防止内存泄漏)
            EVP_MD_CTX_free(ctx);

            // 5. 将二进制转换为十六进制字符串
            std::stringstream ss;
            for (unsigned int i = 0; i < hash_len; ++i)
            {
                ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
            }

            return ss.str();
        }

        std::string HashUtil::sha256_file(const std::string &path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open())
                return "";

            EVP_MD_CTX *ctx = EVP_MD_CTX_new();
            if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1)
            {
                EVP_MD_CTX_free(ctx);
                return "";
            }

            std::array<char, 1024 * 1024> buffer;
            while (file)
            {
                file.read(buffer.data(), buffer.size());
                const std::streamsize count = file.gcount();
                if (count > 0 && EVP_DigestUpdate(ctx, buffer.data(), static_cast<size_t>(count)) != 1)
                {
                    EVP_MD_CTX_free(ctx);
                    return "";
                }
            }

            if (!file.eof())
            {
                EVP_MD_CTX_free(ctx);
                return "";
            }

            unsigned char hash[EVP_MAX_MD_SIZE];
            unsigned int hash_len = 0;
            if (EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1)
            {
                EVP_MD_CTX_free(ctx);
                return "";
            }
            EVP_MD_CTX_free(ctx);

            std::stringstream result;
            for (unsigned int i = 0; i < hash_len; ++i)
                result << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
            return result.str();
        }

        std::string HashUtil::url_decode(const std::string &str)
        {
            std::string res;
            for (size_t i = 0; i < str.length(); ++i)
            {
                if (str[i] == '%' && i + 2 < str.length())
                {
                    res += (char)std::strtol(str.substr(i + 1, 2).c_str(), nullptr, 16);
                    i += 2;
                }
                else if (str[i] == '+')
                {
                    res += ' ';
                }
                else
                {
                    res += str[i];
                }
            }
            return res;
        }

    } // namespace utils
} // namespace smartnas
