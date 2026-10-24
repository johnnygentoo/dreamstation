// SPDX-License-Identifier: GPL-3.0-or-later
#include "application.hpp"
#include <dreamstation/common/log.hpp>
#include <SDL3/SDL.h>
#include <glib.h>

int main(int argc, char* argv[]) {
    ds::common::log()->info("DreamStation starting…");

    // The engine installs its own SIGSEGV handler (fast memory access for the
    // recompiler); SDL must not replace it.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_AUDIO))
        ds::common::log()->warn("SDL init failed (no pads/audio): {}", SDL_GetError());

    // The engine renders with OpenGL into a GtkGLArea. With GTK's Vulkan
    // compositor every frame is copied across APIs (measured: 42 redraws/s
    // instead of the display's 60), so default to the GL compositor. An
    // explicit GSK_RENDERER in the environment still wins.
    g_setenv("GSK_RENDERER", "gl", FALSE);

    auto app = ds::frontend::Application::create();
    const int rc = app->run(argc, argv);
    SDL_Quit();
    return rc;
}
