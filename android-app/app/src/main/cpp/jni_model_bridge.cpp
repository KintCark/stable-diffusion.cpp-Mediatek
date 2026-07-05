// JNI bridge and minimal model integration (scaffold + queue processor + best-effort generation)

#include <jni.h>
#include <string>
#include <vector>
#include <mutex>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <ctime>
#include <thread>
#include <algorithm>
#include <sstream>
#include <dlfcn.h>
#include <cstring>

#include "model_manager.h"

static std::unique_ptr<ModelManager> g_manager;
static std::mutex g_manager_mutex;

static inline bool has_extension(const std::string &s, const std::string &ext) {
    if (s.size() < ext.size()) return false;
    auto a = s.substr(s.size()-ext.size());
    for (size_t i=0;i<ext.size();++i) a[i] = tolower(a[i]);
    std::string lowext = ext;
    for (char &c: lowext) c = tolower(c);
    return a == lowext;
}

static std::string json_array_from_vector(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        out += '"';
        for (char c: items[i]) {
            if (c == '\\') out += "\\\\";
            else if (c == '"') out += "\\\"";
            else out += c;
        }
        out += '"';
        if (i + 1 < items.size()) out += ',';
    }
    out += "]";
    return out;
}

// Lightweight JSON extraction helpers (no external dependency). Assumes well-formed JSON from the app.
static std::string extract_json_string(const std::string &json, const std::string &key) {
    std::string q = '"' + key + '"';
    auto pos = json.find(q);
    if (pos == std::string::npos) return std::string();
    auto colon = json.find(':', pos + q.size());
    if (colon == std::string::npos) return std::string();
    auto s = json.find('"', colon);
    if (s == std::string::npos) return std::string();
    ++s;
    std::string out;
    for (size_t i = s; i < json.size(); ++i) {
        char c = json[i];
        if (c == '\\') {
            if (i + 1 < json.size()) {
                char n = json[i+1];
                if (n == '"' || n == '\\' || n == '/') { out += n; ++i; continue; }
                if (n == 'n') { out += '\n'; ++i; continue; }
                if (n == 't') { out += '\t'; ++i; continue; }
                ++i; continue;
            }
        } else if (c == '"') {
            break;
        } else {
            out += c;
        }
    }
    return out;
}

static std::vector<std::string> extract_json_string_array(const std::string &json, const std::string &key) {
    std::vector<std::string> out;
    std::string q = '"' + key + '"';
    auto pos = json.find(q);
    if (pos == std::string::npos) return out;
    auto colon = json.find(':', pos + q.size());
    if (colon == std::string::npos) return out;
    auto lb = json.find('[', colon);
    if (lb == std::string::npos) return out;
    size_t i = lb + 1;
    while (i < json.size()) {
        while (i < json.size() && isspace((unsigned char)json[i])) ++i;
        if (i >= json.size()) break;
        if (json[i] == ']') break;
        if (json[i] == '"') {
            ++i;
            std::string item;
            for (; i < json.size(); ++i) {
                char c = json[i];
                if (c == '\\') {
                    if (i + 1 < json.size()) {
                        char n = json[i+1];
                        if (n == '"' || n == '\\' || n == '/') { item += n; i++; continue; }
                        if (n == 'n') { item += '\n'; i++; continue; }
                        if (n == 't') { item += '\t'; i++; continue; }
                        i++; continue;
                    }
                } else if (c == '"') {
                    ++i;
                    break;
                } else {
                    item += c;
                }
            }
            out.push_back(item);
            while (i < json.size() && json[i] != ',' && json[i] != ']') ++i;
            if (i < json.size() && json[i] == ',') ++i;
            continue;
        }
        while (i < json.size() && json[i] != ',' && json[i] != ']') ++i;
        if (i < json.size() && json[i] == ',') ++i;
    }
    return out;
}

static bool file_exists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static std::string read_entire_file(const std::string &path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return std::string();
    std::string out;
    char buf[4096];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, buf + r);
    fclose(f);
    return out;
}

static bool write_entire_file(const std::string &path, const std::string &contents) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t w = fwrite(contents.data(), 1, contents.size(), f);
    fclose(f);
    return w == contents.size();
}

