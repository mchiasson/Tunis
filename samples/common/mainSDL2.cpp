
#include <SDL2/SDL.h>
#include "SampleApp.h"

#include <chrono>
#include <cstdarg>

#include <easy/profiler.h>

int main( int argc, char* args[] )
{
    EASY_PROFILER_ENABLE;
    profiler::startListen();

    if (SDL_Init( SDL_INIT_VIDEO ) < 0 )
    {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, 3 );
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, 2 );
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE );
    SDL_GL_SetAttribute( SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute( SDL_GL_MULTISAMPLESAMPLES, 4);
    SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, 8);

    SDL_Window* window = SDL_CreateWindow(SampleApp::getSampleName(),
                                          SDL_WINDOWPOS_UNDEFINED,
                                          SDL_WINDOWPOS_UNDEFINED,
                                          SampleApp::getWindowWidth(),
                                          SampleApp::getWindowHeight(),
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN );

    if(!window)
    {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }


    SDL_GLContext context = SDL_GL_CreateContext(window);
    if(!context)
    {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_GL_SetSwapInterval(1);

    {
        // tunis::Context can only be instantiated after a context is created
        // and made current
        SampleApp app;

        SDL_Event e = {};
        Uint64 start = SDL_GetPerformanceCounter();

        bool quit = false;
        while( !quit )
        {
            EASY_BLOCK("Frame");

            //Handle events on queue
            while( SDL_PollEvent( &e ) != 0 )
            {
                if( e.type == SDL_QUIT )
                {
                    quit = true;
                }
            }

            int winWidth, winHeight, fbWidth , fbHeight;
            SDL_GetWindowSize(window, &winWidth, &winHeight);
            SDL_GL_GetDrawableSize(window, &fbWidth, &fbHeight);

            // Calculate pixel ration for hi-dpi devices.
            float pxRatio = static_cast<float>(fbWidth) / static_cast<float>(winWidth);


            double frameTime = static_cast<double>(SDL_GetPerformanceCounter() - start) / SDL_GetPerformanceFrequency();
            app.ctx.clearFrame(0, 0, fbWidth, fbHeight);
            app.ctx.beginFrame(winWidth, winHeight, pxRatio);
            app.render(frameTime);
            app.ctx.endFrame();

            EASY_END_BLOCK;

            SDL_GL_SwapWindow(window);
        }
    }

    SDL_Quit();

    profiler::dumpBlocksToFile("easy_profiler.prof");

    return 0;
}

