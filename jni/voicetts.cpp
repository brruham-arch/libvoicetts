/**
 * voicetts.cpp - AML TTS Mod untuk SA-MP Android (SampVoice)
 * Alur: /tts <text> -> sherpa-onnx (Piper VITS) -> GetData hook inject -> SampVoice
 *       + optional local playback via BASS push stream
 * Author: brruham
 */

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#include "sherpa-onnx/sherpa-onnx-c-api.h"

#define LOG_TAG "libvoicetts"
#define LOGFILE "/storage/emulated/0/voicetts_log.txt"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

static void logf_impl(const char* msg) {
    FILE* f = fopen(LOGFILE, "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
    LOGI("%s", msg);
}
#define LOGF(fmt, ...) do { char _b[512]; snprintf(_b,sizeof(_b),fmt,##__VA_ARGS__); logf_impl(_b); } while(0)

// ============================================================
// BASS types
// ============================================================
typedef unsigned int DWORD;
typedef unsigned int HRECORD;
typedef unsigned int HSTREAM;
typedef unsigned int HDSP;
typedef int          BOOL;

typedef DWORD (*STREAMPROC)(HSTREAM handle, void* buffer, DWORD length, void* user);
typedef void  (*DSPPROC)(HDSP handle, DWORD channel, void* buffer, DWORD length, void* user);

#define STREAMPROC_PUSH  ((STREAMPROC)-1)
#define BASS_ATTRIB_VOL  2

// ============================================================
// Config
// ============================================================
#define MODEL_DIR   "/sdcard/piper"
#define RECORD_RATE 48000

static char  g_model[256]    = "id_ID-news_tts-medium.onnx";
static float g_tts_speed     = 1.0f;
static int   g_tts_enabled   = 1;
static int   g_play_local    = 1;

// ============================================================
// Ring buffer mic
// ============================================================
#define PCM_BUF_SIZE (48000 * 8)
static short           g_pcm_buf[PCM_BUF_SIZE];
static int             g_pcm_write = 0;
static int             g_pcm_read  = 0;
static int             g_pcm_avail = 0;
static pthread_mutex_t g_pcm_mutex = PTHREAD_MUTEX_INITIALIZER;

// ============================================================
// sherpa-onnx function pointers (dlopen)
// ============================================================
static const SherpaOnnxOfflineTts* (*pCreateOfflineTts)(const SherpaOnnxOfflineTtsConfig*)             = nullptr;
static void                        (*pDestroyOfflineTts)(const SherpaOnnxOfflineTts*)                   = nullptr;
static const SherpaOnnxGeneratedAudio* (*pOfflineTtsGenerate)(const SherpaOnnxOfflineTts*, const char*, int32_t, float) = nullptr;
static void                        (*pDestroyGeneratedAudio)(const SherpaOnnxGeneratedAudio*)           = nullptr;
static int32_t                     (*pOfflineTtsSampleRate)(const SherpaOnnxOfflineTts*)                = nullptr;

static void*                    g_sherpa_handle = nullptr;
static const SherpaOnnxOfflineTts* g_tts_engine = nullptr;
static pthread_mutex_t          g_engine_mutex  = PTHREAD_MUTEX_INITIALIZER;

// ============================================================
// BASS function pointers
// ============================================================
static HRECORD (*orig_BASSRecordStart)(DWORD,DWORD,DWORD,void*,void*)   = nullptr;
static HSTREAM (*pBASSStreamCreate)(DWORD,DWORD,DWORD,STREAMPROC,void*) = nullptr;
static DWORD   (*pBASSStreamPutData)(HSTREAM,const void*,DWORD)          = nullptr;
static BOOL    (*pBASSChannelPlay)(DWORD,BOOL)                            = nullptr;
static BOOL    (*orig_BASSChannelPause)(DWORD)                            = nullptr;
static DWORD   (*orig_BASSChannelIsActive)(DWORD)                         = nullptr;
static HDSP    (*pBASSChannelSetDSP)(DWORD,DSPPROC,void*,int)            = nullptr;
static DWORD   (*orig_BASSChannelGetData)(DWORD,void*,DWORD)             = nullptr;
static BOOL    (*pBASSChannelStop)(DWORD)                                 = nullptr;
static BOOL    (*pBASSChannelSetAttribute)(DWORD,DWORD,float)             = nullptr;

// Dobby
static void* (*pDobbySymbolResolver)(const char*, const char*) = nullptr;
static int   (*pDobbyHook)(void*, void*, void**)               = nullptr;

// ============================================================
// Globals
// ============================================================
static HRECORD      g_hrecord      = 0;
static HSTREAM      g_local_stream = 0;
static int          g_dsp_log_count= 0;
static volatile int g_dsp_call_count=0;
static float        g_mic_btn_x    = -1.0f;
static float        g_mic_btn_y    = -1.0f;

// ============================================================
// Feed float PCM [-1,1] → resample ke RECORD_RATE → ring buffer + local
// ============================================================
static void feed_float_pcm(const float* src, int src_n, int src_rate) {
    if (!src || src_n <= 0) return;

    int out_n = (int)(((long long)src_n * RECORD_RATE + src_rate - 1) / src_rate);

    // mic ring buffer
    pthread_mutex_lock(&g_pcm_mutex);
    for (int i = 0; i < out_n && g_pcm_avail < PCM_BUF_SIZE; i++) {
        float pos  = (float)i * src_rate / RECORD_RATE;
        int   idx  = (int)pos;
        float frac = pos - idx;
        float s0   = src[idx];
        float s1   = (idx + 1 < src_n) ? src[idx + 1] : s0;
        float s    = s0 + frac * (s1 - s0);
        // clamp + convert float->short
        s = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
        g_pcm_buf[g_pcm_write] = (short)(s * 32767.0f);
        g_pcm_write = (g_pcm_write + 1) % PCM_BUF_SIZE;
        g_pcm_avail++;
    }
    pthread_mutex_unlock(&g_pcm_mutex);

    // local playback
    if (g_play_local && g_local_stream && pBASSStreamPutData) {
        short tmp[4096];
        int   filled = 0;
        for (int i = 0; i < out_n; i++) {
            float pos  = (float)i * src_rate / RECORD_RATE;
            int   idx  = (int)pos;
            float frac = pos - idx;
            float s0   = src[idx];
            float s1   = (idx + 1 < src_n) ? src[idx + 1] : s0;
            float s    = s0 + frac * (s1 - s0);
            s = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
            tmp[filled++] = (short)(s * 32767.0f);
            if (filled == 4096) {
                pBASSStreamPutData(g_local_stream, tmp, (DWORD)(filled * sizeof(short)));
                filled = 0;
            }
        }
        if (filled > 0)
            pBASSStreamPutData(g_local_stream, tmp, (DWORD)(filled * sizeof(short)));
    }
}

// ============================================================
// Local stream
// ============================================================
static void create_local_stream() {
    if (!pBASSStreamCreate || !pBASSChannelPlay || g_local_stream) return;
    g_local_stream = pBASSStreamCreate(RECORD_RATE, 1, 0, STREAMPROC_PUSH, nullptr);
    if (!g_local_stream) { LOGF("[TTS] ERROR: local stream create failed"); return; }
    if (pBASSChannelSetAttribute)
        pBASSChannelSetAttribute(g_local_stream, BASS_ATTRIB_VOL, 1.0f);
    pBASSChannelPlay(g_local_stream, 0);
    LOGF("[TTS] local stream: %u", g_local_stream);
}

static void destroy_local_stream() {
    if (!g_local_stream) return;
    if (pBASSChannelStop) pBASSChannelStop(g_local_stream);
    g_local_stream = 0;
}

// ============================================================
// sherpa-onnx engine: init / reinit
// ============================================================
static int init_tts_engine() {
    if (!pCreateOfflineTts || !pDestroyOfflineTts ||
        !pOfflineTtsGenerate || !pDestroyGeneratedAudio) {
        LOGF("[TTS] ERROR: sherpa symbols not loaded");
        return 0;
    }

    pthread_mutex_lock(&g_engine_mutex);

    // Destroy existing engine kalau ada
    if (g_tts_engine) {
        pDestroyOfflineTts(g_tts_engine);
        g_tts_engine = nullptr;
    }

    char model_path[512];
    char tokens_path[512];
    snprintf(model_path,  sizeof(model_path),  "%s/%s",        MODEL_DIR, g_model);
    snprintf(tokens_path, sizeof(tokens_path), "%s/%s.json",   MODEL_DIR, g_model);

    SherpaOnnxOfflineTtsConfig config;
    memset(&config, 0, sizeof(config));

    config.model.vits.model      = model_path;
    config.model.vits.tokens     = tokens_path;
    config.model.vits.data_dir   = nullptr;
    config.model.vits.lexicon    = nullptr;
    config.model.vits.noise_scale   = 0.667f;
    config.model.vits.noise_scale_w = 0.8f;
    config.model.vits.length_scale  = 1.0f / g_tts_speed;
    config.model.num_threads     = 2;
    config.model.provider        = "cpu";
    config.model.debug           = 0;
    config.max_num_sentences     = 1;

    g_tts_engine = pCreateOfflineTts(&config);
    pthread_mutex_unlock(&g_engine_mutex);

    if (!g_tts_engine) {
        LOGF("[TTS] ERROR: CreateOfflineTts failed (model=%s)", model_path);
        return 0;
    }

    int sr = pOfflineTtsSampleRate(g_tts_engine);
    LOGF("[TTS] engine ready, model=%s rate=%d", g_model, sr);
    return 1;
}

// ============================================================
// Speak thread
// ============================================================
struct SpeakJob { char text[512]; };

static void* speak_thread_func(void* arg) {
    SpeakJob* job = (SpeakJob*)arg;

    if (!g_tts_engine) {
        LOGF("[TTS] engine null, skip");
        free(job); return nullptr;
    }

    if (g_play_local && !g_local_stream) create_local_stream();

    pthread_mutex_lock(&g_engine_mutex);
    const SherpaOnnxGeneratedAudio* audio =
        pOfflineTtsGenerate(g_tts_engine, job->text, 0, g_tts_speed);
    pthread_mutex_unlock(&g_engine_mutex);

    if (!audio) {
        LOGF("[TTS] generate failed");
        free(job); return nullptr;
    }

    LOGF("[TTS] generated n=%d rate=%d", audio->n, audio->sample_rate);
    feed_float_pcm(audio->samples, audio->n, audio->sample_rate);
    pDestroyGeneratedAudio(audio);

    free(job);
    return nullptr;
}

static void _tts_speak(const char* text) {
    if (!text || !g_tts_enabled || !g_tts_engine) return;
    LOGF("[TTS] speak: %s", text);
    g_dsp_log_count  = 0;
    g_dsp_call_count = 0;

    SpeakJob* job = (SpeakJob*)malloc(sizeof(SpeakJob));
    if (!job) return;
    snprintf(job->text, sizeof(job->text), "%s", text);

    pthread_t thr;
    pthread_create(&thr, nullptr, speak_thread_func, job);
    pthread_detach(thr);
}

// ============================================================
// BASS hooks
// ============================================================
static DWORD hook_BASSChannelGetData(DWORD handle, void* buf, DWORD len) {
    DWORD ret = orig_BASSChannelGetData(handle, buf, len);
    if (handle != g_hrecord) return ret;
    if (ret == 0 || ret == (DWORD)-1 || ret == (DWORD)-2) return ret;
    if (len & 0x40000000) return ret;
    if (!buf || !g_tts_enabled) return ret;

    short* pcm     = (short*)buf;
    int    samples = (int)(ret / sizeof(short));
    pthread_mutex_lock(&g_pcm_mutex);
    int avail = g_pcm_avail;
    if (avail > 0) {
        int inject = avail < samples ? avail : samples;
        for (int i = 0; i < inject; i++) {
            pcm[i] = g_pcm_buf[g_pcm_read];
            g_pcm_read = (g_pcm_read + 1) % PCM_BUF_SIZE;
            g_pcm_avail--;
        }
        for (int i = inject; i < samples; i++) pcm[i] = 0;
        if (g_dsp_log_count < 10) {
            g_dsp_log_count++;
            LOGF("[TTS] inject %d/%d (left=%d)", inject, samples, g_pcm_avail);
        }
    } else {
        memset(buf, 0, ret);
    }
    pthread_mutex_unlock(&g_pcm_mutex);
    return ret;
}

static DWORD hook_BASSChannelIsActive(DWORD handle) {
    if (handle == g_hrecord) {
        pthread_mutex_lock(&g_pcm_mutex);
        int avail = g_pcm_avail;
        pthread_mutex_unlock(&g_pcm_mutex);
        if (avail > 0) return 1;
    }
    return orig_BASSChannelIsActive(handle);
}

static BOOL hook_BASSChannelPause(DWORD handle) {
    if (handle == g_hrecord) {
        pthread_mutex_lock(&g_pcm_mutex);
        int avail = g_pcm_avail;
        pthread_mutex_unlock(&g_pcm_mutex);
        if (avail > 0) { LOGF("[TTS] pause BLOCKED"); return 1; }
    }
    if (handle == g_local_stream) return 1;
    if (orig_BASSChannelPause) return orig_BASSChannelPause(handle);
    return 0;
}

static void tts_dsp_proc(HDSP dsp, DWORD channel, void* buffer, DWORD length, void* user) {
    int cc = __sync_add_and_fetch(&g_dsp_call_count, 1);
    if (cc == 1) LOGF("[TTS] DSP: mic aktif");
}

static HRECORD hook_BASSRecordStart(DWORD freq, DWORD chans, DWORD flags, void* proc, void* user) {
    HRECORD handle = orig_BASSRecordStart(freq, chans, flags, proc, user);
    LOGF("[TTS] RecordStart freq=%u chans=%u handle=%u", freq, chans, handle);
    if (pBASSChannelSetDSP && handle) {
        g_hrecord = handle;
        pBASSChannelSetDSP(handle, tts_dsp_proc, nullptr, 0);
    }
    return handle;
}

// ============================================================
// Transmit thread
// ============================================================
static void inject_mic_tap() {
    if (g_mic_btn_x < 0 || g_mic_btn_y < 0) return;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "input touchscreen tap %.0f %.0f", g_mic_btn_x, g_mic_btn_y);
    system(cmd);
    LOGF("[TTS] mic tap %.0f,%.0f", g_mic_btn_x, g_mic_btn_y);
}

