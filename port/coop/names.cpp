#include "names.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "hle_call.h"
#include "ps2_runtime.h"

namespace kh2names
{
namespace
{
constexpr uint32_t kMessageGet = 0x00192048u; // message id -> its text
constexpr uint32_t kPartMessage = 0x001B77A8u; // YS::OBJPART::GetMessage(part): a character's name
constexpr uint32_t kNameTable = 0x0036DAD0u;  // u16 name ids, by character + 1
constexpr int kCharacters = 16; // table entries (by character + 1; the last, 15, is Ping)
constexpr uint32_t kBuffer = 0x01E4DC00u;     // free guest memory (see puppets.cpp)
constexpr uint32_t kSlotSize = 0x20;          // 6 names * 0x20 = up to 0x01E4DCC0

PS2Runtime::RecompiledFunction g_original = nullptr, g_partOriginal = nullptr;
std::mutex g_mutex;
std::map<int, std::string> g_names; // character -> encoded name
std::map<int, uint32_t> g_byPart;    // OBJPART part (character + 1) -> its text (EE thread)
std::map<uint16_t, uint32_t> g_byId; // message id -> its text in guest memory (EE thread)
bool g_dirty = true;

// The names' text in guest memory and the ids that lead to it (under g_mutex).
void rebuild(uint8_t *rdram)
{
    g_dirty = false;
    g_byId.clear();
    g_byPart.clear();
    uint32_t slot = 0;
    std::map<std::string, uint32_t> written; // one copy of each name (every world companion can be one player's)
    auto put = [&](const std::string &text) -> uint32_t {
        if (auto w = written.find(text); w != written.end())
            return w->second;
        if (slot >= 6)
            return 0;
        const uint32_t at = kBuffer + slot++ * kSlotSize;
        std::memset(rdram + at, 0, kSlotSize);
        std::memcpy(rdram + at, text.data(), std::min<size_t>(text.size(), kSlotSize - 1));
        written[text] = at;
        return at;
    };
    for (const auto &[character, text] : g_names)
    {
        if (character < 0 || character + 1 >= kCharacters)
            continue;
        uint16_t nameId;
        std::memcpy(&nameId, rdram + kNameTable + 2u * uint32_t(character + 1), 2);
        if (nameId == 0)
            continue;
        if (const uint32_t at = put(text))
        {
            g_byId[nameId] = at;
            g_byPart[character + 1] = at;
        }
    }
}

void partHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_dirty)
            rebuild(rdram);
        auto it = g_byPart.find(static_cast<int>(getRegU32(ctx, 4)));
        if (it != g_byPart.end())
        {
            setReturnU32(ctx, it->second);
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
    }
    kh2::callOriginal(g_partOriginal, rdram, ctx, runtime);
}

void hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t id = getRegU32(ctx, 4) & 0xFFFFu;
    static const bool log = [] {
        const char *e = std::getenv("KH2_NAMES_LOG");
        return e && *e == '1';
    }();
    if (log)
    {
        static std::map<uint32_t, int> seen;
        if (seen[getRegU32(ctx, 4)]++ == 0)
            std::printf("[names] message %08X asked (from %08X)\n", getRegU32(ctx, 4), getRegU32(ctx, 31));
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_dirty)
            rebuild(rdram);
        auto it = g_byId.find(static_cast<uint16_t>(id));
        if (it != g_byId.end())
        {
            setReturnU32(ctx, it->second);
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
    }
    kh2::callOriginal(g_original, rdram, ctx, runtime);
}
} // namespace

bool install(PS2Runtime &runtime)
{
    g_original = runtime.lookupFunction(kMessageGet);
    if (!g_original || !runtime.replaceFunction(kMessageGet, &hook))
    {
        std::fprintf(stderr, "[names] could not hook 0x%08X\n", kMessageGet);
        return false;
    }
    g_partOriginal = runtime.lookupFunction(kPartMessage);
    if (!g_partOriginal || !runtime.replaceFunction(kPartMessage, &partHook))
        std::fprintf(stderr, "[names] could not hook 0x%08X\n", kPartMessage);
    return true;
}

std::string encode(const std::string &name, size_t max)
{
    std::string out;
    for (char c : name)
    {
        if (out.size() >= max)
            break;
        if (c >= 'A' && c <= 'Z')
            out += static_cast<char>(0x2E + (c - 'A'));
        else if (c >= 'a' && c <= 'z')
            out += static_cast<char>(0x9A + (c - 'a'));
        else if (c >= '0' && c <= '9')
            out += static_cast<char>(0x21 + (c - '0'));
        else if (c == ' ' && !out.empty())
            out += '\x01';
    }
    while (!out.empty() && out.back() == '\x01')
        out.pop_back();
    return out;
}

void set(const std::map<int, std::string> &byCharacter)
{
    auto encodeAll = [](const std::map<int, std::string> &in) {
        std::map<int, std::string> out;
        for (const auto &[k, name] : in)
        {
            const std::string e = encode(name);
            if (!e.empty())
                out[k] = e;
        }
        return out;
    };
    std::map<int, std::string> chars = encodeAll(byCharacter);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (chars == g_names)
        return;
    g_names = chars;
    g_dirty = true;
    for (const auto &[character, name] : byCharacter)
        std::printf("[names] character %d is called %s\n", character, name.c_str());
    std::fflush(stdout);
}
} // namespace kh2names
