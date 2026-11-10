#include "App.hpp"

#include <SDL3/SDL_main.h>

int main(int /*argc*/, char** /*argv*/)
{
    auto app = viewer::App::create();
    if (!app) {
        return 1;
    }
    return app->run();
}
