// Keyboard bindings: which keys press each pad button, stick direction and
// the fast-forward key. Keys are Windows virtual-key codes (stable numbers, so
// this file also builds and is tested elsewhere); names are what the settings
// file and menu show ("X", "Space", "LShift", "Numpad0", "Up").
//
// kh2recomp.ini: one line per changed action, keys comma-separated:
//   key-cross = X, Space
//   key-lstick-up = W, Up      (an empty list, "key-l3 = none", unbinds)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kh2keys
{
enum Action : int
{
    DpadUp, DpadDown, DpadLeft, DpadRight,
    Cross, Circle, Square, Triangle,
    L1, R1, L2, R2, L3, R3,
    Start, Select,
    LStickUp, LStickDown, LStickLeft, LStickRight,
    RStickUp, RStickDown, RStickLeft, RStickRight,
    FastForward,
    ActionCount
};

// "cross", "lstick-up", "fast-forward"... (the ini key is "key-" + this).
const char *actionId(Action a);
// "Cross", "Left stick up"... for the menu.
const char *actionLabel(Action a);
bool actionFromId(const std::string &id, Action &out);

// Key names, case-insensitive on input.
std::string keyName(int vk);
int keyFromName(const std::string &name); // 0 if unknown
// "X, Space" -> {0x58, 0x20}; "none" or "" -> {}. False (with the bad name in
// *error) if a name is unknown.
bool parseKeyList(const std::string &text, std::vector<int> &out, std::string *error = nullptr);
std::string keyListText(const std::vector<int> &keys); // "none" when empty

// Keys the host keeps for itself (F1-F12, Alt, Windows keys): not bindable.
bool reserved(int vk);

struct Bindings
{
    std::vector<int> keys[ActionCount];
    static Bindings defaults();
    // From an ini line's key and value ("key-cross", "X, Space"); false if the
    // action or a key is unknown.
    bool apply(const std::string &iniKey, const std::string &value, std::string *error = nullptr);
    // The ini lines that differ from the defaults ("key-cross = X, Space").
    std::vector<std::string> iniLines() const;
    bool operator==(const Bindings &o) const;
};
} // namespace kh2keys
