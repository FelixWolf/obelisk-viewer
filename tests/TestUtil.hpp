#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace viewer::test
{

inline std::vector<uint8_t> readTestFile(const std::string& name)
{
    std::ifstream in(std::filesystem::path(VIEWER_TEST_DATA_DIR) / name, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A fresh, empty directory under the build tree, removed when destroyed.
class TempDirectory
{
public:
    explicit TempDirectory(const std::string& name)
        : path_(std::filesystem::path(VIEWER_TEST_TEMP_DIR) / name)
    {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace viewer::test
