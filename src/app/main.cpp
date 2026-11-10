#include "App.hpp"

#include <SDL3/SDL_main.h>

int main(int argc, char** argv)
{
    const auto options = viewer::parseArguments({argv, static_cast<size_t>(argc)});
    if(!options)
    {
        return 2;
    }
    auto app = viewer::App::create(*options);
    if(!app)
    {
        return 1;
    }
    return app->run();
}
