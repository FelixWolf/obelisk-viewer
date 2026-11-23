#pragma once

// Remembered accounts: which ones (grid, name and picture, in accounts.llsd,
// LLSD notation like settings.llsd; nothing secret) and their secrets (the password's hash, and the
// MFA hash a two-factor login returns, which spares the authenticator code for a while) in the
// system's credential store: the Secret Service on Linux (GNOME Keyring, KWallet), the Keychain on
// macOS, the Credential Manager on Windows. Never the plain password: the viewer only ever needs
// its hash.

#include "viewer/core/Uuid.hpp"
#include "viewer/llsd/Value.hpp"
#include "viewer/session/Login.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace viewer
{

// Where secrets are kept, by service and user: the system's store, or
// (tests) memory.
class SecretStore
{
public:
    virtual ~SecretStore() = default;
    virtual std::optional<std::string> get(const std::string& service, const std::string& user) = 0;
    virtual bool set(const std::string& service, const std::string& user,
                     const std::string& secret) = 0;
    virtual void remove(const std::string& service, const std::string& user) = 0;

    // The system's (through the keychain library).
    static std::unique_ptr<SecretStore> system();
    // Kept in memory only.
    static std::unique_ptr<SecretStore> memory();
};

struct SavedAccount
{
    std::string loginUri;
    session::AgentName name;
    // Its profile picture (a texture), as last seen logged in; null: none
    // known. Not part of which account it is.
    Uuid image;
    // What the file had for it that this version doesn't know (written by
    // a newer viewer), kept so saving doesn't lose it.
    llsd::Map unknown;
    bool operator==(const SavedAccount& o) const
    {
        return loginUri == o.loginUri && name.first == o.name.first && name.last == o.name.last;
    }
};

struct SavedSecret
{
    std::string passwordHash;
    std::string mfaHash; // empty: none yet
};

class SavedLogins
{
public:
    // `listFile`: the accounts list (created when one is saved). An old
    // accounts.txt beside it (tab separated) is read once and replaced.
    SavedLogins(std::filesystem::path listFile, std::unique_ptr<SecretStore> store);

    // The accounts remembered, most recently used first.
    const std::vector<SavedAccount>& accounts() const { return accounts_; }
    std::optional<SavedSecret> secret(const SavedAccount& account) const;
    // Remembers (or updates) an account, as the most recent; false if the
    // store refused (nothing is listed then).
    bool save(const SavedAccount& account, const SavedSecret& secret);
    void forget(const SavedAccount& account);
    // The account's profile picture, if it's remembered.
    void setImage(const SavedAccount& account, const Uuid& image);

    // The list file's default place: the platform's config directory.
    static std::filesystem::path defaultListFile();

private:
    void load();
    bool loadLegacy(const std::filesystem::path& file);
    void write() const;
    static std::string service(const SavedAccount& account);
    static std::string user(const SavedAccount& account);

    std::filesystem::path listFile_;
    std::unique_ptr<SecretStore> store_;
    std::vector<SavedAccount> accounts_;
    // The file's top-level entries other than the accounts (newer viewers').
    llsd::Map unknown_;
};

} // namespace viewer
