#pragma once
#include <string>

namespace smartnas
{
    namespace core
    {

        struct User
        {
            int id;
            std::string username;
            std::string password_hash; // 存储密码的 SHA-256
            std::string salt;          // 随机盐值，增加安全性
        };

    } // namespace core
} // namespace smartnas