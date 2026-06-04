-- voicetts.lua v3.0 by brruham
-- Piper TTS backend

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
        void         (*set_piper_rate)(int);
    } TtsAPI;
    typedef unsigned long uintptr_t;
]]

-- Daftar model .onnx di /sdcard/piper/
-- Format: "nama_tampil", "nama_file.onnx", sample_rate
local MODELS = {
    { label = "id_ID argenia (medium)", file = "id_ID-argenia-medium.onnx",   rate = 22050 },
    { label = "id_ID argenia (low)",    file = "id_ID-argenia-low.onnx",       rate = 16000 },
    { label = "en_US lessac (medium)",  file = "en_US-lessac-medium.onnx",     rate = 22050 },
    { label = "en_US lessac (high)",    file = "en_US-lessac-high.onnx",       rate = 22050 },
    { label = "en_US ryan (medium)",    file = "en_US-ryan-medium.onnx",       rate = 22050 },
    { label = "ms_MY custom (medium)",  file = "ms_MY-custom-medium.onnx",     rate = 22050 },
}

local modelItems = ffi.new("const char*[?]", #MODELS)
for i, m in ipairs(MODELS) do modelItems[i-1] = m.label end

local CFG_FILE = "voicetts"
local ini = inicfg.load({
    settings = {
        speed      = 1.0,
        volume     = 100,
        enabled    = true,
        model_idx  = 1,
        play_local = true,
    }
}, CFG_FILE)

local showSettings = new.bool(false)
local showInput    = new.bool(false)
local inputBuf     = new.char[512]()
local sliderSpeed  = new.float(ini.settings.speed or 1.0)
local sliderVol    = new.int(ini.settings.volume or 100)
local chkEnabled   = new.bool(ini.settings.enabled ~= false)
local chkPlayLocal = new.bool(ini.settings.play_local ~= false)
local modelIdx     = new.int((ini.settings.model_idx or 1) - 1)

local tts       = nil
local micLocked = false
local ADDR_FILE = "/storage/emulated/0/voicetts_addr.txt"

local function saveConfig()
    ini.settings.speed      = sliderSpeed[0]
    ini.settings.volume     = sliderVol[0]
    ini.settings.enabled    = chkEnabled[0]
    ini.settings.model_idx  = modelIdx[0] + 1
    ini.settings.play_local = chkPlayLocal[0]
    inicfg.save(ini, CFG_FILE)
end

local function applyModel(idx)
    if not tts then return end
    local m = MODELS[idx + 1]
    if not m then return end
    tts.set_voice(m.file)
    tts.set_piper_rate(m.rate)
    sampAddChatMessage("[TTS] Model: " .. m.label .. " (" .. m.rate .. "Hz)", 0x00FFFF)
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

-- GUI 1: Settings
imgui.OnFrame(
    function() return showSettings[0] end,
    function()
        imgui.SetNextWindowSize(imgui.ImVec2(310, 0), imgui.Cond.Always)
        imgui.SetNextWindowPos(imgui.ImVec2(sizeX*0.02, sizeY*0.15), imgui.Cond.FirstUseEver)
        imgui.Begin("VoiceTTS - Settings", showSettings,
            imgui.WindowFlags.NoResize + imgui.WindowFlags.NoScrollbar)

        local ttsOn = chkEnabled[0]
        imgui.TextColored(
            ttsOn and imgui.ImVec4(0.3,1,0.3,1) or imgui.ImVec4(0.7,0.7,0.7,1),
            "TTS: " .. (ttsOn and "ON" or "OFF"))
        imgui.SameLine()
        imgui.TextColored(
            micLocked and imgui.ImVec4(0.3,1,0.3,1) or imgui.ImVec4(1,0.4,0.4,1),
            "   MIC: " .. (micLocked and "LOCK" or "OFF"))
        imgui.Separator()

        if imgui.Button(ttsOn and "TTS OFF" or "TTS ON", imgui.ImVec2(140, 30)) then
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
        if imgui.Button(micLocked and "MIC LOCK" or "MIC OFF", imgui.ImVec2(140, 30)) then
            if chkEnabled[0] then setMicLock(not micLocked)
            else sampAddChatMessage("[TTS] Aktifkan TTS dulu", 0xFFAA00) end
        end
        imgui.PopStyleColor(2)

        imgui.Spacing(); imgui.Separator()
        imgui.PushItemWidth(280)

        -- Model selector
        imgui.Text("Model (Voice):")
        if imgui.Combo("##model", modelIdx, modelItems, #MODELS) then
            applyModel(modelIdx[0])
            saveConfig()
        end

        imgui.Spacing()

        -- Speed
        imgui.Text("Speed:")
        if imgui.SliderFloat("##speed", sliderSpeed, 0.5, 3.0, "%.2f") then
            if tts then tts.set_speed(sliderSpeed[0]) end; saveConfig()
        end

        -- Volume (info saja, dikontrol via BASS di sisi C++)
        imgui.Text("Volume:")
        if imgui.SliderInt("##vol", sliderVol, 0, 200) then
            if tts then tts.set_volume(sliderVol[0]) end; saveConfig()
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

-- GUI 2: Input Text
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
        local clicked = imgui.Button(">>", imgui.ImVec2(36, 0))
        if submitted or clicked then
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

    sampAddChatMessage("[VoiceTTS] v3.0 (Piper) loading...", 0xFFFF00)

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

    -- Apply config
    tts.set_speed(sliderSpeed[0])
    tts.set_volume(sliderVol[0])
    tts.set_play_local(chkPlayLocal[0] and 1 or 0)
    applyModel(modelIdx[0])
    if chkEnabled[0] then tts.enable() else tts.disable() end

    local m = MODELS[modelIdx[0] + 1]
    sampAddChatMessage("[TTS] OK — model: " .. m.label, 0x00FF88)
    sampAddChatMessage("[TTS] /ttsui | /tts <text>|on|off | /miclock", 0x00FF88)

    -- Commands
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

    -- Ganti model langsung via chat: /ttsmodel 1..N
    sampRegisterChatCommand("ttsmodel", function(arg)
        local idx = tonumber(arg)
        if idx and MODELS[idx] then
            modelIdx[0] = idx - 1
            applyModel(modelIdx[0])
            saveConfig()
        else
            sampAddChatMessage("[TTS] /ttsmodel <1-" .. #MODELS .. ">", 0xFFFF00)
            for i, m in ipairs(MODELS) do
                sampAddChatMessage("  " .. i .. ". " .. m.label, 0xCCCCCC)
            end
        end
    end)

    sampRegisterChatCommand("ttsrate", function(arg)
        local v = tonumber(arg)
        if v and tts then
            sliderSpeed[0] = v; tts.set_speed(v); saveConfig()
            sampAddChatMessage("[TTS] Speed=" .. string.format("%.2f", v), 0x00FFFF)
        end
    end)

    sampRegisterChatCommand("ttsvol", function(arg)
        local v = tonumber(arg)
        if v and tts then
            sliderVol[0] = math.floor(v); tts.set_volume(math.floor(v)); saveConfig()
            sampAddChatMessage("[TTS] Vol=" .. math.floor(v), 0x00FFFF)
        end
    end)

    while true do wait(1000) end
end