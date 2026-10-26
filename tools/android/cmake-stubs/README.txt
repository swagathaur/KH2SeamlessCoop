Empty package configs for the Android Game Development Kit libraries that
Granite's Android platform layer (granite-platform) asks for. That layer is
not used (the game has its own, port/runtime/android_host.cpp) and is never
built; these only let CMake configure. build-apk.ps1 points CMake here.
