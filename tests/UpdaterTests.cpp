#include "Updater.hpp"

#include <doctest/doctest.h>

using namespace viewer;

TEST_CASE("updates: only from https, or this computer")
{
    CHECK(updateFolder("https://vc.example/api/v1/", "Viewer") ==
          "https://vc.example/api/v1/updates/Viewer/");
    CHECK(updateFolder("https://vc.example/api/v1", "Viewer-Debug") ==
          "https://vc.example/api/v1/updates/Viewer-Debug/");
    CHECK(updateFolder("http://127.0.0.1:8000/api/v1/", "Viewer") ==
          "http://127.0.0.1:8000/api/v1/updates/Viewer/");
    CHECK(updateFolder("http://localhost/api/v1/", "V") == "http://localhost/api/v1/updates/V/");
    CHECK(updateFolder("http://vc.example/api/v1/", "Viewer").empty());
    CHECK(updateFolder("http://localhost.example/api/v1/", "Viewer").empty());
    CHECK(updateFolder("http://localhost:x@vc.example/", "Viewer").empty());
    CHECK(updateFolder("", "Viewer").empty());
    CHECK(updateFolder("https://vc.example/", "").empty());
}
