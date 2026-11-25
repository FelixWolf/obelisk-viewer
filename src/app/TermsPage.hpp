#pragma once

// The agreement a grid's login server asks to be accepted ("tos"), read
// from its web page into plain blocks of text for the login screen. Only
// text and https links are taken from the page: none of its markup reaches
// our interface.

#include <string>
#include <string_view>
#include <vector>

namespace viewer
{

// The page the official viewer shows for Linden Lab's grids.
inline constexpr const char* kTermsUrl = "https://secondlife.com/app/tos/";

// Whether a login URI is one of Linden Lab's, whose terms are kTermsUrl's.
bool usesLindenTerms(std::string_view loginUri);

struct TermsSpan
{
    std::string text;
    std::string link; // https://...; empty: not a link
    bool bold = false;
};

struct TermsBlock
{
    bool heading = false;
    bool item = false; // of a list (its number or bullet is in the text)
    std::vector<TermsSpan> spans;
};

std::vector<TermsBlock> parseTerms(std::string_view html);

// Whether there's enough text to be an agreement (not an error page).
bool plausibleTerms(const std::vector<TermsBlock>& blocks);

} // namespace viewer
