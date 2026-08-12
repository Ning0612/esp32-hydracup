"""Layer sdkconfig.debug.defaults on top of sdkconfig.defaults for the debug environment.

PlatformIO always feeds the project's sdkconfig.defaults to ESP-IDF and offers no
per-environment override, but ESP-IDF's project.cmake honours a SDKCONFIG_DEFAULTS
environment variable holding a ';'-separated list where later files win. Setting it
here (as a `pre:` script) lets esp32dev-debug rebuild with -Og while esp32dev keeps -Os.
"""

import os

Import("env")  # noqa: F821  (injected by SCons)

os.environ["SDKCONFIG_DEFAULTS"] = "sdkconfig.defaults;sdkconfig.debug.defaults"
