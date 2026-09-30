#pragma once

#include <SDL3/SDL_stdinc.h>

// QA hooks are PRISMATIX_E2E_* environment variables that drive scripted
// journeys, redirect saves, capture frames to a directory or start a new game
// at any Story label. Only the PrismatiXPlayerQA build reads them. The shipped
// PrismatiXPlayer compiles every read to nullptr, so a player cannot use them
// to skip locked content or make the game write files elsewhere, and the
// variable names do not appear in the executable at all.
#ifndef PRISMATIX_QA_HOOKS
#define PRISMATIX_QA_HOOKS 0
#endif

#if PRISMATIX_QA_HOOKS
#define PX_QA_ENV(name) SDL_getenv(name)
#else
#define PX_QA_ENV(name) static_cast<const char*>(nullptr)
#endif

namespace px::player {
inline constexpr bool kQaHooks = PRISMATIX_QA_HOOKS != 0;
}
