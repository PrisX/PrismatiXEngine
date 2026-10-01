#include "Engine/Accessibility/SpeechService.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sapi.h>
#elif defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#include <jni.h>

#include <cstdint>
#include <vector>
#endif

namespace px::accessibility {

#if defined(__ANDROID__)
namespace {

// The Java half lives in the Gradle template:
// com.prismatix.player.PrismatiXSpeech wraps android.speech.tts.TextToSpeech.
constexpr const char* kSpeechClass = "com.prismatix.player.PrismatiXSpeech";

bool ClearException(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

// FindClass on the SDL thread searches the system class loader, which cannot
// see the app's classes, so load the helper through the activity's loader.
jclass LoadAppClass(JNIEnv* env, jobject activity, const char* name) {
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getClassLoader = env->GetMethodID(
        activityClass, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = getClassLoader
        ? env->CallObjectMethod(activity, getClassLoader) : nullptr;
    env->DeleteLocalRef(activityClass);
    if (ClearException(env) || !loader) return nullptr;
    jclass loaderClass = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClass = loaderClass ? env->GetMethodID(
        loaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;") : nullptr;
    jclass result = nullptr;
    if (loadClass) {
        jstring javaName = env->NewStringUTF(name);
        result = static_cast<jclass>(env->CallObjectMethod(loader, loadClass, javaName));
        env->DeleteLocalRef(javaName);
        if (ClearException(env)) result = nullptr;
    }
    if (loaderClass) env->DeleteLocalRef(loaderClass);
    env->DeleteLocalRef(loader);
    return result;
}

// NewStringUTF expects modified UTF-8, which differs from UTF-8 for
// characters outside the BMP (emoji), so hand Java UTF-16 instead.
std::vector<jchar> Utf16(const std::string& utf8) {
    std::vector<jchar> out;
    out.reserve(utf8.size());
    const auto* bytes = reinterpret_cast<const unsigned char*>(utf8.data());
    const std::size_t size = utf8.size();
    for (std::size_t i = 0; i < size;) {
        const unsigned char lead = bytes[i];
        std::uint32_t code = 0xFFFD;
        std::size_t length = 1;
        if (lead < 0x80) { code = lead; }
        else if ((lead & 0xE0) == 0xC0) { length = 2; code = lead & 0x1Fu; }
        else if ((lead & 0xF0) == 0xE0) { length = 3; code = lead & 0x0Fu; }
        else if ((lead & 0xF8) == 0xF0) { length = 4; code = lead & 0x07u; }
        else { length = 0; }
        bool valid = length != 0 && i + length <= size;
        for (std::size_t k = 1; valid && k < length; ++k) {
            if ((bytes[i + k] & 0xC0) != 0x80) valid = false;
            else code = (code << 6) | (bytes[i + k] & 0x3Fu);
        }
        if (valid && length > 1) {
            static constexpr std::uint32_t kMinimum[] = {0, 0, 0x80, 0x800, 0x10000};
            if (code < kMinimum[length] || code > 0x10FFFF ||
                (code >= 0xD800 && code <= 0xDFFF))
                valid = false;
        }
        if (!valid) { code = 0xFFFD; length = 1; }
        if (code >= 0x10000) {
            code -= 0x10000;
            out.push_back(static_cast<jchar>(0xD800 + (code >> 10)));
            out.push_back(static_cast<jchar>(0xDC00 + (code & 0x3FF)));
        } else {
            out.push_back(static_cast<jchar>(code));
        }
        i += length;
    }
    return out;
}

void CallVoid(void* voice, const char* method) {
    if (!voice) return;
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (!env) return;
    auto* object = static_cast<jobject>(voice);
    jclass type = env->GetObjectClass(object);
    jmethodID id = env->GetMethodID(type, method, "()V");
    if (id) env->CallVoidMethod(object, id);
    ClearException(env);
    env->DeleteLocalRef(type);
}

}  // namespace
#endif

SpeechService::SpeechService() {
#ifdef _WIN32
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_comInitialized = SUCCEEDED(initialized);
    ISpVoice* voice = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL,
                                   IID_ISpVoice, reinterpret_cast<void**>(&voice))))
        m_voice = voice;
#elif defined(__ANDROID__)
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(env ? SDL_GetAndroidActivity() : nullptr);
    if (!activity) return;
    if (jclass type = LoadAppClass(env, activity, kSpeechClass)) {
        jmethodID constructor = env->GetMethodID(type, "<init>", "(Landroid/content/Context;)V");
        jobject voice = constructor ? env->NewObject(type, constructor, activity) : nullptr;
        if (!ClearException(env) && voice) m_voice = env->NewGlobalRef(voice);
        if (voice) env->DeleteLocalRef(voice);
        env->DeleteLocalRef(type);
    }
    env->DeleteLocalRef(activity);
#endif
}

SpeechService::~SpeechService() {
    Stop();
#ifdef _WIN32
    if (m_voice) static_cast<ISpVoice*>(m_voice)->Release();
    if (m_comInitialized) CoUninitialize();
#elif defined(__ANDROID__)
    CallVoid(m_voice, "shutdown");
    if (m_voice) {
        if (auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv()))
            env->DeleteGlobalRef(static_cast<jobject>(m_voice));
    }
#endif
}

void SpeechService::Speak(const std::string& utf8Text, const bool interrupt) {
#ifdef _WIN32
    if (!m_voice || utf8Text.empty()) return;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Text.c_str(),
                                          static_cast<int>(utf8Text.size()), nullptr, 0);
    if (count <= 0) return;
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Text.c_str(),
                        static_cast<int>(utf8Text.size()), wide.data(), count);
    const DWORD flags = SPF_ASYNC | (interrupt ? SPF_PURGEBEFORESPEAK : 0);
    static_cast<ISpVoice*>(m_voice)->Speak(wide.c_str(), flags, nullptr);
#elif defined(__ANDROID__)
    if (!m_voice || utf8Text.empty()) return;
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (!env) return;
    const std::vector<jchar> utf16 = Utf16(utf8Text);
    jstring text = env->NewString(utf16.data(), static_cast<jsize>(utf16.size()));
    if (ClearException(env) || !text) return;
    auto* object = static_cast<jobject>(m_voice);
    jclass type = env->GetObjectClass(object);
    jmethodID speak = env->GetMethodID(type, "speak", "(Ljava/lang/String;Z)V");
    if (speak) env->CallVoidMethod(object, speak, text, static_cast<jboolean>(interrupt));
    ClearException(env);
    env->DeleteLocalRef(type);
    env->DeleteLocalRef(text);
#else
    (void)utf8Text; (void)interrupt;
#endif
}

void SpeechService::Stop() {
#ifdef _WIN32
    if (m_voice) static_cast<ISpVoice*>(m_voice)->Speak(L"", SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
#elif defined(__ANDROID__)
    CallVoid(m_voice, "stop");
#endif
}

}  // namespace px::accessibility
