#include "MediaManager.hpp"

#include "viewer/media/Protocol.hpp"
#include "viewer/media/SharedMemory.hpp"
#include "viewer/media/UrlPolicy.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace viewer;
using namespace viewer::media;

namespace
{

// A host that does what it's told at once, and says what it's told to.
class FakeHost final : public MediaHost
{
public:
    State state() const override { return state_; }
    std::string error() const override { return "fake"; }
    bool send(const Message& m) override
    {
        sent.push_back(m);
        return state_ != State::Failed;
    }
    void take(std::vector<Message>& out) override
    {
        out = std::move(says);
        says.clear();
    }
    std::shared_ptr<TabAudio> audio(TabId) override { return nullptr; }

    template <typename T>
    std::vector<T> sentOf() const
    {
        std::vector<T> out;
        for(const Message& m : sent)
        {
            if(const auto* v = std::get_if<T>(&m))
            {
                out.push_back(*v);
            }
        }
        return out;
    }
    // The tabs open now (opened, not closed since).
    std::vector<Open> open() const
    {
        std::vector<Open> out;
        for(const Message& m : sent)
        {
            if(const auto* o = std::get_if<Open>(&m))
            {
                out.push_back(*o);
            }
            else if(const auto* c = std::get_if<Close>(&m))
            {
                std::erase_if(out, [&](const Open& x) { return x.tab == c->tab; });
            }
        }
        return out;
    }

    State state_ = State::Ready;
    std::vector<Message> sent;
    std::vector<Message> says;
};

struct Fixture
{
    FakeHost* host = nullptr;
    int started = 0;
    MediaManager media{nullptr, nullptr, [this]
                       {
                           started++;
                           auto h = std::make_unique<FakeHost>();
                           host = h.get();
                           return h;
                       }};
    MediaWhitelist whitelist{{{"youtube.com", true, "/"}}, 3600};
    std::vector<MediaEntry> entries;
    std::vector<MediaSource> sources;

    MediaFaceKey add(const std::string& url, bool autoPlay, float distance, uint8_t face = 0)
    {
        MediaEntry e;
        e.homeUrl = url;
        e.autoPlay = autoPlay;
        entries.push_back(e);
        Uuid id;
        id.bytes[0] = static_cast<uint8_t>(entries.size());
        sources.push_back({{id, face}, nullptr, distance, true});
        return sources.back().key;
    }
    void update(const std::string& profile = "me")
    {
        for(size_t i = 0; i < sources.size(); i++)
        {
            sources[i].entry = &entries[i];
        }
        media.update(sources, whitelist, profile);
    }
};

} // namespace

TEST_CASE("media protocol: every message there and back")
{
    const std::vector<Message> all = {
        Init{kProtocolVersion, "/root", true, false},
        Open{3, "acct", "https://a.b/c", 640, 360, "/mem", 30},
        Navigate{3, "https://x"},
        Resize{3, 10, 20, "/m2"},
        Close{3},
        Mouse{3, MouseKind::Down, -4, 5, MouseButton::Right, 2},
        Wheel{3, 1, 2, -120, 0},
        Visibility{3, true, 1},
        Release{3, 1},
        Quit{},
        Ready{kProtocolVersion, "CEF"},
        Failed{"no"},
        Frame{3, 1, 640, 360},
        Resized{3},
        Address{3, "https://y"},
        Title{3, "T\xC3\xA9"},
        Loading{3, true},
        AudioStart{3, 48000, 2},
        Audio{3, {0.5f, -0.25f, 1.0f, 0.0f}},
        AudioStop{3},
        Closed{3},
        Crashed{3},
    };
    std::vector<uint8_t> stream;
    for(const Message& m : all)
    {
        const auto bytes = encode(m);
        stream.insert(stream.end(), bytes.begin(), bytes.end());
    }
    // Fed a byte at a time, as a pipe might.
    Parser parser;
    std::vector<Message> back;
    for(uint8_t b : stream)
    {
        parser.feed(std::span(&b, 1));
        while(auto m = parser.next())
        {
            back.push_back(*m);
        }
    }
    CHECK_FALSE(parser.broken());
    REQUIRE(back.size() == all.size());
    for(size_t i = 0; i < all.size(); i++)
    {
        CAPTURE(i);
        CHECK(back[i].index() == all[i].index());
        CHECK(encode(back[i]) == encode(all[i]));
    }
    const auto& a = std::get<Audio>(back[18]);
    CHECK(a.samples == std::vector<float>{0.5f, -0.25f, 1.0f, 0.0f});
    CHECK(std::get<Mouse>(back[5]).x == -4);

    // Garbage: an unknown type, a short body, a huge length.
    for(std::vector<uint8_t> bad :
        {std::vector<uint8_t>{1, 0, 0, 0, 200}, std::vector<uint8_t>{3, 0, 0, 0, 4, 1, 0},
         std::vector<uint8_t>{0xFF, 0xFF, 0xFF, 0x7F, 4}})
    {
        Parser p;
        p.feed(bad);
        CHECK_FALSE(p.next());
        CHECK(p.broken());
    }
}

