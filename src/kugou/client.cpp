#include "kugou/client.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>

#include <array>
#include <chrono>
#include <format>
#include <memory>

#include "core/crypto.hpp"
#include "core/encoding.hpp"
#include "kugou/json.hpp"

namespace lyren::kugou {

    namespace {
        constexpr std::string_view device_public_key = "-----BEGIN PUBLIC KEY-----\n"
                                                       "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDIAG7QOELSYoIJvTFJhMpe1s/gbjDJ"
                                                       "X51HBNnEl5HXqTW6lQ7LC8jr9fWZTwusknp+sVGzwd40MwP6U5yDE27M/X1+UR4tvOG"
                                                       "Oqp94TJtQ1EPnWGWXngpeIW5GxoQGao1rmYWAu6oi1z9XkChrsUdC6DJE5E221wf/4WL"
                                                       "FxwAtRQIDAQAB\n-----END PUBLIC KEY-----\n";

        void append_escaped(std::string &out, std::string_view value) {
            constexpr std::string_view digits = "0123456789ABCDEF";
            for (unsigned char c : value) {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
                    out += static_cast<char>(c);
                } else {
                    out += '%';
                    out += digits[c >> 4];
                    out += digits[c & 15];
                }
            }
        }
    } // namespace

    std::string sign(const Params &query, std::string_view body, std::string_view salt) {
        size_t size = salt.size() * 2 + body.size();
        for (const auto &[name, value] : query) {
            size += name.size() + 1 + value.size();
        }
        std::string input;
        input.reserve(size);
        input += salt;
        for (const auto &[name, value] : query) {
            input += name;
            input += '=';
            input += value;
        }
        input += body;
        input += salt;
        return crypto::md5_hex(input);
    }

    net::Request request(std::string url, const Params &query, std::optional<std::string> body, std::string_view salt, std::vector<std::string> headers) {
        size_t size = url.size() + (salt.empty() ? 0 : 43);
        for (const auto &[name, value] : query) {
            size += name.size() + 2 + value.size();
        }
        url.reserve(size);
        char separator = '?';
        for (const auto &[name, value] : query) {
            url += separator;
            append_escaped(url, name);
            url += '=';
            append_escaped(url, value);
            separator = '&';
        }
        if (!salt.empty()) {
            url += separator;
            url += "signature=";
            url += sign(query, body ? std::string_view(*body) : std::string_view{}, salt);
        }
        headers.emplace_back("Content-Type: application/json");
        return {.url = std::move(url), .body = std::move(body), .headers = std::move(headers)};
    }

    const std::string &Client::get(std::string_view name) const {
        static const std::string empty;
        auto it = session.find(name);
        return it == session.end() ? empty : it->second;
    }

    bool Client::logged_in() const {
        return !get("userid").empty() && !get("token").empty();
    }

    Params Client::common() const {
        using namespace std::chrono;
        return {{"dfid", get("dfid").empty() ? "-" : get("dfid")},
                {"mid", get("mid")},
                {"uuid", "-"},
                {"appid", "1005"},
                {"clientver", "20489"},
                {"clienttime", std::to_string(duration_cast<seconds>(system_clock::now().time_since_epoch()).count())}};
    }

    std::vector<std::string> Client::headers(const Params &query, std::string_view router) const {
        std::vector<std::string> out{"dfid: " + query.at("dfid"), "mid: " + query.at("mid"), "clienttime: " + query.at("clienttime"),
                                     "User-Agent: Android712-AndroidPhone-11451-376-0-FeeCacheUpdate-wifi"};
        if (!router.empty()) {
            out.emplace_back("x-router: " + std::string(router));
        }
        return out;
    }

    Result<net::Request> Client::device_request(std::string_view secret) const {
        auto query = common();
        if (logged_in()) {
            query.insert({{"userid", get("userid")}, {"token", get("token")}});
        }
        const auto portrait = json::write(json::Value{{"aes", std::string(secret)}, {"uid", logged_in() ? json::Value(get("userid")) : json::Value(0)}, {"token", get("token")}});
        std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(device_public_key.data(), static_cast<int>(device_public_key.size())), BIO_free);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> rsa(EVP_PKEY_CTX_new(key.get(), nullptr), EVP_PKEY_CTX_free);
        EVP_PKEY_encrypt_init(rsa.get());
        EVP_PKEY_CTX_set_rsa_padding(rsa.get(), RSA_PKCS1_PADDING);
        size_t size = static_cast<size_t>(EVP_PKEY_get_size(key.get()));
        std::string encrypted(size, '\0');
        if (EVP_PKEY_encrypt(rsa.get(), reinterpret_cast<unsigned char *>(encrypted.data()), &size, reinterpret_cast<const unsigned char *>(portrait.data()), portrait.size()) !=
            1) {
            return std::unexpected(std::string(tr("Could not prepare KuGou Music device registration")));
        }
        encrypted.resize(size);
        query.insert({{"part", "1"}, {"platid", "1"}, {"p", encoding::hex(encrypted)}});

        // Register this Linux client with its persistent ID; no phone hardware or sensor data is claimed.
        json::Value device{{"availableRamSize", 0}, {"availableRomSize", 0},    {"availableSDSize", 0}, {"basebandVer", ""}, {"batteryLevel", 0}, {"batteryStatus", 1},
                           {"brand", "Linux"},      {"buildSerial", "unknown"}, {"device", "lyren"},    {"imei", ""},        {"imsi", ""},        {"manufacturer", ""},
                           {"uuid", get("mid")}};
        for (auto sensor : {"accelerometer", "gravity", "gyroscope", "light", "magnetic", "orientation", "pressure", "step_counter", "temperature"}) {
            device[sensor] = false;
            device[std::string(sensor) + "Value"] = "";
        }
        const auto plain = json::write(device), digest = crypto::md5_hex(secret);
        std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> aes(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
        EVP_EncryptInit_ex(aes.get(), EVP_aes_128_cbc(), nullptr, reinterpret_cast<const unsigned char *>(digest.data()),
                           reinterpret_cast<const unsigned char *>(digest.data() + 16));
        std::string body(plain.size() + 16, '\0');
        int first = 0, last = 0;
        auto *out = reinterpret_cast<unsigned char *>(body.data());
        EVP_EncryptUpdate(aes.get(), out, &first, reinterpret_cast<const unsigned char *>(plain.data()), static_cast<int>(plain.size()));
        EVP_EncryptFinal_ex(aes.get(), out + first, &last);
        body.resize(static_cast<size_t>(first + last));
        auto req = request("https://userservice.kugou.com/risk/v3/r_register_dev", query, encoding::base64(body), android_salt, headers(query));
        // The encrypted Base64 is the entire form body, without a field name or JSON quotes.
        req.headers.back() = "Content-Type: application/x-www-form-urlencoded";
        return req;
    }

    Result<std::string> parse_device_response(std::string_view body, std::string_view secret) {
        std::string plain;
        // Service errors may be plain JSON; successful registration replies are AES-CBC bytes.
        if (!json::parse(body)) {
            const auto digest = crypto::md5_hex(secret);
            std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> aes(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
            EVP_DecryptInit_ex(aes.get(), EVP_aes_128_cbc(), nullptr, reinterpret_cast<const unsigned char *>(digest.data()),
                               reinterpret_cast<const unsigned char *>(digest.data() + 16));
            plain.resize(body.size() + 16);
            int first = 0, last = 0;
            auto *out = reinterpret_cast<unsigned char *>(plain.data());
            if (EVP_DecryptUpdate(aes.get(), out, &first, reinterpret_cast<const unsigned char *>(body.data()), static_cast<int>(body.size())) != 1 ||
                EVP_DecryptFinal_ex(aes.get(), out + first, &last) != 1) {
                return std::unexpected(std::string(tr("KuGou Music returned an invalid device registration")));
            }
            plain.resize(static_cast<size_t>(first + last));
            body = plain;
        }
        return json::response(body, true).and_then([](const json::Value &value) -> Result<std::string> {
            const auto *dfid = json::get(json::get(value, "data"), "dfid").get_if<std::string>();
            if (!dfid || dfid->empty() || *dfid == "-") {
                return std::unexpected(std::string(tr("KuGou Music returned an invalid device registration")));
            }
            return *dfid;
        });
    }

    Task<Result<void>> Client::ensure_device() {
        if (get("mid").empty()) {
            session["mid"] = crypto::random_hex(32);
        }
        if (!get("dfid").empty() && get("dfid") != "-") {
            co_return Result<void>{};
        }
        constexpr std::string_view alphabet = "0123456789abcdefghijklmnopqrstuvwxyz";
        std::array<unsigned char, 6> random;
        RAND_bytes(random.data(), static_cast<int>(random.size()));
        std::string secret;
        for (auto byte : random) {
            secret += alphabet[byte % alphabet.size()];
        }
        auto req = device_request(secret);
        if (!req) {
            co_return std::unexpected(req.error());
        }
        auto reply = co_await send(std::move(*req), net::Retry::never);
        if (!reply) {
            co_return std::unexpected(reply.error());
        }
        auto dfid = parse_device_response(*reply, secret);
        if (!dfid) {
            co_return std::unexpected(dfid.error());
        }
        session["dfid"] = std::move(*dfid);
        if (on_session) {
            on_session();
        }
        co_return Result<void>{};
    }

    Task<Result<std::string>> Client::send(net::Request req, net::Retry retry) {
        const auto generation = session_gen;
        auto reply = co_await net::fetch_retry(loop_, http_, std::move(req), retry);
        if (generation != session_gen) {
            co_return std::unexpected("KuGou Music session changed; retry the action");
        }
        if (!reply.error.empty()) {
            co_return std::unexpected("Network connection failed");
        }
        if (reply.status < 200 || reply.status >= 300) {
            co_return std::unexpected(std::format("KuGou Music request failed (HTTP {})", reply.status));
        }
        co_return std::move(reply.body);
    }

} // namespace lyren::kugou