static void* tts_transmit_thread(void*) {
    struct timespec ts = {0, 100000000L};
    while (1) {
        nanosleep(&ts, nullptr);
        if (g_mic_btn_x < 0) continue;
        pthread_mutex_lock(&g_pcm_mutex);
        int avail = g_pcm_avail;
        pthread_mutex_unlock(&g_pcm_mutex);
        if (avail <= 0) continue;
        if (orig_BASSChannelIsActive && g_hrecord &&
            orig_BASSChannelIsActive(g_hrecord) == 1) continue;
        LOGF("[TTS] tap mic ON (avail=%d)", avail);
        inject_mic_tap();
        int waited = 0;
        while (waited < 10) {
            nanosleep(&ts, nullptr);
            if (orig_BASSChannelIsActive && orig_BASSChannelIsActive(g_hrecord) == 1) break;
            waited++;
        }
        while (1) {
            nanosleep(&ts, nullptr);
            pthread_mutex_lock(&g_pcm_mutex);
            int left = g_pcm_avail;
            pthread_mutex_unlock(&g_pcm_mutex);
            if (left <= 0) break;
        }
        inject_mic_tap();
        LOGF("[TTS] tap mic OFF");
    }
    return nullptr;
}

// ============================================================
// API
// ============================================================
static void _tts_set_speed(float v) {
    g_tts_speed = v < 0.5f ? 0.5f : (v > 3.0f ? 3.0f : v);
    // reinit engine dengan speed baru
    if (g_tts_engine) init_tts_engine();
}
static void  _tts_set_pitch(float v)  { /* reserved */ }
static void  _tts_set_volume(int v)   { /* reserved */ }
static void  _tts_enable(void)        { g_tts_enabled = 1; }
static void  _tts_disable(void)       { g_tts_enabled = 0; }
static int   _tts_is_enabled(void)    { return g_tts_enabled; }
static float _tts_get_pitch(void)     { return 1.0f; }
static float _tts_get_speed(void)     { return g_tts_speed; }

