-- voicetts.lua v1.0
-- /tts <text>     = ucapkan teks via SampVoice
-- /ttspitch <val> = set pitch (0.5 - 2.0, default 1.0)
-- /ttsrate <val>  = set speed (0.5 - 3.0, default 1.0)
-- /ttsvol <val>   = set volume (0 - 200, default 100)
-- /tts off/on     = toggle
--
-- Requires: libvoicetts.so (AML mod)
--           espeak-ng-data di /storage/emulated/0/espeak-ng-data/
--
-- Author: brruham

local ffi = require("ffi")

ffi.cdef[[
    typedef struct {
        void  (*speak)(const char* text);
        void  (*set_pitch)(float v);
        void  (*set_speed)(float v);
        void  (*set_volume)(int v);
        void  (*enable)(void);
        void  (*disable)(void);
        int   (*is_enabled)(void);
        float (*get_pitch)(void);
        float (*get_speed)(void);
    } TtsAPI;
    typedef unsigned long uintptr_t;
]]

local tts       = nil
local ADDR_FILE = "/storage/emulated/0/voicetts_addr.txt"
local CFG_FILE  = getWorkingDirectory() .. "/config/voicetts.ini"

-- ============================================================
-- CONFIG
-- ============================================================
local function saveConfig()
    if not tts then return end
    os.execute('mkdir -p "' .. getWorkingDirectory() .. '/config"')
    local f = io.open(CFG_FILE, "w")
    if not f then return end
    f:write("[voicetts]\n")
    f:write("pitch="   .. string.format("%.2f", tts.get_pitch()) .. "\n")
    f:write("speed="   .. string.format("%.2f", tts.get_speed()) .. "\n")
    f:write("enabled=" .. tostring(tts.is_enabled())             .. "\n")
    f:close()
end

local function loadConfig()
    local f = io.open(CFG_FILE, "r")
    if not f then return end
    local cfg = {}
    for line in f:lines() do
        local k, v = line:match("^(%w+)=(.+)$")
        if k then cfg[k] = v end
    end
    f:close()
    return cfg
end

-- ============================================================
-- LOAD ENGINE
-- ============================================================
local function loadEngine()
    local f = io.open(ADDR_FILE, "r")
    if not f then
        sampAddChatMessage("[TTS] ERROR: voicetts_addr.txt tidak ditemukan!", 0xFF4444)
        sampAddChatMessage("[TTS] Pastikan libvoicetts.so sudah di-load AML", 0xFFAA00)
        return nil
    end
    local addrStr = f:read("*l")
    f:close()

    local addr = tonumber(addrStr)
    if not addr or addr == 0 then
        sampAddChatMessage("[TTS] ERROR: addr tidak valid: " .. tostring(addrStr), 0xFF4444)
        return nil
    end

    local ok, api = pcall(function()
        return ffi.cast("TtsAPI*", addr)
    end)
    if not ok or not api then
        sampAddChatMessage("[TTS] ERROR: ffi cast gagal", 0xFF4444)
        return nil
    end

    -- Validasi pointer
    local spPtr = tonumber(ffi.cast("uintptr_t", api.speak))
    if spPtr == 0 then
        sampAddChatMessage("[TTS] ERROR: speak pointer null!", 0xFF4444)
        return nil
    end

    os.remove(ADDR_FILE)
    sampAddChatMessage("[TTS] Engine OK", 0x00FF88)
    return api
end

-- ============================================================
-- MAIN
-- ============================================================
function main()
    while not isSampAvailable() do wait(100) end
    wait(2500)

    sampAddChatMessage("[VoiceTTS] v1.0 by brruham - loading...", 0xFFFF00)

    -- Retry load engine
    for i = 1, 10 do
        tts = loadEngine()
        if tts then break end
        wait(1000)
    end

    if not tts then
        sampAddChatMessage("[TTS] GAGAL load engine. Cek log: voicetts_log.txt", 0xFF4444)
        return
    end

    -- Apply config tersimpan
    local cfg = loadConfig()
    if cfg then
        if cfg.pitch   then tts.set_pitch(tonumber(cfg.pitch) or 1.0) end
        if cfg.speed   then tts.set_speed(tonumber(cfg.speed) or 1.0) end
        if cfg.enabled then
            if cfg.enabled == "0" then tts.disable() else tts.enable() end
        end
        sampAddChatMessage("[TTS] Config loaded: pitch=" .. string.format("%.2f", tts.get_pitch())
            .. " speed=" .. string.format("%.2f", tts.get_speed()), 0x00FFFF)
    end

    sampAddChatMessage("[TTS] Commands: /tts <text> | /ttspitch | /ttsrate | /ttsvol | /tts on/off", 0x00FF88)

    -- ============================================================
    -- COMMANDS
    -- ============================================================

    -- /tts <text> atau /tts on/off
    sampRegisterChatCommand("tts", function(arg)
        if not tts then return end
        if arg == "on" then
            tts.enable()
            sampAddChatMessage("[TTS] ON", 0x00FF88)
            saveConfig()
        elseif arg == "off" then
            tts.disable()
            sampAddChatMessage("[TTS] OFF", 0xFF8800)
            saveConfig()
        elseif arg and #arg > 0 then
            if tts.is_enabled() == 0 then
                sampAddChatMessage("[TTS] Aktifkan dulu dengan /tts on", 0xFFAA00)
                return
            end
            tts.speak(arg)
            sampAddChatMessage("[TTS] >> " .. arg, 0xAAAAAA)
        else
            sampAddChatMessage("[TTS] Usage: /tts <text> | /tts on | /tts off", 0xFFFF00)
        end
    end)

    -- /ttspitch <0.5-2.0>
    sampRegisterChatCommand("ttspitch", function(arg)
        if not tts then return end
        local v = tonumber(arg)
        if v then
            tts.set_pitch(v)
            saveConfig()
            sampAddChatMessage("[TTS] Pitch = " .. string.format("%.2f", v), 0x00FFFF)
        else
            sampAddChatMessage("[TTS] /ttspitch <0.5 - 2.0>  (default: 1.0)", 0xFFFF00)
        end
    end)

    -- /ttsrate <0.5-3.0>
    sampRegisterChatCommand("ttsrate", function(arg)
        if not tts then return end
        local v = tonumber(arg)
        if v then
            tts.set_speed(v)
            saveConfig()
            sampAddChatMessage("[TTS] Speed = " .. string.format("%.2f", v), 0x00FFFF)
        else
            sampAddChatMessage("[TTS] /ttsrate <0.5 - 3.0>  (default: 1.0)", 0xFFFF00)
        end
    end)

    -- /ttsvol <0-200>
    sampRegisterChatCommand("ttsvol", function(arg)
        if not tts then return end
        local v = tonumber(arg)
        if v then
            tts.set_volume(math.floor(v))
            sampAddChatMessage("[TTS] Volume = " .. math.floor(v), 0x00FFFF)
        else
            sampAddChatMessage("[TTS] /ttsvol <0 - 200>  (default: 100)", 0xFFFF00)
        end
    end)

    while true do wait(1000) end
end