TEST_CASE("media: shared memory between two")
{
    auto made = SharedMemory::create("obelisk-test", 4096);
    REQUIRE(made);
    made->data()[100] = 42;
    auto opened = SharedMemory::open(made->name(), 4096);
    REQUIRE(opened);
    CHECK(opened->data()[100] == 42);
    opened->data()[200] = 7;
    CHECK(made->data()[200] == 7);
    opened->forget();
    // Gone by name now; both still see it.
    CHECK_FALSE(SharedMemory::open(made->name(), 4096));
    CHECK(made->data()[200] == 7);
    // Not anything else, nor more than there is.
    CHECK_FALSE(SharedMemory::open("/etc/passwd", 16));
    auto again = SharedMemory::create("obelisk-test", 64);
    REQUIRE(again);
    CHECK_FALSE(SharedMemory::open(again->name(), 4096));
}

TEST_CASE("media: what tabs may go to")
{
    for(const char* url : {"https://youtube.com/", "http://example.org:8080/x", "https://8.8.8.8/",
                           "https://[2001:db8::1]/", "about:blank", "https://localhostx.com/"})
    {
        CAPTURE(url);
        CHECK(navigable(url, false));
    }
    for(const char* url : {"file:///etc/passwd",
                           "chrome://settings",
                           "javascript:alert(1)",
                           "data:text/html,x",
                           "ftp://x.org/",
                           "https://localhost/",
                           "http://localhost:8080/",
                           "https://a.localhost/",
                           "http://127.0.0.1/",
                           "http://127.1.2.3/",
                           "http://10.0.0.1/",
                           "http://192.168.1.1/",
                           "http://172.16.0.1/",
                           "http://172.31.255.255/",
                           "http://169.254.169.254/latest",
                           "http://100.64.0.1/",
                           "http://0.0.0.0/",
                           "http://[::1]/",
                           "http://[::]/",
                           "http://[fe80::1]/",
                           "http://[fd00::1]/",
                           "http://[::ffff:7f00:1]/",
                           "http://user@127.0.0.1/",
                           "http:///",
                           "about:config",
                           "blob:https://x/1"})
    {
        CAPTURE(url);
        CHECK_FALSE(navigable(url, false));
    }
    CHECK(navigable("http://172.32.0.1/", false));
    CHECK(navigable("data:text/html,x", true));
    CHECK(loadable("data:image/png;base64,xx", false));
    CHECK(loadable("blob:https://x/1", false));
    CHECK(loadable("wss://x.org/s", false));
    CHECK_FALSE(loadable("ws://localhost/s", false));
    CHECK_FALSE(loadable("file:///x", false));
}

