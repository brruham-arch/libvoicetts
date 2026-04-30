/**
 * voicetts.cpp - AML TTS Mod untuk SA-MP Android (SampVoice)
 * Target: ARM 32-bit, Android API 19+
 * Alur: /tts <text> -> eSpeak-NG PCM -> inject BASS record buffer -> SampVoice encode+kirim
 *
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

// eSpeak-NG
#include "espeak-ng/speak_lib.h"

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
typedef int BOOL;

// STREAMPROC untuk inject PCM custom
typedef DWORD (*STREAMPROC)(HSTREAM handle, void* buffer, DWORD length, void* user);

#define BASS_SAMPLE_FLOAT   0x100
#define BASS_STREAM_DECODE  0x200000
#define STREAMPROC_PUSH     ((STREAMPROC)-1)

// ============================================================
// Globals
// ============================================================
static int   g_tts_enabled  = 1;
static float g_tts_pitch    = 1.0f;  // 0.5 - 2.0
static float g_tts_speed    = 1.0f;  // 0.5 - 2.0
static int   g_tts_volume   = 100;   // 0 - 200

// PCM ring buffer untuk feed ke BASS
#define PCM_BUF_SIZE (48000 * 4)  // 4 detik @ 48kHz mono 16-bit
static short  g_pcm_buf[PCM_BUF_SIZE];
static int    g_pcm_write = 0;
static int    g_pcm_read  = 0;
static int    g_pcm_avail = 0;
static pthread_mutex_t g_pcm_mutex = PTHREAD_MUTEX_INITIALIZER;

// BASS stream untuk inject
static HSTREAM g_pushStream = 0;
static int    g_espeak_ready = 0;

// ============================================================
// BASS function pointers
// ============================================================
static HRECORD (*orig_BASSRecordStart)(DWORD,DWORD,DWORD,void*,void*)    = nullptr;
static HSTREAM (*pBASSStreamCreate)(DWORD,DWORD,DWORD,STREAMPROC,void*)  = nullptr;
static DWORD   (*pBASSStreamPutData)(HSTREAM,const void*,DWORD)          = nullptr;
static BOOL    (*pBASSChannelPlay)(HSTREAM,BOOL)                          = nullptr;
static BASS_ChannelSetDSP_t pBASSChannelSetDSP                              = nullptr;

typedef unsigned int HDSP;
typedef HDSP (*BASS_ChannelSetDSP_t)(DWORD,void*,void*,int);

// Dobby
static void* (*pDobbySymbolResolver)(const char*, const char*) = nullptr;
static int   (*pDobbyHook)(void*, void*, void**)               = nullptr;

// ============================================================
// eSpeak-NG callback — terima PCM, masuk ring buffer
// ============================================================
static int espeak_synth_cb(short* wav, int numsamples, espeak_EVENT* events) {
    if (!wav || numsamples <= 0) return 0;

    pthread_mutex_lock(&g_pcm_mutex);
    for (int i = 0; i < numsamples && g_pcm_avail < PCM_BUF_SIZE; i++) {
        g_pcm_buf[g_pcm_write] = wav[i];
        g_pcm_write = (g_pcm_write + 1) % PCM_BUF_SIZE;
        g_pcm_avail++;
    }
    pthread_mutex_unlock(&g_pcm_mutex);

    // Ring buffer diisi, DSP akan inject ke HRECORD saat ada audio

    return 0;
}

// ============================================================
// DSP callback — dipanggil BASS tiap ada mic audio
// Kalau ada TTS PCM di ring buffer, replace mic dengan TTS
// ============================================================
static void CALLBACK tts_dsp_proc(HDSP dsp, DWORD channel, void* buffer, DWORD length, void* user) {
    short* pcm = (short*)buffer;
    int samples = (int)(length / sizeof(short));

    pthread_mutex_lock(&g_pcm_mutex);
    if (g_pcm_avail >= samples) {
        for (int i = 0; i < samples; i++) {
            pcm[i] = g_pcm_buf[g_pcm_read];
            g_pcm_read = (g_pcm_read + 1) % PCM_BUF_SIZE;
            g_pcm_avail--;
        }
        LOGF("[TTS] DSP: inject %d samples ke SampVoice", samples);
    }
    // Kalau tidak ada TTS, mic audio lewat apa adanya
    pthread_mutex_unlock(&g_pcm_mutex);
}

// ============================================================
// Hook BASS_RecordStart
// Saat SampVoice init recorder, kita juga buat push stream
// ============================================================
static HRECORD hook_BASSRecordStart(DWORD freq, DWORD chans, DWORD flags, void* proc, void* user) {
    HRECORD handle = orig_BASSRecordStart(freq, chans, flags, proc, user);
    LOGF("[TTS] BASSRecordStart hooked freq=%u chans=%u handle=%u", freq, chans, handle);

    // Pasang DSP ke HRECORD supaya TTS PCM bisa diinject ke SampVoice
    if (pBASSChannelSetDSP) {
        pBASSChannelSetDSP(handle, (void*)tts_dsp_proc, nullptr, 0);
        LOGF("[TTS] DSP installed on HRECORD %u", handle);
    }
    return handle;
}

// ============================================================
// TTS API
// ============================================================
static int lazy_espeak_init() {
    if (g_espeak_ready) return 1;
    const char* espeakData = "/storage/emulated/0/espeak-ng-data";
    // Cek file phondata — wajib ada, kalau tidak ada espeak_Initialize SIGSEGV
    char phondata_path[256];
    snprintf(phondata_path, sizeof(phondata_path), "%s/phondata", espeakData);
    FILE* pf = fopen(phondata_path, "rb");
    if (!pf) {
        LOGF("[TTS] ERROR: phondata tidak ada di %s", phondata_path);
        return 0;
    }
    fseek(pf, 0, SEEK_END);
    long psize = ftell(pf);
    fclose(pf);
    LOGF("[TTS] phondata size: %ld bytes", psize);
    if (psize < 1000) {
        LOGF("[TTS] ERROR: phondata %ld bytes - ini LFS pointer bukan data asli!", psize);
        LOGF("[TTS] Jalankan CI baru (dengan git lfs pull) lalu re-download artifact.");
        return 0;
    }
    LOGF("[TTS] phondata OK (%ld bytes), memanggil espeak_Initialize...", psize);
    int sr = espeak_Initialize(AUDIO_OUTPUT_RETRIEVAL, 0, espeakData, 0);
    LOGF("[TTS] espeak_Initialize returned: %d", sr);
    if (sr < 0) { LOGF("[TTS] ERROR: espeak_Initialize failed: %d", sr); return 0; }
    LOGF("[TTS] SetSynthCallback...");
    espeak_SetSynthCallback(espeak_synth_cb);
    LOGF("[TTS] SetVoiceByName...");
    espeak_SetVoiceByName("id");
    g_espeak_ready = 1;
    return 1;
}

static void _tts_speak(const char* text) {
    if (!text || !g_tts_enabled) return;
    if (!lazy_espeak_init()) return;
    LOGF("[TTS] Speaking: %s", text);

    // Set parameter eSpeak
    espeak_SetParameter(espeakRATE,   (int)(175 * g_tts_speed),  0);
    espeak_SetParameter(espeakPITCH,  (int)(50  * g_tts_pitch),  0);
    espeak_SetParameter(espeakVOLUME, g_tts_volume,               0);

    espeak_Synth(text, strlen(text) + 1, 0, POS_CHARACTER, 0,
                 espeakCHARS_UTF8, nullptr, nullptr);
    espeak_Synchronize();
}

static void  _tts_set_pitch(float v)  { g_tts_pitch   = v < 0.5f ? 0.5f : (v > 2.0f ? 2.0f : v); }
static void  _tts_set_speed(float v)  { g_tts_speed   = v < 0.5f ? 0.5f : (v > 3.0f ? 3.0f : v); }
static void  _tts_set_volume(int v)   { g_tts_volume  = v < 0   ? 0   : (v > 200 ? 200 : v); }
static void  _tts_enable(void)        { g_tts_enabled = 1; }
static void  _tts_disable(void)       { g_tts_enabled = 0; }
static int   _tts_is_enabled(void)    { return g_tts_enabled; }
static float _tts_get_pitch(void)     { return g_tts_pitch; }
static float _tts_get_speed(void)     { return g_tts_speed; }

// ============================================================
// Exported API struct (dibaca Lua via FFI)
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
};

#define EXPORT __attribute__((visibility("default")))

extern "C" {

EXPORT TtsAPI tts_api = {
    _tts_speak,
    _tts_set_pitch,
    _tts_set_speed,
    _tts_set_volume,
    _tts_enable,
    _tts_disable,
    _tts_is_enabled,
    _tts_get_pitch,
    _tts_get_speed,
};

EXPORT void* __GetModInfo() {
    static const char* info = "libvoicetts|1.0|VoiceTTS eSpeak-NG for SampVoice|brruham";
    return (void*)info;
}

EXPORT void OnModPreLoad() {
    remove(LOGFILE);
    LOGF("[TTS] OnModPreLoad");
}

EXPORT void OnModLoad() {
    LOGF("[TTS] OnModLoad start");

    // --- Dobby ---
    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { LOGF("[TTS] ERROR: libdobby"); return; }
    pDobbySymbolResolver = (void*(*)(const char*,const char*))dlsym(hDobby, "DobbySymbolResolver");
    pDobbyHook           = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    if (!pDobbySymbolResolver || !pDobbyHook) { LOGF("[TTS] ERROR: Dobby sym"); return; }

    // --- BASS ---
    void* hBASS = dlopen("libBASS.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hBASS) { LOGF("[TTS] ERROR: libBASS"); return; }
    pBASSStreamCreate   = (HSTREAM(*)(DWORD,DWORD,DWORD,STREAMPROC,void*))dlsym(hBASS, "BASS_StreamCreate");
    pBASSStreamPutData  = (DWORD(*)(HSTREAM,const void*,DWORD))dlsym(hBASS, "BASS_StreamPutData");
    pBASSChannelPlay    = (BOOL(*)(HSTREAM,BOOL))dlsym(hBASS, "BASS_ChannelPlay");
    pBASSChannelSetDSP  = (BASS_ChannelSetDSP_t)dlsym(hBASS, "BASS_ChannelSetDSP");
    LOGF("[TTS] BASS StreamCreate=%p PutData=%p", pBASSStreamCreate, pBASSStreamPutData);

    // Hook BASS_RecordStart
    void* addrRec = pDobbySymbolResolver("libBASS.so", "BASS_RecordStart");
    if (!addrRec) { LOGF("[TTS] ERROR: BASS_RecordStart addr"); return; }
    if (pDobbyHook(addrRec, (void*)hook_BASSRecordStart, (void**)&orig_BASSRecordStart) != 0) {
        LOGF("[TTS] ERROR: DobbyHook RecordStart"); return;
    }
    LOGF("[TTS] BASS_RecordStart hooked");

    // Tulis addr API ke file untuk Lua
    FILE* af = fopen("/storage/emulated/0/voicetts_addr.txt", "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&tts_api); fclose(af); }

    LOGF("[TTS] OnModLoad SELESAI (espeak lazy)!");
}

} // extern "C"
