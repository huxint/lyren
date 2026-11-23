#pragma once
#include <optional>
#include <string>
#include <vector>

#include "app/env.hpp"
#include "sys/creds.hpp"

namespace lyren {

    class Browser;
    class Library;

    // Signing in and out of the platforms, and the credentials that keep the sessions between runs.
    class Accounts {
    public:
        // A sign-in under way: the platform's QR code, or connection settings pasted in.
        struct SignIn {
            std::optional<Source> source; // empty: none is under way
            double since = -10;           // when the step on show changed
            // The code to scan: modules row by row, or the service's own picture, kept in the image cache as "qr".
            std::vector<uint8_t> modules;
            int size = 0;
            bool image = false;
            std::optional<QrStatus> status;
            std::string error;
            std::string settings; // pasted connection settings
            bool importing = false;

            bool has_code() const {
                return image || (size > 0 && !modules.empty());
            }
        };

        Accounts(Env &env, Library &library, Browser &browser);
        ~Accounts();
        Accounts(const Accounts &) = delete;
        Accounts &operator=(const Accounts &) = delete;

        SignIn &sign_in() {
            return sign_in_;
        }
        // Shows the platform's sign-in; one that takes a QR code starts fetching it.
        void begin(Source source);
        // Back to the list of accounts. A late answer cannot sign anyone in.
        void cancel();
        void refresh_code();
        void import_settings();
        void sign_out(Source source);
        // Synchronously writes any unsaved session changes before shutdown.
        void flush();

    private:
        Task<void> startup();
        Task<void> initialize(Source source);
        Task<void> scan();
        Task<void> import(Source source);
        // The session is in: the account's data replaces whatever was there.
        Task<void> arrived(Source source, uint64_t gen);
        void save(bool immediate = false);
        Task<void> write();
        std::string snapshot();

        Env &env_;
        Library &library_;
        Browser &browser_;
        bool can_save_ = false; // the stored credentials were read, so writing them back loses nothing
        bool dirty_ = false;    // session state changed since the last completed write
        bool saving_ = false;   // a write is currently in flight
        creds::Credentials credentials_;
        std::string saved_json_;
        Loop::TimerId save_timer_ = 0;
        uint64_t save_gen_ = 0;
        SignIn sign_in_;
        uint64_t gen_ = 0; // disowns the sign-in requests under way
    };

} // namespace lyren