TEST_CASE("media: a creator's whitelist")
{
    MediaEntry e;
    CHECK(e.allows("https://anything/"));
    e.whitelistEnable = true;
    CHECK(e.allows("https://anything/")); // an empty list checks nothing
    e.whitelist = {"*.example.org", "http://pages.net/media/*"};
    CHECK(e.allows("https://www.example.org/x"));
    CHECK(e.allows("HTTPS://WWW.EXAMPLE.ORG/x"));
    CHECK_FALSE(e.allows("http://www.example.org/x")); // no scheme said: https
    CHECK_FALSE(e.allows("https://example.org.evil.net/"));
    CHECK(e.allows("http://pages.net/media/a.webm"));
    CHECK_FALSE(e.allows("http://pages.net/other"));
    e.whitelistEnable = false;
    CHECK(e.allows("http://pages.net/other"));
}

TEST_CASE("media: what plays by itself, and what waits")
{
    Fixture f;
    const auto listed = f.add("https://youtube.com/a", true, 5.0f);
    const auto other = f.add("https://elsewhere.net/", true, 5.0f);
    const auto notAuto = f.add("https://youtube.com/b", false, 5.0f);
    f.update();
    // The host starts when something wants it; tabs open once it's ready.
    REQUIRE(f.started == 1);
    CHECK(f.host->open().empty());
    f.update();
    auto open = f.host->open();
    REQUIRE(open.size() == 1);
    CHECK(open[0].url == "https://youtube.com/a");
    CHECK(open[0].profile == "me");
    CHECK(f.media.state(listed) == MediaFaceState::Starting);
    CHECK(f.media.state(other) == MediaFaceState::Click);
    CHECK(f.media.state(notAuto) == MediaFaceState::Click);

    // A frame shows it (and the slot goes back).
    f.host->says.push_back(Frame{open[0].tab, 1, open[0].width, open[0].height});
    f.update();
    CHECK(f.media.state(listed) == MediaFaceState::Playing);
    REQUIRE(f.host->sentOf<Release>().size() == 1);
    CHECK(f.host->sentOf<Release>()[0].slot == 1);

    // A click plays the others.
    CHECK(f.media.press(other.object, other.face, {0.5f, 0.5f}));
    CHECK(f.media.press(notAuto.object, notAuto.face, {0.5f, 0.5f}));
    f.update();
    CHECK(f.host->open().size() == 3);

    // Disabled: everything closes, and clicks do nothing.
    f.media.setPlay(MediaPlay::Disable);
    f.update();
    CHECK(f.host->open().empty());
    CHECK(f.media.state(listed) == MediaFaceState::Off);
    CHECK_FALSE(f.media.press(listed.object, listed.face, {0.5f, 0.5f}));
}

TEST_CASE("media: click to play, and pressing on the page")
{
    Fixture f;
    f.media.setPlay(MediaPlay::Click);
    const auto face = f.add("https://youtube.com/a", true, 5.0f);
    f.update();
    CHECK(f.started == 0); // nothing wanted yet
    CHECK(f.media.state(face) == MediaFaceState::Click);
    CHECK_FALSE(f.media.press(face.object, 1, {0.5f, 0.5f})); // another face: a touch
    CHECK(f.media.press(face.object, face.face, {0.5f, 0.5f}));
    f.update();
    f.update();
    auto open = f.host->open();
    REQUIRE(open.size() == 1);
    f.host->says.push_back(Frame{open[0].tab, 0, open[0].width, open[0].height});
    f.update();
    // On the page: pixels from its top left (v runs up), repeats wrapped.
    CHECK(f.media.press(face.object, face.face, {1.25f, 0.75f}));
    f.media.release();
    const auto mice = f.host->sentOf<Mouse>();
    REQUIRE(mice.size() == 3);
    CHECK(mice[1].kind == MouseKind::Down);
    CHECK(mice[1].x == 256);
    CHECK(mice[1].y == 256);
    CHECK(mice[2].kind == MouseKind::Up);
    // Hovering moves; leaving says so.
    f.media.hover(face, {0.5f, 0.5f});
    f.media.hover(std::nullopt, {});
    const auto after = f.host->sentOf<Mouse>();
    REQUIRE(after.size() == 5);
    CHECK(after[3].kind == MouseKind::Move);
    CHECK(after[3].x == 512);
    CHECK(after[4].kind == MouseKind::Leave);
}

