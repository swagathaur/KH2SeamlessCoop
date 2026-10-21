// Running game code from the host side, safely: jobs are queued from any
// thread and run on the EE thread at a fixed point of the frame (when the
// player object's matrix is built, see puppets.cpp), where they can call
// guest functions with call().
#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>

class PS2Runtime;
struct R5900Context;

namespace kh2guest
{
struct Env
{
    uint8_t *rdram;
    R5900Context *ctx;
    PS2Runtime *runtime;
    // Guest scratch memory for pointer arguments (64 bytes, 16-aligned, below
    // the current stack; valid for the duration of the job).
    uint32_t scratch;
    uint8_t *host(uint32_t guestAddress) const { return rdram + (guestAddress & 0x01FFFFFFu); }
};

using Job = std::function<void(const Env &)>;

// Queue a job for the next frame (any thread).
void post(Job job);
// EE thread: run queued jobs (called from the frame hook).
void runPending(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

// From inside a job: call a guest function with integer arguments in a0..
// and float arguments in f12..; returns v0. The caller's registers are
// preserved.
uint32_t call(const Env &env, uint32_t address, std::initializer_list<uint32_t> args,
              std::initializer_list<float> fargs = {});
} // namespace kh2guest
