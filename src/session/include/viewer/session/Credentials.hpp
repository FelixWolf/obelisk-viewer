#pragma once

// Login details from a small text file, for development and automated tests
// with a test account:
//
//   line 1: username ("First Last", "first.last" or "first")
//   line 2: password, or its hash as "$1$<md5 hex>" (preferred)
//   line 3: login URI (optional; defaults to the Aditi beta grid, so a
//           credentials file never logs into the main grid by accident)
//
// Keep the file outside the repository and readable only by you. The
// password is hashed as soon as it's read; neither it nor the hash is logged.

#include "viewer/session/Login.hpp"

#include <expected>
#include <filesystem>
#include <string>

namespace viewer::session
{

struct Credentials
{
    AgentName name{};
    std::string passwordHash{};
    std::string loginUri{};
};

// Reads and validates the file. Warns (via the log) if other users can read it.
std::expected<Credentials, std::string> readCredentialsFile(const std::filesystem::path& path);

} // namespace viewer::session
