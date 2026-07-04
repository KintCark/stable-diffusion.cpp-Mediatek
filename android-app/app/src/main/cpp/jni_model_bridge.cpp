# JNI bridge and minimal model integration

#include <jni.h>
#include <string>
#include <vector>
#include <mutex>
#include <dirent.h>
#include <sys/stat.h>

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

extern "C" JNIEXPORT jstring JNICALL
Java_com_kintcark_sdmediatek_NativeBridge_generateImage(JNIEnv* env, jobject /* this */, jstring jparamsJson) {
    const char* params = env->GetStringUTFChars(jparamsJson, 0);
    // Minimal placeholder generation response; real pipeline invocation will be implemented next.
    std::string out = "{\"status\":\"ok\",\"image_path\":\"/storage/emulated/0/Download/sd_output.png\"}";
    env->ReleaseStringUTFChars(jparamsJson, params);
    return env->NewStringUTF(out.c_str());
}
