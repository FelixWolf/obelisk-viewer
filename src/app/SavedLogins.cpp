#include "SavedLogins.hpp"

#include "viewer/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <system_error>
#include <utility>

#if defined(VIEWER_HAVE_KEYCHAIN)
#include <keychain/keychain.h>
#endif

namespace fs = std::filesystem;

namespace viewer
{

namespace
{

constexpr const char* kPackage = "viewer"; // the app's name in the store

class MemoryStore final : public SecretStore
{
public:
    std::optional<std::string> get(const std::string& service, const std::string& user) override
    {
        const auto it = secrets_.find({service, user});
        return it == secrets_.end() ? std::nullopt : std::optional(it->second);
    }
    bool set(const std::string& service, const std::string& user,
             const std::string& secret) override
    {
        secrets_[{service, user}] = secret;
        return true;
    }
    void remove(const std::string& service, const std::string& user) override
    {
        secrets_.erase({service, user});
    }

private:
    std::map<std::pair<std::string, std::string>, std::string> secrets_;
};

#if defined(VIEWER_HAVE_KEYCHAIN)
class SystemStore final : public SecretStore
{
public:
    std::optional<std::string> get(const std::string& service, const std::string& user) override
    {
        keychain::Error error;
        std::string secret = keychain::getPassword(kPackage, service, user, error);
        if(error)
        {
            if(error.type != keychain::ErrorType::NotFound)
            {
                log::warn("credential store: reading failed: {}", error.message);
            }
            return std::nullopt;
        }
        return secret;
    }
    bool set(const std::string& service, const std::string& user,
             const std::string& secret) override
    {
        keychain::Error error;
        keychain::setPassword(kPackage, service, user, secret, error);
        if(error)
        {
            log::warn("credential store: saving failed: {}", error.message);
            return false;
        }
        return true;
    }
    void remove(const std::string& service, const std::string& user) override
    {
        keychain::Error error;
        keychain::deletePassword(kPackage, service, user, error);
        if(error && error.type != keychain::ErrorType::NotFound)
        {
            log::warn("credential store: removing failed: {}", error.message);
        }
    }
};
#endif

// The secret as stored: the two hashes on their own lines.
std::string encode(const SavedSecret& s)
{
    return s.passwordHash + "\n" + s.mfaHash;
}

SavedSecret decode(const std::string& text)
{
    SavedSecret s;
    const size_t newline = text.find('\n');
    s.passwordHash = text.substr(0, newline);
    if(newline != std::string::npos)
    {
        s.mfaHash = text.substr(newline + 1);
    }
    return s;
}

} // namespace

std::unique_ptr<SecretStore> SecretStore::system()
{
#if defined(VIEWER_HAVE_KEYCHAIN)
    return std::make_unique<SystemStore>();
#else
    log::warn("no system credential store: saved logins last until the viewer quits");
    return memory();
#endif
}

std::unique_ptr<SecretStore> SecretStore::memory()
{
    return std::make_unique<MemoryStore>();
}

SavedLogins::SavedLogins(fs::path listFile, std::unique_ptr<SecretStore> store)
    : listFile_(std::move(listFile)), store_(std::move(store))
{
    load();
}

fs::path SavedLogins::defaultListFile()
{
#if defined(_WIN32)
    if(const char* appData = std::getenv("APPDATA"))
    {
        return fs::path(appData) / kPackage / "accounts.txt";
    }
#elif defined(__APPLE__)
    if(const char* home = std::getenv("HOME"))
    {
        return fs::path(home) / "Library" / "Application Support" / kPackage / "accounts.txt";
    }
#else
    if(const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
    {
        return fs::path(xdg) / kPackage / "accounts.txt";
    }
    if(const char* home = std::getenv("HOME"))
    {
        return fs::path(home) / ".config" / kPackage / "accounts.txt";
    }
#endif
    return fs::temp_directory_path() / kPackage / "accounts.txt";
}

std::string SavedLogins::service(const SavedAccount& account)
{
    return "Second Life login: " + account.loginUri;
}

std::string SavedLogins::user(const SavedAccount& account)
{
    std::string name = account.name.first + " " + account.name.last;
    std::ranges::transform(name, name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
}

void SavedLogins::load()
{
    // One account a line: the login URI, a tab, first and last name, and
    // (since profile pictures) a tab and its picture's id.
    std::ifstream in(listFile_);
    std::string line;
    while(std::getline(in, line))
    {
        const size_t tab = line.find('\t');
        if(tab == std::string::npos)
        {
            continue;
        }
        const size_t imageTab = line.find('\t', tab + 1);
        auto name = session::parseAgentName(line.substr(tab + 1, imageTab - tab - 1));
        if(name)
        {
            const Uuid image = imageTab == std::string::npos
                                   ? Uuid()
                                   : Uuid::parse(line.substr(imageTab + 1)).value_or(Uuid());
            accounts_.push_back({line.substr(0, tab), std::move(*name), image});
        }
    }
}

void SavedLogins::write() const
{
    std::error_code error;
    fs::create_directories(listFile_.parent_path(), error);
    std::ofstream out(listFile_, std::ios::trunc);
    for(const SavedAccount& a : accounts_)
    {
        out << a.loginUri << '\t' << a.name.first << ' ' << a.name.last;
        if(!a.image.isNull())
        {
            out << '\t' << a.image.toString();
        }
        out << '\n';
    }
    if(!out)
    {
        log::warn("couldn't write the saved accounts list {}", listFile_.string());
    }
}

std::optional<SavedSecret> SavedLogins::secret(const SavedAccount& account) const
{
    const auto text = store_->get(service(account), user(account));
    if(!text)
    {
        return std::nullopt;
    }
    return decode(*text);
}

bool SavedLogins::save(const SavedAccount& account, const SavedSecret& secret)
{
    if(!store_->set(service(account), user(account), encode(secret)))
    {
        return false;
    }
    SavedAccount saved = account;
    for(const SavedAccount& a : accounts_)
    {
        if(saved.image.isNull() && a.loginUri == account.loginUri && user(a) == user(account))
        {
            saved.image = a.image; // still its picture
        }
    }
    std::erase_if(accounts_, [&](const SavedAccount& a)
                  { return a.loginUri == account.loginUri && user(a) == user(account); });
    accounts_.insert(accounts_.begin(), std::move(saved));
    write();
    return true;
}

void SavedLogins::setImage(const SavedAccount& account, const Uuid& image)
{
    for(SavedAccount& a : accounts_)
    {
        if(a.loginUri == account.loginUri && user(a) == user(account) && a.image != image)
        {
            a.image = image;
            write();
        }
    }
}

void SavedLogins::forget(const SavedAccount& account)
{
    store_->remove(service(account), user(account));
    std::erase_if(accounts_, [&](const SavedAccount& a)
                  { return a.loginUri == account.loginUri && user(a) == user(account); });
    write();
}

} // namespace viewer
