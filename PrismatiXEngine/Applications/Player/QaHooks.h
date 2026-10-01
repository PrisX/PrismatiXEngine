#pragma once

#include <SDL3/SDL_stdinc.h>

// QA hooks are what only the PrismatiXPlayerQA build reads: the QA IPC test
// interface (PRISMATIX_QA_IPC, see QaIpc.h) that `prismatix test` drives to
// click controls, jump to Story labels, capture frames and save. The shipped
// PrismatiXPlayer compiles every read to nullptr and QaIpc.cpp to nothing, so
// a player cannot use them to skip locked content or make the game write
// files elsewhere, and the names do not appear in the executable at all.
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
