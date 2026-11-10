# Bootstraps CPM.cmake (https://github.com/cpm-cmake/CPM.cmake) at a pinned version.
#
# Package sources are cached in CPM_SOURCE_CACHE so separate build directories
# (build/debug, build/release, ...) do not re-download everything. Set the
# CPM_SOURCE_CACHE environment variable (e.g. ~/.cache/CPM) to share the cache
# across projects; otherwise it defaults to build/_cpm-cache.

set(CPM_DOWNLOAD_VERSION 0.43.2)
set(CPM_HASH_SUM "49a3bef91ceb65bb66d57255e12d1ffd22abc2f6408fa9fe4534c544a2f232aa")

if(NOT DEFINED CPM_SOURCE_CACHE AND NOT DEFINED ENV{CPM_SOURCE_CACHE})
  set(CPM_SOURCE_CACHE "${PROJECT_SOURCE_DIR}/build/_cpm-cache" CACHE PATH "CPM package source cache")
endif()

set(CPM_DOWNLOAD_LOCATION "${PROJECT_SOURCE_DIR}/build/_cpm-cache/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")

if(NOT EXISTS "${CPM_DOWNLOAD_LOCATION}")
  message(STATUS "Downloading CPM.cmake v${CPM_DOWNLOAD_VERSION}")
  file(DOWNLOAD
    "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake"
    "${CPM_DOWNLOAD_LOCATION}"
    EXPECTED_HASH SHA256=${CPM_HASH_SUM}
  )
endif()

include("${CPM_DOWNLOAD_LOCATION}")
