// Android host: the APK runs as a NativeActivity (no Java of its own). raylib
// (which the runtime uses for audio) provides android_main, which calls our
// main(). From there this file takes the app's events: the window the GPU
// renderer presents into, the gamepad and touch, and focus. It also finds the
// game's data folder on the shared storage, asks for access to it, and writes
// the log there.
#if defined(__ANDROID__)

#include "android_host.h"

#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "raylib.h"

extern "C" struct android_app *GetAndroidApp(void); // raylib's rcore_android.c

namespace kh2android
{
namespace
{
namespace fs = std::filesystem;

android_app *g_app = nullptr;
ANativeWindow *g_window = nullptr;
std::atomic<unsigned> g_windowGen{0};
bool g_hadWindow = false;
bool g_focused = true;
bool g_quit = false;
void (*g_lostFn)() = nullptr;
void (*g_regainedFn)() = nullptr;
bool g_needRegain = false;
std::string g_dataDir;

std::mutex g_inputMutex;
Pad g_pad;
uint32_t g_keyButtons = 0; // buttons from key events
uint32_t g_hatButtons = 0; // d-pad from a hat axis
float g_lt = 0, g_rt = 0;
bool g_back = false;
Touch g_touch;
double g_padSeen = -1.0;

double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---- Log: stdout and stderr to KH2/logs/play.log and logcat ----------------
int g_logPipe[2] = {-1, -1};
FILE *g_logFile = nullptr;
std::mutex g_logMutex;

void startLog()
{
    if (pipe(g_logPipe) != 0)
        return;
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(g_logPipe[1], STDOUT_FILENO);
    dup2(g_logPipe[1], STDERR_FILENO);
    std::thread([] {
        char buf[2048];
        std::string line;
        ssize_t n;
        while ((n = read(g_logPipe[0], buf, sizeof(buf))) > 0)
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            if (g_logFile)
            {
                fwrite(buf, 1, size_t(n), g_logFile);
                fflush(g_logFile);
            }
            for (ssize_t i = 0; i < n; ++i)
            {
                if (buf[i] == '\n')
                {
                    __android_log_write(ANDROID_LOG_INFO, "kh2recomp", line.c_str());
                    line.clear();
                }
                else
                    line += buf[i];
            }
        }
    }).detach();
}

void openLogFile(const std::string &dir)
{
    std::error_code ec;
    fs::create_directories(fs::path(dir) / "logs", ec);
    const fs::path log = fs::path(dir) / "logs" / "play.log";
    if (fs::exists(log, ec))
        fs::rename(log, fs::path(dir) / "logs" / "play.prev.log", ec);
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logFile = std::fopen(log.c_str(), "w");
}

// ---- JNI --------------------------------------------------------------------
struct Jni
{
    JNIEnv *env = nullptr;
    JavaVM *vm = nullptr;
    Jni()
    {
        vm = g_app->activity->vm;
        vm->AttachCurrentThread(&env, nullptr);
    }
    ~Jni() { vm->DetachCurrentThread(); }
    void clear()
    {
        if (env->ExceptionCheck())
            env->ExceptionClear();
    }
};

int sdkInt()
{
    Jni j;
    jclass ver = j.env->FindClass("android/os/Build$VERSION");
    if (!ver)
    {
        j.clear();
        return 0;
    }
    jfieldID f = j.env->GetStaticFieldID(ver, "SDK_INT", "I");
    const int v = f ? j.env->GetStaticIntField(ver, f) : 0;
    j.clear();
    return v;
}

// All-files access (Android 11+): needed to read the disc image and write saves
// in a folder of the user's own on the shared storage.
bool hasStorageAccess()
{
    if (sdkInt() >= 30)
    {
        Jni j;
        jclass envc = j.env->FindClass("android/os/Environment");
        jmethodID m = envc ? j.env->GetStaticMethodID(envc, "isExternalStorageManager", "()Z") : nullptr;
        const bool ok = m && j.env->CallStaticBooleanMethod(envc, m);
        j.clear();
        return ok;
    }
    // Older: the runtime permission.
    Jni j;
    jobject act = g_app->activity->clazz;
    jclass actc = j.env->GetObjectClass(act);
    jmethodID check = j.env->GetMethodID(actc, "checkSelfPermission", "(Ljava/lang/String;)I");
    jstring perm = j.env->NewStringUTF("android.permission.WRITE_EXTERNAL_STORAGE");
    const bool ok = check && j.env->CallIntMethod(act, check, perm) == 0;
    j.clear();
    return ok;
}

void askStorageAccess()
{
    Jni j;
    jobject act = g_app->activity->clazz;
    jclass actc = j.env->GetObjectClass(act);
    if (sdkInt() >= 30)
    {
        jmethodID getPkg = j.env->GetMethodID(actc, "getPackageName", "()Ljava/lang/String;");
        jstring pkg = (jstring)j.env->CallObjectMethod(act, getPkg);
        const char *p = j.env->GetStringUTFChars(pkg, nullptr);
        const std::string uriText = std::string("package:") + p;
        j.env->ReleaseStringUTFChars(pkg, p);
        jclass uric = j.env->FindClass("android/net/Uri");
        jmethodID parse = j.env->GetStaticMethodID(uric, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
        jobject uri = j.env->CallStaticObjectMethod(uric, parse, j.env->NewStringUTF(uriText.c_str()));
        jclass intc = j.env->FindClass("android/content/Intent");
        jmethodID ctor = j.env->GetMethodID(intc, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
        jobject intent = j.env->NewObject(intc, ctor, j.env->NewStringUTF("android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION"), uri);
        jmethodID start = j.env->GetMethodID(actc, "startActivity", "(Landroid/content/Intent;)V");
        j.env->CallVoidMethod(act, start, intent);
        if (j.env->ExceptionCheck())
        {
            // Some devices only have the list of all apps.
            j.env->ExceptionClear();
            jmethodID ctor1 = j.env->GetMethodID(intc, "<init>", "(Ljava/lang/String;)V");
            jobject intent2 = j.env->NewObject(intc, ctor1, j.env->NewStringUTF("android.settings.MANAGE_ALL_FILES_ACCESS_PERMISSION"));
            j.env->CallVoidMethod(act, start, intent2);
        }
        j.clear();
        return;
    }
    jclass strc = j.env->FindClass("java/lang/String");
    jobjectArray perms = j.env->NewObjectArray(2, strc, nullptr);
    j.env->SetObjectArrayElement(perms, 0, j.env->NewStringUTF("android.permission.READ_EXTERNAL_STORAGE"));
    j.env->SetObjectArrayElement(perms, 1, j.env->NewStringUTF("android.permission.WRITE_EXTERNAL_STORAGE"));
    jmethodID req = j.env->GetMethodID(actc, "requestPermissions", "([Ljava/lang/String;I)V");
    if (req)
        j.env->CallVoidMethod(act, req, perms, 1);
    j.clear();
}

void keepScreenOn()
{
    ANativeActivity_setWindowFlags(g_app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
}

// ---- Events -----------------------------------------------------------------
uint32_t keyButton(int32_t code)
{
    switch (code)
    {
    case AKEYCODE_BUTTON_A: return PadA;
    case AKEYCODE_BUTTON_B: return PadB;
    case AKEYCODE_BUTTON_X: return PadX;
    case AKEYCODE_BUTTON_Y: return PadY;
    case AKEYCODE_BUTTON_L1: return PadL1;
    case AKEYCODE_BUTTON_R1: return PadR1;
    case AKEYCODE_BUTTON_L2: return PadL2;
    case AKEYCODE_BUTTON_R2: return PadR2;
    case AKEYCODE_BUTTON_THUMBL: return PadL3;
    case AKEYCODE_BUTTON_THUMBR: return PadR3;
    case AKEYCODE_BUTTON_START: return PadStart;
    case AKEYCODE_BUTTON_SELECT: return PadSelect;
    case AKEYCODE_DPAD_UP: return PadUp;
    case AKEYCODE_DPAD_DOWN: return PadDown;
    case AKEYCODE_DPAD_LEFT: return PadLeft;
    case AKEYCODE_DPAD_RIGHT: return PadRight;
    case AKEYCODE_DPAD_CENTER: return PadA;
    default: return 0;
    }
}

float deadzone(float v)
{
    const float a = std::fabs(v);
    if (a < 0.08f)
        return 0.0f;
    return v;
}

int32_t onInput(android_app *, AInputEvent *event)
{
    const int32_t type = AInputEvent_getType(event);
    const int32_t source = AInputEvent_getSource(event);
    if (type == AINPUT_EVENT_TYPE_KEY)
    {
        const int32_t code = AKeyEvent_getKeyCode(event);
        const int32_t action = AKeyEvent_getAction(event);
        if (code == AKEYCODE_VOLUME_UP || code == AKEYCODE_VOLUME_DOWN || code == AKEYCODE_VOLUME_MUTE ||
            code == AKEYCODE_POWER || code == AKEYCODE_HOME)
            return 0; // the system's
        std::lock_guard<std::mutex> lock(g_inputMutex);
        if (code == AKEYCODE_BACK || code == AKEYCODE_BUTTON_MODE)
        {
            if (action == AKEY_EVENT_ACTION_UP)
                g_back = true;
            return 1; // not "close the app"
        }
        const uint32_t b = keyButton(code);
        if (!b)
            return 0;
        if (action == AKEY_EVENT_ACTION_DOWN)
            g_keyButtons |= b;
        else if (action == AKEY_EVENT_ACTION_UP)
            g_keyButtons &= ~b;
        g_padSeen = now();
        return 1;
    }
    if (type != AINPUT_EVENT_TYPE_MOTION)
        return 0;
    if ((source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK ||
        (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD)
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        auto axis = [&](int32_t a) { return AMotionEvent_getAxisValue(event, a, 0); };
        g_pad.lx = deadzone(axis(AMOTION_EVENT_AXIS_X));
        g_pad.ly = deadzone(axis(AMOTION_EVENT_AXIS_Y));
        g_pad.rx = deadzone(axis(AMOTION_EVENT_AXIS_Z));
        g_pad.ry = deadzone(axis(AMOTION_EVENT_AXIS_RZ));
        g_lt = std::max(axis(AMOTION_EVENT_AXIS_LTRIGGER), axis(AMOTION_EVENT_AXIS_BRAKE));
        g_rt = std::max(axis(AMOTION_EVENT_AXIS_RTRIGGER), axis(AMOTION_EVENT_AXIS_GAS));
        const float hx = axis(AMOTION_EVENT_AXIS_HAT_X), hy = axis(AMOTION_EVENT_AXIS_HAT_Y);
        g_hatButtons = (hx < -0.5f ? PadLeft : 0) | (hx > 0.5f ? PadRight : 0) | (hy < -0.5f ? PadUp : 0) |
                       (hy > 0.5f ? PadDown : 0);
        g_padSeen = now();
        return 1;
    }
    if ((source & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN)
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        const int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
        g_touch.x = AMotionEvent_getX(event, 0);
        g_touch.y = AMotionEvent_getY(event, 0);
        if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_MOVE)
            g_touch.down = true;
        else if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL)
            g_touch.down = false;
        return 1;
    }
    return 0;
}

void onCmd(android_app *app, int32_t cmd)
{
    switch (cmd)
    {
    case APP_CMD_INIT_WINDOW:
        g_window = app->window;
        ++g_windowGen;
        if (g_hadWindow)
            g_needRegain = true;
        g_hadWindow = true;
        keepScreenOn();
        break;
    case APP_CMD_TERM_WINDOW:
        // The swapchain must go before the window does (it goes when this returns).
        if (g_lostFn && g_window)
            g_lostFn();
        g_window = nullptr;
        ++g_windowGen;
        break;
    case APP_CMD_GAINED_FOCUS:
        g_focused = true;
        break;
    case APP_CMD_LOST_FOCUS:
        g_focused = false;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_keyButtons = g_hatButtons = 0;
            g_pad.lx = g_pad.ly = g_pad.rx = g_pad.ry = 0;
            g_lt = g_rt = 0;
            g_touch.down = false;
        }
        break;
    case APP_CMD_DESTROY:
        g_quit = true;
        break;
    default:
        break;
    }
}

void installHandlers()
{
    g_app->onAppCmd = onCmd;
    g_app->onInputEvent = onInput;
    g_app->userData = nullptr;
}

void pollOnce(int timeoutMs)
{
    int events = 0;
    android_poll_source *source = nullptr;
    int r;
    while ((r = ALooper_pollOnce(timeoutMs, nullptr, &events, reinterpret_cast<void **>(&source))) >= 0 ||
           r == ALOOPER_POLL_CALLBACK)
    {
        if (source)
            source->process(g_app, source);
        source = nullptr;
        if (g_app->destroyRequested)
            g_quit = true;
        timeoutMs = 0;
    }
}

// ---- Screens before the game (raylib's GLES window) ---------------------------
// Text on a dark screen while waiting for something; returns when `done`
// is true, the app closes, or (with a button) the user presses A or touches.
template <typename Done> void screen(const std::string &title, const std::string &body, Done done, bool waitForButton)
{
    InitWindow(0, 0, "KH2");
    SetTargetFPS(30);
    bool released = false;
    while (!WindowShouldClose() && !done())
    {
        BeginDrawing();
        ClearBackground(Color{12, 14, 24, 255});
        const int w = GetScreenWidth();
        const int size = std::max(20, w / 40);
        DrawText(title.c_str(), size * 2, size * 2, size * 2, Color{235, 210, 120, 255});
        int y = size * 5;
        size_t start = 0;
        while (start <= body.size())
        {
            size_t end = body.find('\n', start);
            if (end == std::string::npos)
                end = body.size();
            DrawText(body.substr(start, end - start).c_str(), size * 2, y, size, RAYWHITE);
            y += size * 3 / 2;
            start = end + 1;
        }
        EndDrawing();
        if (waitForButton)
        {
            const bool pressed = IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN) || IsMouseButtonDown(MOUSE_BUTTON_LEFT) ||
                                 IsKeyDown(KEY_ENTER);
            if (!pressed)
                released = true;
            else if (released)
                break;
        }
    }
    CloseWindow();
}

std::string findIso(const fs::path &dir)
{
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(dir, ec))
    {
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (e.is_regular_file(ec) && ext == ".iso")
            return e.path().string();
    }
    return {};
}

