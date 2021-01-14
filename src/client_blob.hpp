#ifndef GDK_CLIENT_BLOB_HPP
#define GDK_CLIENT_BLOB_HPP
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>

#include "ga_wally.hpp"
#include <nlohmann/json.hpp>

namespace ga {
namespace sdk {

    // Client-only data, stored on the server as a server-unreadable blob
    class client_blob {
    public:
        client_blob();
        client_blob(const client_blob&) = delete;
        client_blob& operator=(const client_blob&) = delete;
        client_blob(client_blob&&) = delete;
        client_blob& operator=(client_blob&&) = delete;

        void set_subaccount_name(uint32_t subaccount, const std::string& name);
        std::string get_subaccount_name(uint32_t subaccount) const;

        void set_tx_memo(const std::string& txhash_hex, const std::string& memo);
        std::string get_tx_memo(const std::string& txhash_hex) const;

        void load(const byte_span_t& data);
        std::pair<std::vector<unsigned char>, std::string> save(const pbkdf2_hmac512_t& key) const;

        static bool is_zero_hmac(const std::string& hmac);
        static std::string compute_hmac(const pbkdf2_hmac512_t& key, const byte_span_t& data);

    private:
        nlohmann::json m_data;
    };

} // namespace sdk
} // namespace ga

#endif
