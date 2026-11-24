#include "LoginFlow.hpp"
#include "SavedLogins.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

using namespace viewer;
namespace fs = std::filesystem;

namespace
{

// A temporary accounts list, removed afterwards.
struct ListFile
{
    fs::path path = fs::temp_directory_path() / ("viewer-accounts-" + Uuid::random().toString()) /
                    "accounts.llsd";
    ~ListFile()
    {
        std::error_code error;
        fs::remove_all(path.parent_path(), error);
    }
};

session::AgentName name(const char* first, const char* last)
{
    session::AgentName n;
    n.first = first;
    n.last = last;
    return n;
}

} // namespace

TEST_CASE("Saved logins: the list in a file, the secrets in the store")
{
    ListFile file;
    auto store = SecretStore::memory();
    SecretStore* secrets = store.get();
    {
        SavedLogins saved(file.path, std::move(store));
        CHECK(saved.accounts().empty());
        const SavedAccount a{std::string(session::kAgniLoginUri), name("Ann", "Resident"), {}, {}};
        const SavedAccount b{std::string(session::kAditiLoginUri), name("Bob", "Tester"), {}, {}};
        CHECK(saved.save(a, {"$1$aaa", "mfa-a"}));
        CHECK(saved.save(b, {"$1$bbb", ""}));
        // Most recent first; saving again moves it up (names compared without case).
        CHECK(saved.accounts().front() == b);
        CHECK(saved.save({a.loginUri, name("ann", "resident"), {}, {}}, {"$1$aaa", "mfa-a2"}));
        REQUIRE(saved.accounts().size() == 2);
        CHECK(saved.secret(a)->mfaHash == "mfa-a2");
        CHECK(saved.secret(b)->passwordHash == "$1$bbb");
        // The file has no secrets. (Read in its own scope: Windows won't
        // replace the list, as forget() below does, while it is open.)
        std::string text;
        {
            std::ifstream in(file.path);
            text.assign(std::istreambuf_iterator<char>(in), {});
        }
        CHECK(text.find("$1$") == std::string::npos);
        CHECK(text.find("mfa") == std::string::npos);
        CHECK(text.find("'Tester'") != std::string::npos);
        saved.forget(b);
        CHECK(saved.accounts().size() == 1);
        CHECK_FALSE(secrets->get("Second Life login: " + std::string(session::kAditiLoginUri),
                                 "bob tester"));
    }
    // Read back: the list from the file (the secrets would be in the
    // system's store; this one was memory).
    SavedLogins again(file.path, SecretStore::memory());
    REQUIRE(again.accounts().size() == 1);
    CHECK(again.accounts()[0].name.first == "ann");
}

TEST_CASE("Saved logins: a profile picture remembered, kept when saved again")
{
    ListFile file;
    const Uuid picture = Uuid::random();
    const SavedAccount a{std::string(session::kAgniLoginUri), name("Ann", "Resident"), {}, {}};
    {
        // The old tab-separated list from before pictures (no third
        // column), beside where the new one goes: read and replaced.
        const fs::path legacy = file.path.parent_path() / "accounts.txt";
        fs::create_directories(file.path.parent_path());
        std::ofstream(legacy) << a.loginUri << "\tAnn Resident\n";
        SavedLogins saved(file.path, SecretStore::memory());
        REQUIRE(saved.accounts().size() == 1);
        CHECK(fs::exists(file.path));
        CHECK_FALSE(fs::exists(legacy));
        CHECK(saved.accounts()[0].image.isNull());
        saved.setImage(a, picture);
        // Logged in again: saved without a picture, which keeps the old one.
        CHECK(saved.save(a, {"$1$aaa", ""}));
        CHECK(saved.accounts()[0].image == picture);
    }
    SavedLogins again(file.path, SecretStore::memory());
    REQUIRE(again.accounts().size() == 1);
    CHECK(again.accounts()[0].image == picture);
    CHECK(again.accounts()[0].name.last == "Resident");
}

