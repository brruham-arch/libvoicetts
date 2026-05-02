/**
 * voicetts.cpp - AML TTS Mod untuk SA-MP Android (SampVoice)
 * Alur: /tts <text> -> eSpeak-NG PCM -> GetData hook inject -> SampVoice encode+kirim
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
typedef unsigned int HDSP;
typedef int          BOOL;

typedef DWORD (*STREAMPROC)(HSTREAM handle, void* buffer, DWORD length, void* user);
typedef void  (*DSPPROC)(HDSP handle, DWORD channel, void* buffer, DWORD length, void* user);

#define BASS_SAMPLE_FLOAT   0x100
#define BASS_STREAM_DECODE  0x200000
#define STREAMPROC_PUSH     ((STREAMPROC)-1)

// ============================================================
// Globals
// ============================================================
static int   g_tts_enabled = 1;
static float g_tts_pitch   = 1.0f;
static float g_tts_speed   = 1.0f;
static int   g_tts_volume  = 100;

// PCM ring buffer
#define PCM_BUF_SIZE (48000 * 4)  // 4 detik @ 48kHz mono 16-bit
static short  g_pcm_buf[PCM_BUF_SIZE];
static int    g_pcm_write = 0;
static int    g_pcm_read  = 0;
static int    g_pcm_avail = 0;
static pthread_mutex_t g_pcm_mutex = PTHREAD_MUTEX_INITIALIZER;

static int              g_espeak_ready  = 0;
static int              g_dsp_log_count = 0;
static volatile int     g_dsp_call_count = 0;  // FIX: deklarasi di atas sebelum dipakai
static HRECORD          g_hrecord       = 0;
static void*            g_orig_rec_proc = nullptr;

typedef int (*RECORDPROC_t)(DWORD, const void*, DWORD, void*);

// ============================================================
// BASS function pointers
// ============================================================
static HRECORD (*orig_BASSRecordStart)(DWORD,DWORD,DWORD,void*,void*) = nullptr;
static HSTREAM (*pBASSStreamCreate)(DWORD,DWORD,DWORD,STREAMPROC,void*) = nullptr;
static DWORD   (*pBASSStreamPutData)(HSTREAM,const void*,DWORD)       = nullptr;
static BOOL    (*pBASSChannelPlay)(DWORD,BOOL)                         = nullptr;
// FIX: hapus pBASSChannelPause (tidak dipakai langsung, hanya hook)
static BOOL    (*orig_BASSChannelPause)(DWORD)                         = nullptr;
static DWORD   (*pBASSChannelIsActive)(DWORD)                          = nullptr;
static DWORD   (*orig_BASSChannelIsActive)(DWORD)                      = nullptr;
static HDSP    (*pBASSChannelSetDSP)(DWORD,DSPPROC,void*,int)         = nullptr;
static DWORD   (*orig_BASSChannelGetData)(DWORD,void*,DWORD)          = nullptr;

// Dobby
static void* (*pDobbySymbolResolver)(const char*, const char*) = nullptr;
static int   (*pDobbyHook)(void*, void*, void**)               = nullptr;

// ============================================================
// Forward declarations  — FIX: wajib ada sebelum dipakai
// ============================================================
static void  tts_dsp_proc(HDSP, DWORD, void*, DWORD, void*);
static void* tts_transmit_thread(void*);

// ============================================================
// eSpeak callback — resample 22050->48000 lalu masuk ring buffer
// ============================================================
#define ESPEAK_RATE   22050
#define RECORD_RATE   48000

static int espeak_synth_cb(short* wav, int numsamples, espeak_EVENT* events) {
    if (!wav || numsamples <= 0) return 0;
    int out_count = (int)(((long long)numsamples * RECORD_RATE + ESPEAK_RATE - 1) / ESPEAK_RATE);
    pthread_mutex_lock(&g_pcm_mutex);
    for (int i = 0; i < out_count && g_pcm_avail < PCM_BUF_SIZE; i++) {
        float src_pos = (float)i * ESPEAK_RATE / RECORD_RATE;
        int   src_i   = (int)src_pos;
        float frac    = src_pos - src_i;
        short s0 = wav[src_i];
        short s1 = (src_i + 1 < numsamples) ? wav[src_i + 1] : s0;
        g_pcm_buf[g_pcm_write] = (short)(s0 + frac * (s1 - s0));
        g_pcm_write = (g_pcm_write + 1) % PCM_BUF_SIZE;
        g_pcm_avail++;
    }
    pthread_mutex_unlock(&g_pcm_mutex);
    return 0;
}

// ============================================================
// Hook BASS_ChannelGetData
// ============================================================
static DWORD hook_BASSChannelGetData(DWORD handle, void* buf, DWORD len) {
    DWORD ret = orig_BASSChannelGetData(handle, buf, len);
    if (handle != g_hrecord) return ret;
    if (ret == 0 || ret == (DWORD)-1 || ret == (DWORD)-2) return ret;
    if (len & 0x40000000) return ret;
    if (buf == nullptr) return ret;

    // TTS disabled — kembalikan mic asli
    if (!g_tts_enabled) return ret;

    // TTS aktif — selalu timpa mic, inject TTS jika ada, silence jika tidak ada
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
            LOGF("[TTS] GetData inject %d/%d (avail_left=%d)", inject, samples, g_pcm_avail);
        }
    } else {
        memset(buf, 0, ret);
    }
    pthread_mutex_unlock(&g_pcm_mutex);
    return ret;
}

// ============================================================
// Hook BASS_ChannelIsActive
// ============================================================
static DWORD hook_BASSChannelIsActive(DWORD handle) {
    if (handle == g_hrecord) {
        pthread_mutex_lock(&g_pcm_mutex);
        int avail = g_pcm_avail;
        pthread_mutex_unlock(&g_pcm_mutex);
        if (avail > 0) return 1;
    }
    return orig_BASSChannelIsActive(handle);
}

// ============================================================
// Hook BASS_ChannelPause — FIX: hanya di C++, Lua tidak boleh hook lagi
// ============================================================
static BOOL hook_BASSChannelPause(DWORD handle) {
    if (handle == g_hrecord) {
        pthread_mutex_lock(&g_pcm_mutex);
        int avail = g_pcm_avail;
        pthread_mutex_unlock(&g_pcm_mutex);
        if (avail > 0) {
            LOGF("[TTS] ChannelPause BLOCKED (avail=%d)", avail);
            return 1;  // pura-pura sukses tapi tidak pause
        }
    }
    // FIX: guard null — orig bisa null jika Dobby gagal
    if (orig_BASSChannelPause)
        return orig_BASSChannelPause(handle);
    return 0;
}

static volatile int g_tts_playing = 0;

// Args RecordStart untuk thread
static DWORD  g_rec_freq  = 0;
static DWORD  g_rec_chans = 0;
static DWORD  g_rec_flags = 0;
static void*  g_rec_proc  = nullptr;
static void*  g_rec_user  = nullptr;

// Koordinat mic button
static float g_mic_btn_x = -1.0f;
static float g_mic_btn_y = -1.0f;

static void inject_mic_tap() {
    if (g_mic_btn_x < 0 || g_mic_btn_y < 0) return;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "input touchscreen tap %.0f %.0f",
             g_mic_btn_x, g_mic_btn_y);
    system(cmd);
    LOGF("[TTS] mic tap: %.0f,%.0f", g_mic_btn_x, g_mic_btn_y);
}

// ============================================================
// Transmit thread
// ============================================================
static void* tts_transmit_thread(void*) {
    struct timespec ts = {0, 100000000L};
    while (1) {
        nanosleep(&ts, nullptr);
        if (g_mic_btn_x < 0) continue;
        pthread_mutex_lock(&g_pcm_mutex);
        int avail = g_pcm_avail;
        pthread_mutex_unlock(&g_pcm_mutex);
        if (avail <= 0) continue;
        // Skip jika mic sudah aktif
        if (orig_BASSChannelIsActive && g_hrecord &&
            orig_BASSChannelIsActive(g_hrecord) == 1) continue;
        LOGF("[TTS] thread: tap mic ON (avail=%d)", avail);
        inject_mic_tap();
        // Tunggu mic aktif max 1 detik
        int waited = 0;
        while (waited < 10) {
            nanosleep(&ts, nullptr);
            if (orig_BASSChannelIsActive && orig_BASSChannelIsActive(g_hrecord) == 1) break;
            waited++;
        }
        LOGF("[TTS] thread: mic active=%u",
             orig_BASSChannelIsActive ? orig_BASSChannelIsActive(g_hrecord) : 99);
        // Tunggu buffer habis
        while (1) {
            nanosleep(&ts, nullptr);
            pthread_mutex_lock(&g_pcm_mutex);
            int left = g_pcm_avail;
            pthread_mutex_unlock(&g_pcm_mutex);
            if (left <= 0) break;
        }
        // Tap OFF
        inject_mic_tap();
        LOGF("[TTS] thread: tap mic OFF");
    }
    return nullptr;
}

// ============================================================
// DSP callback
// ============================================================
static void tts_dsp_proc(HDSP dsp, DWORD channel, void* buffer, DWORD length, void* user) {
    short* pcm     = (short*)buffer;
    int    samples = (int)(length / sizeof(short));
    int cc = __sync_add_and_fetch(&g_dsp_call_count, 1);
    if (cc == 1) LOGF("[TTS] DSP: mic aktif, GetData akan inject");
}

// ============================================================
// Hook BASS_RecordStart
// ============================================================
static HRECORD hook_BASSRecordStart(DWORD freq, DWORD chans, DWORD flags, void* proc, void* user) {
    g_rec_freq = freq; g_rec_chans = chans; g_rec_flags = flags;
    g_rec_proc = proc; g_rec_user  = user;
    HRECORD handle = orig_BASSRecordStart(freq, chans, flags, proc, user);
    LOGF("[TTS] BASSRecordStart hooked freq=%u chans=%u handle=%u", freq, chans, handle);
    if (pBASSChannelSetDSP && handle) {
        g_hrecord = handle;
        pBASSChannelSetDSP(handle, tts_dsp_proc, nullptr, 0);
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

    char phondata_path[256];
    snprintf(phondata_path, sizeof(phondata_path), "%s/phondata", espeakData);
    FILE* pf = fopen(phondata_path, "rb");
    if (!pf) { LOGF("[TTS] ERROR: phondata tidak ada"); return 0; }
    fseek(pf, 0, SEEK_END);
    long psize = ftell(pf);
    fclose(pf);
    LOGF("[TTS] phondata %ld bytes", psize);
    if (psize < 1000) { LOGF("[TTS] ERROR: phondata LFS pointer!"); return 0; }

    LOGF("[TTS] espeak_Initialize...");
    int sr = espeak_Initialize(AUDIO_OUTPUT_RETRIEVAL, 0, espeakData, 0);
    LOGF("[TTS] espeak_Initialize returned: %d", sr);
    if (sr < 0) { LOGF("[TTS] ERROR: espeak_Initialize failed"); return 0; }

    espeak_SetSynthCallback(espeak_synth_cb);
    espeak_SetVoiceByName("id");
    g_espeak_ready = 1;
    LOGF("[TTS] espeak ready!");
    return 1;
}

static void _tts_speak(const char* text) {
    if (!text || !g_tts_enabled) return;
    if (!lazy_espeak_init()) return;
    LOGF("[TTS] Speaking: %s", text);
    g_dsp_log_count  = 0;
    g_dsp_call_count = 0;

    espeak_SetParameter(espeakRATE,    (int)(175 * g_tts_speed), 0);
    espeak_SetParameter(espeakPITCH,   (int)(50  * g_tts_pitch), 0);
    espeak_SetParameter(espeakVOLUME,  g_tts_volume,              0);
    espeak_SetParameter(espeakWORDGAP, 10,                        0);

    espeak_Synth(text, strlen(text) + 1, 0, POS_CHARACTER, 0,
                 espeakCHARS_UTF8, nullptr, nullptr);
    espeak_Synchronize();

    LOGF("[TTS] PCM ready (avail=%d), waiting SampVoice transmit", g_pcm_avail);
}

static void  _tts_set_pitch(float v)  { g_tts_pitch  = v < 0.5f ? 0.5f : (v > 2.0f ? 2.0f : v); }
static void  _tts_set_speed(float v)  { g_tts_speed  = v < 0.5f ? 0.5f : (v > 3.0f ? 3.0f : v); }
static void  _tts_set_volume(int v)   { g_tts_volume = v < 0 ? 0 : (v > 200 ? 200 : v); }
static void  _tts_enable(void)        { g_tts_enabled = 1; }
static void  _tts_disable(void)       { g_tts_enabled = 0; }
static int   _tts_is_enabled(void)    { return g_tts_enabled; }
static float _tts_get_pitch(void)     { return g_tts_pitch; }
static float _tts_get_speed(void)     { return g_tts_speed; }

static void _tts_notify_mic_on(unsigned int handle) {
    if (handle && handle != g_hrecord) {
        LOGF("[TTS] notify_mic_on handle=%u (old=%u)", handle, g_hrecord);
        g_hrecord = handle;
        if (pBASSChannelSetDSP)
            pBASSChannelSetDSP(handle, tts_dsp_proc, nullptr, 0);
    }
}

static int _tts_pcm_avail(void) {
    pthread_mutex_lock(&g_pcm_mutex);
    int a = g_pcm_avail;
    pthread_mutex_unlock(&g_pcm_mutex);
    return a;
}

static unsigned int _tts_get_hrecord(void) { return (unsigned int)g_hrecord; }

static void _tts_set_mic_pos(float x, float y) {
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
};

#define EXPORT __attribute__((visibility("default")))

extern "C" {

EXPORT TtsAPI tts_api = {
    _tts_speak, _tts_set_pitch, _tts_set_speed, _tts_set_volume,
    _tts_enable, _tts_disable, _tts_is_enabled, _tts_get_pitch, _tts_get_speed,
    _tts_notify_mic_on, _tts_pcm_avail, _tts_get_hrecord, _tts_set_mic_pos,
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

    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { LOGF("[TTS] ERROR: libdobby"); return; }
    pDobbySymbolResolver = (void*(*)(const char*,const char*))dlsym(hDobby, "DobbySymbolResolver");
    pDobbyHook           = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    if (!pDobbySymbolResolver || !pDobbyHook) { LOGF("[TTS] ERROR: Dobby sym"); return; }

    void* hBASS = dlopen("libBASS.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hBASS) { LOGF("[TTS] ERROR: libBASS"); return; }

    pBASSStreamCreate  = (HSTREAM(*)(DWORD,DWORD,DWORD,STREAMPROC,void*))dlsym(hBASS, "BASS_StreamCreate");
    pBASSStreamPutData = (DWORD(*)(HSTREAM,const void*,DWORD))dlsym(hBASS, "BASS_StreamPutData");
    pBASSChannelPlay   = (BOOL(*)(DWORD,BOOL))dlsym(hBASS, "BASS_ChannelPlay");
    pBASSChannelSetDSP = (HDSP(*)(DWORD,DSPPROC,void*,int))dlsym(hBASS, "BASS_ChannelSetDSP");

    // Hook BASS_ChannelIsActive
    void* addrIsActive = dlsym(hBASS, "BASS_ChannelIsActive");
    if (addrIsActive) {
        pDobbyHook(addrIsActive, (void*)hook_BASSChannelIsActive, (void**)&orig_BASSChannelIsActive);
        LOGF("[TTS] BASS_ChannelIsActive hooked");
    }

    // Hook BASS_RecordStart
    void* addrRec = pDobbySymbolResolver("libBASS.so", "BASS_RecordStart");
    if (!addrRec) { LOGF("[TTS] ERROR: BASS_RecordStart addr"); return; }
    if (pDobbyHook(addrRec, (void*)hook_BASSRecordStart, (void**)&orig_BASSRecordStart) != 0) {
        LOGF("[TTS] ERROR: DobbyHook RecordStart"); return;
    }
    LOGF("[TTS] BASS_RecordStart hooked");

    // FIX: brace mismatch diperbaiki — addrPause dan addrGetData sejajar, tidak nested
    void* addrPause = pDobbySymbolResolver("libBASS.so", "BASS_ChannelPause");
    if (addrPause) {
        pDobbyHook(addrPause, (void*)hook_BASSChannelPause, (void**)&orig_BASSChannelPause);
        LOGF("[TTS] BASS_ChannelPause hooked");
    }

    void* addrGetData = pDobbySymbolResolver("libBASS.so", "BASS_ChannelGetData");
    if (addrGetData) {
        pDobbyHook(addrGetData, (void*)hook_BASSChannelGetData, (void**)&orig_BASSChannelGetData);
        LOGF("[TTS] BASS_ChannelGetData hooked");
    }

    // Tulis alamat API ke file untuk dibaca Lua
    FILE* af = fopen("/storage/emulated/0/voicetts_addr.txt", "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&tts_api); fclose(af); }

    pthread_t thr;
    pthread_create(&thr, nullptr, tts_transmit_thread, nullptr);
    pthread_detach(thr);
    LOGF("[TTS] transmit thread started");
    LOGF("[TTS] OnModLoad SELESAI!");
}

} // extern "C"
