/**
 * voicetts.cpp - AML TTS Mod untuk SA-MP Android (SampVoice)
 * Alur: /tts <text> -> Piper TTS PCM -> GetData hook inject -> SampVoice encode+kirim
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

#define LOG_TAG  "libvoicetts"
#define LOGFILE  "/storage/emulated/0/voicetts_log.txt"
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

#define BASS_STREAM_DECODE  0x200000
#define STREAMPROC_PUSH     ((STREAMPROC)-1)
#define BASS_ATTRIB_VOL     2

// ============================================================
// Piper config
// ============================================================
#define PIPER_BIN     "/sdcard/piper/piper"
#define PIPER_MODELS  "/sdcard/piper"
#define RECORD_RATE   48000

static char  g_piper_model[256] = "id_ID-argenia-medium.onnx";
static int   g_piper_rate       = 22050;  // sample rate model, set dari Lua jika beda
static float g_tts_speed        = 1.0f;
static float g_tts_pitch        = 1.0f;  // Piper tidak support pitch langsung, reserved
static int   g_tts_volume       = 100;
static int   g_tts_enabled      = 1;
static int   g_play_local       = 1;

// ============================================================
// Ring buffer mic (untuk SampVoice inject)
// ============================================================
#define PCM_BUF_SIZE (48000 * 8)
static short           g_pcm_buf[PCM_BUF_SIZE];
static int             g_pcm_write = 0;
static int             g_pcm_read  = 0;
static int             g_pcm_avail = 0;
static pthread_mutex_t g_pcm_mutex = PTHREAD_MUTEX_INITIALIZER;

// ============================================================
// BASS function pointers
// ============================================================
static HRECORD (*orig_BASSRecordStart)(DWORD,DWORD,DWORD,void*,void*)    = nullptr;
static HSTREAM (*pBASSStreamCreate)(DWORD,DWORD,DWORD,STREAMPROC,void*)  = nullptr;
static DWORD   (*pBASSStreamPutData)(HSTREAM,const void*,DWORD)           = nullptr;
static BOOL    (*pBASSChannelPlay)(DWORD,BOOL)                             = nullptr;
static BOOL    (*orig_BASSChannelPause)(DWORD)                             = nullptr;
static DWORD   (*orig_BASSChannelIsActive)(DWORD)                          = nullptr;
static HDSP    (*pBASSChannelSetDSP)(DWORD,DSPPROC,void*,int)             = nullptr;
static DWORD   (*orig_BASSChannelGetData)(DWORD,void*,DWORD)              = nullptr;
static BOOL    (*pBASSChannelStop)(DWORD)                                  = nullptr;
static BOOL    (*pBASSChannelSetAttribute)(DWORD,DWORD,float)              = nullptr;

// Dobby
static void* (*pDobbySymbolResolver)(const char*, const char*) = nullptr;
static int   (*pDobbyHook)(void*, void*, void**)               = nullptr;

// ============================================================
// Globals
// ============================================================
static HRECORD          g_hrecord       = 0;
static HSTREAM          g_local_stream  = 0;
static int              g_dsp_log_count = 0;
static volatile int     g_dsp_call_count= 0;
static float            g_mic_btn_x     = -1.0f;
static float            g_mic_btn_y     = -1.0f;

// ============================================================
// Resample helper: src_rate -> RECORD_RATE (linear interp)
// feed langsung ke g_pcm_buf dan local stream
// ============================================================
static void feed_pcm(const short* src, int src_samples, int src_rate) {
    if (!src || src_samples <= 0) return;

    int out_count = (int)(((long long)src_samples * RECORD_RATE + src_rate - 1) / src_rate);

    // --- mic ring buffer ---
    pthread_mutex_lock(&g_pcm_mutex);
    for (int i = 0; i < out_count && g_pcm_avail < PCM_BUF_SIZE; i++) {
        float pos  = (float)i * src_rate / RECORD_RATE;
        int   idx  = (int)pos;
        float frac = pos - idx;
        short s0   = src[idx];
        short s1   = (idx + 1 < src_samples) ? src[idx + 1] : s0;
        g_pcm_buf[g_pcm_write] = (short)(s0 + frac * (s1 - s0));
        g_pcm_write = (g_pcm_write + 1) % PCM_BUF_SIZE;
        g_pcm_avail++;
    }
    pthread_mutex_unlock(&g_pcm_mutex);

    // --- local playback ---
    if (g_play_local && g_local_stream && pBASSStreamPutData) {
        short tmp[4096];
        int   filled = 0;
        for (int i = 0; i < out_count; i++) {
            float pos  = (float)i * src_rate / RECORD_RATE;
            int   idx  = (int)pos;
            float frac = pos - idx;
            short s0   = src[idx];
            short s1   = (idx + 1 < src_samples) ? src[idx + 1] : s0;
            tmp[filled++] = (short)(s0 + frac * (s1 - s0));
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
    LOGF("[TTS] local stream created: %u", g_local_stream);
}

static void destroy_local_stream() {
    if (!g_local_stream) return;
    if (pBASSChannelStop) pBASSChannelStop(g_local_stream);
    g_local_stream = 0;
    LOGF("[TTS] local stream destroyed");
}

// ============================================================
// Piper speak — blocking, dipanggil dari speak thread
// ============================================================
static void piper_speak_sync(const char* text) {
    if (!text || text[0] == '\0') return;

    // Sanitize text: ganti ' dengan spasi supaya tidak rusak shell
    char safe_text[512];
    int  j = 0;
    for (int i = 0; text[i] && j < 510; i++) {
        safe_text[j++] = (text[i] == '\'') ? ' ' : text[i];
    }
    safe_text[j] = '\0';

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "echo '%s' | %s --model %s/%s --output_raw --length_scale %.2f 2>/dev/null",
        safe_text,
        PIPER_BIN,
        PIPER_MODELS,
        g_piper_model,
        1.0f / g_tts_speed   // length_scale: 1.0=normal, >1=lambat, <1=cepat
    );

    LOGF("[TTS] piper cmd: %s", cmd);

    FILE* pipe = popen(cmd, "r");
    if (!pipe) { LOGF("[TTS] ERROR: popen failed"); return; }

    if (g_play_local && !g_local_stream) create_local_stream();

    short buf[4096];
    size_t n;
    int total = 0;
    while ((n = fread(buf, sizeof(short), 4096, pipe)) > 0) {
        feed_pcm(buf, (int)n, g_piper_rate);
        total += (int)n;
    }
    pclose(pipe);

    LOGF("[TTS] piper done, total_samples=%d pcm_avail=%d", total, g_pcm_avail);
}

// ============================================================
// Speak thread — supaya _tts_speak tidak blocking game thread
// ============================================================
struct SpeakJob {
    char text[512];
};

static void* speak_thread_func(void* arg) {
    SpeakJob* job = (SpeakJob*)arg;
    piper_speak_sync(job->text);
    free(job);
    return nullptr;
}

static void _tts_speak(const char* text) {
    if (!text || !g_tts_enabled) return;
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
        LOGF("[TTS] DSP installed on %u", handle);
    }
    return handle;
}

// ============================================================
// Transmit thread (mic tap)
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
// API setters
// ============================================================
static void  _tts_set_pitch(float v)      { g_tts_pitch  = v; } // reserved
static void  _tts_set_speed(float v)      { g_tts_speed  = v < 0.5f ? 0.5f : (v > 3.0f ? 3.0f : v); }
static void  _tts_set_volume(int v)       { g_tts_volume = v; } // reserved, kontrol via BASS
static void  _tts_enable(void)            { g_tts_enabled = 1; }
static void  _tts_disable(void)           { g_tts_enabled = 0; }
static int   _tts_is_enabled(void)        { return g_tts_enabled; }
static float _tts_get_pitch(void)         { return g_tts_pitch; }
static float _tts_get_speed(void)         { return g_tts_speed; }

static void _tts_set_voice(const char* model_filename) {
    if (!model_filename || model_filename[0] == '\0') return;
    snprintf(g_piper_model, sizeof(g_piper_model), "%s", model_filename);
    LOGF("[TTS] model=%s", g_piper_model);
}

static void _tts_set_piper_rate(int rate) {
    if (rate > 0) g_piper_rate = rate;
    LOGF("[TTS] piper_rate=%d", g_piper_rate);
}

static void _tts_set_play_local(int v) {
    g_play_local = v;
    if (v) create_local_stream();
    else   destroy_local_stream();
    LOGF("[TTS] play_local=%d", v);
}
static int _tts_get_play_local(void) { return g_play_local; }

static void _tts_notify_mic_on(unsigned int handle) {
    if (handle && handle != g_hrecord) {
        g_hrecord = handle;
        if (pBASSChannelSetDSP)
            pBASSChannelSetDSP(handle, tts_dsp_proc, nullptr, 0);
        LOGF("[TTS] notify_mic_on handle=%u", handle);
    }
}

static int           _tts_pcm_avail(void)   {
    pthread_mutex_lock(&g_pcm_mutex);
    int a = g_pcm_avail;
    pthread_mutex_unlock(&g_pcm_mutex);
    return a;
}
static unsigned int  _tts_get_hrecord(void) { return (unsigned int)g_hrecord; }
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
    void         (*set_voice)(const char*);   // sekarang = nama file model .onnx
    void         (*set_play_local)(int);
    int          (*get_play_local)(void);
    void         (*set_piper_rate)(int);      // NEW: set sample rate model
};

#define EXPORT __attribute__((visibility("default")))

extern "C" {

EXPORT TtsAPI tts_api = {
    _tts_speak, _tts_set_pitch, _tts_set_speed, _tts_set_volume,
    _tts_enable, _tts_disable, _tts_is_enabled, _tts_get_pitch, _tts_get_speed,
    _tts_notify_mic_on, _tts_pcm_avail, _tts_get_hrecord, _tts_set_mic_pos,
    _tts_set_voice, _tts_set_play_local, _tts_get_play_local,
    _tts_set_piper_rate,
};

EXPORT void* __GetModInfo() {
    static const char* info = "libvoicetts|2.0|VoiceTTS Piper for SampVoice|brruham";
    return (void*)info;
}

EXPORT void OnModPreLoad() {
    remove(LOGFILE);
    LOGF("[TTS] OnModPreLoad v2.0 (Piper)");
}

EXPORT void OnModLoad() {
    LOGF("[TTS] OnModLoad start");

    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { LOGF("[TTS] ERROR: libdobby"); return; }
    pDobbySymbolResolver = (void*(*)(const char*,const char*))dlsym(hDobby, "DobbySymbolResolver");
    pDobbyHook           = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    if (!pDobbySymbolResolver || !pDobbyHook) { LOGF("[TTS] ERROR: Dobby sym"); return; }

    void* hBASS = dlopen("libBASS.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hBASS) { LOGF("[TTS] ERROR: libBASS"); return; }

    pBASSStreamCreate        = (HSTREAM(*)(DWORD,DWORD,DWORD,STREAMPROC,void*))dlsym(hBASS, "BASS_StreamCreate");
    pBASSStreamPutData       = (DWORD(*)(HSTREAM,const void*,DWORD))dlsym(hBASS, "BASS_StreamPutData");
    pBASSChannelPlay         = (BOOL(*)(DWORD,BOOL))dlsym(hBASS, "BASS_ChannelPlay");
    pBASSChannelSetDSP       = (HDSP(*)(DWORD,DSPPROC,void*,int))dlsym(hBASS, "BASS_ChannelSetDSP");
    pBASSChannelStop         = (BOOL(*)(DWORD))dlsym(hBASS, "BASS_ChannelStop");
    pBASSChannelSetAttribute = (BOOL(*)(DWORD,DWORD,float))dlsym(hBASS, "BASS_ChannelSetAttribute");

    void* addrIsActive = dlsym(hBASS, "BASS_ChannelIsActive");
    if (addrIsActive)
        pDobbyHook(addrIsActive, (void*)hook_BASSChannelIsActive, (void**)&orig_BASSChannelIsActive);

    void* addrRec = pDobbySymbolResolver("libBASS.so", "BASS_RecordStart");
    if (!addrRec) { LOGF("[TTS] ERROR: BASS_RecordStart addr"); return; }
    if (pDobbyHook(addrRec, (void*)hook_BASSRecordStart, (void**)&orig_BASSRecordStart) != 0) {
        LOGF("[TTS] ERROR: DobbyHook RecordStart"); return;
    }

    void* addrPause = pDobbySymbolResolver("libBASS.so", "BASS_ChannelPause");
    if (addrPause)
        pDobbyHook(addrPause, (void*)hook_BASSChannelPause, (void**)&orig_BASSChannelPause);

    void* addrGetData = pDobbySymbolResolver("libBASS.so", "BASS_ChannelGetData");
    if (addrGetData)
        pDobbyHook(addrGetData, (void*)hook_BASSChannelGetData, (void**)&orig_BASSChannelGetData);

    if (g_play_local) create_local_stream();

    FILE* af = fopen("/storage/emulated/0/voicetts_addr.txt", "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&tts_api); fclose(af); }

    pthread_t thr;
    pthread_create(&thr, nullptr, tts_transmit_thread, nullptr);
    pthread_detach(thr);

    LOGF("[TTS] OnModLoad SELESAI! model=%s rate=%d", g_piper_model, g_piper_rate);
}

} // extern "C"