#include "qishui/media.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <limits>
#include <tuple>

#include "core/encoding.hpp"

namespace lyren::qishui {

    std::vector<LyricLine> parse_lyrics(std::string_view text, std::string_view format) {
        return format == "krc" ? lyren::parse_krc(text) : lyren::parse_timed_lrc(text);
    }

    Result<crypto::Aes128Key> stream_key(std::string_view play_auth) {
        const auto bytes = encoding::unbase64(play_auth);
        constexpr auto invalid = "Qishui Music returned an invalid audio key";
        if (bytes.size() < 3) {
            return std::unexpected(invalid);
        }
        const int padding = (static_cast<unsigned char>(bytes[0]) ^ static_cast<unsigned char>(bytes[1]) ^ static_cast<unsigned char>(bytes[2])) - 48;
        if (padding < 0 || static_cast<size_t>(padding) >= bytes.size() - 1) {
            return std::unexpected(invalid);
        }
        const auto input = std::string_view(bytes).substr(1, bytes.size() - padding - 1);
        std::string decoded;
        decoded.reserve(input.size());
        for (size_t i = 0; i < input.size(); ++i) {
            const unsigned char previous = i < 2 ? (i == 0 ? 0xfa : 0x55) : static_cast<unsigned char>(input[i - 2]);
            int value = (static_cast<unsigned char>(input[i]) ^ previous) - std::popcount(i) - 21;
            if (value < 0) {
                value += 255;
            }
            decoded += static_cast<char>(value);
        }
        const char first = decoded.front();
        const int skip = first >= '0' && first <= '9' ? first - '0' : first >= 'a' && first <= 'z' ? first - 'a' + 10 : -1;
        if (skip < 0 || decoded.size() != static_cast<size_t>(skip) + 33) {
            return std::unexpected(invalid);
        }
        crypto::Aes128Key key;
        for (size_t i = 1; i < 33; i += 2) {
            unsigned byte = 0;
            const auto [end, error] = std::from_chars(decoded.data() + i, decoded.data() + i + 2, byte, 16);
            if (error != std::errc{} || end != decoded.data() + i + 2) {
                return std::unexpected(invalid);
            }
            key[(i - 1) / 2] = static_cast<unsigned char>(byte);
        }
        return key;
    }

    namespace {
        struct Candidate {
            std::string_view url, quality, auth;
            double duration = 0, bitrate = 0;
            int rank = 0;
        };

        int quality_rank(std::string_view quality) {
            if (quality == "hi_res") {
                return 4;
            }
            if (quality == "lossless") {
                return 3;
            }
            if (quality == "highest") {
                return 2;
            }
            return quality == "higher" ? 1 : 0;
        }

        void candidates(const Json &value, const auto &select, std::string_view auth = {}, double duration = 0) {
            if (value.is_array()) {
                for (const auto &child : value.get_array()) {
                    candidates(child, select, auth, duration);
                }
                return;
            }
            if (!value.is_object()) {
                return;
            }
            if (auto own = json::text_view(json::field(json::field(value, "encrypt_info"), "spade_a")); !own.empty()) {
                auth = own;
            }
            if (auto own = json::number(json::field(value, "video_duration")); own > 0) {
                duration = own;
            }
            const bool pascal = value.contains("MainPlayUrl") || value.contains("BackupPlayUrl");
            auto url = json::text_view(json::field(value, pascal ? "MainPlayUrl" : "main_play_url"));
            if (url.empty()) {
                url = json::text_view(json::field(value, "main_url"));
            }
            if (url.empty()) {
                url = json::text_view(json::field(value, pascal ? "BackupPlayUrl" : "backup_play_url"));
            }
            if (url.empty()) {
                url = json::text_view(json::field(value, "backup_url"));
            }
            if (!url.empty()) {
                const auto &meta = json::field(value, "video_meta");
                if (auto own = json::text_view(json::field(value, pascal ? "PlayAuth" : "play_auth")); !own.empty()) {
                    auth = own;
                }
                if (auto own = json::number(json::field(value, pascal ? "Duration" : "duration")); own > 0) {
                    duration = own;
                }
                auto quality = json::text_view(json::field(value, pascal ? "Quality" : "quality"));
                if (quality.empty()) {
                    quality = json::text_view(json::field(value, "definition"));
                }
                if (quality.empty()) {
                    quality = json::text_view(json::field(meta, "quality"));
                }
                double bitrate = json::number(json::field(value, pascal ? "Bitrate" : "bitrate"));
                if (bitrate <= 0) {
                    bitrate = json::number(json::field(meta, "bitrate"));
                }
                if ((url.starts_with("https://") || url.starts_with("http://")) && quality != "spatial" && quality != "immersive") {
                    select(Candidate{url, quality, auth, duration, bitrate, quality_rank(quality)});
                }
                return;
            }
            for (const auto &[name, child] : value.get_object()) {
                candidates(child, select, auth, duration);
            }
        }
    } // namespace

    Result<Stream> parse_stream(const Json &response, Quality quality, uint32_t track_duration) {
        const int desired = static_cast<int>(quality);
        // Borrow candidate strings while comparing; only the selected stream is copied.
        auto score = [desired, track_duration](const Candidate &item) {
            const bool complete = item.duration <= 0 || track_duration == 0 || item.duration >= track_duration - 2.0;
            const int fit = item.rank <= desired ? item.rank : -item.rank;
            return std::tuple{complete, fit, item.bitrate};
        };
        std::optional<Candidate> best;
        auto select = [&](Candidate item) {
            if (!best || score(item) > score(*best)) {
                best = item;
            }
        };
        const auto &player = json::field(response, "track_player");
        const auto &raw_model = json::field(player, "video_model");
        Json model;
        if (raw_model.is_string()) {
            if (glz::read_json(model, raw_model.get<std::string>())) {
                return std::unexpected("Qishui Music returned invalid playback information");
            }
            candidates(model, select);
        } else {
            candidates(raw_model, select);
        }
        candidates(json::field(json::field(json::field(response, "track"), "audio_info"), "play_info_list"), select);
        candidates(json::field(json::field(json::field(response, "Result"), "Data"), "PlayInfoList"), select);
        if (!best) {
            return std::unexpected("Qishui Music did not provide playable audio for this account");
        }
        Stream stream{.url = std::string(best->url), .level = std::string(best->quality)};
        stream.trial = best->duration > 0 && track_duration > 0 && best->duration < track_duration - 2.0;
        if (!best->auth.empty()) {
            auto key = stream_key(best->auth);
            if (!key) {
                return std::unexpected(key.error());
            }
            stream.decrypt_key = std::move(*key);
        }
        return stream;
    }

} // namespace lyren::qishui