TEST_CASE("Saved logins: what a newer viewer wrote is kept")
{
    ListFile file;
    fs::create_directories(file.path.parent_path());
    std::ofstream(file.path) << "<?llsd/notation?>\n"
                                "{'accounts':[{'login_uri':'https://example/','first_name':'Ann',"
                                "'last_name':'Resident','colour':i3},{'first_name':'NoGrid'}],"
                                "'later':'yes'}";
    const Uuid picture = Uuid::random();
    {
        SavedLogins saved(file.path, SecretStore::memory());
        REQUIRE(saved.accounts().size() == 1); // the one without a grid skipped
        CHECK(saved.accounts()[0].unknown.find("colour")->asInteger() == 3);
        saved.setImage(saved.accounts()[0], picture);
        // Saved again from a login (which knows nothing of it): still kept.
        CHECK(saved.save({"https://example/", name("Ann", "Resident"), {}, {}}, {"$1$aaa", ""}));
    }
    SavedLogins again(file.path, SecretStore::memory());
    REQUIRE(again.accounts().size() == 1);
    CHECK(again.accounts()[0].image == picture);
    CHECK(again.accounts()[0].unknown.find("colour")->asInteger() == 3);
    std::ifstream in(file.path);
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    CHECK(text.find("'later'") != std::string::npos);
}

TEST_CASE("Login flow: an authenticator code asked for, the MFA hash saved and reused")
{
    ListFile file;
    SavedLogins saved(file.path, SecretStore::memory());
    LoginFlow flow(saved, {}, "viewer", "1.0");
    const auto who = name("Ann", "Resident");

    // A first login: no MFA hash; the grid asks for a code.
    LoginAction first =
        flow.logIn({who, "$1$aaa", std::string(session::kAgniLoginUri), "last", true});
    REQUIRE(first.kind == LoginAction::Kind::LogIn);
    CHECK(first.parameters.mfaHash.empty());
    int session1 = 0;
    const session::LoginFailure challenge{"mfa_challenge", "Enter your code"};
    flow.observe(&session1, &challenge, nullptr);
    CHECK(flow.prompt() == LoginFlow::Prompt::Code);
    CHECK(flow.answerCode("").kind == LoginAction::Kind::None);

    // The code, with the same password hash; sent once.
    LoginAction retry = flow.answerCode("123456");
    REQUIRE(retry.kind == LoginAction::Kind::LogIn);
    CHECK(retry.parameters.mfaToken == "123456");
    CHECK(retry.parameters.passwordHash == "$1$aaa");
    CHECK(flow.prompt() == LoginFlow::Prompt::None);

    // Logged in (the new session at the same address, even): saved with
    // the hash the grid returned.
    session::LoginSuccess success;
    success.mfaHash = "hash-from-grid";
    flow.observe(&session1, nullptr, &success);
    REQUIRE(saved.accounts().size() == 1);
    CHECK(saved.secret(saved.accounts()[0])->mfaHash == "hash-from-grid");

    // Next time, from the saved account: the hash goes along, no code.
    LoginAction next = flow.logInSaved(saved.accounts()[0], "home");
    REQUIRE(next.kind == LoginAction::Kind::LogIn);
    CHECK(next.parameters.mfaHash == "hash-from-grid");
    CHECK(next.parameters.mfaToken.empty());
    CHECK(next.parameters.passwordHash == "$1$aaa");
    CHECK(next.parameters.start == "home");

    // A failure that isn't a prompt.
    int session2 = 0;
    const session::LoginFailure wrong{"key", "Wrong password"};
    flow.observe(&session2, &wrong, nullptr);
    CHECK(flow.prompt() == LoginFlow::Prompt::Error);
    CHECK(flow.message() == "Wrong password");
}

TEST_CASE("Login flow: not remembered unless asked; terms accepted on retry")
{
    ListFile file;
    SavedLogins saved(file.path, SecretStore::memory());
    LoginFlow flow(saved, {}, "viewer", "1.0");
    flow.logIn(
        {name("Bob", "Tester"), "$1$bbb", std::string(session::kAditiLoginUri), "last", false});
    int s = 0;
    const session::LoginFailure tos{"tos", "Terms..."};
    flow.observe(&s, &tos, nullptr);
    REQUIRE(flow.prompt() == LoginFlow::Prompt::Terms);
    const LoginAction agreed = flow.accept();
    REQUIRE(agreed.kind == LoginAction::Kind::LogIn);
    CHECK(agreed.parameters.agreeToTos);
    session::LoginSuccess success;
    flow.observe(&s, nullptr, &success);
    CHECK(saved.accounts().empty());
}
