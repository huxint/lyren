#include "audio/player.hpp"

#include <algorithm>
#include <cmath>

#include "core/log.hpp"

namespace lyren::audio {

    static constexpr size_t chunk = 4096; // frames per decode call

    Player::Player(Loop &loop, net::Http &http)
    : loop_(loop),
      http_(http),
      out_(wake_, {.drained =
                       [this] {
                           loop_.post([this, gen = writing_gen_.load()] {
                               if (gen != gen_ || state_ != State::playing) {
                                   return;
                               }
                               state_ = State::ended;
                               out_.set_active(false);
                               changed();
                           });
                       },
                   .volume =
                       [this](float linear) {
                           loop_.post([this, v = std::cbrt(linear)] {
                               volume_ = v;
                               changed();
                           });
                       }}) {
        thread_ = std::thread([this] {
            run();
        });
    }

    Player::~Player() {
        if (spool_) {
            spool_->close();
        }
        push({.kind = Cmd::quit});
        thread_.join();
    }

    void Player::push(Cmd c) {
        {
            std::lock_guard lk(m_);
            cmds_.push_back(std::move(c));
        }
        wake_.fetch_add(1, std::memory_order_release);
        wake_.notify_one();
    }

    void Player::changed() {
        if (on_change) {
            on_change();
        }
    }

    void Player::open(std::shared_ptr<Spool> spool, bool play) {
        // Closing wakes the decoder thread if it is waiting for the old track's bytes.
        if (spool_) {
            spool_->close();
        }
        spool_ = std::move(spool);
        ++gen_;
        state_ = State::loading;
        want_play_ = play;
        pending_seek_.reset();
        duration_ = last_pos_ = 0;
        codec_ = {};
        error_.clear();
        out_.set_active(false);
        push({.kind = Cmd::open, .gen = gen_, .spool = spool_});
        changed();
    }

    void Player::play() {
        want_play_ = true;
        if (state_ == State::ended) {
            seek(0);
        }
        if (state_ == State::paused) {
            state_ = State::playing;
            out_.set_active(true);
        }
        changed();
    }

    void Player::pause() {
        want_play_ = false;
        if (state_ == State::playing) {
            state_ = State::paused;
            out_.set_active(false);
        }
        changed();
    }

    void Player::seek(double seconds) {
        if (state_ == State::idle || state_ == State::failed) {
            return;
        }
        seconds = std::max(0.0, duration_ > 0 ? std::min(seconds, duration_) : seconds);
        pending_seek_ = seconds;
        push({.kind = Cmd::seek, .gen = gen_, .seconds = seconds, .seq = ++seek_seq_});
        if (state_ == State::ended) {
            state_ = State::paused;
        }
        changed();
    }

    void Player::stop() {
        if (spool_) {
            spool_->close();
        }
        spool_.reset();
        ++gen_;
        state_ = State::idle;
        pending_seek_.reset();
        duration_ = last_pos_ = 0;
        out_.set_active(false);
        push({.kind = Cmd::stop, .gen = gen_});
        changed();
    }

    void Player::set_volume(float v) {
        volume_ = std::clamp(v, 0.f, 1.f);
        out_.set_volume(volume_ * volume_ * volume_);
    }

    double Player::position() {
        if (pending_seek_) {
            return *pending_seek_;
        }
        if (state_ == State::idle || state_ == State::loading || state_ == State::failed) {
            return 0;
        }
        if (state_ == State::ended) {
            return duration_;
        }
        double p = static_cast<double>(out_.position()) / format_.rate;
        // Latency estimates jitter by a few milliseconds; never step backwards because of that.
        if (p < last_pos_ && last_pos_ - p < 0.25) {
            p = last_pos_;
        }
        last_pos_ = p;
        return duration_ > 0 ? std::min(p, duration_) : p;
    }

    bool Player::buffering() const {
        return state_ == State::playing && spool_ && spool_->waiting() && out_.queued() < format_.rate / 4;
    }

    void Player::run() {
        std::unique_ptr<Decoder> dec;
        std::shared_ptr<Spool> spool;
        uint64_t gen = 0;
        bool done = true;
        std::vector<float> buf;
        auto fail = [this](uint64_t gen, std::string error) {
            loop_.post([this, gen, error = std::move(error)] {
                if (gen != gen_) {
                    return;
                }
                log::warn("playback failed: {}", error);
                state_ = State::failed;
                error_ = error;
                out_.set_active(false);
                changed();
            });
        };

        for (;;) {
            uint32_t seen = wake_.load(std::memory_order_acquire);
            std::vector<Cmd> cmds;
            {
                std::lock_guard lk(m_);
                cmds.swap(cmds_);
            }
            for (size_t i = 0; i < cmds.size(); ++i) {
                Cmd &c = cmds[i];
                if (c.kind == Cmd::quit) {
                    return;
                }
                if (c.kind == Cmd::stop) {
                    dec.reset(), spool.reset(), done = true;
                } else if (c.kind == Cmd::open) {
                    dec.reset(), done = true;
                    spool = std::move(c.spool), gen = c.gen;
                    std::string error;
                    dec = open_decoder(*spool, error);
                    if (!dec) {
                        fail(gen, std::move(error));
                        continue;
                    }
                    if (!out_.start(dec->format)) {
                        dec.reset();
                        fail(gen, "PipeWire is not running");
                        continue;
                    }
                    writing_gen_ = gen, done = false;
                    buf.resize(chunk * dec->format.channels);
                    log::debug("open: {} {} Hz {} ch, {} frames", dec->codec, dec->format.rate, dec->format.channels, dec->length);
                    loop_.post([this, gen, f = dec->format, len = dec->length, codec = dec->codec] {
                        if (gen != gen_) {
                            return;
                        }
                        format_ = f, codec_ = codec;
                        duration_ = static_cast<double>(len) / f.rate;
                        if (state_ == State::loading) {
                            state_ = want_play_ ? State::playing : State::paused;
                            out_.set_active(want_play_);
                        }
                        changed();
                    });
                } else if (c.kind == Cmd::seek) {
                    // While the slider is dragged only the newest of a burst matters.
                    bool superseded = i + 1 < cmds.size() && cmds[i + 1].kind == Cmd::seek;
                    if (!dec || c.gen != gen || superseded) {
                        continue;
                    }
                    uint64_t frame = static_cast<uint64_t>(c.seconds * dec->format.rate);
                    if (dec->length) {
                        frame = std::min(frame, dec->length);
                    }
                    if (!dec->seek(frame)) {
                        log::warn("seek to {} failed", frame);
                    }
                    out_.flush(frame);
                    done = false;
                    loop_.post([this, gen, seq = c.seq] {
                        if (gen != gen_ || seq != seek_seq_) {
                            return;
                        }
                        last_pos_ = *pending_seek_;
                        pending_seek_.reset();
                        changed();
                    });
                }
            }

            if (done) {
                wake_.wait(seen, std::memory_order_acquire);
                continue;
            }
            size_t space = out_.space();
            if (space < out_.capacity() / 4) {
                // The realtime thread bumps wake_ once a quarter of the ring is free again.
                out_.set_waiting(true);
                if (out_.space() < out_.capacity() / 4) {
                    wake_.wait(seen, std::memory_order_acquire);
                }
                out_.set_waiting(false);
                continue;
            }
            size_t n = dec->read(buf.data(), std::min(space, chunk));
            if (!n) {
                done = true;
                out_.finish();
                if (std::string e = spool->error(); !e.empty()) {
                    fail(gen, std::move(e));
                }
                continue;
            }
            out_.write(buf.data(), n);
        }
    }

} // namespace lyren::audio
