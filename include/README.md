Create `include/secrets.h` locally from `secrets.example.h`, or provide the
three `CLOUDBASE_*` macros through PlatformIO `build_flags`. Keep credentials
out of source control. The ESP32 device-sync key is not the Flutter
publishable key.
