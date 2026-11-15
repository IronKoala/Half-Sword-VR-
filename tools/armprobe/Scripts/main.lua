-- HalfSwordVR_ArmProbe: a read-only UE4SS Lua mod for Phase 2 research.
--
-- It never writes game properties, injects input or changes physics.
-- It only reads reflected values and writes text files into its own
-- "output" folder next to this Scripts folder.
--
-- Keys (only while a match is loaded):
--   F7  one-shot dump: every property, function and component of the player
--       pawn (Willie_BP_C), its DriverSkeleton anim BP (ABP_Willie_C), the
--       physical mesh's anim BP (ABP_CopyPose1_C), physics constraints and
--       the physical-animation component. Includes the TYPE of every
--       AnimGraph node, which tells us how ABP_Willie poses the arms.
--   F8  8-second recording. Writes every pawn / ABP_Willie number, bool,
--       vector and rotator whenever it changes, plus arm bone poses, and
--       counts which Blueprint functions run. Move the sword with the mouse
--       while it records.

local VERSION = "armprobe-2"
local RECORD_SECONDS = 8
local MAX_DEPTH = 6 -- class inheritance levels to walk

----------------------------------------------------------------------------
-- paths
local source = debug.getinfo(1, "S").source:gsub("^@", "")
local scripts_dir = assert(source:match("^(.*[/\\])"), "cannot find mod path")
local out_dir = scripts_dir .. "..\\output\\"
os.execute('if not exist "' .. out_dir .. '" mkdir "' .. out_dir .. '"')

local function log(msg) print("[ArmProbe] " .. msg .. "\n") end

----------------------------------------------------------------------------
-- safe helpers (everything is pcall-wrapped: a bad read never crashes)
local function try(f, ...)
    local ok, v = pcall(f, ...)
    if ok then return v end
    return nil
end
local function read(obj, key)
    if obj == nil then return nil end
    return try(function() return obj[key] end)
end
local function call(obj, method, ...)
    if obj == nil then return nil end
    local args = { ... }
    return try(function() return obj[method](obj, table.unpack(args)) end)
end
local function valid(obj)
    return obj ~= nil and call(obj, "IsValid") == true
end
local function fullname(obj)
    if not valid(obj) then return "none" end
    return tostring(call(obj, "GetFullName") or "?")
end
local function num(v)
    if type(v) == "number" and v == v and math.abs(v) < 1e15 then return v end
end
local function fmtnum(v) return string.format("%.5g", v) end

-- Turn any reflected value into a short string, or nil if unreadable.
local function value_text(v, depth)
    depth = depth or 0
    local t = type(v)
    if v == nil then return nil end
    if t == "number" then return fmtnum(v) end
    if t == "boolean" or t == "string" then return tostring(v) end
    if t ~= "userdata" then return nil end
    -- vectors / rotators / quats / transforms
    local x, y, z = num(read(v, "X")), num(read(v, "Y")), num(read(v, "Z"))
    if x and y and z then
        local w = num(read(v, "W"))
        if w then return string.format("(%s, %s, %s, %s)", fmtnum(x), fmtnum(y), fmtnum(z), fmtnum(w)) end
        return string.format("(%s, %s, %s)", fmtnum(x), fmtnum(y), fmtnum(z))
    end
    local p, yw, r = num(read(v, "Pitch")), num(read(v, "Yaw")), num(read(v, "Roll"))
    if p and yw and r then
        return string.format("P=%s Y=%s R=%s", fmtnum(p), fmtnum(yw), fmtnum(r))
    end
    if depth < 1 then
        local tr = read(v, "Translation")
        local rot = read(v, "Rotation")
        if tr ~= nil and rot ~= nil then
            return "T" .. (value_text(tr, depth + 1) or "?") .. " Q" .. (value_text(rot, depth + 1) or "?")
        end
    end
    -- FName / FString / FText
    local s = call(v, "ToString")
    if type(s) == "string" then return '"' .. s:sub(1, 120) .. '"' end
    -- UObject
    if call(v, "IsValid") ~= nil then return fullname(v) end
    return nil
