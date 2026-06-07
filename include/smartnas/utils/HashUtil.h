#pragma once
#include <string>

namespace smartnas
{
    namespace utils
    {

        class HashUtil
        {
        public:
            // 计算内存块的 SHA-256 哈希值
            // 返回值：64位的十六进制字符串
            static std::string sha256(const void *data, size_t size);
            static std::string url_decode(const std::string &str);
        };

    } // namespace utils
} // namespace smartnas