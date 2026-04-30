# libvoicetts — TTS untuk SA-MP Android (SampVoice)

**Author:** brruham  
**Target:** ARM 32-bit, Android API 19+, armeabi-v7a

---

## Cara kerja

```
/tts <text>
    → libvoicetts.so
    → eSpeak-NG generate PCM 48kHz mono
    → inject ke BASS push stream
    → SampVoice encode Opus + kirim UDP port 8001
```

---

## Setup setelah build

### 1. Push espeak-ng-data ke device
```bash
# Di Termux atau ADB
# Download espeak-ng-data dari release eSpeak-NG
# Copy folder espeak-ng-data ke:
cp -r espeak-ng-data /storage/emulated/0/espeak-ng-data
```

### 2. Install .so
Copy `libvoicetts.so` ke folder mod AML kamu (biasanya `/data/data/com.rockstargames.gtasa/...`).

### 3. Install Lua script
Copy `lua/voicetts.lua` ke folder MoNetLoader scripts.

---

## Build via GitHub Actions

1. Fork/push repo ini ke GitHub
2. GitHub Actions otomatis build saat push ke `main`
3. Download artifact `libvoicetts-armeabi-v7a` dari tab Actions

---

## Commands in-game

| Command | Fungsi |
|---------|--------|
| `/tts <text>` | Ucapkan teks |
| `/tts on` | Aktifkan TTS |
| `/tts off` | Nonaktifkan TTS |
| `/ttspitch <0.5-2.0>` | Set pitch suara |
| `/ttsrate <0.5-3.0>` | Set kecepatan bicara |
| `/ttsvol <0-200>` | Set volume |

---

## Dependencies

- `libdobby.so` — hooking
- `libBASS.so` — audio stream
- `libsamp.so` — SampVoice encode + kirim
- eSpeak-NG data di `/storage/emulated/0/espeak-ng-data/`

---

## Log files

- `/storage/emulated/0/voicetts_log.txt` — debug log
- `/storage/emulated/0/voicetts_addr.txt` — API address (dihapus otomatis setelah Lua load)
