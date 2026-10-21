// Key binding names, parsing and the ini round trip (port/runtime/key_bindings).
#include "key_bindings.h"

#include <cstdio>

static int g_failed = 0;
#define CHECK(c)                                                                 \
    do                                                                           \
    {                                                                            \
        if (!(c))                                                                \
        {                                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);             \
            ++g_failed;                                                          \
        }                                                                        \
    } while (0)

int main()
{
    using namespace kh2keys;
    CHECK(keyFromName("x") == 'X');
    CHECK(keyFromName(" Space ") == 0x20);
    CHECK(keyFromName("esc") == 0x1B);
    CHECK(keyFromName("numpad7") == 0x67);
    CHECK(keyFromName("F5") == 0x74);
    CHECK(keyFromName("lshift") == 0xA0);
    CHECK(keyFromName("bogus") == 0);
    CHECK(keyName(0x67) == "Numpad7");
    CHECK(keyName('Q') == "Q");
    for (int vk = 1; vk < 0x100; ++vk)
    {
        const std::string n = keyName(vk);
        CHECK(keyFromName(n) == vk);
    }

    std::vector<int> keys;
    std::string err;
    CHECK(parseKeyList("X, Space,x", keys, &err) && keys.size() == 2);
    CHECK(parseKeyList("none", keys) && keys.empty());
    CHECK(!parseKeyList("X, Blah", keys, &err) && err.find("Blah") != std::string::npos);
    CHECK(!parseKeyList("F1", keys, &err)); // reserved

    Bindings b = Bindings::defaults();
    CHECK(b.iniLines().empty());
    CHECK(b.apply("key-cross", "K, Enter"));
    CHECK(b.apply("--key-l3", "none"));
    CHECK(!b.apply("key-jump", "J", &err));
    const auto lines = b.iniLines();
    CHECK(lines.size() == 2);
    Bindings c = Bindings::defaults();
    for (const std::string &l : lines)
    {
        const size_t eq = l.find('=');
        CHECK(c.apply(l.substr(0, eq), l.substr(eq + 1)));
    }
    CHECK(c == b);
    CHECK(keyListText(c.keys[Cross]) == "K, Enter");

    if (g_failed)
        return 1;
    std::printf("key_bindings_test: all passed\n");
    return 0;
}
