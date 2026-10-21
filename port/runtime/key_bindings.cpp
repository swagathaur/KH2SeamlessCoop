#include "key_bindings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <initializer_list>

namespace kh2keys
{
namespace
{
struct ActionInfo
{
    const char *id;
    const char *label;
};
const ActionInfo kActions[ActionCount] = {
    {"up", "D-pad up"},
    {"down", "D-pad down"},
    {"left", "D-pad left"},
    {"right", "D-pad right"},
    {"cross", "Cross"},
    {"circle", "Circle"},
    {"square", "Square"},
    {"triangle", "Triangle"},
    {"l1", "L1"},
    {"r1", "R1"},
    {"l2", "L2"},
    {"r2", "R2"},
    {"l3", "L3"},
    {"r3", "R3"},
    {"start", "Start"},
    {"select", "Select"},
    {"lstick-up", "Left stick up"},
    {"lstick-down", "Left stick down"},
    {"lstick-left", "Left stick left"},
    {"lstick-right", "Left stick right"},
    {"rstick-up", "Camera up"},
    {"rstick-down", "Camera down"},
    {"rstick-left", "Camera left"},
    {"rstick-right", "Camera right"},
    {"fast-forward", "Fast-forward (hold)"},
};

struct KeyInfo
{
    int vk;
    const char *name;
};
// Windows virtual-key codes.
const KeyInfo kNamed[] = {
    {0x08, "Backspace"}, {0x09, "Tab"},      {0x0D, "Enter"},    {0x13, "Pause"},     {0x14, "CapsLock"},
    {0x1B, "Escape"},    {0x20, "Space"},    {0x21, "PageUp"},   {0x22, "PageDown"},  {0x23, "End"},
    {0x24, "Home"},      {0x25, "Left"},     {0x26, "Up"},       {0x27, "Right"},     {0x28, "Down"},
    {0x2D, "Insert"},    {0x2E, "Delete"},   {0x6A, "NumpadMultiply"}, {0x6B, "NumpadAdd"},
    {0x6D, "NumpadSubtract"}, {0x6E, "NumpadDecimal"}, {0x6F, "NumpadDivide"},
    {0xA0, "LShift"},    {0xA1, "RShift"},   {0xA2, "LCtrl"},    {0xA3, "RCtrl"},
    {0xBA, "Semicolon"}, {0xBB, "Equals"},   {0xBC, "Comma"},    {0xBD, "Minus"},     {0xBE, "Period"},
    {0xBF, "Slash"},     {0xC0, "Backquote"}, {0xDB, "LBracket"}, {0xDC, "Backslash"}, {0xDD, "RBracket"},
    {0xDE, "Quote"},
    // Mouse buttons (the right one also drives the mouse camera).
    {0x01, "MouseLeft"}, {0x02, "MouseRight"}, {0x04, "MouseMiddle"}, {0x05, "Mouse4"}, {0x06, "Mouse5"},
};
const KeyInfo kAliases[] = {
    {0x1B, "Esc"}, {0x0D, "Return"}, {0xA2, "Ctrl"}, {0xA0, "Shift"}, {0x2E, "Del"}, {0x2D, "Ins"},
    {0x20, "Spacebar"}, {0x01, "LMB"}, {0x02, "RMB"}, {0x04, "MMB"},
};

std::string lower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string &v)
{
    const size_t b = v.find_first_not_of(" \t\r\n");
    const size_t e = v.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : v.substr(b, e - b + 1);
}

void bind(Bindings &b, Action a, std::initializer_list<int> keys) { b.keys[a].assign(keys); }
} // namespace

const char *actionId(Action a) { return a >= 0 && a < ActionCount ? kActions[a].id : "?"; }
const char *actionLabel(Action a) { return a >= 0 && a < ActionCount ? kActions[a].label : "?"; }

bool actionFromId(const std::string &id, Action &out)
{
    const std::string l = lower(trim(id));
    for (int i = 0; i < ActionCount; ++i)
        if (l == kActions[i].id)
        {
            out = Action(i);
            return true;
        }
    return false;
}

std::string keyName(int vk)
{
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
        return std::string(1, char(vk));
    if (vk >= 0x60 && vk <= 0x69)
        return "Numpad" + std::to_string(vk - 0x60);
    if (vk >= 0x70 && vk <= 0x87)
        return "F" + std::to_string(vk - 0x70 + 1);
    for (const KeyInfo &k : kNamed)
        if (k.vk == vk)
            return k.name;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%02X", unsigned(vk));
    return buf;
}

