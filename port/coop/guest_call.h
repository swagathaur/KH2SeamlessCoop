// Running game code from the host side, safely.
//
// A job is a little state machine of guest calls: each time its previous call
// has finished (with that call's v0), it may prepare memory and return the
// next call to make, or nothing when it is done. Calls are made on the EE
// thread from the frame hook (puppets.cpp, when the player object's matrix is
// built) as scheduler invocations (EeScheduler::invokeCurrent): the guest
// function runs to completion on its own stack, preemptible like any guest
// code, and the hook then runs again. So a job advances by one call per
// frame.
//
// (Calling a translated function directly from C++ is not enough: every call
// inside it is a scheduler safe point, and when one is due the C++ stack
// unwinds with the rest of the call still to run.)
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

class PS2Runtime;
struct R5900Context;

namespace kh2guest
{
struct Env
{
    uint8_t *rdram;
    // Guest scratch memory for pointer arguments (256 bytes, 16-aligned, below
    // the current stack; valid until the next call has run).
    uint32_t scratch;
    uint8_t *host(uint32_t guestAddress) const { return rdram + (guestAddress & 0x01FFFFFFu); }
};

struct Call
{
    uint32_t address = 0;
    std::vector<uint32_t> args;  // a0..a3, t0..t3
    std::vector<float> fargs;    // f12, f13, ...
};

// Called with the previous call's v0 (0 the first time, `first` true);
// returns the next call, or nothing when finished.
using Step = std::function<std::optional<Call>(const Env &env, uint32_t lastResult, bool first)>;

// Queue a job (any thread).
void post(Step job);
// A job that makes one call and hands its result to `done` (may be empty).
void postCall(Call call, std::function<void(const Env &, uint32_t)> done = {});

// Called from the frame hook on the EE thread, with the hook's own address:
// if a call is due it does not return (the hook runs again afterwards).
void runPending(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t hookAddress);
} // namespace kh2guest