// KH2 on the shared storage or an SD card, with the game folder in it.
std::string findDataDir(std::string &iso)
{
    std::vector<fs::path> roots = {"/storage/emulated/0", "/sdcard"};
    std::error_code ec;
    for (const auto &e : fs::directory_iterator("/storage", ec))
        if (e.path().filename() != "emulated" && e.path().filename() != "self")
            roots.push_back(e.path());
    for (const fs::path &r : roots)
        for (const char *name : {"KH2", "kh2", "Kh2"})
        {
            const fs::path d = r / name;
            if (fs::exists(d / "game" / "SLPM_666.75", ec))
            {
                iso = findIso(d);
                if (iso.empty())
                    iso = findIso(d / "Src");
                return d.string();
            }
        }
    return {};
}

void writeDefaultSettings(const fs::path &dir)
{
    std::error_code ec;
    const fs::path ini = dir / "kh2recomp.ini";
    if (fs::exists(ini, ec))
        return;
    FILE *f = std::fopen(ini.c_str(), "w");
    if (!f)
        return;
    std::fprintf(f, "patch = widescreen\nshow-fps = 1\n");
    if (fs::exists(dir / "game" / "files" / "english" / "msg", ec))
        std::fprintf(f, "english = 1\n");
    std::fclose(f);
}
} // namespace

