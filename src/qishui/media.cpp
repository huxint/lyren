#include "qishui/media.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <limits>
#include <tuple>

#include "core/encoding.hpp"

namespace lyren::qishui {

    namespace {
        bool timing(std::string_view tag, uint32_t &start, uint32_t &duration) {
            auto comma = tag.find(',');
            if (comma == tag.npos) {
                return false;
            }
            auto [first, error] = std::from_chars(tag.data(), tag.data() + comma, start);
            auto [last, error2] = std::from_chars(tag.data() + comma + 1, tag.data() + tag.size(), duration);
            return error == std::errc{} && error2 == std::errc{} && first == tag.data() + comma && last == tag.data() + tag.size();
        }
    } // namespace

    std::vector<LyricLine> parse_lyrics(std::string_view text, std::string_view format) {
        if (format == "krc") {
            return lyren::parse_krc(text);
        }
        auto result = lyren::parse_lyrics(text);
        size_t next = 0;
        while (next < text.size()) {
            const size_t end = std::min(text.find('\n', next), text.size());
            auto line = text.substr(next, end - next);
            next = end + 1;
            if (line.ends_with('\r')) {
                line.remove_suffix(1);
            }
            if (!line.starts_with('[')) {
                continue;
            }
            const size_t close = line.find(']');
            uint32_t start = 0, duration = 0;
            if (close == line.npos || !timing(line.substr(1, close - 1), start, duration)) {
                continue;
            }
            LyricLine parsed{.time = start / 1000.0};
            line.remove_prefix(close + 1);
            size_t position = 0, segment = 0;
            std::optional<std::pair<uint32_t, uint32_t>> word;
            auto append = [&](std::string_view part) {
                const auto from = static_cast<uint32_t>(parsed.text.size());
                parsed.text += part;
                if (word && !part.empty()) {
                    parsed.words.push_back({word->first, word->second, from, static_cast<uint32_t>(parsed.text.size())});
                }
            };
            while ((position = line.find('<', position)) != line.npos) {
                const auto tag_end = line.find('>', position + 1);
                uint32_t at = 0, length = 0;
                if (tag_end == line.npos || !timing(line.substr(position + 1, tag_end - position - 1), at, length)) {
                    ++position;
                    continue;
                }
                append(line.substr(segment, position - segment));
                word = std::pair{at, length};
                position = segment = tag_end + 1;
            }
            append(line.substr(segment));
            if (!parsed.text.empty()) {
                result.push_back(std::move(parsed));
            }
        }
        std::ranges::stable_sort(result, {}, &LyricLine::time);
        return result;
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
            if (auto own = text(field(field(value, "encrypt_info"), "spade_a")); !own.empty()) {
                auth = own;
            }
            if (auto own = number(field(value, "video_duration")); own > 0) {
                duration = own;
            }
            const bool pascal = value.contains("MainPlayUrl") || value.contains("BackupPlayUrl");
            auto url = text(field(value, pascal ? "MainPlayUrl" : "main_play_url"));
            if (url.empty()) {
                url = text(field(value, "main_url"));
            }
            if (url.empty()) {
                url = text(field(value, pascal ? "BackupPlayUrl" : "backup_play_url"));
            }
            if (url.empty()) {
                url = text(field(value, "backup_url"));
            }
            if (!url.empty()) {
                const auto &meta = field(value, "video_meta");
                if (auto own = text(field(value, pascal ? "PlayAuth" : "play_auth")); !own.empty()) {
                    auth = own;
                }
                if (auto own = number(field(value, pascal ? "Duration" : "duration")); own > 0) {
                    duration = own;
                }
                auto quality = text(field(value, pascal ? "Quality" : "quality"));
                if (quality.empty()) {
                    quality = text(field(value, "definition"));
                }
                if (quality.empty()) {
                    quality = text(field(meta, "quality"));
                }
                double bitrate = number(field(value, pascal ? "Bitrate" : "bitrate"));
                if (bitrate <= 0) {
                    bitrate = number(field(meta, "bitrate"));
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
        const auto &player = field(response, "track_player");
        const auto &raw_model = field(player, "video_model");
        Json model;
        if (raw_model.is_string()) {
            if (glz::read_json(model, raw_model.get<std::string>())) {
                return std::unexpected("Qishui Music returned invalid playback information");
            }
            candidates(model, select);
        } else {
            candidates(raw_model, select);
        }
        candidates(field(field(field(response, "track"), "audio_info"), "play_info_list"), select);
        candidates(field(field(field(response, "Result"), "Data"), "PlayInfoList"), select);
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
