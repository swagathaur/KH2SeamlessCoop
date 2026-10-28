#include "image_slots.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hle_call.h"
#include "ps2_runtime.h"

// The menus' image slots: 64 records of 0x18 bytes at 0x0035E440 (in use u8
// +0, image id +4 (-1: the rest of a longer image), slot count +8, VRAM
// addresses +0x10/+0x14), next id at 0x0035EA40. FUN_00236D98 finds room for
// an image; when there is none it frees slots from the first one on, up to
// the end of the image that holds slot N-1, by that record's count. Freeing
// (FUN_00236D60, and that same loop) clears the in-use byte and the id but
// leaves the count: a free record at N-1 then still says "8 slots" from an
// image long gone, and the loop clears records past the 64th, into the
// vtables after the table. The mission menu in the Land of Dragons met one
// at 0x0035EBC0 (dk::Obj2D's) and crashed calling through it.
//
// The game's own function runs; free records get a count of 1 first, which
// is what the loop needs (it clears that one slot), and no record says it
// runs past the table.
namespace kh2imageslots
{
namespace
{
constexpr uint32_t kFindSlots = 0x00236D98; // (YI::IMAGE*) from YI::IMAGE::LoadImage
constexpr uint32_t kTable = 0x0035E440;
constexpr uint32_t kRecords = 64;
constexpr uint32_t kRecordSize = 0x18;

PS2Runtime::RecompiledFunction g_original = nullptr;
std::atomic<int> g_reports{0};

void findSlots(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // (Resumed after one of its calls, pc is at that label: only the original.)
    for (uint32_t i = 0; ctx->pc == kFindSlots && i < kRecords; ++i)
    {
        uint8_t *r = rdram + kTable + i * kRecordSize;
        uint32_t id, count;
        std::memcpy(&id, r + 4, 4);
        std::memcpy(&count, r + 8, 4);
        uint32_t want = count;
        if (r[0] == 0)
            want = 1;
        else if (id != 0xFFFFFFFFu && count > kRecords - i)
            want = kRecords - i;
        if (want != count)
        {
            std::memcpy(r + 8, &want, 4);
            if (r[0] != 0 && g_reports.fetch_add(1) < 8)
            {
                std::printf("[image-slots] slot %u (image %u in use) said %u slots: %u\n", i, id, count, want);
                std::fflush(stdout);
            }
        }
    }
    kh2::callOriginal(g_original, rdram, ctx, runtime);
}
} // namespace

bool install(PS2Runtime &runtime)
{
    if (const char *e = std::getenv("KH2_IMAGE_SLOTS"); e && *e == '0')
        return false;
    g_original = runtime.lookupFunction(kFindSlots);
    if (!g_original || !runtime.replaceFunction(kFindSlots, &findSlots))
    {
        std::fprintf(stderr, "[image-slots] could not hook FUN_00236D98\n");
        g_original = nullptr;
        return false;
    }
    return true;
}
} // namespace kh2imageslots
