#include "MusicStream.hpp"

#include "viewer/core/Log.hpp"

#include <algorithm>
#include <charconv>
#include <format>

namespace viewer
{

void IcyReader::feed(std::span<const uint8_t> bytes,
                     const std::function<void(std::span<const uint8_t>)>& audio)
{
    if(interval_ == 0)
    {
        audio(bytes);
        return;
    }
    while(!bytes.empty())
    {
        if(inMeta_)
        {
            if(metaLeft_ == 0)
            {
                // The length byte.
                metaLeft_ = static_cast<size_t>(bytes[0]) * 16;
                bytes = bytes.subspan(1);
                meta_.clear();
                if(metaLeft_ == 0)
                {
                    inMeta_ = false; // nothing new
                    untilMeta_ = interval_;
                }
                continue;
            }
            const size_t n = std::min(metaLeft_, bytes.size());
            meta_.append(reinterpret_cast<const char*>(bytes.data()), n);
            bytes = bytes.subspan(n);
            metaLeft_ -= n;
            if(metaLeft_ == 0)
            {
                if(auto title = icyStreamTitle(meta_))
                {
                    title_ = std::move(title);
                }
                inMeta_ = false;
                untilMeta_ = interval_;
            }
            continue;
        }
        const size_t n = std::min(untilMeta_, bytes.size());
        if(n > 0)
        {
            audio(bytes.first(n));
        }
        bytes = bytes.subspan(n);
        untilMeta_ -= n;
        if(untilMeta_ == 0)
        {
            inMeta_ = true;
            metaLeft_ = 0;
        }
    }
}

std::optional<std::string> IcyReader::takeTitle()
{
    std::optional<std::string> t = std::move(title_);
    title_.reset();
    return t;
}

std::optional<std::string> icyStreamTitle(std::string_view metadata)
{
    constexpr std::string_view kKey = "StreamTitle='";
    const size_t at = metadata.find(kKey);
    if(at == std::string_view::npos)
    {
        return std::nullopt;
    }
    std::string_view value = metadata.substr(at + kKey.size());
    // Up to "';" (the title itself may have quotes), else the last quote.
    size_t end = value.find("';");
    if(end == std::string_view::npos)
    {
        end = value.rfind('\'');
    }
    if(end != std::string_view::npos)
    {
        value = value.substr(0, end);
    }
    // Padding (NULs) never belongs to it.
    while(!value.empty() && (value.back() == '\0' || value.back() == ' '))
    {
        value.remove_suffix(1);
    }
    return toUtf8(value);
}

std::string toUtf8(std::string_view text)
{
    // Valid UTF-8 as is.
    bool valid = true;
    for(size_t i = 0; i < text.size() && valid;)
    {
        const auto c = static_cast<uint8_t>(text[i]);
        const size_t length = c < 0x80           ? 1
                              : (c >> 5) == 0x6  ? 2
                              : (c >> 4) == 0xE  ? 3
                              : (c >> 3) == 0x1E ? 4
                                                 : 0;
        if(length == 0 || i + length > text.size())
        {
            valid = false;
            break;
        }
        for(size_t k = 1; k < length; k++)
        {
            if((static_cast<uint8_t>(text[i + k]) & 0xC0) != 0x80)
            {
                valid = false;
            }
        }
        i += length;
    }
    if(valid)
    {
        return std::string(text);
    }
    std::string out;
    out.reserve(text.size() + text.size() / 4);
    for(char ch : text)
    {
        const auto c = static_cast<uint8_t>(ch);
        if(c < 0x80)
        {
            out += ch;
        }
        else
        {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

struct MusicStream::Connection
{
    std::mutex mutex;
    bool started = false; // the first bytes came
    int status = 0;
    std::shared_ptr<audio::AudioStream> stream; // once the first bytes came
    std::optional<IcyReader> icy;
    std::string station;
    std::string genre;
    int bitrate = 0;
    std::optional<std::string> title;
    // The request ended (the stream got finish()).
    bool done = false;
    net::HttpError error = net::HttpError::None;
    std::string errorMessage;

    // The I/O thread's: a piece of the body.
    void data(int code, const net::HttpHeaders& headers, std::span<const uint8_t> piece)
    {
        std::lock_guard lock(mutex);
        if(!started)
        {
            started = true;
            status = code;
            if(code < 200 || code >= 300)
            {
                return;
            }
            const auto header = [&](std::string_view name)
            { return std::string(headers.find(name).value_or("")); };
            size_t interval = 0;
            const std::string metaint = header("icy-metaint");
            std::from_chars(metaint.data(), metaint.data() + metaint.size(), interval);
            icy.emplace(interval);
            station = toUtf8(header("icy-name"));
            genre = toUtf8(header("icy-genre"));
            const std::string br = header("icy-br");
            std::from_chars(br.data(), br.data() + br.size(), bitrate);
            stream = std::make_shared<audio::AudioStream>(header("content-type"));
        }
        if(!stream)
        {
            return; // an error's body
        }
        icy->feed(piece, [&](std::span<const uint8_t> audio) { stream->push(audio); });
        if(auto t = icy->takeTitle())
        {
            title = std::move(t);
        }
    }

    void finished(const net::HttpResponse& response)
    {
        std::lock_guard lock(mutex);
        done = true;
        error = response.error;
        errorMessage = response.errorMessage;
        if(!started)
        {
            status = response.status;
        }
        if(stream)
        {
            stream->finish();
        }
    }
};

MusicStream::MusicStream(net::HttpClient& http, audio::AudioEngine* engine)
    : http_(http), engine_(engine), group_(std::make_unique<net::HttpRequestGroup>(http))
{
}

MusicStream::~MusicStream()
{
    disconnect(false);
}

std::string_view MusicStream::stateName(State state)
{
    switch(state)
    {
    case State::Stopped:
        return "Stopped";
    case State::Connecting:
        return "Connecting";
    case State::Buffering:
        return "Buffering";
    case State::Playing:
        return "Playing";
    case State::Failed:
        return "Failed";
    }
    return "";
}

void MusicStream::set(State state, std::string error)
{
    if(status_.state != state || status_.error != error)
    {
        status_.state = state;
        status_.error = std::move(error);
        revision_++;
    }
}

void MusicStream::play(const std::string& url)
{
    disconnect(true);
    status_ = Status{};
    status_.url = url;
    everPlayed_ = false;
    reconnects_ = 0;
    reconnectAt_.reset();
    revision_++;
    if(url.empty())
    {
        return;
    }
    log::info("music: {}", url);
    connect();
}

void MusicStream::setVolume(float volume)
{
    volume_ = std::clamp(volume, 0.0f, 1.0f);
    if(engine_ && voice_)
    {
        engine_->setGain(voice_, volume_);
    }
    revision_++;
}

void MusicStream::connect()
{
    auto connection = std::make_shared<Connection>();
    net::HttpRequest request{.method = net::HttpMethod::Get, .url = status_.url};
    request.headers.add("Icy-MetaData", "1");
    request.timeout = std::chrono::milliseconds(0); // it doesn't end
    request.connectTimeout = std::chrono::seconds(15);
    request.stallTimeout = std::chrono::seconds(20);
    request.pool = net::HttpPool::Stream;
    request.onData =
        [connection](int code, const net::HttpHeaders& headers, std::span<const uint8_t> piece)
    { connection->data(code, headers, piece); };
    request_ = group_->send(std::move(request), [connection](net::HttpResponse&& response)
                            { connection->finished(response); });
    connection_ = std::move(connection);
    set(State::Connecting);
}

void MusicStream::disconnect(bool fade)
{
    if(request_)
    {
        group_->cancel(request_);
        request_ = 0;
    }
    if(engine_ && voice_)
    {
        engine_->fadeOut(voice_, fade ? kFadeSeconds : 0.0f);
    }
    voice_ = 0;
    if(connection_)
    {
        std::lock_guard lock(connection_->mutex);
        if(connection_->stream)
        {
            connection_->stream->finish();
        }
    }
    connection_.reset();
}

void MusicStream::update()
{
    const auto now = std::chrono::steady_clock::now();
    if(reconnectAt_ && now >= *reconnectAt_)
    {
        reconnectAt_.reset();
        connect();
    }
    if(!connection_)
    {
        return;
    }

    std::shared_ptr<audio::AudioStream> stream;
    bool done = false;
    std::string failure;
    {
        Connection& c = *connection_;
        std::lock_guard lock(c.mutex);
        stream = c.stream;
        done = c.done;
        if(c.station != status_.station || c.genre != status_.genre || c.bitrate != status_.bitrate)
        {
            status_.station = c.station;
            status_.genre = c.genre;
            status_.bitrate = c.bitrate;
            revision_++;
        }
        if(c.title && *c.title != status_.title)
        {
            status_.title = *c.title;
            revision_++;
        }
        if(c.started && (c.status < 200 || c.status >= 300))
        {
            failure = std::format("HTTP {}", c.status);
        }
        else if(done && c.error != net::HttpError::None)
        {
            failure =
                c.errorMessage.empty() ? std::string(net::errorName(c.error)) : c.errorMessage;
        }
        else if(done && !c.started)
        {
            failure = c.status != 0 && (c.status < 200 || c.status >= 300)
                          ? std::format("HTTP {}", c.status)
                          : std::string("the station sent nothing");
        }
    }

    if(stream)
    {
        if(status_.format != stream->format() || status_.sampleRate != stream->sampleRate() ||
           status_.channels != stream->channels())
        {
            status_.format = stream->format();
            status_.sampleRate = stream->sampleRate();
            status_.channels = stream->channels();
            revision_++;
        }
        if(engine_ && voice_ == 0 && stream->sampleRate() != 0 &&
           stream->state() != audio::AudioStream::State::Failed)
        {
            audio::AudioEngine::PlayParams params;
            params.positional = false;
            params.gain = volume_;
            params.fadeInSeconds = kFadeSeconds;
            voice_ = engine_->playStream(stream, params);
        }
        switch(stream->state())
        {
        case audio::AudioStream::State::Buffering:
            set(State::Buffering);
            return;
        case audio::AudioStream::State::Playing:
            everPlayed_ = true;
            set(State::Playing);
            return;
        case audio::AudioStream::State::Failed:
            failure = stream->error();
            break;
        case audio::AudioStream::State::Ended:
            if(failure.empty())
            {
                failure = "the station ended the stream";
            }
            break;
        }
    }
    else if(failure.empty())
    {
        set(State::Connecting);
        return;
    }

    // It ended or failed: again if it played (a dropped connection), up to
    // a few times; otherwise that's it.
    const bool decodeFailed = stream && stream->state() == audio::AudioStream::State::Failed;
    disconnect(false);
    if(everPlayed_ && !decodeFailed && reconnects_ < kMaxReconnects)
    {
        reconnects_++;
        log::info("music: reconnecting ({}): {}", reconnects_, failure);
        reconnectAt_ = now + kReconnectDelay;
        set(State::Connecting);
        return;
    }
    log::warn("music: {}: {}", status_.url, failure);
    set(State::Failed, failure);
}

} // namespace viewer