static void _tts_set_voice(const char* model_filename) {
    if (!model_filename || model_filename[0] == '\0') return;
    snprintf(g_model, sizeof(g_model), "%s", model_filename);
    LOGF("[TTS] model=%s, reinit...", g_model);
    init_tts_engine();
}

static void _tts_set_play_local(int v) {
    g_play_local = v;
    if (v) create_local_stream();
    else   destroy_local_stream();
}
static int _tts_get_play_local(void) { return g_play_local; }

static void _tts_set_piper_rate(int v) { /* tidak dipakai, rate dari model */ }

static void _tts_notify_mic_on(unsigned int handle) {
    if (handle && handle != g_hrecord) {
        g_hrecord = handle;
        if (pBASSChannelSetDSP)
            pBASSChannelSetDSP(handle, tts_dsp_proc, nullptr, 0);
        LOGF("[TTS] notify_mic_on=%u", handle);
    }
}

static int           _tts_pcm_avail(void) {
    pthread_mutex_lock(&g_pcm_mutex);
    int a = g_pcm_avail;
    pthread_mutex_unlock(&g_pcm_mutex);
    return a;
}
static unsigned int  _tts_get_hrecord(void)       { return (unsigned int)g_hrecord; }
static void          _tts_set_mic_pos(float x, float y) {
    g_mic_btn_x = x; g_mic_btn_y = y;
    LOGF("[TTS] mic_pos=%.0f,%.0f", x, y);
}

