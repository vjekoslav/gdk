#include <cstring>

#include "memory.hpp"
#include "network_parameters.hpp"
#include "utils.hpp"
#include "xpub_hdkey.hpp"

namespace green {

    namespace {
        static const uint32_t GAIT_GENERATION_PATH = harden(0x4741); // 'GA'
        static const unsigned char GAIT_GENERATION_NONCE[30] = { 'G', 'r', 'e', 'e', 'n', 'A', 'd', 'd', 'r', 'e', 's',
            's', '.', 'i', 't', ' ', 'H', 'D', ' ', 'w', 'a', 'l', 'l', 'e', 't', ' ', 'p', 'a', 't', 'h' };
    } // namespace

    xpub_hdkey::xpub_hdkey(bool is_main_net, const xpub_t& xpub, uint32_span_t path)
    {
        const uint32_t version = is_main_net ? BIP32_VER_MAIN_PUBLIC : BIP32_VER_TEST_PUBLIC;
        wally_ext_key_ptr master = bip32_key_init_alloc(version, 0, 0, xpub.first, xpub.second);

        if (!path.empty()) {
            m_ext_key = bip32_public_key_from_parent_path(*master, path);
        } else {
            m_ext_key = *master;
        }
    }

    xpub_hdkey xpub_hdkey::from_public_key(bool is_main_net, byte_span_t public_key)
    {
        GDK_RUNTIME_ASSERT(public_key.size() == EC_PUBLIC_KEY_LEN);
        GDK_VERIFY(wally_ec_public_key_verify(public_key.data(), public_key.size()));
        xpub_t xpub{ { 0 }, { 0 } };
        memcpy(xpub.second.data(), public_key.data(), public_key.size());
        return { is_main_net, xpub };
    }

    xpub_hdkey::~xpub_hdkey() { wally_bzero(&m_ext_key, sizeof(m_ext_key)); }

    xpub_hdkey xpub_hdkey::derive(uint32_span_t path)
    {
        return xpub_hdkey(bip32_public_key_from_parent_path(m_ext_key, path));
    }

    xpub_t xpub_hdkey::to_xpub_t() const { return make_xpub(&m_ext_key); }

    pub_key_t xpub_hdkey::get_public_key() const
    {
        pub_key_t ret;
        std::copy(m_ext_key.pub_key, m_ext_key.pub_key + ret.size(), ret.begin());
        return ret;
    }

    std::vector<unsigned char> xpub_hdkey::get_fingerprint() const
    {
        auto copy = m_ext_key;
        return bip32_key_get_fingerprint(copy);
    }

    std::string xpub_hdkey::to_base58() const { return bip32_key_to_base58(&m_ext_key, BIP32_FLAG_KEY_PUBLIC); }

    std::string xpub_hdkey::to_hashed_identifier(const std::string& network) const
    {
        // Return a hashed id from which the xpub cannot be extracted
        const auto key_data = bip32_key_serialize(m_ext_key, BIP32_FLAG_KEY_PUBLIC);
        const auto hashed = pbkdf2_hmac_sha512_256(key_data, ustring_span(network));
        return b2h(hashed);
    }

    xpub_hdkeys::xpub_hdkeys(const network_parameters& net_params)
        : m_is_main_net(net_params.is_main_net())
        , m_is_liquid(net_params.is_liquid())
    {
    }

    xpub_hdkeys::xpub_hdkeys(const network_parameters& net_params, const xpub_t& xpub)
        : m_is_main_net(net_params.is_main_net())
        , m_xpub(xpub)
    {
    }

    xpub_hdkey xpub_hdkeys::derive(uint32_t subaccount, uint32_t pointer, std::optional<bool> is_internal)
    {
        std::vector<uint32_t> path;
        if (is_internal.has_value()) {
            path.push_back(*is_internal ? 1u : 0u);
        }
        path.push_back(pointer);
        return get_subaccount(subaccount).derive(path);
    }

