#pragma once

#include "viewer/net/HttpClient.hpp"

namespace viewer
{

// Whether an asset server response means the asset doesn't exist. Second
// Life's asset servers answer 404, or 503 for missing assets (so viewers
// stop asking again); neither is worth retrying or warning about.
inline bool assetMissing(const net::HttpResponse& response)
{
    return response.error == net::HttpError::None &&
           (response.status == 404 || response.status == 503);
}

} // namespace viewer
