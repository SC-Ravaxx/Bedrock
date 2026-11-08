-- What changed between two sets of the game's files, in words - SC Blackbox's
-- Diff.cs in Lua, so a backup made here reads the same as one made there.
-- Each change twice: with the game's names ("Shield reset levels · Keyboard:
-- Numpad 5 → Numpad 0") and with the names in the files
-- ("spaceship_defensive › v_shield_reset_level").
--
-- Sorting as .NET sorts: ordinal (byte by byte) or ordinal ignoring case
-- (both in capitals first) - never Lua's < on text, which follows the
-- system's locale.

local xml = bedrock.require("xml")
local NAMES = bedrock.data("names")

local M = {}

local byte, char, find, sub, gsub, match = string.byte, string.char, string.find, string.sub, string.gsub, string.match
local concat = table.concat

M.SEP = "\1"
local SEP = M.SEP
local ARROW = " \u{2192} "
local DOT = " \u{B7} "
local TECHSEP = " \u{203A} "

-- ---- text, as .NET handles it ---------------------------------------------

-- Capitals for a-z and the Latin-1 letters (à-þ, ÿ)
local UP, LOW, UP2, LOW2 = {}, {}, {}, {}
for c = 97, 122 do
  UP[char(c)] = char(c - 32)
  LOW[char(c - 32)] = char(c)
end
for c = 0xE0, 0xFE do
  if c ~= 0xF7 then
    UP2[utf8.char(c)] = utf8.char(c - 32)
    LOW2[utf8.char(c - 32)] = utf8.char(c)
  end
end
UP2["\u{FF}"], LOW2["\u{178}"] = "\u{178}", "\u{FF}"

local function upper(s)
  s = gsub(s, "[a-z]", UP)
  if find(s, "\195", 1, true) then s = gsub(s, "\195[\160-\191]", UP2) end
  return s
end

local function lower(s)
  s = gsub(s, "[A-Z]", LOW)
  if find(s, "[\195\197]") then
    s = gsub(s, "\195[\128-\158]", LOW2)
    s = gsub(s, "\197\184", LOW2)
  end
  return s
end

local function upperFirst(s)
  local c = byte(s, 1)
  if c and c >= 97 and c <= 122 then return char(c - 32) .. sub(s, 2) end
  local u = (c == 195) and UP2[sub(s, 1, 2)]
  if u then return u .. sub(s, 3) end
  return s
end

-- a before b, byte by byte (StringComparer.Ordinal)
local function before(a, b)
  if a == b then return false end
  local n = #a < #b and #a or #b
  for i = 1, n do
    local x, y = byte(a, i), byte(b, i)
    if x ~= y then return x < y end
  end
  return #a < #b
end

-- the same ignoring case (StringComparer.OrdinalIgnoreCase)
local function beforeIgnoringCase(a, b)
  return before(upper(a), upper(b))
end

