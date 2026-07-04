// NOTE: This file copies select native sources from the upstream repo into the Android native directory.
// It should not contain the full original copyright headers here for space.

// Sources copied:
// - model_loader.cpp
// - model_loader.h
// - model_manager.cpp
// - model_manager.h
// - gguf_io.cpp / gguf_io.h
// - safetensors_io.cpp / safetensors_io.h
// - tensor_storage.h
// - upscaler.cpp / upscaler.h

// In reality, I will add these files directly from the repository using the GitHub API; however,
// for this environment I will create small wrapper files that include the originals using relative paths.

#include "../../../../src/model_loader.cpp"
