/**
 * voicetts.cpp - AML TTS Mod untuk SA-MP Android (SampVoice)
 * Alur: /tts <text> -> eSpeak-NG PCM -> DSP inject ke HRECORD -> SampVoice encode+kirim
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

static int     g_espeak_ready  = 0;
static int     g_dsp_log_count = 0;
static HRECORD g_hrecord       = 0;   // handle mic SampVoice
static void*   g_orig_rec_proc = nullptr;  // SampVoice RECORDPROC asli

// Tipe RECORDPROC BASS: BOOL(HRECORD handle, const void* buf, DWORD len, void* user)
typedef int (*RECORDPROC_t)(DWORD, const void*, DWORD, void*);


// ============================================================
// BASS function pointers
// ============================================================
static HRECORD (*orig_BASSRecordStart)(DWORD,DWORD,DWORD,void*,void*) = nullptr;
static HSTREAM (*pBASSStreamCreate)(DWORD,DWORD,DWORD,STREAMPROC,void*) = nullptr;
static DWORD   (*pBASSStreamPutData)(HSTREAM,const void*,DWORD)       = nullptr;
static BOOL    (*pBASSChannelPlay)(DWORD,BOOL)                         = nullptr;
static BOOL    (*pBASSChannelPause)(DWORD)                             = nullptr;
static BOOL    (*orig_BASSChannelPause)(DWORD)                         = nullptr;
static DWORD   (*pBASSChannelIsActive)(DWORD)                          = nullptr;  // 0=stop 1=play 2=stall 3=pause
static HDSP    (*pBASSChannelSetDSP)(DWORD,DSPPROC,void*,int)         = nullptr;
static DWORD   (*orig_BASSChannelGetData)(DWORD,void*,DWORD)          = nullptr;

// Dobby
static void* (*pDobbySymbolResolver)(const char*, const char*) = nullptr;
static int   (*pDobbyHook)(void*, void*, void**)               = nullptr;

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
// Hook BASS_ChannelGetData — SampVoice polling mic PCM via ini
// ============================================================
static DWORD hook_BASSChannelGetData(DWORD handle, void* buf, DWORD len) {
    DWORD ret = orig_BASSChannelGetData(handle, buf, len);
    // Skip jika bukan HRECORD mic, atau error, atau pakai BASS_DATA_* flags (bukan raw PCM)
    if (handle != g_hrecord) return ret;
    if (ret == 0 || ret == (DWORD)-1 || ret == (DWORD)-2) return ret;
    if (len & 0x40000000) return ret;  // BASS_DATA_FLOAT flag
    if (buf == nullptr) return ret;
    // Hanya inject jika ini HRECORD mic SampVoice
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
    }
    pthread_mutex_unlock(&g_pcm_mutex);
    return ret;
}

// ============================================================
// Hook BASS_ChannelPause — block pause saat TTS sedang transmit
// ============================================================
static BOOL hook_BASSChannelPause(DWORD handle) {
    if (handle == g_hrecord) {
        pthread_mutex_lock(&g_pcm_mutex);
        int avail = g_pcm_avail;
        pthread_mutex_unlock(&g_pcm_mutex);
        if (avail > 0) {
            LOGF("[TTS] ChannelPause BLOCKED (avail=%d)", avail);
            return 1;
        }
    }
    return orig_BASSChannelPause(handle);
}

static volatile int g_tts_playing = 0;

// Thread TTS: poll GetData saat mic off supaya inject jalan
// Args SampVoice untuk re-trigger RecordStart
static DWORD  g_rec_freq  = 0;
static DWORD  g_rec_chans = 0;
static DWORD  g_rec_flags = 0;
static void*  g_rec_proc  = nullptr;
static void*  g_rec_user  = nullptr;

// Forward declaration
static void tts_dsp_proc(HDSP, DWORD, void*, DWORD, void*);

static void* tts_transmit_thread(void*) {
    struct timespec ts50 = {0, 50000000L};
    while (1) {
        nanosleep(&ts50, nullptr);
        if (!g_rec_freq) continue;  // belum ada RecordStart
        pthread_mutex_lock(&g_pcm_mutex);
        int avail = g_pcm_avail;
        pthread_mutex_unlock(&g_pcm_mutex);
        if (avail <= 0) continue;
        // Mic sudah aktif — biarkan SampVoice yang poll via GetData
        if (pBASSChannelIsActive && g_hrecord &&
            pBASSChannelIsActive(g_hrecord) == 1) continue;
        // Mic off tapi ada PCM — simulasi mic on via RecordStart
        LOGF("[TTS] thread: trigger RecordStart (avail=%d)", avail);
        HRECORD h = orig_BASSRecordStart(g_rec_freq, g_rec_chans, g_rec_flags,
                                          g_rec_proc, g_rec_user);
        if (!h) { LOGF("[TTS] thread: RecordStart gagal"); continue; }
        g_hrecord = h;
        if (pBASSChannelSetDSP) pBASSChannelSetDSP(h, tts_dsp_proc, nullptr, 0);
        LOGF("[TTS] thread: RecordStart OK handle=%u", h);
        // Tunggu sampai buffer habis
        while (1) {
            nanosleep(&ts50, nullptr);
            pthread_mutex_lock(&g_pcm_mutex);
            int left = g_pcm_avail;
            pthread_mutex_unlock(&g_pcm_mutex);
            if (left <= 0) break;
        }
        // Pause HRECORD yang kita buat
        if (pBASSChannelPause) pBASSChannelPause(h);
        LOGF("[TTS] thread: selesai, HRECORD paused");
    }
    return nullptr;
}

// ============================================================
// DSP callback — inject TTS PCM, auto-pause HRECORD saat selesai
// ============================================================
static volatile int g_dsp_call_count = 0;
static void tts_dsp_proc(HDSP dsp, DWORD channel, void* buffer, DWORD length, void* user) {
    short* pcm     = (short*)buffer;
    int    samples = (int)(length / sizeof(short));
    // Log pertama kali DSP dipanggil setelah TTS
    // DSP hanya untuk deteksi mic aktif — inject dilakukan oleh GetData hook
    int cc = __sync_add_and_fetch(&g_dsp_call_count, 1);
    if (cc == 1) LOGF("[TTS] DSP: mic aktif, GetData akan inject");
}

// ============================================================
// Hook BASS_RecordStart — pasang DSP ke HRECORD SampVoice
// ============================================================
static HRECORD hook_BASSRecordStart(DWORD freq, DWORD chans, DWORD flags, void* proc, void* user) {
    // Simpan args untuk dipakai thread TTS
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
    g_dsp_log_count = 0;

    espeak_SetParameter(espeakRATE,   (int)(175 * g_tts_speed), 0);
    espeak_SetParameter(espeakPITCH,  (int)(50  * g_tts_pitch), 0);
    espeak_SetParameter(espeakVOLUME, g_tts_volume,              0);

    // Synth dulu — isi ring buffer sebelum aktifkan mic
    espeak_Synth(text, strlen(text) + 1, 0, POS_CHARACTER, 0,
                 espeakCHARS_UTF8, nullptr, nullptr);
    espeak_Synchronize();

    // BASS_ChannelPause di-hook: selama avail>0, SampVoice tidak bisa pause HRECORD
    // Jadi audio otomatis terkirim sampai buffer habis
    g_dsp_call_count = 0;
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
};

#define EXPORT __attribute__((visibility("default")))

extern "C" {

EXPORT TtsAPI tts_api = {
    _tts_speak, _tts_set_pitch, _tts_set_speed, _tts_set_volume,
    _tts_enable, _tts_disable, _tts_is_enabled, _tts_get_pitch, _tts_get_speed,
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
    pBASSChannelPause  = (BOOL(*)(DWORD))dlsym(hBASS, "BASS_ChannelPause");
    pBASSChannelIsActive = (DWORD(*)(DWORD))dlsym(hBASS, "BASS_ChannelIsActive");
    pBASSChannelSetDSP = (HDSP(*)(DWORD,DSPPROC,void*,int))dlsym(hBASS, "BASS_ChannelSetDSP");
    LOGF("[TTS] BASS StreamCreate=%p PutData=%p ChannelSetDSP=%p",
         pBASSStreamCreate, pBASSStreamPutData, pBASSChannelSetDSP);

    void* addrRec = pDobbySymbolResolver("libBASS.so", "BASS_RecordStart");
    if (!addrRec) { LOGF("[TTS] ERROR: BASS_RecordStart addr"); return; }
    if (pDobbyHook(addrRec, (void*)hook_BASSRecordStart, (void**)&orig_BASSRecordStart) != 0) {
        LOGF("[TTS] ERROR: DobbyHook RecordStart"); return;
    }
    LOGF("[TTS] BASS_RecordStart hooked");

    void* addrPause = pDobbySymbolResolver("libBASS.so", "BASS_ChannelPause");
    if (addrPause) {
        pDobbyHook(addrPause, (void*)hook_BASSChannelPause, (void**)&orig_BASSChannelPause);
        LOGF("[TTS] BASS_ChannelPause hooked");

    void* addrGetData = pDobbySymbolResolver("libBASS.so", "BASS_ChannelGetData");
    if (addrGetData) {
        pDobbyHook(addrGetData, (void*)hook_BASSChannelGetData, (void**)&orig_BASSChannelGetData);
        LOGF("[TTS] BASS_ChannelGetData hooked");
    }
    }

    FILE* af = fopen("/storage/emulated/0/voicetts_addr.txt", "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&tts_api); fclose(af); }

    pthread_t thr;
    pthread_create(&thr, nullptr, tts_transmit_thread, nullptr);
    pthread_detach(thr);
    LOGF("[TTS] transmit thread started");
    LOGF("[TTS] OnModLoad SELESAI!");
}

} // extern "C"