static std::string write_request_to_downloads_file(const std::string &contents, const char* suffix) {
    time_t t = time(nullptr);
    char fname[512];
    if (suffix) snprintf(fname, sizeof(fname), "/storage/emulated/0/Download/sd_request_%lld_%s.json", (long long)t, suffix);
    else snprintf(fname, sizeof(fname), "/storage/emulated/0/Download/sd_request_%lld.json", (long long)t);
    if (!write_entire_file(fname, contents)) return std::string();
    return std::string(fname);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_kintcark_sdmediatek_NativeBridge_initNative(JNIEnv* env, jobject /* this */) {
    std::lock_guard<std::mutex> lock(g_manager_mutex);
    if (!g_manager) {
        g_manager = std::make_unique<ModelManager>();
        int cores = (int)std::thread::hardware_concurrency();
        g_manager->set_n_threads(std::max(1, cores > 1 ? cores - 1 : 1));
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_kintcark_sdmediatek_NativeBridge_discoverModels(JNIEnv* env, jobject /* this */, jstring jpath) {
    const char* path = env->GetStringUTFChars(jpath, 0);
    std::vector<std::string> found;

    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            std::string name = ent->d_name;
            if (name == "." || name == "..") continue;
            std::string full = std::string(path) + "/" + name;
            struct stat st;
            if (stat(full.c_str(), &st) == 0) {
                if (S_ISREG(st.st_mode)) {
                    if (has_extension(name, ".gguf") || has_extension(name, ".safetensors") || has_extension(name, ".ckpt")) {
                        found.push_back(full);
                    }
                }
            }
        }
        closedir(dir);
    }

    env->ReleaseStringUTFChars(jpath, path);
    std::string s = json_array_from_vector(found);
    return env->NewStringUTF(s.c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_kintcark_sdmediatek_NativeBridge_loadModel(JNIEnv* env, jobject /* this */, jstring jmodelPath, jstring jquant) {
    const char* modelPath = env->GetStringUTFChars(jmodelPath, 0);
    const char* quant = env->GetStringUTFChars(jquant, 0);

    std::lock_guard<std::mutex> lock(g_manager_mutex);
    if (!g_manager) {
        env->ReleaseStringUTFChars(jmodelPath, modelPath);
        env->ReleaseStringUTFChars(jquant, quant);
        return JNI_FALSE;
    }

    bool ok = g_manager->loader().init_from_file_and_convert_name(std::string(modelPath), "", VERSION_COUNT);

    env->ReleaseStringUTFChars(jmodelPath, modelPath);
    env->ReleaseStringUTFChars(jquant, quant);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_kintcark_sdmediatek_NativeBridge_unloadModel(JNIEnv* env, jobject /* this */) {
    std::lock_guard<std::mutex> lock(g_manager_mutex);
    if (!g_manager) return JNI_FALSE;
    g_manager.reset();
    return JNI_TRUE;
}

// Best-effort: try to locate a generation symbol in-process or in common shared libs
// The code intentionally uses dlsym to discover a well-known C entrypoint that may
// exist in other native modules. If none is found, generation is not performed.
static bool generate_from_request(const std::string &request_json, std::vector<std::string> &out_image_paths, std::string &out_error) {
    out_image_paths.clear();
    out_error.clear();

    // Basic validation: ensure modelPath exists (if provided)
    std::string model_path = extract_json_string(request_json, "modelPath");
    if (!model_path.empty() && !file_exists(model_path)) {
        out_error = "modelPath not found: " + model_path;
        return false;
    }

    // Attempt to load model via ModelManager if modelPath provided
    if (!model_path.empty()) {
        std::lock_guard<std::mutex> lock(g_manager_mutex);
        if (!g_manager) {
            out_error = "model manager not initialized";
            return false;
        }
        bool ok = g_manager->loader().init_from_file_and_convert_name(model_path, "", VERSION_COUNT);
        if (!ok) {
            out_error = "model load failed for: " + model_path;
            return false;
        }
    }

    // Candidate libraries and symbols to try. This is a best-effort list and may need
    // to be expanded based on the repository's actual native API.
    const char* candidate_libs[] = { "", "libsd.so", "libstable_diffusion.so", "libnative-lib.so", "libmediatek.so", nullptr };
    const char* candidate_symbols[] = {
        "sd_generate_from_json",
        "generate_image_from_json",
        "krea2_generate_from_json",
        "photomaker_generate_from_json",
        "generate_image",
        "sd_generate",
        nullptr
    };

    // Generic function signature we will attempt: int fn(const char* request_json, char* out_json, size_t out_sz)
    typedef int (*gen_fn_t)(const char*, char*, size_t);

    const size_t OUT_SZ = 256 * 1024; // 256KB output buffer
    std::vector<char> outbuf(OUT_SZ);

    // Try RTLD_DEFAULT first (symbols already loaded into process)
    for (const char** sym = candidate_symbols; *sym != nullptr; ++sym) {
        void* fptr = dlsym(RTLD_DEFAULT, *sym);
        if (!fptr) continue;
        gen_fn_t fn = (gen_fn_t)fptr;
        memset(outbuf.data(), 0, OUT_SZ);
        int r = -1;
        // call safely — cannot catch segfaults; hope signatures match
        r = fn(request_json.c_str(), outbuf.data(), OUT_SZ);
        if (r >= 0) {
            std::string json_out(outbuf.data(), strnlen(outbuf.data(), OUT_SZ));
            // Attempt to extract output image paths from output JSON
            auto imgs = extract_json_string_array(json_out, "output_images");
            if (!imgs.empty()) {
                out_image_paths = imgs;
                return true;
            }
            // fallback: if the function returned paths as a single string
            std::string single = extract_json_string(json_out, "output");
            if (!single.empty()) {
                out_image_paths.push_back(single);
                return true;
            }
            // If function succeeded but didn't return expected JSON, return raw output as an image path if it looks like a path
            if (!json_out.empty()) {
                out_image_paths.push_back("(generator_output_json)" + json_out);
                return true;
            }
        }
    }

    // Try opening common libraries and lookup symbols
    for (const char** lib = candidate_libs; *lib != nullptr; ++lib) {
        void* handle = nullptr;
        if (strlen(*lib) > 0) handle = dlopen(*lib, RTLD_NOW | RTLD_LOCAL);
        if (!handle) continue;
        for (const char** sym = candidate_symbols; *sym != nullptr; ++sym) {
            void* fptr = dlsym(handle, *sym);
            if (!fptr) continue;
            gen_fn_t fn = (gen_fn_t)fptr;
            memset(outbuf.data(), 0, OUT_SZ);
            int r = fn(request_json.c_str(), outbuf.data(), OUT_SZ);
            if (r >= 0) {
                std::string json_out(outbuf.data(), strnlen(outbuf.data(), OUT_SZ));
                auto imgs = extract_json_string_array(json_out, "output_images");
                if (!imgs.empty()) {
                    out_image_paths = imgs;
                    dlclose(handle);
                    return true;
                }
                std::string single = extract_json_string(json_out, "output");
                if (!single.empty()) {
                    out_image_paths.push_back(single);
                    dlclose(handle);
                    return true;
                }
                if (!json_out.empty()) {
                    out_image_paths.push_back("(generator_output_json)" + json_out);
                    dlclose(handle);
                    return true;
                }
            }
        }
        dlclose(handle);
    }

    out_error = "no compatible generation symbol found in process or common libs";
    return false;
}

// Process queued scaffold requests in Downloads. For each file named
// sd_request_*_scaffold.json the worker will:
//  - read the request
//  - write a sd_result_<ts>_processing.json file
//  - call generate_from_request (best-effort)
//  - write sd_result_<ts>.json with status/success or status/failed
//  - rename the processed request file to *.processed
extern "C" JNIEXPORT jstring JNICALL
Java_com_kintcark_sdmediatek_NativeBridge_processQueuedRequests(JNIEnv* env, jobject /* this */) {
    const char* downloads = "/storage/emulated/0/Download";
    DIR *dir = opendir(downloads);
    if (!dir) {
        std::string err = "{\"status\":\"error\",\"message\":\"failed to open Downloads directory\"}";
        return env->NewStringUTF(err.c_str());
    }

    struct dirent *ent;
    int processed = 0;
    std::vector<std::string> results;

    while ((ent = readdir(dir)) != NULL) {
        std::string name = ent->d_name;
        if (name.size() < 20) continue;
        // look for sd_request_<ts>_scaffold.json
        if (name.rfind("sd_request_", 0) != 0) continue; // not starting with prefix
        if (name.find("_scaffold.json") == std::string::npos) continue;

        std::string full = std::string(downloads) + "/" + name;
        std::string req = read_entire_file(full);
        if (req.empty()) continue;

        time_t t = time(nullptr);
        char result_name[512];
        snprintf(result_name, sizeof(result_name), "/storage/emulated/0/Download/sd_result_%lld.json", (long long)t);
        // write processing marker
        std::string processing = "{\"status\":\"processing\",\"request\":\"" + name + "\"}";
        write_entire_file(result_name, processing);

        // Attempt generation (best-effort)
        std::vector<std::string> out_paths;
        std::string error;
        bool ok = generate_from_request(req, out_paths, error);

        std::ostringstream res;
        if (ok) {
            res << "{\"status\":\"success\",\"request\":\"" << name << "\",\"output_images\":" << json_array_from_vector(out_paths) << "}";
        } else {
            res << "{\"status\":\"failed\",\"request\":\"" << name << "\",\"error\":\"";
            // escape error
            for (char c: error) { if (c == '\\') res << "\\\\"; else if (c == '"') res << "\\\""; else res << c; }
            res << "\"}";
        }

        write_entire_file(result_name, res.str());
        results.push_back(result_name);

        // rename original request file so we don't process it again
        std::string processed_name = full + ".processed";
        rename(full.c_str(), processed_name.c_str());

        ++processed;
    }

    closedir(dir);

    std::ostringstream summary;
    summary << "{\"status\":\"done\",\"processed\":" << processed << ",\"results\":" << json_array_from_vector(results) << "}";
    return env->NewStringUTF(summary.str().c_str());
}