std::vector<std::string> prepare()
{
    g_app = GetAndroidApp();
    startLog();
    std::printf("[android] KH2 for Android, SDK %d\n", sdkInt());
    // The screens below use raylib's window, which takes the app's events
    // itself until it closes; ours are installed after.
    if (!hasStorageAccess())
    {
        askStorageAccess();
        screen("KH2: storage access",
               "This app reads your disc image and game files from the KH2 folder\n"
               "on your storage, and keeps its saves there.\n\n"
               "Allow it in the settings screen that opened (All files access),\n"
               "then come back here. The game starts as soon as it is allowed.",
               [] { return hasStorageAccess(); }, false);
        if (!hasStorageAccess())
            return {};
    }

    std::string iso;
    g_dataDir = findDataDir(iso);
    if (g_dataDir.empty() || iso.empty())
    {
        std::fprintf(stderr, "[android] no data: dir '%s' iso '%s'\n", g_dataDir.c_str(), iso.c_str());
        screen("KH2: game files not found",
               g_dataDir.empty()
                   ? "Put a folder called KH2 on the storage (the top level, next to\n"
                     "Download and DCIM) with:\n"
                     "  - your disc image (the .iso from the PC's Src folder)\n"
                     "  - the game folder from the PC (KH2/game/SLPM_666.75 and the rest)\n\n"
                     "Then start the app again. Press A to close."
                   : ("Found " + g_dataDir + ", but no disc image (.iso) in it.\n"
                      "Copy the .iso from the PC's Src folder there, then start again.\nPress A to close."),
               [] { return false; }, true);
        return {};
    }
    openLogFile(g_dataDir);
    chdir(g_dataDir.c_str());
    writeDefaultSettings(g_dataDir);
    std::printf("[android] data %s, disc %s\n", g_dataDir.c_str(), iso.c_str());

    installHandlers();
    keepScreenOn();
    g_window = g_app->window;
    g_hadWindow = g_window != nullptr;
    while (!g_window && !g_quit)
        pollOnce(100);
    if (g_quit)
        return {};
    return {g_dataDir + "/kh2recomp", "game/SLPM_666.75", "--iso=" + iso, "--cd-root=game"};
}

