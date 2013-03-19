#ifdef EMSCRIPTEN

#include "gameplay.h"

using namespace gameplay;

/**
 * Main entry point.
 */
int main(int argc, char** argv)
{
    printf("Starting main...\n");
    Game* game = Game::getInstance();
    Platform* platform = Platform::create(game);
    GP_ASSERT(platform);
    int result = platform->enterMessagePump();
    delete platform;
    return result;
}

#endif