-- `list` sorted by keyOf(item), keeping the order of equal ones (LINQ's OrderBy)
local function sortBy(list, keyOf, ignoreCase)
  local d = {}
  for i, v in ipairs(list) do
    local k = keyOf(v)
    d[i] = { ignoreCase and upper(k) or k, i, v }
  end
  table.sort(d, function(p, q)
    if p[1] ~= q[1] then return before(p[1], q[1]) end
    return p[2] < q[2]
  end)
  local out = {}
  for i, e in ipairs(d) do out[i] = e[3] end
  return out
end

-- the keys of two tables, sorted, once each
local function unionKeys(a, b)
  local keys, seen = {}, {}
  for k in pairs(a) do
    seen[k] = true
    keys[#keys + 1] = k
  end
  for k in pairs(b) do
    if not seen[k] then keys[#keys + 1] = k end
  end
  table.sort(keys, before)
  return keys
end

local function split(s, sep)
  local out, i = {}, 1
  while true do
    local j = find(s, sep, i, true)
    if not j then
      out[#out + 1] = sub(s, i)
      return out
    end
    out[#out + 1] = sub(s, i, j - 1)
    i = j + #sep
  end
end

-- String.Trim: white space off both ends (ASCII, no-break space, U+0085)
local function trim(s)
  local i, j = 1, #s
  while i <= j do
    local c = byte(s, i)
    if c == 32 or (c >= 9 and c <= 13) then
      i = i + 1
    elseif c == 194 and (byte(s, i + 1) == 160 or byte(s, i + 1) == 133) then
      i = i + 2
    else
      break
    end
  end
  while j >= i do
    local c = byte(s, j)
    if c == 32 or (c >= 9 and c <= 13) then
      j = j - 1
    elseif (c == 160 or c == 133) and j > i and byte(s, j - 1) == 194 then
      j = j - 2
    else
      break
    end
  end
  return sub(s, i, j)
end

-- Regex.Replace(s, @"\s+", " ")
local function collapse(s)
  s = gsub(s, "\194[\160\133]", " ")
  s = gsub(s, "[ \t\n\v\f\r]+", " ")
  return s
end

local function plural(n, word)
  return n .. " " .. word .. (n == 1 and "" or "s")
end

M.upper, M.lower, M.before, M.beforeIgnoringCase, M.sortBy, M.split, M.plural =
  upper, lower, before, beforeIgnoringCase, sortBy, split, plural

-- ---- the files --------------------------------------------------------------
-- Paths relative to a channel's user folder, with backslashes:
--   client\<n>\Profiles\default\actionmaps.xml    keybinds
--   client\<n>\controls\mappings\<name>.xml       saved layouts
--   client\<n>\Profiles\default\attributes.xml    game settings
--   client\<n>\CustomCharacters\<name>.chf        characters

function M.kindOf(rel)
  local l = lower(rel)
  if find(l, "^client\\[^\\]+\\profiles\\default\\actionmaps%.xml$") then return "actionmaps" end
  if find(l, "^client\\[^\\]+\\profiles\\default\\attributes%.xml$") then return "attributes" end
  if find(l, "^client\\[^\\]+\\controls\\mappings\\[^\\]+%.xml$") then return "layout" end
  if find(l, "^client\\[^\\]+\\customcharacters\\[^\\]+%.chf$") then return "character" end
  return nil
end

local PART = { actionmaps = "keybinds", layout = "keybinds", attributes = "settings", character = "characters" }
function M.partOf(rel)
  return PART[M.kindOf(rel) or ""]
end

local function clientOf(rel)
  local parts = split(rel, "\\")
  return #parts > 1 and parts[2] or ""
end

-- A set of files: by path ignoring case, and in order (FileSet). `files` as
-- the host gives them ({[rel] = {data, time}}); `parts` (optional) the parts
-- wanted: {keybinds = true, ...}
function M.set(files, parts)
  local set = { map = {}, list = {} }
  for rel, f in pairs(files or {}) do
    local kind = M.kindOf(rel)
    if kind and (not parts or parts[PART[kind]]) then
      local key = upper(rel)
      local old = set.map[key]
      if not old or before(rel, old.rel) then
        set.map[key] = { rel = rel, key = key, kind = kind, data = type(f) == "table" and f.data or f }
      end
    end
  end
  for _, e in pairs(set.map) do set.list[#set.list + 1] = e end
  table.sort(set.list, function(x, y) return before(x.key, y.key) end)
  return set
end

-- One set made of two: the files of `parts` from `b`, the rest from `a`
function M.merged(a, b, parts)
  local set = { map = {}, list = {} }
  for _, e in ipairs(a.list) do
    if not parts[PART[e.kind]] then set.map[e.key] = e end
  end
  for _, e in ipairs(b.list) do
    if parts[PART[e.kind]] then set.map[e.key] = e end
  end
  for _, e in pairs(set.map) do set.list[#set.list + 1] = e end
  table.sort(set.list, function(x, y) return before(x.key, y.key) end)
  return set
end

local function get(set, rel)
  local e = set.map[upper(rel)]
  return e and e.data
end
M.get = get

-- Whether two sets hold the same files, by name ignoring case, byte for byte
function M.same(a, b)
  if #a.list ~= #b.list then return false end
  for _, e in ipairs(a.list) do
    local other = b.map[e.key]
    if not other or other.data ~= e.data then return false end
  end
  return true
end

-- The XML files of a set that do not read (XmlUtil.IsValid), in order
function M.damaged(set)
  local bad = {}
  for _, e in ipairs(set.list) do
    if e.kind ~= "character" and not xml.valid(e.data) then bad[#bad + 1] = e.rel end
  end
  return bad
end

-- ---- the game's names (GameNames) --------------------------------------------

local PREFIXES = { "v_", "pc_", "ui_", "foip_" }

-- "v_toggle_quantum_mode" -> "Toggle quantum mode", "HDRMaxBrightness" -> "HDR Max Brightness"
local function words(name)
  if name == nil or name == "" then return name or "" end
  local s = name
  for _, p in ipairs(PREFIXES) do
    if sub(s, 1, #p) == p then
      s = sub(s, #p + 1)
      break
    end
  end
  local out, n = {}, #s
  for i = 1, n do
    local c = byte(s, i)
    out[#out + 1] = char(c)
    local d = byte(s, i + 1)
    if d and d >= 65 and d <= 90 then
      if (c >= 97 and c <= 122) or (c >= 48 and c <= 57) then
        out[#out + 1] = " "
      elseif c >= 65 and c <= 90 then
        local e = byte(s, i + 2)
        if e and e >= 97 and e <= 122 then out[#out + 1] = " " end
      end
    end
  end
  s = concat(out)
  s = gsub(s, "_", " ")
  s = trim(collapse(s))
  if s == "" then return name end
  return upperFirst(s)
end
M.words = words

local function mapLabel(map)
  local s = NAMES.maps[lower(map)]
  if s and s ~= "" then return s end
  return words(map)
end

local function actionInfo(map, action)
  local m = NAMES.actions[lower(map)]
  return m and m[lower(action)]
end

local function actionLabel(map, action)
  local info = actionInfo(map, action)
  if info and info[1] ~= "" then return info[1] end
  return words(action)
end

local KIND_INDEX = { kb = 2, mo = 3, gp = 4, js = 5 }

-- The default input for a device kind (kb, mo, gp, js), "" when there is
-- none, nil when unknown
local function defaultInput(map, action, kind)
  local info = actionInfo(map, action)
  if not info then return nil end
  local i = KIND_INDEX[kind]
  return i and info[i] or nil
end

-- "Master Sensitivity > ... > Flight (Pitch)" -> "Flight (Pitch)"
local function optionLabel(deviceType, group)
  local d = NAMES.options[lower(deviceType)]
  local s = d and d[lower(group)]
  if not s then return words(group) end
  local parts = split(s, " > ")
  return parts[#parts]
end

M.build = NAMES.game or ""

-- ---- game settings as the menus name them ---------------------------------------
-- names.json "settings": attribute (lower case) -> { label, kind, faint }
--   kind   pct (shown x 100, "43 %"), onoff (On, Off), num, device (an id: no values)
--   faint  1 where it is no setting of the menus (a device's id, a retired value,
--          the game's own state)

local SETTINGS = NAMES.settings or {}

local function settingInfo(name)
  return SETTINGS[lower(name)]
end

-- A decimal number's sign, whole part and fraction ("-0.5" -> "-", "0", "5";
-- "12" -> "", "12", ""); nil for other text
local function figures(v)
  local sign, whole, frac = match(v, "^(%-?)(%d+)%.(%d+)$")
  if sign then return sign, whole, frac end
  sign, whole = match(v, "^(%-?)(%d+)$")
  if sign then return sign, whole, "" end
  return nil
end

-- The figures rounded to `places` decimals, half away from zero, on the figures
-- as written; trailing zeros cut
local function rounded(sign, whole, frac, places)
  local digits = whole .. sub(frac .. string.rep("0", places), 1, places)
  local after = byte(frac, places + 1)
  if after and after >= 53 then
    local out, carry = {}, 1
    for i = #digits, 1, -1 do
      local d = byte(digits, i) - 48 + carry
      carry = d >= 10 and 1 or 0
      out[i] = char(48 + d % 10)
    end
    digits = (carry == 1 and "1" or "") .. concat(out)
  end
  local w = gsub(sub(digits, 1, #digits - places), "^0+(%d)", "%1")
  local f = gsub(sub(digits, #digits - places + 1), "0+$", "")
  local s = f == "" and w or (w .. "." .. f)
  if not find(s, "[1-9]") then return "0" end
  return sign .. s
end

-- "0.098999992" -> "0.1", "5.70707" -> "5.71", "1" -> "1"; other text as it is
local function num(v)
  local sign, whole, frac = figures(v)
  if not sign or frac == "" then return v end
  return rounded(sign, whole, frac, 2)
end

-- "0.43" -> "43 %"
local function percent(v)
  local sign, whole, frac = figures(v)
  if not sign then return v end
  frac = frac .. "00"
  return rounded(sign, whole .. sub(frac, 1, 2), sub(frac, 3), 0) .. " %"
end

local function settingValue(kind, v)
  if kind == "pct" then return percent(v) end
  if kind == "onoff" then return v == "1" and "On" or v == "0" and "Off" or v end
  return num(v)
end

M.num, M.percent = num, percent

-- ---- input names (InputNames) --------------------------------------------------

local function names(list)
  local t = {}
  for i = 1, #list, 2 do t[list[i]] = list[i + 1] end
  return t
end

local KEYS = names {
  "lshift", "Left Shift", "rshift", "Right Shift", "lctrl", "Left Ctrl", "rctrl", "Right Ctrl",
  "lalt", "Left Alt", "ralt", "Right Alt", "lwin", "Left Windows", "rwin", "Right Windows",
  "space", "Space", "enter", "Enter", "tab", "Tab", "escape", "Esc", "backspace", "Backspace",
  "capslock", "Caps Lock", "numlock", "Num Lock", "scrolllock", "Scroll Lock", "print", "Print Screen",
  "pause", "Pause", "insert", "Insert", "delete", "Delete", "home", "Home", "end", "End",
  "pgup", "Page Up", "pgdn", "Page Down", "up", "Arrow Up", "down", "Arrow Down",
  "left", "Arrow Left", "right", "Arrow Right", "comma", "Comma", "period", "Period",
  "minus", "Minus", "equals", "Equals", "semicolon", "Semicolon", "apostrophe", "Apostrophe",
  "slash", "Slash", "backslash", "Backslash", "lbracket", "Left Bracket", "rbracket", "Right Bracket",
  "grave", "Grave", "tilde", "Tilde", "np_add", "Numpad +", "np_subtract", "Numpad -",
  "np_multiply", "Numpad *", "np_divide", "Numpad /", "np_period", "Numpad .", "np_enter", "Numpad Enter",
  "mwheel_up", "Mouse wheel up", "mwheel_down", "Mouse wheel down",
}

local MOUSE = names {
  "mouse1", "Left button", "mouse2", "Right button", "mouse3", "Middle button",
  "mouse4", "Button 4", "mouse5", "Button 5", "mwheel_up", "Wheel up", "mwheel_down", "Wheel down",
  "maxis_x", "Left/right", "maxis_y", "Up/down", "maxis_z", "Wheel",
}

local PAD = names {
  "shoulderl", "Left bumper", "shoulderr", "Right bumper", "triggerl_btn", "Left trigger",
  "triggerr_btn", "Right trigger", "triggerl", "Left trigger (axis)", "triggerr", "Right trigger (axis)",
  "thumbl", "Left stick press", "thumbr", "Right stick press", "thumblx", "Left stick left/right",
  "thumbly", "Left stick up/down", "thumbrx", "Right stick left/right", "thumbry", "Right stick up/down",
  "thumbl_up", "Left stick up", "thumbl_down", "Left stick down", "thumbl_left", "Left stick left",
  "thumbl_right", "Left stick right", "thumbr_up", "Right stick up", "thumbr_down", "Right stick down",
  "thumbr_left", "Right stick left", "thumbr_right", "Right stick right", "dpad_up", "D-pad up",
  "dpad_down", "D-pad down", "dpad_left", "D-pad left", "dpad_right", "D-pad right",
  "back", "Back", "start", "Start",
}

local AXES = names {
  "x", "X axis", "y", "Y axis", "z", "Z axis", "rotx", "X rotation", "roty", "Y rotation", "rotz", "Z rotation",
}

local HAT = { up = true, down = true, left = true, right = true }

local function one(kind, t)
  local s, n
  if kind == "js" then
    n = match(t, "^button([0-9]+)$")
    if n then return "Button " .. n end
    local h, d = match(t, "^hat([0-9]+)_([a-z]+)$")
    if h and HAT[d] then return "Hat " .. h .. " " .. d end
    n = match(t, "^slider([0-9]+)$")
    if n then return "Slider " .. n end
    s = AXES[lower(t)]
    if s then return s end
  elseif kind == "mo" then
    s = MOUSE[lower(t)]
    if s then return s end
  elseif kind == "gp" then
    s = PAD[lower(t)]
    if s then return s end
  end
  s = KEYS[lower(t)]
  if s then return s end
  n = match(t, "^np_([0-9])$")
  if n then return "Numpad " .. n end
  n = match(t, "^f([0-9][0-9]?)$")
  if n then return "F" .. n end
  n = match(t, "^mouse([0-9])$")
  if n then return "Mouse " .. n end
  if utf8.len(t) == 1 then return upper(t) end
  return t
end

-- kind: kb, mo, js or gp; token: what follows "js1_", e.g. "button3" or
-- "lalt+f"; empty = not bound
local function pretty(kind, token)
  token = trim(token or "")
  if token == "" then return "not bound" end
  local parts = split(token, "+")
  local shown = {}
  for i, p in ipairs(parts) do
    shown[i] = (i < #parts and KEYS[lower(p)]) or one(kind, p)
  end
  return concat(shown, " + ")
end
M.pretty = pretty

local function startsWith(s, p) return sub(s, 1, #p) == p end

local function extra(name, value)
  if name == "activationMode" then
    if startsWith(value, "double_tap") then return "double tap" end
    if startsWith(value, "delayed_press") then return "long press" end
    if startsWith(value, "hold") then return "hold" end
    if startsWith(value, "tap") then return "tap" end
    return (gsub(value, "_", " "))
  elseif name == "multiTap" then
    return value == "2" and "double tap" or value == "3" and "triple tap" or ("tap " .. value .. " times")
  end
  return name .. " " .. value
end

-- ---- devices ------------------------------------------------------------------

local function devKey(type, instance)
  local p = type == "keyboard" and "kb" or type == "mouse" and "mo" or type == "joystick" and "js" or
    type == "gamepad" and "gp" or type
  return p .. instance
end

local function devKind(dev)
  local k = match(dev or "", "^([A-Za-z]+)[0-9]*$")
  return k and lower(k) or ""
end

-- int.Parse of the figures at the end (1 when none); too large stops the
-- comparison, as in SC Blackbox
local function devNumber(dev)
  local n = match(dev or "", "([0-9]+)$")
  if not n then return 1 end
  local digits = gsub(n, "^0+", "")
  if #digits > 10 or (tonumber(digits) or 0) > 2147483647 then error("device number too large", 0) end
  return tonumber(n)
end

local DEVKINDS = { kb = "Keyboard", mo = "Mouse", js = "Joystick", gp = "Gamepad" }

local function devLabel(dev)
  local k, n = match(dev or "", "^([A-Za-z]+)([0-9]+)$")
  if not k then return dev end
  local kind = DEVKINDS[lower(k)]
  if not kind then return dev end
  if kind == "Joystick" or n ~= "1" then return kind .. " " .. n end
  return kind
end

-- "T.16000M  {B10A044F-0000-...}" -> "T.16000M"
local function cleanProduct(product)
  local s = gsub(product or "", "{[0-9A-Fa-f%-]+}", "")
  return trim(collapse(s))
end

-- Keyboard, Mouse, Gamepad, or the joystick's own name ("T.16000M")
local function deviceName(dev, a, b)
  if devKind(dev) ~= "js" then return devLabel(dev) end
  local product = b.devices[dev] or a.devices[dev]
  if not product then return devLabel(dev) end
  local name = cleanProduct(product)
  if name == "" then return devLabel(dev) end
  local doc = b.devices[dev] and b or a
  local same = 0
  for key, value in pairs(doc.devices) do
    if devKind(key) == "js" and cleanProduct(value) == name then same = same + 1 end
  end
  return same > 1 and (name .. " (" .. lower(devLabel(dev)) .. ")") or name
end

-- ---- the files read (KeybindDoc, SettingsDoc) -----------------------------------

local function attr(el, name)
  return xml.attr(el, name) or ""
end

local function canon(bi)
  local xs = {}
  for i, e in ipairs(bi.extras) do xs[i] = e[1] .. "=" .. e[2] end
  return bi.token .. "|" .. concat(xs, ",")
end

local function canonList(list)
  local out = {}
  for i, bi in ipairs(list) do out[i] = canon(bi) end
  return concat(out, "\n")
end

-- Curves and other nested options: "flight_move_pitch/nonlinearity_curve/point[2]"
local function flatten(into, keyPrefix, pathPrefix, el)
  local counts, seen = {}, {}
  for _, c in ipairs(el.kids) do counts[c.lname] = (counts[c.lname] or 0) + 1 end
  for _, child in ipairs(el.kids) do
    local name = child.lname
    local idx = (seen[name] or 0) + 1
    seen[name] = idx
    local path = pathPrefix .. (counts[name] > 1 and (name .. "[" .. idx .. "]") or name)
    for _, a in ipairs(child.attrs) do into[keyPrefix .. path .. SEP .. a[2]] = a[3] end
    if #child.kids > 0 then flatten(into, keyPrefix, path .. "/", child) end
  end
end

-- One actionmaps.xml or saved layout, in comparable pieces:
--   binds    map|action|device ("js1") -> its inputs
--   devices  "js1" -> the product as written, with its id
--   options  opt|device|type|path|attribute, dev|device name|input|attribute, mod, fmt|attribute
-- An error where the file does not read.
function M.keybinds(data)
  local root, why = xml.parse(data)
  if not root then error(why, 0) end
  local kd = { binds = {}, devices = {}, options = {}, rebinds = 0 }
  -- actionmaps.xml wraps everything in <ActionProfiles>, layouts don't
  local profiles = {}
  for _, e in ipairs(root.kids) do
    if e.lname == "ActionProfiles" then profiles[#profiles + 1] = e end
  end
  if #profiles == 0 then profiles[1] = root end
  for _, prof in ipairs(profiles) do
    for _, a in ipairs({ "version", "optionsVersion", "rebindVersion" }) do
      local v = xml.attr(prof, a)
      if v then kd.options["fmt" .. SEP .. a] = v end
    end
    for _, el in ipairs(prof.kids) do
      local ln = el.lname
      if ln == "options" then
        local type = lower(attr(el, "type"))
        local dev = devKey(type, attr(el, "instance"))
        local product = trim(attr(el, "Product"))
        if product ~= "" then kd.devices[dev] = product end
        flatten(kd.options, "opt" .. SEP .. dev .. SEP .. type .. SEP, "", el)
      elseif ln == "deviceoptions" then
        local name = attr(el, "name")
        for _, opt in ipairs(el.kids) do
          for _, a in ipairs(opt.attrs) do
            if a[2] ~= "input" then
              kd.options["dev" .. SEP .. name .. SEP .. attr(opt, "input") .. SEP .. a[2]] = a[3]
            end
          end
        end
      elseif ln == "modifiers" then
        local mods = {}
        for _, m in ipairs(el.kids) do
          local input = attr(m, "input")
          if input ~= "" then mods[#mods + 1] = input end
        end
        if #mods > 0 then
          table.sort(mods, before)
          kd.options["mod"] = concat(mods, ", ")
        end
      elseif ln == "actionmap" then
        local map = attr(el, "name")
        for _, act in ipairs(el.kids) do
          if act.lname == "action" then
            local action = attr(act, "name")
            for _, rb in ipairs(act.kids) do
              if rb.lname == "rebind" then
                kd.rebinds = kd.rebinds + 1
                local input = attr(rb, "input")
                local us = find(input, "_", 1, true)
                local has = us ~= nil and us > 1
                local bi = { token = has and trim(sub(input, us + 1)) or "", extras = {} }
                for _, a in ipairs(rb.attrs) do
                  if a[2] ~= "input" then bi.extras[#bi.extras + 1] = { a[2], a[3] } end
                end
                local key = map .. SEP .. action .. SEP .. (has and sub(input, 1, us - 1) or input)
                local list = kd.binds[key]
                if not list then
                  list = {}
                  kd.binds[key] = list
                end
                list[#list + 1] = bi
              end
            end
          end
        end
      end
    end
  end
  for key, list in pairs(kd.binds) do
    if #list > 1 then
      local d = {}
      for i, bi in ipairs(list) do d[i] = { canon(bi), bi } end
      table.sort(d, function(x, y) return before(x[1], y[1]) end)
      for i, e in ipairs(d) do list[i] = e[2] end
    end
  end
  return kd
end

local function tryKeybinds(data)
  local ok, kd = pcall(M.keybinds, data)
  return ok and kd or nil
end

local function emptyKeybinds()
  return { binds = {}, devices = {}, options = {}, rebinds = 0 }
end

-- attributes.xml: <Attributes Version="35"><Attr name="..." value="..."/>...
-- values: by name ignoring case -> { name = as first written, value = as last }
function M.settings(data)
  local root, why = xml.parse(data)
  if not root then error(why, 0) end
  local values = {}
  local function put(name, value)
    local k = upper(name)
    local e = values[k]
    if e then e.value = value else values[k] = { name = name, value = value } end
  end
  local version = xml.attr(root, "Version")
  if version then put("(file version)", version) end
  for _, e in ipairs(root.kids) do
    if e.lname == "Attr" then
      local n = xml.attr(e, "name")
      if n then put(n, xml.attr(e, "value") or "") end
    end
  end
  return { values = values }
end

-- Values the game changes with a key press or a quick pick, not settings
-- made in the options: the walking speed the mouse wheel sets on foot, head
-- tracking switched on and off with its key, the server region picked in the
-- main menu. No changes to list, no reason for a new backup.
local GAMESTATE = { "SpeedThrottleDefaultFixedSpeed", "HeadtrackingToggle", "PreferredRegionId" }
local GAMESTATE_UP, GAMESTATE_LIST = {}, {}
for i, n in ipairs(GAMESTATE) do
  GAMESTATE_UP[upper(n)] = true
  GAMESTATE_LIST[i] = upper(n)
end

local function wordByte(c)
  return c ~= nil and ((c >= 48 and c <= 57) or (c >= 65 and c <= 90) or (c >= 97 and c <= 122) or c == 95 or c >= 128)
end

-- The file without its GameState values, so the rest compares exactly - as
-- SC Blackbox's regex removes them:
-- [ \t]*<Attr\b[^>]*\bname="(?:names)"[^>]*/>[ \t]*(?:\r?\n)?  (ignoring case)
function M.withoutGameState(s)
  local U = gsub(s, "[a-z]", UP)
  local out, from, pos = {}, 1, 1
  while true do
    local t = find(U, "<ATTR", pos, true)
    if not t then break end
    local gt
    if not wordByte(byte(U, t + 5)) then
      gt = find(U, ">", t + 5, true)
      if gt and byte(U, gt - 1) ~= 47 then gt = nil end
    end
    local hit = false
    if gt then
      local q = t + 5
      while not hit do
        local n = find(U, 'NAME="', q, true)
        if not n or n > gt - 2 then break end
        if not wordByte(byte(U, n - 1)) then
          for _, name in ipairs(GAMESTATE_LIST) do
            local close = n + 6 + #name
            if close <= gt - 2 and byte(U, close) == 34 and sub(U, n + 6, close - 1) == name then
              hit = true
              break
            end
          end
        end
        q = n + 1
      end
    end
    if hit then
      local start = t
      while start > from and (byte(s, start - 1) == 32 or byte(s, start - 1) == 9) do start = start - 1 end
      local e = gt
      while byte(s, e + 1) == 32 or byte(s, e + 1) == 9 do e = e + 1 end
      if byte(s, e + 1) == 13 and byte(s, e + 2) == 10 then
        e = e + 2
      elseif byte(s, e + 1) == 10 then
        e = e + 1
      end
      out[#out + 1] = sub(s, from, start - 1)
      from, pos = e + 1, e + 1
    else
      pos = t + 1
    end
  end
  out[#out + 1] = sub(s, from)
  return concat(out)
end

-- ---- comparing (Differ) ---------------------------------------------------------
-- A result: lines (with the game's names) and tech (with the files' names),
-- each { kind, text, indent } with kind section, group, added, removed,
-- changed or note; and how many of each part changed.

local function newResult()
  return { lines = {}, tech = {}, keybinds = 0, devices = 0, options = 0, settings = 0, layouts = 0,
    characters = 0, keybindFileMissing = false }
end

local function add(list, kind, text, indent)
  list[#list + 1] = { kind, text, indent }
end

local function both(r, kind, text, tech, indent)
  add(r.lines, kind, text, indent)
  add(r.tech, kind, tech, indent)
end

-- Readable lines can be grouped (keybinds under the game's menu headings),
-- the technical ones stay flat
local function emit(r, title, changes, indent, grouped)
  if #changes == 0 then return end
  both(r, "section", title, title, indent)
  if grouped then
    local groups, byGroup = {}, {}
    for _, c in ipairs(changes) do
      local g = byGroup[c.group]
      if not g then
        g = { label = c.groupLabel, items = {} }
        byGroup[c.group] = g
        groups[#groups + 1] = g
      end
      g.items[#g.items + 1] = c
    end
    for _, g in ipairs(sortBy(groups, function(g) return g.label end, true)) do
      add(r.lines, "group", g.label, indent + 1)
      for _, c in ipairs(sortBy(g.items, function(c) return c.sort end, true)) do
        add(r.lines, c.kind, c.text, indent + 2)
      end
    end
  else
    for _, c in ipairs(sortBy(changes, function(c) return c.sort end, true)) do
      add(r.lines, c.kind, c.text, indent + 1)
    end
  end
  for _, c in ipairs(sortBy(changes, function(c) return c.techSort end, false)) do
    add(r.tech, c.kind, c.tech, indent + 1)
  end
end

local function kindWord(ha, hb)
  return (not ha) and "added" or ((not hb) and "removed" or "changed")
end

local function inputs(kind, list)
  local out = {}
  for i, bi in ipairs(list) do
    local s = pretty(kind, bi.token)
    if bi.token ~= "" and #bi.extras > 0 then
      local seen, xs = {}, {}
      for _, e in ipairs(bi.extras) do
        local x = extra(e[1], e[2])
        if not seen[x] then
          seen[x] = true
          xs[#xs + 1] = x
        end
      end
      s = s .. " (" .. concat(xs, ", ") .. ")"
    end
    out[i] = s
  end
  return concat(out, " / ")
end

local function techInputs(list)
  local out = {}
  for i, bi in ipairs(list) do
    local s = bi.token == "" and "(unbound)" or bi.token
    if #bi.extras > 0 then
      local xs = {}
      for j, e in ipairs(bi.extras) do xs[j] = e[1] .. "=" .. e[2] end
      s = s .. " (" .. concat(xs, ", ") .. ")"
    end
    out[i] = s
  end
  return concat(out, " / ")
end

-- What the game uses when there is no custom bind: its default key, or nothing
local function defaultText(map, action, dev)
  local kind = devKind(dev)
  if kind == "js" and devNumber(dev) ~= 1 then return "default" end
  local token = defaultInput(map, action, kind)
  if token == nil then return "default" end
  local first = trim(split(token, "/")[1])
  if first == "" then return "not bound" end
  return pretty(kind, first) .. " (default)"
end

local ATTRLABELS = { exponent = "curve exponent", invert = "inverted", ["in"] = "in", out = "out" }

local function attrLabel(a)
  return ATTRLABELS[a] or lower(words(a))
end

local function optionValue(a, value)
  if a == "invert" then return value == "1" and "yes" or value == "0" and "no" or value end
  return num(value)
end

local function optionText(key, a, b)
  local p = split(key, SEP)
  if p[1] == "opt" then -- opt, device, type, path, attribute
    local path = split(p[4], "/")
    local label = optionLabel(p[3], (gsub(path[1], "%[[0-9]+%]$", "")))
    local curve = #path > 1 and (", curve " .. gsub(path[#path], "^point%[([0-9]+)%]$", "point %1")) or ""
    -- the game keeps the mouse curves under its keyboard device
    local device = p[3] == "keyboard" and "Mouse" or deviceName(p[2], a, b)
    return device .. DOT .. label .. curve .. ", " .. attrLabel(p[5])
  elseif p[1] == "dev" then -- dev, device name, input, attribute
    return cleanProduct(p[2]) .. DOT .. pretty("js", p[3]) .. ", " .. attrLabel(p[4])
  elseif p[1] == "mod" then
    return "Modifier keys"
  elseif p[1] == "fmt" then
    return "Keybind file version (" .. p[2] .. ")"
  end
  return (gsub(key, SEP, " "))
end

local function optionTech(key)
  local p = split(key, SEP)
  if p[1] == "opt" then return devLabel(p[2]) .. TECHSEP .. p[4] .. " " .. p[5] end
  if p[1] == "dev" then return cleanProduct(p[2]) .. TECHSEP .. p[3] .. " " .. p[4] end
  if p[1] == "mod" then return "Modifier keys" end
  if p[1] == "fmt" then return "File format" .. TECHSEP .. p[2] end
  return (gsub(key, SEP, " "))
end

local function lastPart(key)
  local p = split(key, SEP)
  return p[#p]
end

local function diffDocs(r, a, b, suffix, indent)
  -- keybinds
  local binds = {}
  for _, key in ipairs(unionKeys(a.binds, b.binds)) do
    local va, vb = a.binds[key], b.binds[key]
    local ha, hb = va ~= nil, vb ~= nil
    if not (ha and hb and canonList(va) == canonList(vb)) then
      local p = split(key, SEP)
      if #p >= 3 then
        local map, action, dev = p[1], p[2], p[3]
        local kind = devKind(dev)
        local label = actionLabel(map, action)
        local fallback = defaultText(map, action, dev)
        local was, now = ha and inputs(kind, va) or fallback, hb and inputs(kind, vb) or fallback
        -- (binds that read the same, such as an input cleared where the game
        -- has none, are no change to list)
        if was ~= now then
          binds[#binds + 1] = {
            kind = kindWord(ha, hb),
            group = map,
            groupLabel = mapLabel(map),
            text = label .. DOT .. deviceName(dev, a, b) .. ": " .. was .. ARROW .. now,
            tech = map .. TECHSEP .. action .. " [" .. devLabel(dev) .. "]: " .. (ha and techInputs(va) or "(default)") ..
              ARROW .. (hb and techInputs(vb) or "(default)"),
            sort = label .. "\1" .. dev,
            techSort = key,
          }
        end
      end
    end
  end
  emit(r, "Keybinds" .. suffix, binds, indent, true)
  r.keybinds = r.keybinds + #binds

  -- devices: which joystick is number 1, 2, ...
  local devices = {}
  for _, key in ipairs(unionKeys(a.devices, b.devices)) do
    local va, vb = a.devices[key], b.devices[key]
    local ha, hb = va ~= nil, vb ~= nil
    if not (ha and hb and va == vb) then
      local sameName = ha and hb and cleanProduct(va) == cleanProduct(vb)
      local sa = not ha and "none" or (sameName and va or cleanProduct(va))
      local sb = not hb and "none" or (sameName and vb or cleanProduct(vb))
      local text = devLabel(key) .. ": " .. sa .. ARROW .. sb
      devices[#devices + 1] = { kind = kindWord(ha, hb), text = text, tech = text, sort = key, techSort = key }
    end
  end
  emit(r, "Devices" .. suffix, devices, indent, false)
  r.devices = r.devices + #devices

  -- curves, inversion, deadzones and the like
  local options = {}
  for _, key in ipairs(unionKeys(a.options, b.options)) do
    local va, vb = a.options[key], b.options[key]
    local ha, hb = va ~= nil, vb ~= nil
    if not (ha and hb and va == vb) then
      local at = lastPart(key)
      local text = optionText(key, a, b)
      local was, now = ha and optionValue(at, va) or "default", hb and optionValue(at, vb) or "default"
      if was ~= now then
        options[#options + 1] = {
          kind = kindWord(ha, hb),
          text = text .. ": " .. was .. ARROW .. now,
          tech = optionTech(key) .. ": " .. (ha and va or "(default)") .. ARROW .. (hb and vb or "(default)"),
          sort = text,
          techSort = key,
        }
      end
    end
  end
  emit(r, "Control settings" .. suffix, options, indent, false)
  r.options = r.options + #options
end

local function keybindFile(r, x, y, suffix, main)
  if x == nil and y == nil then return end
  if x ~= nil and y ~= nil and x == y then return end
  if y == nil then
    both(r, "section", "Keybinds" .. suffix, "Keybinds" .. suffix, 0)
    both(r, "removed", "The keybind file (actionmaps.xml) is missing.", "actionmaps.xml is missing.", 1)
    if main then r.keybindFileMissing = true else r.keybinds = r.keybinds + 1 end
    return
  end
  local da = x == nil and emptyKeybinds() or tryKeybinds(x)
  local db = tryKeybinds(y)
  if da == nil or db == nil then
    local which = db == nil and "newer" or "older"
    both(r, "section", "Keybinds" .. suffix, "Keybinds" .. suffix, 0)
    both(r, "note", "The keybind file of the " .. which .. " state can't be read.",
      "actionmaps.xml (" .. which .. ") can't be read.", 1)
    r.keybinds = r.keybinds + 1
    return
  end
  if x == nil then
    both(r, "note", "There was no keybind file" .. suffix .. " in the older state.",
      "actionmaps.xml" .. suffix .. " didn't exist before.", 0)
  end
  diffDocs(r, da, db, suffix, 0)
end

local function settingsFile(r, x, y, suffix)
  if x == nil and y == nil then return end
  if x ~= nil and y ~= nil and x == y then return end
  local title = "Game settings" .. suffix
  if y == nil then
    both(r, "section", title, title, 0)
    both(r, "removed", "The settings file (attributes.xml) is missing.", "attributes.xml is missing.", 1)
    r.settings = r.settings + 1
    return
  end
  local ok, sa, sb = pcall(function()
    return x == nil and { values = {} } or M.settings(x), M.settings(y)
  end)
  if not ok then
    both(r, "section", title, title, 0)
    both(r, "note", "One of the settings files can't be read.", "attributes.xml can't be read.", 1)
    r.settings = r.settings + 1
    return
  end
  -- the names of both, ignoring case, as the older file writes them
  local keys = {}
  for k, e in pairs(sa.values) do keys[k] = e.name end
  for k, e in pairs(sb.values) do
    if not keys[k] then keys[k] = e.name end
  end
  local order = {}
  for k in pairs(keys) do order[#order + 1] = k end
  table.sort(order, before)
  local changes = {}
  for _, k in ipairs(order) do
    if not GAMESTATE_UP[k] then
      local key = keys[k]
      local ea, eb = sa.values[k], sb.values[k]
      local ha, hb = ea ~= nil, eb ~= nil
      local va, vb = ea and ea.value, eb and eb.value
      if not (ha and hb and va == vb) then
        local info = key ~= "(file version)" and settingInfo(key)
        local label = key == "(file version)" and "File version" or
          (info and info[1] ~= "" and info[1] or words(key))
        local kind = info and info[2] or "num"
        local sa, sb = ha and settingValue(kind, va) or "not set", hb and settingValue(kind, vb) or "not set"
        -- (an id has no value to show; values that read the same are no change)
        if kind == "device" or sa ~= sb then
          changes[#changes + 1] = {
            kind = kindWord(ha, hb),
            text = kind == "device" and (label .. " changed") or (label .. ": " .. sa .. ARROW .. sb),
            tech = key .. ": " .. (ha and va or "(not set)") .. ARROW .. (hb and vb or "(not set)"),
            sort = label,
            techSort = key,
          }
        end
      end
    end
  end
  emit(r, title, changes, 0, false)
  r.settings = r.settings + #changes
end

-- The files below `prefix` in either set, by name ignoring case, sorted so
local function namesBelow(a, b, prefix)
  local P = upper(prefix)
  local list, seen = {}, {}
  for _, set in ipairs({ a, b }) do
    for _, e in ipairs(set.list) do
      if sub(e.key, 1, #P) == P then
        local name = sub(e.rel, #prefix + 1)
        local k = upper(name)
        if not seen[k] then
          seen[k] = true
          list[#list + 1] = name
        end
      end
    end
  end
  return sortBy(list, function(n) return n end, true)
end

-- "layout_MyLayout_exported.xml" -> "MyLayout"
local function layoutName(file)
  local l = lower(file)
  if #file > 11 and sub(l, 1, 7) == "layout_" and sub(l, -4) == ".xml" then
    local middle = sub(file, 8, -5)
    if #middle > 9 and sub(lower(middle), -9) == "_exported" then middle = sub(middle, 1, -10) end
    return middle
  end
  return (gsub(file, "%.[^%.\\/]*$", ""))
end

local function layoutFiles(r, a, b, client, suffix)
  local prefix = "client\\" .. client .. "\\controls\\mappings\\"
  local header = false
  for _, name in ipairs(namesBelow(a, b, prefix)) do
    local x, y = get(a, prefix .. name), get(b, prefix .. name)
    if not (x ~= nil and y ~= nil and x == y) then
      if not header then
        both(r, "section", "Saved layouts" .. suffix, "Saved layouts" .. suffix, 0)
        header = true
      end
      r.layouts = r.layouts + 1
      local shown = 'Layout "' .. layoutName(name) .. '"'
      if x == nil then
        both(r, "added", shown .. " (new)", name .. " (new)", 1)
      elseif y == nil then
        both(r, "removed", shown .. " (removed)", name .. " (removed)", 1)
      else
        local da, db = tryKeybinds(x), tryKeybinds(y)
        if da == nil or db == nil then
          both(r, "changed", shown .. " (can't be read)", name .. " (can't be read)", 1)
        else
          local inner = newResult()
          diffDocs(inner, da, db, "", 2)
          local what = #inner.lines == 0 and "small file changes" or lower(M.summary(inner))
          both(r, "changed", shown .. ": " .. what, name .. ": " .. what, 1)
          for _, l in ipairs(inner.lines) do r.lines[#r.lines + 1] = l end
          for _, l in ipairs(inner.tech) do r.tech[#r.tech + 1] = l end
        end
      end
    end
  end
end

-- Custom characters (not in SC Blackbox): new, removed or changed, by name
local function characterFiles(r, a, b, client, suffix)
  local prefix = "client\\" .. client .. "\\CustomCharacters\\"
  local header = false
  for _, name in ipairs(namesBelow(a, b, prefix)) do
    local x, y = get(a, prefix .. name), get(b, prefix .. name)
    if not (x ~= nil and y ~= nil and x == y) then
      if not header then
        both(r, "section", "Characters" .. suffix, "Characters" .. suffix, 0)
        header = true
      end
      r.characters = r.characters + 1
      local shown = 'Character "' .. gsub(name, "%.[^%.]*$", "") .. '"'
      local what = x == nil and "new" or (y == nil and "removed" or "changed")
      both(r, x == nil and "added" or (y == nil and "removed" or "changed"), shown .. " (" .. what .. ")",
        name .. " (" .. what .. ")", 1)
    end
  end
end

-- a and b: sets (M.set). What changed from a to b.
function M.compare(a, b)
  local r = newResult()
  local clients, seen = {}, {}
  for _, set in ipairs({ a, b }) do
    for _, e in ipairs(set.list) do
      local c = clientOf(e.rel)
      local k = upper(c)
      if not seen[k] then
        seen[k] = true
        clients[#clients + 1] = c
      end
    end
  end
  table.sort(clients, function(x, y)
    if (x == "0") ~= (y == "0") then return x == "0" end
    return beforeIgnoringCase(x, y)
  end)
  for _, c in ipairs(clients) do
    local suffix = c == "0" and "" or (" (profile folder " .. c .. ")")
    local actionmaps = "client\\" .. c .. "\\Profiles\\default\\actionmaps.xml"
    local attributes = "client\\" .. c .. "\\Profiles\\default\\attributes.xml"
    keybindFile(r, get(a, actionmaps), get(b, actionmaps), suffix, c == "0")
    settingsFile(r, get(a, attributes), get(b, attributes), suffix)
    layoutFiles(r, a, b, c, suffix)
    characterFiles(r, a, b, c, suffix)
  end
  return r
end

function M.summary(r)
  local parts = {}
  if r.keybindFileMissing then parts[#parts + 1] = "keybind file missing" end
  if r.keybinds > 0 then parts[#parts + 1] = plural(r.keybinds, "keybind") end
  if r.devices > 0 then parts[#parts + 1] = plural(r.devices, "device") end
  if r.options > 0 then parts[#parts + 1] = plural(r.options, "control setting") end
  if r.settings > 0 then parts[#parts + 1] = plural(r.settings, "game setting") end
  if r.layouts > 0 then parts[#parts + 1] = plural(r.layouts, "layout") end
  if r.characters > 0 then parts[#parts + 1] = plural(r.characters, "character") end
  if #parts == 0 then return "No differences" end
  return upperFirst(concat(parts, ", "))
end

-- The first change in words, and how many more: a backup's detail line.
-- `firstCharacters`: no backup before counts characters - where they are all
-- that changed, "9 characters saved"
function M.detail(r, firstCharacters)
  local first, n = nil, 0
  for _, l in ipairs(r.lines) do
    if l[1] == "added" or l[1] == "removed" or l[1] == "changed" then
      n = n + 1
      first = first or l[2]
    end
  end
  if not first then return "" end
  if firstCharacters and r.characters == n then return plural(n, "character") .. " saved" end
  return first .. (n > 1 and ("   (+" .. (n - 1) .. " more)") or "")
end

-- The custom binds of a set: rebind entries in its actionmaps.xml files
function M.countBinds(set)
  local n = 0
  for _, e in ipairs(set.list) do
    if e.kind == "actionmaps" then
      local kd = tryKeybinds(e.data)
      if kd then n = n + kd.rebinds end
    end
  end
  return n
end

-- Whether two sets of XML files differ in nothing but the GameState values
function M.onlyGameStateDiffers(a, b)
  if #a.list ~= #b.list then return false end
  for _, e in ipairs(a.list) do
    local other = get(b, e.rel)
    if other == nil then return false end
    if other ~= e.data then
      if e.kind ~= "attributes" then return false end
      if M.withoutGameState(e.data) ~= M.withoutGameState(other) then return false end
    end
  end
  return true
end

-- ---- a backup in the list ---------------------------------------------------------
-- How a backup's change reads in the list, from the summary and detail in its
-- backup-info.txt - in the menus' words, or as SC Blackbox and Blackbox 1.0
-- wrote them, with the files' names and values; old backups are not written
-- again. { change, more, device, faint }:
--   change  "Master Volume  43 % → 100 %", "Hail Target  Button 14 → Button 3"
--   more    "+2 more", or nil
--   device  a keybind's or control setting's device, or nil
--   faint   the one change is no setting of the menus (an id, a retired
--           value, the game's own state)

-- The settings by the names backups give them: the menu's label, or the
-- attribute's name as words() wrote it - which is, without its spaces and in
-- lower case, the attribute's name in lower case without its underscores
local BYLABEL, BYWORDS = {}, {}
for key, s in pairs(SETTINGS) do
  if s[1] ~= "" then BYLABEL[s[1]] = key end
  BYWORDS[(gsub(key, "_", ""))] = key
end

local function settingNamed(what)
  local key = BYLABEL[what] or BYWORDS[lower((gsub(what, " ", "")))]
  return key and SETTINGS[key]
end

local function lastAt(s, p)
  local at, from = nil, 1
  while true do
    local i = find(s, p, from, true)
    if not i then return at end
    at, from = i, i + #p
  end
end

-- A value as the list shows it: "not set" as "default", a setting's by its
-- kind, a number of more than 2 decimals to 2
local function listValue(v, kind)
  if v == "not set" then return "default" end
  if kind == "pct" or kind == "onoff" then return settingValue(kind, v) end
  if find(v, "^%-?%d+%.%d%d%d+$") then return num(v) end
  return v
end

function M.shown(summary, detail)
  summary, detail = summary or "", detail or ""
  local body, more = match(detail, "^(.-)   %(%+(%d+) more%)$")
  body = body or detail
  if body == "" then return { change = summary } end
  -- characters as they were written before "9 characters saved": the first
  -- of them, and how many more
  local count = tonumber(match(summary, "^(%d+) characters$"))
  if count and find(body, '^Character ".*" %(new%)$') and count == 1 + (tonumber(more) or 0) then
    return { change = plural(count, "character") .. " saved" }
  end
  local out = { more = more and ("+" .. more .. " more") or nil }
  -- "<what>: <old> → <new>", or a change without values
  local what, old, new = body, nil, nil
  local arrow = lastAt(body, ARROW)
  if arrow then
    local left = sub(body, 1, arrow - 1)
    local colon = lastAt(left, ": ")
    if colon then what, old, new = sub(left, 1, colon - 1), sub(left, colon + 2), sub(body, arrow + #ARROW) end
  end
  -- what the first change is: the summary's first part
  local part = match(summary, "^[^,]*")
  local kind
  if find(part, "^%d+ keybinds?$") then
    -- "<action> · <device>"
    local at = lastAt(what, DOT)
    if at then what, out.device = sub(what, 1, at - 1), sub(what, at + #DOT) end
  elseif find(part, "^%d+ control settings?$") then
    -- "<device> · <option>"
    local at = find(what, DOT, 1, true)
    if at then what, out.device = upperFirst(sub(what, at + #DOT)), sub(what, 1, at - 1) end
  elseif find(part, "^%d+ game settings?$") then
    local s = settingNamed(what) or (not old and settingNamed((gsub(what, " changed$", ""))))
    out.faint = not more and ((s and s[3] == 1) or find(what, "DEPRECATED$") ~= nil)
    if s then
      kind = s[2]
      if s[1] ~= "" then what = s[1] end
      if kind == "device" then what, old = what .. " changed", nil end
    end
  end
  out.change = old and (what .. "  " .. listValue(old, kind) .. ARROW .. listValue(new, kind)) or what
  return out
end

return M