end

-- "DoubleProperty /Game/...Willie_BP_C:Axis Value X" -> kind, owner, name
local function split_prop(full)
    if type(full) ~= "string" then return nil end
    local kind, path = full:match("^(%w+) (.+)$")
    if not kind then return nil end
    local owner, name = path:match("^(.*):([^:]+)$")
    return kind, owner or path, name or path
end

-- Walk a class/struct chain and call fn(kind, owner, name, prop) per property.
local function each_property(struct, fn, max_depth)
    local depth = 0
    while valid(struct) and depth < (max_depth or MAX_DEPTH) do
        local sname = fullname(struct)
        if sname:find("/Script/Engine.Actor$") or sname:find("/Script/CoreUObject.Object$") then break end
        call(struct, "ForEachProperty", function(prop)
            local kind, owner, name = split_prop(call(prop, "GetFullName"))
            if kind then fn(kind, owner, name, prop) end
        end)
        struct = call(struct, "GetSuperStruct")
        depth = depth + 1
    end
end

local function each_function(class, fn, max_depth)
    local depth = 0
    while valid(class) and depth < (max_depth or MAX_DEPTH) do
        local cname = fullname(class)
        if not cname:find("/Game/", 1, true) then break end -- only Blueprint classes
        call(class, "ForEachFunction", function(f) fn(fullname(f), f) end)
        class = call(class, "GetSuperStruct")
        depth = depth + 1
    end
end

----------------------------------------------------------------------------
-- find the player and the objects we care about
local function find_player()
    local pcs = FindAllOf("PlayerController") or {}
    for _, pc in ipairs(pcs) do
        if valid(pc) and call(pc, "IsLocalController") == true then
            local pawn = read(pc, "Pawn")
            if valid(pawn) then return pawn, pc end
        end
    end
end