int keyFromName(const std::string &name)
{
    const std::string n = trim(name);
    if (n.empty())
        return 0;
    if (n.size() == 1)
    {
        const char c = char(std::toupper(static_cast<unsigned char>(n[0])));
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))
            return c;
    }
    const std::string l = lower(n);
    if (l.rfind("numpad", 0) == 0 && l.size() == 7 && std::isdigit(static_cast<unsigned char>(l[6])))
        return 0x60 + (l[6] - '0');
    if (l.size() >= 2 && l[0] == 'f' && std::all_of(l.begin() + 1, l.end(), ::isdigit))
    {
        const int f = std::stoi(l.substr(1));
        if (f >= 1 && f <= 24)
            return 0x70 + f - 1;
    }
    if (l.rfind("0x", 0) == 0 && l.size() > 2)
    {
        const long v = std::strtol(l.c_str() + 2, nullptr, 16);
        return v > 0 && v < 0x100 ? int(v) : 0;
    }
    for (const KeyInfo &k : kNamed)
        if (l == lower(k.name))
            return k.vk;
    for (const KeyInfo &k : kAliases)
        if (l == lower(k.name))
            return k.vk;
    return 0;
}

bool reserved(int vk)
{
    return (vk >= 0x70 && vk <= 0x7B) // F1-F12: menu, HUD, fullscreen, screenshot
           || vk == 0x10 || vk == 0x11 || vk == 0x12 // generic Shift/Ctrl/Alt (the sided ones are bindable)
           || vk == 0xA4 || vk == 0xA5               // Alt (Alt+Enter)
           || vk == 0x5B || vk == 0x5C;              // Windows keys
}

bool parseKeyList(const std::string &text, std::vector<int> &out, std::string *error)
{
    out.clear();
    const std::string t = trim(text);
    if (t.empty() || lower(t) == "none")
        return true;
    size_t start = 0;
    while (start <= t.size())
    {
        size_t comma = t.find(',', start);
        if (comma == std::string::npos)
            comma = t.size();
        const std::string name = trim(t.substr(start, comma - start));
        if (!name.empty())
        {
            const int vk = keyFromName(name);
            if (!vk || reserved(vk))
            {
                if (error)
                    *error = vk ? name + " is reserved" : "unknown key '" + name + "'";
                return false;
            }
            if (std::find(out.begin(), out.end(), vk) == out.end())
                out.push_back(vk);
        }
        start = comma + 1;
    }
    return true;
}

std::string keyListText(const std::vector<int> &keys)
{
    if (keys.empty())
        return "none";
    std::string s;
    for (int vk : keys)
        s += (s.empty() ? "" : ", ") + keyName(vk);
    return s;
}

Bindings Bindings::defaults()
{
    // Same layout as the runtime's raylib bindings (ps2_pad.cpp), plus the
    // numpad face buttons.
    Bindings b;
    bind(b, DpadUp, {0x26});
    bind(b, DpadDown, {0x28});
    bind(b, DpadLeft, {0x25});
    bind(b, DpadRight, {0x27});
    bind(b, Cross, {'X', 0x20});
    bind(b, Circle, {'C', 0x1B});
    bind(b, Square, {'Z', 0x60});
    bind(b, Triangle, {'V', 0x61});
    bind(b, L1, {'Q'});
    bind(b, R1, {'E'});
    bind(b, L2, {0xA0});
    bind(b, R2, {0xA1});
    bind(b, L3, {'R'});
    bind(b, R3, {'F'});
    bind(b, Start, {0x0D});
    bind(b, Select, {0x08});
    bind(b, LStickUp, {'W'});
    bind(b, LStickDown, {'S'});
    bind(b, LStickLeft, {'A'});
    bind(b, LStickRight, {'D'});
    bind(b, RStickUp, {'I'});
    bind(b, RStickDown, {'K'});
    bind(b, RStickLeft, {'J'});
    bind(b, RStickRight, {'L'});
    bind(b, FastForward, {0x09});
    return b;
}

bool Bindings::apply(const std::string &iniKey, const std::string &value, std::string *error)
{
    std::string id = trim(iniKey);
    if (id.rfind("--", 0) == 0)
        id.erase(0, 2);
    if (lower(id).rfind("key-", 0) == 0)
        id.erase(0, 4);
    Action a;
    if (!actionFromId(id, a))
    {
        if (error)
            *error = "unknown action '" + id + "'";
        return false;
    }
    std::vector<int> list;
    if (!parseKeyList(value, list, error))
        return false;
    keys[a] = std::move(list);
    return true;
}

std::vector<std::string> Bindings::iniLines() const
{
    const Bindings def = defaults();
    std::vector<std::string> out;
    for (int i = 0; i < ActionCount; ++i)
        if (keys[i] != def.keys[i])
            out.push_back(std::string("key-") + kActions[i].id + " = " + keyListText(keys[i]));
    return out;
}

bool Bindings::operator==(const Bindings &o) const
{
    for (int i = 0; i < ActionCount; ++i)
        if (keys[i] != o.keys[i])
            return false;
    return true;
}
} // namespace kh2keys
