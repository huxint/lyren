#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lyren::net {

struct Get {
    std::string url;
    uint64_t from = 0, end = 0;  // byte range [from, end); end 0 = to the end of the file
    std::vector<std::string> headers;
    std::chrono::milliseconds delay{0};  // start later, for retries
};

// Called on the network thread; keep them short.
struct Stream {
    // Before the first byte: where the body starts and the full resource size (0 if unknown).
    std::function<void(uint64_t offset, uint64_t total)> head;
    std::function<void(uint64_t offset, std::span<const std::byte> data)> data;
    // status is the HTTP status, 0 without a response; error is empty on success. Not called after cancel().
    std::function<void(long status, std::string error)> done;
};

// curl multi on its own thread.
class Http {
public:
    using Id = uint64_t;

    Http();
    ~Http();
    Http(const Http&) = delete;
    Http& operator=(const Http&) = delete;

    // Any thread.
    Id stream(Get get, Stream cb);
    void cancel(Id id);

    struct Transfer;

private:
    void run();
    std::chrono::milliseconds apply();

    void* multi_;
    std::thread thread_;
    std::mutex m_;
    bool quit_ = false;
    Id next_ = 1;
    std::vector<std::unique_ptr<Transfer>> adds_;  // waiting for the network thread, some for their start time
    std::vector<Id> cancels_;
    std::unordered_map<Id, std::unique_ptr<Transfer>> live_;  // network thread only
};

}  // namespace lyren::net
