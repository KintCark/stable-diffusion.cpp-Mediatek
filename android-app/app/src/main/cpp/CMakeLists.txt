#cmake_minimum_required(VERSION 3.10)
#project(sdmediatek-native)

# Collect local CPP sources (native stubs, wrappers)
set(NATIVE_SRC
    src/main/cpp/native-stubs.cpp
    src/main/cpp/native-lib.cpp
    src/main/cpp/_include_model_loader.cpp
    src/main/cpp/jni_model_bridge.cpp
)

add_library(native-lib SHARED ${NATIVE_SRC})

# Include the upstream src directory so headers resolve
target_include_directories(native-lib PRIVATE ${CMAKE_SOURCE_DIR}/src/main/cpp ${CMAKE_SOURCE_DIR}/src/..)

target_compile_features(native-lib PRIVATE cxx_std_17)

# Optimize for ARM64 CPU (Dimensity 7025)
if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_compile_options(native-lib PRIVATE -O3 -fno-exceptions -fvisibility=hidden)
    # Ensure PIE and other Android flags are set by toolchain
endif()

find_library(log-lib log)

target_link_libraries(native-lib ${log-lib})

# Note: This build compiles upstream C++ files directly from the repo's src/ tree into the Android native library.
# When adding more upstream sources, append them to _include_model_loader.cpp or add separate wrappers here.