// ============================================================
// Exported API struct
// ============================================================
struct TtsAPI {
    void  (*speak)(const char*);
    void  (*set_pitch)(float);
    void  (*set_speed)(float);
    void  (*set_volume)(int);
    void  (*enable)(void);
    void  (*disable)(void);
    int   (*is_enabled)(void);
    float (*get_pitch)(void);
    float (*get_speed)(void);
    void         (*notify_mic_on)(unsigned int);
    int          (*pcm_avail)(void);
    unsigned int (*get_hrecord)(void);
    void         (*set_mic_pos)(float, float);
    void         (*set_voice)(const char*);
    void         (*set_play_local)(int);
    int          (*get_play_local)(void);
    void         (*set_piper_rate)(int);
};

#define EXPORT __attribute__((visibility("default")))

extern "C" {

EXPORT TtsAPI tts_api = {
    _tts_speak, _tts_set_pitch, _tts_set_speed, _tts_set_volume,
    _tts_enable, _tts_disable, _tts_is_enabled, _tts_get_pitch, _tts_get_speed,
    _tts_notify_mic_on, _tts_pcm_avail, _tts_get_hrecord, _tts_set_mic_pos,
    _tts_set_voice, _tts_set_play_local, _tts_get_play_local, _tts_set_piper_rate,
};

EXPORT void* __GetModInfo() {
    static const char* info = "libvoicetts|3.0|VoiceTTS sherpa-onnx/Piper|brruham";
    return (void*)info;
}

EXPORT void OnModPreLoad() {
    remove(LOGFILE);
    LOGF("[TTS] OnModPreLoad v3.0 (sherpa-onnx)");
}

EXPORT void OnModLoad() {
    LOGF("[TTS] OnModLoad start");

    // Load sherpa-onnx via dlopen dari folder mod
    // Coba dari beberapa path
    const char* sherpa_paths[] = {
        "/data/data/com.rockstargames.gtasa/files/AML/libsherpa-onnx-c-api.so",
        "/sdcard/piper/libsherpa-onnx-c-api.so",
        "libsherpa-onnx-c-api.so",
        nullptr
    };
    for (int i = 0; sherpa_paths[i]; i++) {
        g_sherpa_handle = dlopen(sherpa_paths[i], RTLD_NOW | RTLD_GLOBAL);
        if (g_sherpa_handle) { LOGF("[TTS] sherpa loaded: %s", sherpa_paths[i]); break; }
    }
    if (!g_sherpa_handle) { LOGF("[TTS] ERROR: sherpa-onnx not found"); return; }

    pCreateOfflineTts    = (decltype(pCreateOfflineTts))   dlsym(g_sherpa_handle, "SherpaOnnxCreateOfflineTts");
    pDestroyOfflineTts   = (decltype(pDestroyOfflineTts))  dlsym(g_sherpa_handle, "SherpaOnnxDestroyOfflineTts");
    pOfflineTtsGenerate  = (decltype(pOfflineTtsGenerate)) dlsym(g_sherpa_handle, "SherpaOnnxOfflineTtsGenerate");
    pDestroyGeneratedAudio=(decltype(pDestroyGeneratedAudio))dlsym(g_sherpa_handle,"SherpaOnnxDestroyOfflineTtsGeneratedAudio");
    pOfflineTtsSampleRate= (decltype(pOfflineTtsSampleRate))dlsym(g_sherpa_handle, "SherpaOnnxOfflineTtsSampleRate");

    if (!pCreateOfflineTts || !pOfflineTtsGenerate) {
        LOGF("[TTS] ERROR: sherpa symbols missing"); return;
    }
    LOGF("[TTS] sherpa symbols loaded");

    // Init TTS engine
    if (!init_tts_engine()) return;

    // Dobby
    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { LOGF("[TTS] ERROR: libdobby"); return; }
    pDobbySymbolResolver = (void*(*)(const char*,const char*))dlsym(hDobby, "DobbySymbolResolver");
    pDobbyHook           = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    if (!pDobbySymbolResolver || !pDobbyHook) { LOGF("[TTS] ERROR: Dobby sym"); return; }

    // BASS
    void* hBASS = dlopen("libBASS.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hBASS) { LOGF("[TTS] ERROR: libBASS"); return; }

    pBASSStreamCreate        = (HSTREAM(*)(DWORD,DWORD,DWORD,STREAMPROC,void*))dlsym(hBASS,"BASS_StreamCreate");
    pBASSStreamPutData       = (DWORD(*)(HSTREAM,const void*,DWORD))dlsym(hBASS,"BASS_StreamPutData");
    pBASSChannelPlay         = (BOOL(*)(DWORD,BOOL))dlsym(hBASS,"BASS_ChannelPlay");
    pBASSChannelSetDSP       = (HDSP(*)(DWORD,DSPPROC,void*,int))dlsym(hBASS,"BASS_ChannelSetDSP");
    pBASSChannelStop         = (BOOL(*)(DWORD))dlsym(hBASS,"BASS_ChannelStop");
    pBASSChannelSetAttribute = (BOOL(*)(DWORD,DWORD,float))dlsym(hBASS,"BASS_ChannelSetAttribute");

    void* addrIsActive = dlsym(hBASS, "BASS_ChannelIsActive");
    if (addrIsActive)
        pDobbyHook(addrIsActive,(void*)hook_BASSChannelIsActive,(void**)&orig_BASSChannelIsActive);

    void* addrRec = pDobbySymbolResolver("libBASS.so","BASS_RecordStart");
    if (!addrRec) { LOGF("[TTS] ERROR: BASS_RecordStart"); return; }
    pDobbyHook(addrRec,(void*)hook_BASSRecordStart,(void**)&orig_BASSRecordStart);

    void* addrPause = pDobbySymbolResolver("libBASS.so","BASS_ChannelPause");
    if (addrPause)
        pDobbyHook(addrPause,(void*)hook_BASSChannelPause,(void**)&orig_BASSChannelPause);

    void* addrGetData = pDobbySymbolResolver("libBASS.so","BASS_ChannelGetData");
    if (addrGetData)
        pDobbyHook(addrGetData,(void*)hook_BASSChannelGetData,(void**)&orig_BASSChannelGetData);

    if (g_play_local) create_local_stream();

    FILE* af = fopen("/storage/emulated/0/voicetts_addr.txt","w");
    if (af) { fprintf(af,"%lu\n",(unsigned long)&tts_api); fclose(af); }

    pthread_t thr;
    pthread_create(&thr,nullptr,tts_transmit_thread,nullptr);
    pthread_detach(thr);

    LOGF("[TTS] OnModLoad SELESAI! model=%s", g_model);
}

} // extern "C"
