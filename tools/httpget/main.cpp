// httpget: send one request through viewer::net::HttpClient and print the
// result. LLSD replies are re-printed as indented XML.
//
// Usage: httpget [--method M] [--data-file F] [--content-type T] [--header "N: V"]... URL

#include "viewer/llsd/Serialization.hpp"
#include "viewer/net/HttpClient.hpp"

#include <fstream>
#include <future>
#include <iterator>
#include <print>
#include <string>

using namespace viewer::net;

namespace
{

std::optional<HttpMethod> parseMethod(std::string_view name)
{
    for(HttpMethod m : {HttpMethod::Get, HttpMethod::Head, HttpMethod::Post, HttpMethod::Put,
                        HttpMethod::Patch, HttpMethod::Delete, HttpMethod::Options})
    {
        if(methodName(m) == name)
        {
            return m;
        }
    }
    return std::nullopt;
}

int usage()
{
    std::println(stderr, "usage: httpget [--method M] [--data-file F] [--content-type T] "
                         "[--header \"Name: value\"]... URL");
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    HttpRequest request;
    bool methodGiven = false;
    for(int i = 1; i < argc; i++)
    {
        const std::string_view arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if(arg == "--method" && hasValue)
        {
            i++;
            const auto method = parseMethod(argv[i]);
            if(!method)
            {
                return usage();
            }
            request.method = *method;
            methodGiven = true;
        }
        else if(arg == "--data-file" && hasValue)
        {
            i++;
            std::ifstream in(argv[i], std::ios::binary);
            request.body.assign(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
            if(!methodGiven)
            {
                request.method = HttpMethod::Post;
            }
        }
        else if(arg == "--content-type" && hasValue)
        {
            i++;
            request.headers.set("Content-Type", argv[i]);
        }
        else if(arg == "--header" && hasValue)
        {
            i++;
            const std::string_view header = argv[i];
            const size_t colon = header.find(':');
            if(colon == std::string_view::npos)
            {
                return usage();
            }
            std::string_view value = header.substr(colon + 1);
            while(!value.empty() && value.front() == ' ')
            {
                value.remove_prefix(1);
            }
            request.headers.add(std::string(header.substr(0, colon)), std::string(value));
        }
        else if(request.url.empty() && !arg.starts_with("--"))
        {
            request.url = arg;
        }
        else
        {
            return usage();
        }
    }
    if(request.url.empty())
    {
        return usage();
    }

    auto client = createHttpClient();
    std::promise<HttpResponse> promise;
    auto future = promise.get_future();
    client->send(std::move(request),
                 [&promise](HttpResponse&& response) { promise.set_value(std::move(response)); });
    const HttpResponse response = future.get();

    if(response.error != HttpError::None)
    {
        std::println(stderr, "error: {} ({})", errorName(response.error), response.errorMessage);
        return 1;
    }
    std::println("HTTP {}", response.status);
    for(const auto& [name, value] : response.headers)
    {
        std::println("{}: {}", name, value);
    }
    std::println("");

    if(viewer::llsd::detectFormat(response.bodyText()))
    {
        if(const auto value = viewer::llsd::parse(response.bodyText()))
        {
            std::println("{}", viewer::llsd::toXml(*value, {.compact = false, .pretty = true}));
            return 0;
        }
    }
    std::println("{}", response.bodyText());
    return 0;
}