TEST_CASE("media: going elsewhere by itself, or outside the creator's list")
{
    Fixture f;
    const auto face = f.add("https://youtube.com/a", true, 5.0f);
    f.entries[0].whitelistEnable = true;
    f.entries[0].whitelist = {"*.youtube.com", "youtube.com", "elsewhere.net"};
    f.update();
    f.update();
    auto open = f.host->open();
    REQUIRE(open.size() == 1);
    // Off the service's list by itself: closed until clicked.
    f.host->says.push_back(Address{open[0].tab, "https://elsewhere.net/x"});
    f.update();
    CHECK(f.host->open().empty());
    CHECK(f.media.state(face) == MediaFaceState::Click);
    f.update();
    CHECK(f.host->open().empty()); // not again by itself
    CHECK(f.media.press(face.object, face.face, {0.5f, 0.5f}));
    f.update();
    open = f.host->open();
    REQUIRE(open.size() == 1);
    // Clicked, it may go anywhere its creator allows...
    f.host->says.push_back(Address{open[0].tab, "https://elsewhere.net/x"});
    f.update();
    CHECK(f.host->open().size() == 1);
    // ...but not outside that.
    f.host->says.push_back(Address{open[0].tab, "https://evil.net/"});
    f.update();
    CHECK(f.host->open().empty());
    CHECK(f.media.state(face) == MediaFaceState::Blocked);
    CHECK_FALSE(f.media.press(face.object, face.face, {0.5f, 0.5f}));
    // New media on the face: fresh again.
    f.entries[0].homeUrl = "https://youtube.com/c";
    f.update();
    CHECK(f.host->open().size() == 1);
}

TEST_CASE("media: the nearest few, the gone forgotten, and the host failing")
{
    Fixture f;
    f.media.setPlay(MediaPlay::Always);
    for(int i = 0; i < 6; i++)
    {
        f.add(std::format("https://site{}.net/", i), true, 10.0f + static_cast<float>(i));
    }
    f.add("https://far.net/", true, MediaManager::kMaxDistance + 1.0f);
    f.update();
    f.update();
    auto open = f.host->open();
    REQUIRE(open.size() == MediaManager::kMaxTabs);
    for(const Open& o : open)
    {
        CHECK(o.url < "https://site4"); // the nearest four
    }
    // A click on a farther one (in reach) takes a place from the farthest.
    const MediaFaceKey fifth = f.sources[5].key;
    CHECK(f.media.press(fifth.object, fifth.face, {0.5f, 0.5f}));
    f.update();
    open = f.host->open();
    CHECK(std::ranges::any_of(open, [](const Open& o) { return o.url == "https://site5.net/"; }));
    CHECK_FALSE(
        std::ranges::any_of(open, [](const Open& o) { return o.url == "https://site3.net/"; }));

    // Another account: everything opens again as its.
    f.update("other");
    open = f.host->open();
    REQUIRE(open.size() == MediaManager::kMaxTabs);
    CHECK(open[0].profile == "other");

    // Gone from the world: closed.
    f.entries.clear();
    f.sources.clear();
    f.update("other");
    CHECK(f.host->open().empty());

    // The host dies: not started again at once.
    f.add("https://site0.net/", true, 5.0f);
    f.host->state_ = MediaHost::State::Failed;
    f.update("other");
    CHECK_FALSE(f.media.hostRunning());
    CHECK(f.started == 1);
}

TEST_CASE("media: volume with distance")
{
    CHECK(mediaRolloff(0.0f) == 1.0f);
    CHECK(mediaRolloff(40.0f) == 1.0f);
    CHECK(mediaRolloff(60.0f) == doctest::Approx(1.0f / (1.4f * 1.4f)));
    CHECK(mediaRolloff(80.1f) == 0.0f);
}
