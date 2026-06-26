#include "smartnas/db/DatabaseManager.h"
#include "smartnas/utils/HashUtil.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>

namespace
{
    constexpr int kPasswordIterations = 210000;
    constexpr size_t kSaltBytes = 16;
    constexpr size_t kDerivedBytes = 32;

    std::string to_hex(const unsigned char *data, size_t size)
    {
        std::ostringstream out;
        for (size_t i = 0; i < size; ++i)
            out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
        return out.str();
    }

    bool from_hex(const std::string &hex, unsigned char *out, size_t out_size)
    {
        if (hex.size() != out_size * 2)
            return false;
        for (size_t i = 0; i < out_size; ++i)
        {
            const std::string byte = hex.substr(i * 2, 2);
            char *end = nullptr;
            long value = std::strtol(byte.c_str(), &end, 16);
            if (!end || *end != '\0' || value < 0 || value > 255)
                return false;
            out[i] = static_cast<unsigned char>(value);
        }
        return true;
    }

    std::string pbkdf2_hash(const std::string &password)
    {
        unsigned char salt[kSaltBytes];
        unsigned char derived[kDerivedBytes];
        if (RAND_bytes(salt, sizeof(salt)) != 1)
            return "";
        if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                              salt, sizeof(salt), kPasswordIterations, EVP_sha256(),
                              sizeof(derived), derived) != 1)
            return "";
        return "pbkdf2_sha256$" + std::to_string(kPasswordIterations) + "$" +
               to_hex(salt, sizeof(salt)) + "$" + to_hex(derived, sizeof(derived));
    }

    bool verify_pbkdf2(const std::string &password, const std::string &stored)
    {
        const size_t first = stored.find('$');
        const size_t second = stored.find('$', first + 1);
        const size_t third = stored.find('$', second + 1);
        if (first == std::string::npos || second == std::string::npos || third == std::string::npos)
            return false;
        if (stored.substr(0, first) != "pbkdf2_sha256")
            return false;

        int iterations = 0;
        try
        {
            iterations = std::stoi(stored.substr(first + 1, second - first - 1));
        }
        catch (...)
        {
            return false;
        }
        if (iterations < 100000)
            return false;

        unsigned char salt[kSaltBytes];
        unsigned char expected[kDerivedBytes];
        unsigned char actual[kDerivedBytes];
        if (!from_hex(stored.substr(second + 1, third - second - 1), salt, sizeof(salt)) ||
            !from_hex(stored.substr(third + 1), expected, sizeof(expected)))
            return false;
        if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                              salt, sizeof(salt), iterations, EVP_sha256(),
                              sizeof(actual), actual) != 1)
            return false;
        return CRYPTO_memcmp(actual, expected, sizeof(actual)) == 0;
    }
}

namespace smartnas
{
    namespace db
    {

        bool DatabaseManager::register_user(const std::string &username, const std::string &password)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            std::string hash = pbkdf2_hash(password);
            if (hash.empty())
                return false;

            std::string sql = "INSERT INTO users (username, password_hash) VALUES (?, ?);";
            sqlite3_stmt *stmt;
            bool success = false;

            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_DONE)
                {
                    success = true;
                }
                sqlite3_finalize(stmt);
            }
            return success;
        }

        bool DatabaseManager::authenticate_user(const std::string &username, const std::string &password)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            std::string sql = "SELECT password_hash FROM users WHERE username=?;";
            sqlite3_stmt *stmt;
            bool success = false;
            std::string upgraded_hash;

            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW)
                {
                    const char *stored_ptr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
                    const std::string stored = stored_ptr ? stored_ptr : "";
                    success = verify_pbkdf2(password, stored);
                    if (!success && stored == utils::HashUtil::sha256(password.c_str(), password.size()))
                    {
                        success = true;
                        upgraded_hash = pbkdf2_hash(password);
                    }
                }
                sqlite3_finalize(stmt);
            }
            if (success && !upgraded_hash.empty())
            {
                sqlite3_stmt *update_stmt = nullptr;
                const char *update_sql = "UPDATE users SET password_hash=? WHERE username=?;";
                if (sqlite3_prepare_v2(db_, update_sql, -1, &update_stmt, nullptr) == SQLITE_OK)
                {
                    sqlite3_bind_text(update_stmt, 1, upgraded_hash.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(update_stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(update_stmt);
                    sqlite3_finalize(update_stmt);
                }
            }
            return success;
        }

    } // namespace db
} // namespace smartnas