    ga_pubkeys::ga_pubkeys(const network_parameters& net_params, uint32_span_t gait_path)
        : xpub_hdkeys(net_params, make_xpub(net_params.chain_code(), net_params.pub_key()))
    {
        GDK_RUNTIME_ASSERT(static_cast<size_t>(gait_path.size()) == m_gait_path.size());
        std::copy(std::begin(gait_path), std::end(gait_path), m_gait_path.begin());
        get_subaccount(0); // Initialize main account
    }

    std::vector<uint32_t> ga_pubkeys::get_subaccount_root_path(uint32_t subaccount) const
    {
        // Note: This assumes address version v1+.
        // Version 0 addresses are not derived from the users gait_path
        const uint32_t path_prefix = subaccount != 0 ? 3 : 1;
        std::vector<uint32_t> path(m_gait_path.size() + 1);
        init_container(path, gsl::make_span(&path_prefix, 1), m_gait_path);
        if (subaccount != 0) {
            path.push_back(subaccount);
        }
        return path;
    }

    std::vector<uint32_t> ga_pubkeys::get_subaccount_full_path(
        uint32_t subaccount, uint32_t pointer, bool /*is_internal*/) const
    {
        auto path = get_subaccount_root_path(subaccount);
        path.push_back(pointer);
        return path;
    }

    xpub_hdkey ga_pubkeys::get_subaccount(uint32_t subaccount)
    {
        // Note unlike user pubkeys, the Green key is not privately derived,
        // since the user must be able to derive it from the Green service xpub.
        const auto p = m_subaccounts.find(subaccount);
        if (p != m_subaccounts.end()) {
            return p->second;
        }
        const auto path = get_subaccount_root_path(subaccount);
        return m_subaccounts.insert(std::make_pair(subaccount, xpub_hdkey(m_is_main_net, m_xpub, path))).first->second;
    }

    std::array<uint32_t, 1> ga_pubkeys::get_gait_generation_path()
    {
        return std::array<uint32_t, 1>{ { GAIT_GENERATION_PATH } };
    }

    std::array<unsigned char, HMAC_SHA512_LEN> ga_pubkeys::get_gait_path_bytes(const xpub_t& xpub)
    {
        std::array<unsigned char, sizeof(chain_code_t) + sizeof(pub_key_t)> path_data;
        init_container(path_data, xpub.first, xpub.second);
        return hmac_sha512(GAIT_GENERATION_NONCE, path_data);
    }

    ga_user_pubkeys::ga_user_pubkeys(const network_parameters& net_params)
        : user_pubkeys(net_params)
    {
    }

    ga_user_pubkeys::ga_user_pubkeys(const network_parameters& net_params, const xpub_t& xpub)
        : user_pubkeys(net_params, xpub)
    {
        add_subaccount(0, m_xpub);
    }

    std::vector<uint32_t> ga_user_pubkeys::get_ga_subaccount_root_path(uint32_t subaccount)
    {
        if (subaccount != 0u) {
            return std::vector<uint32_t>({ harden(3), harden(subaccount) });
        }
        return std::vector<uint32_t>();
    }

    std::vector<uint32_t> ga_user_pubkeys::get_subaccount_root_path(uint32_t subaccount) const
    {
        return get_ga_subaccount_root_path(subaccount); // Defer to static impl
    }

    std::vector<uint32_t> ga_user_pubkeys::get_ga_subaccount_full_path(
        uint32_t subaccount, uint32_t pointer, bool /*is_internal*/)
    {
        if (subaccount != 0u) {
            return std::vector<uint32_t>({ harden(3), harden(subaccount), 1, pointer });
        }
        return std::vector<uint32_t>({ 1, pointer });
    }

    std::vector<uint32_t> ga_user_pubkeys::get_subaccount_full_path(
        uint32_t subaccount, uint32_t pointer, bool is_internal) const
    {
        return get_ga_subaccount_full_path(subaccount, pointer, is_internal); // Defer to static impl
    }

