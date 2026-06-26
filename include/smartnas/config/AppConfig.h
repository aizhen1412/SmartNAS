#pragma once

#include <filesystem>
#include <string>

namespace smartnas::config
{
    class AppConfig
    {
    public:
        static AppConfig &get_instance();
        bool load(const std::string &path);

        unsigned short core_port() const { return core_port_; }
        unsigned short agent_port() const { return agent_port_; }
        std::size_t upload_chunk_size() const { return upload_chunk_size_; }
        unsigned int upload_concurrency() const { return upload_concurrency_; }
        std::size_t web_crypto_limit() const { return web_crypto_limit_; }
        const std::string &core_host() const { return core_host_; }
        const std::string &agent_host() const { return agent_host_; }
        const std::string &jwt_secret() const { return jwt_secret_; }
        const std::filesystem::path &database_path() const { return database_path_; }
        const std::filesystem::path &data_dir() const { return data_dir_; }
        const std::filesystem::path &web_dir() const { return web_dir_; }
        std::filesystem::path data_path(const std::string &name) const;
        std::filesystem::path web_path(const std::string &name) const;

    private:
        AppConfig() = default;

        unsigned short core_port_ = 8080;
        unsigned short agent_port_ = 8081;
        std::size_t upload_chunk_size_ = 8 * 1024 * 1024;
        unsigned int upload_concurrency_ = 4;
        std::size_t web_crypto_limit_ = 256 * 1024 * 1024;
        std::string core_host_ = "127.0.0.1";
        std::string agent_host_ = "127.0.0.1";
        std::string jwt_secret_;
        std::filesystem::path database_path_ = "var/db/smartnas.db";
        std::filesystem::path data_dir_ = "var/data";
        std::filesystem::path web_dir_ = "web";
    };
}
