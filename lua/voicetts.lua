-- voicetts.lua v2.6 by brruham
-- + MBROLA id1 voice support

local ffi    = require("ffi")
local imgui  = require("mimgui")
local inicfg = require("inicfg")
local new    = imgui.new

ffi.cdef[[
    typedef struct {
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
    } TtsAPI;
    typedef unsigned long uintptr_t;
]]

local VOICES = {
    "mb-id1",
    "id","id+m1","id+m2","id+m3","id+m4","id+m5","id+m6","id+m7","id+m8",
    "id+f1","id+f2","id+f3","id+f4","id+f5",
    "id+croak","id+whisper","id+grandpa","id+grandma",
    "ms","ms+m1","ms+m2","ms+f1","ms+f2",
    "en","en+m1","en+m2","en+m3","en+m4","en+m5","en+m6","en+m7",
    "en+f1","en+f2","en+f3","en+f4",
    "en+croak","en+whisper","en+grandpa","en+grandma",
    "en-US","en-US+m1","en-US+m2","en-US+f1","en-US+f2",
}

local voiceItems = ffi.new("const char*[?]", #VOICES)
for i, v in ipairs(VOICES) do voiceItems[i-1] = v end

local CFG_FILE = "voicetts"
local ini = inicfg.load({
    settings = {
        pitch      = 0.80,
        speed      = 0.80,
        volume     = 100,
        enabled    = true,
        voice_idx  = 1,
        play_local = true,
    }
}, CFG_FILE)

local showSettings = new.bool(false)
local showInput    = new.bool(false)
local inputBuf     = new.char[512]()
local sliderPitch  = new.float(ini.settings.pitch)
local sliderSpeed  = new.float(ini.settings.speed)
local sliderVol    = new.int(ini.settings.volume)
local chkEnabled   = new.bool(ini.settings.enabled)
local chkPlayLocal = new.bool(ini.settings.play_local ~= false)
local voiceIdx     = new.int((ini.settings.voice_idx or 1) - 1)

local tts       = nil
local micLocked = false
local ADDR_FILE = "/storage/emulated/0/voicetts_addr.txt"

local function saveConfig()
    if not tts then return end
    ini.settings.pitch      = sliderPitch[0]
    ini.settings.speed      = sliderSpeed[0]
    ini.settings.volume     = sliderVol[0]
    ini.settings.enabled    = chkEnabled[0]
    ini.settings.voice_idx  = voiceIdx[0] + 1
    ini.settings.play_local = chkPlayLocal[0]
    inicfg.save(ini, CFG_FILE)
end

local function updateInputGui()
    showInput[0] = chkEnabled[0] and micLocked
end

local function setMicLock(state)
    micLocked = state
    sampAddChatMessage("[VoiceTTS] Mic " .. (state and "LOCKED" or "UNLOCKED"),
        state and 0x00FF00 or 0xAAAAAA)
    updateInputGui()
end

local function speakText(text)
    if not tts or not chkEnabled[0] or not micLocked then return end
    text = text:match("^%s*(.-)%s*$")
    if #text == 0 then return end
    tts.speak(text)
    sampAddChatMessage("[TTS] >> " .. text, 0xAAAAAA)
end

local sizeX, sizeY = getScreenResolution()

imgui.OnInitialize(function()
    imgui.GetIO().IniFilename = nil
    local s = imgui.GetStyle()
    s.WindowRounding = 6.0
    s.FrameRounding  = 4.0
    s.Colors[imgui.Col.WindowBg]      = imgui.ImVec4(0.08, 0.08, 0.12, 0.96)
    s.Colors[imgui.Col.TitleBgActive] = imgui.ImVec4(0.12, 0.40, 0.75, 1.00)
    s.Colors[imgui.Col.Button]        = imgui.ImVec4(0.15, 0.42, 0.78, 1.00)
    s.Colors[imgui.Col.ButtonHovered] = imgui.ImVec4(0.22, 0.55, 0.90, 1.00)
    s.Colors[imgui.Col.FrameBg]       = imgui.ImVec4(0.14, 0.14, 0.20, 1.00)
    s.Colors[imgui.Col.SliderGrab]    = imgui.ImVec4(0.22, 0.60, 1.00, 1.00)
    s.Colors[imgui.Col.CheckMark]     = imgui.ImVec4(0.22, 0.80, 1.00, 1.00)
end)

imgui.OnFrame(
    function() return showSettings[0] end,
    function()
        imgui.SetNextWindowSize(imgui.ImVec2(300, 0), imgui.Cond.Always)
        imgui.SetNextWindowPos(imgui.ImVec2(sizeX*0.02, sizeY*0.15), imgui.Cond.FirstUseEver)
        imgui.Begin("VoiceTTS - Settings", showSettings,
            imgui.WindowFlags.NoResize + imgui.WindowFlags.NoScrollbar)

        local ttsOn = chkEnabled[0]
        imgui.TextColored(ttsOn and imgui.ImVec4(0.3,1,0.3,1) or imgui.ImVec4(0.7,0.7,0.7,1),
            "TTS: " .. (ttsOn and "ON" or "OFF"))
        imgui.SameLine()
        imgui.TextColored(micLocked and imgui.ImVec4(0.3,1,0.3,1) or imgui.ImVec4(1,0.4,0.4,1),
            "   MIC: " .. (micLocked and "LOCK" or "OFF"))
        imgui.Separator()

        if imgui.Button(ttsOn and "TTS OFF" or "TTS ON", imgui.ImVec2(135, 30)) then
            chkEnabled[0] = not ttsOn
            if tts then
                if chkEnabled[0] then tts.enable() else tts.disable() end
            end
            if not chkEnabled[0] and micLocked then setMicLock(false) end
            saveConfig(); updateInputGui()
        end
        imgui.SameLine()

        imgui.PushStyleColor(imgui.Col.Button,
            micLocked and imgui.ImVec4(0.15,0.60,0.15,1) or imgui.ImVec4(0.60,0.15,0.15,1))
        imgui.PushStyleColor(imgui.Col.ButtonHovered,
            micLocked and imgui.ImVec4(0.20,0.75,0.20,1) or imgui.ImVec4(0.75,0.20,0.20,1))
        if imgui.Button(micLocked and "MIC LOCK" or "MIC OFF", imgui.ImVec2(135, 30)) then
            if chkEnabled[0] then setMicLock(not micLocked)
            else sampAddChatMessage("[TTS] Aktifkan TTS dulu", 0xFFAA00) end
        end
        imgui.PopStyleColor(2)

        imgui.Spacing(); imgui.Separator()
        imgui.PushItemWidth(270)

        imgui.Text("Pitch:")
        if imgui.SliderFloat("##pitch", sliderPitch, 0.5, 2.0, "%.2f") then
            if tts then tts.set_pitch(sliderPitch[0]) end; saveConfig()
        end
        imgui.Text("Speed:")
        if imgui.SliderFloat("##speed", sliderSpeed, 0.5, 3.0, "%.2f") then
            if tts then tts.set_speed(sliderSpeed[0]) end; saveConfig()
        end
        imgui.Text("Volume:")
        if imgui.SliderInt("##vol", sliderVol, 0, 200) then
            if tts then tts.set_volume(sliderVol[0]) end; saveConfig()
        end

        imgui.Spacing()
        imgui.Text("Voice:")
        if imgui.Combo("##voice", voiceIdx, voiceItems, #VOICES) then
            if tts then tts.set_voice(VOICES[voiceIdx[0] + 1]) end
            saveConfig()
            sampAddChatMessage("[TTS] Voice=" .. VOICES[voiceIdx[0] + 1], 0x00FFFF)
        end

        imgui.Spacing(); imgui.Separator()

        if imgui.Checkbox("Play Local (dengar sendiri)", chkPlayLocal) then
            if tts then tts.set_play_local(chkPlayLocal[0] and 1 or 0) end
            saveConfig()
            sampAddChatMessage("[TTS] Play Local: " .. (chkPlayLocal[0] and "ON" or "OFF"),
                chkPlayLocal[0] and 0x00FF88 or 0xAAAAAA)
        end
        imgui.SameLine()
        imgui.TextDisabled("(?)")
        if imgui.IsItemHovered() then
            imgui.BeginTooltip()
            imgui.Text("Jika ON, suara TTS juga\nterdengar di speaker kamu sendiri.")
            imgui.EndTooltip()
        end

        imgui.PopItemWidth()
        imgui.End()
    end
)

imgui.OnFrame(
    function() return showInput[0] end,
    function()
        imgui.SetNextWindowSize(imgui.ImVec2(340, 52), imgui.Cond.Always)
        imgui.SetNextWindowPos(
            imgui.ImVec2(sizeX*0.5 - 170, sizeY*0.25),
            imgui.Cond.FirstUseEver)
        imgui.SetNextWindowBgAlpha(0.90)
        imgui.Begin("##ttsinput", nil,
            imgui.WindowFlags.NoResize + imgui.WindowFlags.NoScrollbar +
            imgui.WindowFlags.NoTitleBar + imgui.WindowFlags.NoCollapse)
        imgui.PushItemWidth(290)
        local submitted = imgui.InputText("##inp", inputBuf, 512,
            imgui.InputTextFlags.EnterReturnsTrue)
        imgui.PopItemWidth()
        imgui.SameLine()
        if submitted or imgui.Button(">>", imgui.ImVec2(36, 0)) then
            speakText(ffi.string(inputBuf))
            ffi.fill(inputBuf, ffi.sizeof(inputBuf))
            imgui.SetKeyboardFocusHere(-1)
        end
        imgui.End()
    end
)

function main()
    while not isSampAvailable() do wait(100) end
    wait(2500)

    sampAddChatMessage("[VoiceTTS] v2.6 loading...", 0xFFFF00)

    for i = 1, 10 do
        local f = io.open(ADDR_FILE, "r")
        if f then
            local addr = tonumber(f:read("*l")); f:close()
            if addr and addr ~= 0 then
                local ok, api = pcall(function() return ffi.cast("TtsAPI*", addr) end)
                if ok and api then
                    tts = api; os.remove(ADDR_FILE); break
                end
            end
        end
        wait(1000)
    end

    if not tts then
        sampAddChatMessage("[TTS] GAGAL load engine!", 0xFF4444)
        while true do wait(1000) end
    end

    tts.set_pitch(sliderPitch[0])
    tts.set_speed(sliderSpeed[0])
    tts.set_volume(sliderVol[0])
    tts.set_voice(VOICES[voiceIdx[0] + 1])
    tts.set_play_local(chkPlayLocal[0] and 1 or 0)
    if chkEnabled[0] then tts.enable() else tts.disable() end

    sampAddChatMessage("[TTS] OK — /ttsui | voice: " .. VOICES[voiceIdx[0] + 1], 0x00FF88)

    sampRegisterChatCommand("ttsui", function()
        showSettings[0] = not showSettings[0]
    end)

    sampRegisterChatCommand("tts", function(arg)
        if not tts then return end
        if arg == "on" then
            chkEnabled[0] = true; tts.enable(); saveConfig(); updateInputGui()
            sampAddChatMessage("[TTS] ON", 0x00FF88)
        elseif arg == "off" then
            chkEnabled[0] = false; tts.disable()
            if micLocked then setMicLock(false) end
            saveConfig()
            sampAddChatMessage("[TTS] OFF", 0xFF8800)
        elseif arg and #arg > 0 then
            speakText(arg)
        else
            sampAddChatMessage("[TTS] /tts <text>|on|off | /ttsui", 0xFFFF00)
        end
    end)

    sampRegisterChatCommand("miclock", function()
        if chkEnabled[0] then setMicLock(not micLocked)
        else sampAddChatMessage("[TTS] Aktifkan TTS dulu", 0xFFAA00) end
    end)

    sampRegisterChatCommand("ttslocal", function()
        if not tts then return end
        chkPlayLocal[0] = not chkPlayLocal[0]
        tts.set_play_local(chkPlayLocal[0] and 1 or 0)
        saveConfig()
        sampAddChatMessage("[TTS] Play Local: " .. (chkPlayLocal[0] and "ON" or "OFF"),
            chkPlayLocal[0] and 0x00FF88 or 0xAAAAAA)
    end)

    sampRegisterChatCommand("ttsvoice", function(arg)
        if not tts then return end
        if arg and #arg > 0 then
            tts.set_voice(arg)
            sampAddChatMessage("[TTS] Voice=" .. arg, 0x00FFFF)
        else
            sampAddChatMessage("[TTS] /ttsvoice mb-id1|id|en|...", 0xFFFF00)
        end
    end)

    sampRegisterChatCommand("ttspitch", function(arg)
        local v = tonumber(arg)
        if v and tts then sliderPitch[0]=v; tts.set_pitch(v); saveConfig()
            sampAddChatMessage("[TTS] Pitch="..string.format("%.2f",v), 0x00FFFF) end
    end)

    sampRegisterChatCommand("ttsrate", function(arg)
        local v = tonumber(arg)
        if v and tts then sliderSpeed[0]=v; tts.set_speed(v); saveConfig()
            sampAddChatMessage("[TTS] Speed="..string.format("%.2f",v), 0x00FFFF) end
    end)

    sampRegisterChatCommand("ttsvol", function(arg)
        local v = tonumber(arg)
        if v and tts then sliderVol[0]=math.floor(v); tts.set_volume(math.floor(v))
            sampAddChatMessage("[TTS] Vol="..math.floor(v), 0x00FFFF) end
    end)

    while true do wait(1000) end
end