    bool ga_user_pubkeys::have_subaccount(uint32_t subaccount)
    {
        return m_subaccounts.find(subaccount) != m_subaccounts.end();
    }

    void ga_user_pubkeys::add_subaccount(uint32_t subaccount, const xpub_t& xpub)
    {
        std::array<uint32_t, 1> path{ { 1 } };
        auto user_key = xpub_hdkey(m_is_main_net, xpub, path);
        const auto ret = m_subaccounts.emplace(subaccount, user_key);
        if (!ret.second) {
            // Subaccount is already present; xpub must match whats already there
            GDK_RUNTIME_ASSERT(ret.first->second.to_xpub_t() == user_key.to_xpub_t());
        }
    }

    void ga_user_pubkeys::remove_subaccount(uint32_t subaccount)
    {
        // Removing subaccounts is not supported for Green multisig wallets
        (void)subaccount;
        GDK_RUNTIME_ASSERT(false);
    }

    xpub_hdkey ga_user_pubkeys::get_subaccount(uint32_t subaccount)
    {
        const auto p = m_subaccounts.find(subaccount);
        GDK_RUNTIME_ASSERT(p != m_subaccounts.end());
        return p->second;
    }

    bip44_pubkeys::bip44_pubkeys(const network_parameters& net_params)
        : user_pubkeys(net_params)
    {
    }

    std::vector<uint32_t> bip44_pubkeys::get_bip44_subaccount_root_path(
        bool is_main_net, bool is_liquid, uint32_t subaccount)
    {
        const std::array<uint32_t, 3> purpose_lookup{ 49, 84, 44 };
        const uint32_t purpose = purpose_lookup.at(subaccount % 16);
        const uint32_t coin_type = is_main_net ? (is_liquid ? 1776 : 0) : 1;
        const uint32_t account = subaccount / 16;
        return std::vector<uint32_t>{ harden(purpose), harden(coin_type), harden(account) };
    }

    std::vector<uint32_t> bip44_pubkeys::get_bip44_subaccount_full_path(
        bool is_main_net, bool is_liquid, uint32_t subaccount, uint32_t pointer, bool is_internal)
    {
        auto path = get_bip44_subaccount_root_path(is_main_net, is_liquid, subaccount);
        path.emplace_back(is_internal ? 1 : 0);
        path.emplace_back(pointer);
        return path;
    }

    std::vector<uint32_t> bip44_pubkeys::get_subaccount_root_path(uint32_t subaccount) const
    {
        return get_bip44_subaccount_root_path(m_is_main_net, m_is_liquid, subaccount);
    }

    std::vector<uint32_t> bip44_pubkeys::get_subaccount_full_path(
        uint32_t subaccount, uint32_t pointer, bool is_internal) const
    {
        return get_bip44_subaccount_full_path(m_is_main_net, m_is_liquid, subaccount, pointer, is_internal);
    }

    bool bip44_pubkeys::have_subaccount(uint32_t subaccount)
    {
        return m_subaccounts.find(subaccount) != m_subaccounts.end();
    }

    void bip44_pubkeys::add_subaccount(uint32_t subaccount, const xpub_t& xpub)
    {
        auto user_key = xpub_hdkey(m_is_main_net, xpub);
        const auto ret = m_subaccounts.emplace(subaccount, user_key);
        if (!ret.second) {
            // Subaccount is already present; xpub must match whats already there
            GDK_RUNTIME_ASSERT(ret.first->second.to_xpub_t() == user_key.to_xpub_t());
        }
    }

    void bip44_pubkeys::remove_subaccount(uint32_t subaccount)
    {
        // Removing subaccounts is not supported
        (void)subaccount;
        GDK_RUNTIME_ASSERT(false);
    }

    xpub_hdkey bip44_pubkeys::get_subaccount(uint32_t subaccount)
    {
        const auto p = m_subaccounts.find(subaccount);
        GDK_RUNTIME_ASSERT(p != m_subaccounts.end());
        return p->second;
    }

} // namespace green