local function components_of(pawn)
    local cls = StaticFindObject("/Script/Engine.ActorComponent")
    local arr = call(pawn, "K2_GetComponentsByClass", cls)
    local list = {}
    if arr == nil then return list end
    local ok = pcall(function()
        arr:ForEach(function(_, elem)
            local c = elem:get()
            if valid(c) then list[#list + 1] = c end
        end)
    end)
    if not ok or #list == 0 then
        for i = 1, 1024 do
            local c = try(function() return arr[i] end)
            if c == nil then break end
            if type(c) == "userdata" and read(c, "get") then c = call(c, "get") end
            if valid(c) then list[#list + 1] = c end
        end
    end
    return list
end

local function short(obj)
    local n = fullname(obj)
    return n:match("([^%.:]+)$") or n
end

local function find_parts(pawn)
    local parts = { pawn = pawn, mesh = read(pawn, "Mesh") }
    parts.mesh_anim = call(parts.mesh, "GetAnimInstance")
    for _, c in ipairs(components_of(pawn)) do
        local n = short(c)
        local cls = fullname(call(c, "GetClass"))
        if n == "DriverSkeleton" then
            parts.driver = c
            parts.driver_anim = call(c, "GetAnimInstance")
        elseif cls:find("PhysicalAnimationComponent", 1, true) then
            parts.physanim = c
        end
    end
    return parts
end

----------------------------------------------------------------------------
-- F7: full dump
local function dump_object_props(f, title, obj)
    f:write("\n==== " .. title .. ": " .. fullname(obj) .. "\n")
    if not valid(obj) then f:write("(missing)\n"); return end
    local class = call(obj, "GetClass")
    local n = 0
    each_property(class, function(kind, owner, name, prop)
        n = n + 1
        local extra = ""
        if kind == "StructProperty" then
            local st = call(prop, "GetStruct")
            if valid(st) then extra = " <" .. short(st) .. ">" end
        elseif kind == "ObjectProperty" or kind == "ClassProperty" then
            local pc = call(prop, "GetPropertyClass")
            if valid(pc) then extra = " <" .. short(pc) .. ">" end
        end
        local val = value_text(read(obj, name)) or ""
        f:write(string.format("%-18s %-60s%s = %s   [%s]\n", kind, name, extra, val,
            (owner or ""):match("([^/%.]+)$") or ""))
    end)
    f:write("(" .. n .. " properties)\n")
end

-- Every AnimGraph node in an anim BP is a struct property named AnimGraphNode_*.
-- Dump each node's struct type and its readable fields: this is the graph.
local function dump_anim_nodes(f, title, anim)
    f:write("\n==== AnimGraph nodes of " .. title .. ": " .. fullname(anim) .. "\n")
    if not valid(anim) then f:write("(missing)\n"); return end
    each_property(call(anim, "GetClass"), function(kind, owner, name, prop)
        if kind ~= "StructProperty" or not name:find("^AnimGraphNode_") then return end
        local st = call(prop, "GetStruct")
        f:write("\n-- " .. name .. "  <" .. (valid(st) and short(st) or "?") .. ">\n")
        local node = read(anim, name)
        if node == nil or not valid(st) then return end
        each_property(st, function(k2, _, n2)
            if n2 == "Source" or n2 == "BasePose" or n2 == "ComponentPose" then return end
            local v = value_text(read(node, n2))
            if v ~= nil and v ~= "" then f:write(string.format("     %-16s %-40s = %s\n", k2, n2, v)) end
        end, 3)
    end, 2)
end

local function dump_functions(f, title, obj)
    f:write("\n==== Blueprint functions of " .. title .. "\n")
    if not valid(obj) then return end
    each_function(call(obj, "GetClass"), function(name) f:write("  " .. name .. "\n") end)
end

local function dump_constraint(f, c)
    f:write("\n-- " .. fullname(c) .. "\n")
    for _, k in ipairs({ "ComponentName1", "ComponentName2", "OverrideComponent1", "OverrideComponent2" }) do
        local v = read(c, k)
        local inner = v and (read(v, "ComponentName") or v)
        f:write("     " .. k .. " = " .. tostring(value_text(inner) or "?") .. "\n")
    end
    local ci = read(c, "ConstraintInstance")
    if ci == nil then return end
    for _, k in ipairs({ "JointName", "ConstraintBone1", "ConstraintBone2" }) do
        f:write("     " .. k .. " = " .. tostring(value_text(read(ci, k)) or "?") .. "\n")
    end
    local prof = read(ci, "ProfileInstance")
    local ad = prof and read(prof, "AngularDrive")
    local ld = prof and read(prof, "LinearDrive")
    if ad then
        for _, k in ipairs({ "AngularDriveMode", "OrientationTarget", "AngularVelocityTarget" }) do
            f:write("     AngularDrive." .. k .. " = " .. tostring(value_text(read(ad, k)) or "?") .. "\n")
        end
        for _, d in ipairs({ "SlerpDrive", "SwingDrive", "TwistDrive" }) do
            local dr = read(ad, d)
            if dr then
                f:write(string.format("     %s pos=%s vel=%s stiff=%s damp=%s maxF=%s\n", d,
                    tostring(read(dr, "bEnablePositionDrive")), tostring(read(dr, "bEnableVelocityDrive")),
                    tostring(value_text(read(dr, "Stiffness"))), tostring(value_text(read(dr, "Damping"))),
                    tostring(value_text(read(dr, "MaxForce")))))
            end
        end
    end
    if ld then
        f:write("     LinearDrive.PositionTarget = " .. tostring(value_text(read(ld, "PositionTarget")) or "?") .. "\n")
        for _, d in ipairs({ "XDrive", "YDrive", "ZDrive" }) do
            local dr = read(ld, d)
            if dr then
                f:write(string.format("     %s pos=%s stiff=%s damp=%s maxF=%s\n", d,
                    tostring(read(dr, "bEnablePositionDrive")), tostring(value_text(read(dr, "Stiffness"))),
                    tostring(value_text(read(dr, "Damping"))), tostring(value_text(read(dr, "MaxForce")))))
            end
        end
    end
end

local function full_dump()
    local pawn = find_player()
    if not pawn then log("no player pawn yet; load a match first"); return end
    local parts = find_parts(pawn)
    local path = out_dir .. "dump-" .. os.date("%Y%m%d-%H%M%S") .. ".txt"
    local f = io.open(path, "w")
    if not f then log("cannot write " .. path); return end
    f:write(VERSION .. "  " .. os.date() .. "\n")

    f:write("\n==== Components of the pawn\n")
    for _, c in ipairs(components_of(pawn)) do
        local cls = short(call(c, "GetClass"))
        local line = string.format("  %-45s %-36s", short(c), cls)
        local parent = call(c, "GetAttachParent")
        if valid(parent) then line = line .. " parent=" .. short(parent) end
        local sock = call(c, "GetAttachSocketName")
        if sock then line = line .. " socket=" .. tostring(value_text(sock)) end
        local anim = call(c, "GetAnimInstance")
        if valid(anim) then line = line .. " anim=" .. short(call(anim, "GetClass")) end
        if call(c, "IsSimulatingPhysics") == true then line = line .. " SIMULATING" end
        f:write(line .. "\n")
    end

    dump_object_props(f, "Player pawn", pawn)
    dump_functions(f, "Player pawn", pawn)
    dump_object_props(f, "DriverSkeleton anim (ABP_Willie)", parts.driver_anim)
    dump_functions(f, "DriverSkeleton anim", parts.driver_anim)
    dump_anim_nodes(f, "DriverSkeleton anim", parts.driver_anim)
    dump_object_props(f, "Physical mesh anim (ABP_CopyPose1)", parts.mesh_anim)
    dump_anim_nodes(f, "Physical mesh anim", parts.mesh_anim)
    dump_object_props(f, "Physical mesh component (CharacterMesh0)", parts.mesh)
    dump_object_props(f, "DriverSkeleton component", parts.driver)
    dump_object_props(f, "PhysicalAnimationComponent", parts.physanim)

    f:write("\n==== Physics constraint components\n")
    for _, c in ipairs(components_of(pawn)) do
        if fullname(call(c, "GetClass")):find("PhysicsConstraintComponent", 1, true) then dump_constraint(f, c) end
    end

    local weapon = read(pawn, "Weapon R")
    dump_object_props(f, "Right-hand weapon", weapon)
    if valid(weapon) then
        f:write("\n==== Weapon components\n")
        for _, c in ipairs(components_of(weapon)) do
            local line = string.format("  %-40s %-30s", short(c), short(call(c, "GetClass")))
            if call(c, "IsSimulatingPhysics") == true then line = line .. " SIMULATING" end
            local parent = call(c, "GetAttachParent")
            if valid(parent) then line = line .. " parent=" .. fullname(parent) end
            f:write(line .. "\n")
        end
    end

    f:close()
    log("dump written: " .. path)
end

----------------------------------------------------------------------------
-- F8: change recording
local recording = nil
local sample -- defined below, used by start_recording
local hook_counts = {}
local hooks_registered = false

local function scalar_fields(obj, prefix, list)
    if not valid(obj) then return end
    each_property(call(obj, "GetClass"), function(kind, _, name)
        if kind == "DoubleProperty" or kind == "FloatProperty" or kind == "BoolProperty"
            or kind == "IntProperty" or kind == "ByteProperty" or kind == "EnumProperty" then
            list[#list + 1] = { obj = obj, name = name, key = prefix .. name }
        elseif kind == "StructProperty" and not name:find("^AnimGraphNode_") then
            list[#list + 1] = { obj = obj, name = name, key = prefix .. name, struct = true }
        end
    end)
end

local BONES = { "clavicle_r", "upperarm_r", "lowerarm_r", "hand_r", "clavicle_l", "upperarm_l", "lowerarm_l", "hand_l" }

local function register_hooks(parts)
    if hooks_registered then return end
    hooks_registered = true
    local n = 0
    for _, obj in ipairs({ parts.pawn, parts.driver_anim }) do
        each_function(call(obj, "GetClass"), function(fname)
            local path = fname:match("^Function (.+)$")
            if path and not path:find("ExecuteUbergraph", 1, true) then
                local ok = pcall(RegisterHook, path, function()
                    if recording then hook_counts[path] = (hook_counts[path] or 0) + 1 end
                end)
                if ok then n = n + 1 end
            end
        end, 2)
    end
    log("hooked " .. n .. " Blueprint functions (counting only)")
end

local function start_recording()
    if recording then log("already recording"); return end
    local pawn = find_player()
    if not pawn then log("no player pawn yet; load a match first"); return end
    local parts = find_parts(pawn)
    local fields = {}
    scalar_fields(pawn, "pawn.", fields)
    scalar_fields(parts.driver_anim, "driver_anim.", fields)
    register_hooks(parts)
    local path = out_dir .. "rec-" .. os.date("%Y%m%d-%H%M%S") .. ".txt"
    local f = io.open(path, "w")
    if not f then log("cannot write " .. path); return end
    f:write(VERSION .. " recording, " .. #fields .. " fields; lines: t <tab> key <tab> value (only on change)\n")
    hook_counts = {}
    recording = { f = f, path = path, fields = fields, last = {}, parts = parts, t0 = os.clock(), frame = 0 }
    -- Per-frame sampling runs ONLY while a recording is active; nothing runs per frame otherwise.
    LoopInGameThreadAfterFrames(1, function()
        if not recording then return true end
        local ok, err = pcall(sample)
        if not ok then
            log("recording error: " .. tostring(err))
            pcall(function() recording.f:close() end)
            recording = nil
        end
        return recording == nil -- true stops the loop
    end)
    log("recording " .. RECORD_SECONDS .. " s to " .. path .. " - move the sword now")
end

function sample()
    local r = recording
    r.frame = r.frame + 1
    local t = os.clock() - r.t0
    local ts = string.format("%.3f", t)
    for _, fd in ipairs(r.fields) do
        local v = value_text(read(fd.obj, fd.name))
        if v ~= nil and v ~= r.last[fd.key] then
            r.last[fd.key] = v
            r.f:write(ts .. "\t" .. fd.key .. "\t" .. v .. "\n")
        end
    end
    -- bone poses: driver (animation target) vs physical mesh, component space
    for _, src in ipairs({ { "driver", r.parts.driver }, { "physical", r.parts.mesh } }) do
        if valid(src[2]) then
            for _, b in ipairs(BONES) do
                local tr = call(src[2], "GetSocketTransform", FName(b), 2) -- RTS_Component
                local v = value_text(tr)
                if v then r.f:write(ts .. "\tbone." .. src[1] .. "." .. b .. "\t" .. v .. "\n") end
            end
        end
    end
    if t >= RECORD_SECONDS then
        r.f:write("\n==== Blueprint function call counts during recording (" .. r.frame .. " frames)\n")
        local keys = {}
        for k in pairs(hook_counts) do keys[#keys + 1] = k end
        table.sort(keys, function(a, b) return hook_counts[a] > hook_counts[b] end)
        for _, k in ipairs(keys) do r.f:write(string.format("%8d  %s\n", hook_counts[k], k)) end
        r.f:close()
        log("recording done: " .. r.path)
        recording = nil
    end
end

RegisterKeyBind(Key.F7, function() ExecuteInGameThread(function()
    local ok, err = pcall(full_dump)
    if not ok then log("dump error: " .. tostring(err)) end
end) end)
RegisterKeyBind(Key.F8, function() ExecuteInGameThread(function()
    local ok, err = pcall(start_recording)
    if not ok then log("record error: " .. tostring(err)) end
end) end)

log(VERSION .. " ready. F7 = dump, F8 = 8 s recording. Output: " .. out_dir)