void pump()
{
    if (!g_app)
        return;
    pollOnce(0);
    // In the background: wait here for the window to come back (the game pauses).
    while (!g_window && g_hadWindow && !g_quit)
        pollOnce(250);
    if (g_needRegain && g_window)
    {
        g_needRegain = false;
        if (g_regainedFn)
            g_regainedFn();
    }
}

ANativeWindow *window() { return g_window; }
unsigned windowGeneration() { return g_windowGen.load(); }
bool focused() { return g_focused; }
bool quitRequested() { return g_quit; }
void setWindowCallbacks(void (*lost)(), void (*regained)())
{
    g_lostFn = lost;
    g_regainedFn = regained;
}

Pad pad()
{
    std::lock_guard<std::mutex> lock(g_inputMutex);
    Pad p = g_pad;
    p.buttons = g_keyButtons | g_hatButtons | (g_lt > 0.4f ? PadL2 : 0) | (g_rt > 0.4f ? PadR2 : 0);
    p.connected = g_padSeen >= 0;
    p.name = "Android gamepad";
    return p;
}

bool takeBack()
{
    std::lock_guard<std::mutex> lock(g_inputMutex);
    const bool b = g_back;
    g_back = false;
    return b;
}

Touch touch()
{
    std::lock_guard<std::mutex> lock(g_inputMutex);
    return g_touch;
}

const std::string &dataDir() { return g_dataDir; }
} // namespace kh2android

#endif
