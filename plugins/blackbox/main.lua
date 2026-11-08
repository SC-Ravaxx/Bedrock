-- Blackbox: Star Citizen's keybinds, saved layouts, game settings and custom
-- characters, backed up when they change and put back on request - SC
-- Blackbox's work, in its folder and form. A plugin for Bedrock, by Ravaxx.

title = "Blackbox"

if not (bedrock.game and bedrock.backups and bedrock.every) then
  function draw(ui)
    ui.text("NEEDS A NEWER BEDROCK", "warn")
  end
  return
end

local diff = bedrock.require("diff")
local E = bedrock.require("engine")

local B = bedrock.backups

-- ---- words ------------------------------------------------------------------

local DOT = "  \u{B7}  "

-- "Today 15:47", "Yesterday 15:47", else Bedrock's "19 Oct 15:38"
local function when(ms)
  local d = bedrock.date(ms)
  local now = bedrock.now()
  local day = function(t) return (bedrock.date(t):gsub(" %d+:%d+$", "")) end
  if now - ms < 2 * 86400000 and now >= ms - 60000 then
    if day(ms) == day(now) then return "Today " .. d:match("%d+:%d+$") end
    if day(ms) == day(now - 86400000) then return "Yesterday " .. d:match("%d+:%d+$") end
  end
  return d
end

-- ...inside a line: "today 15:47"
local function lowerWhen(ms)
  local w = when(ms)
  return (w:gsub("^Today", "today"):gsub("^Yesterday", "yesterday"))
end

-- Why a backup was made, where it tells something: by hand, or around a
-- restore (SC Blackbox's words)
local REASONS = {
  manual = "Saved by hand",
  ["before-restore"] = "Before a restore",
}

local RESTORED = { keybinds = "Keybinds restored", settings = "Settings restored", characters = "Characters restored" }

-- The id's time: yyyy-MM-dd_HH-mm-ss
local function idTime(id)
  local y, mo, d, h, mi = (id or ""):match("^(%d%d%d%d)%-(%d%d)%-(%d%d)_(%d%d)%-(%d%d)")
  if not y then return nil end
  local months = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" }
  return tonumber(d) .. " " .. months[tonumber(mo)] .. " " .. h .. ":" .. mi
end

local function why(b)
  if b.reason == "restored" then
    local from = idTime(b.restoredfrom)
    return (RESTORED[b.restoredparts] or "Restored") .. (from and (" from " .. from) or "")
  end
  return REASONS[b.reason] or ""
end

local PARTS = { "all", "keybinds", "settings", "characters" }
local PART_WORDS = { all = "All", keybinds = "Keybinds", settings = "Settings", characters = "Characters" }

local function fileNames(rels)
  local names = {}
  for _, rel in ipairs(rels or {}) do names[#names + 1] = rel:match("[^\\]+$") end
  return (#names > 1 and "FILES DAMAGED" or "FILE DAMAGED") .. " - " .. table.concat(names, ", ")
end

-- A channel's notice in a line, and the word on its chip
local function noticeLine(n)
  if n.kind == "reset" then
    return "KEYBINDS RESET - From " .. n.before.binds .. " custom binds to " .. n.after.binds
  elseif n.kind == "nofiles" then
    return "NO KEYBIND FILE"
  end
  return fileNames(n.files)
end
local NOTICE_WORDS = { reset = "RESET", nofiles = "NO FILE", damaged = "DAMAGED" }

-- ---- what the window keeps ------------------------------------------------------
-- showing and restoring are "<channel>/<backup id>" or "<channel>/notice"
local V = {
  showing = nil,      -- whose changes are shown
  lines = nil,        -- ...the lines
  restoring = nil,    -- what is being restored: the part picked, then confirmed
  parts = nil,        -- ...what that backup holds
  message = nil,      -- { channel, id = card or "page", text, tone, at }
  defaultChannel = nil,
}

local function say(name, id, text, tone)
  V.message = { channel = name, id = id, text = text, tone = tone, at = bedrock.now() }
end

local function pickDefaultChannel()
  if V.defaultChannel then return V.defaultChannel end
  local best, newest = 1, -1
  for i, st in ipairs(E.states) do
    local t = bedrock.game.played(st.name) or 0
    if t > newest then best, newest = i, t end
  end
  V.defaultChannel = best
  return best
end

-- ---- the timer -------------------------------------------------------------------
-- Every 2 s the engine looks at the files. A notice and a failure go to the
-- foot of Bedrock's window; a backup made shows on the page alone.
local lastError
bedrock.every(2, function()
  local had = {}
  for name, n in pairs(E.notices) do had[name] = n end
  local status = E.status
  local ok, err = pcall(E.tick)
  if not ok and tostring(err) ~= lastError then
    lastError = tostring(err)
    B.log("Check failed: " .. lastError)
    E.status = { kind = "error", text = lastError }
    bedrock.redraw()
  elseif ok then
    lastError = nil
  end
  local s = E.status
  if s ~= status and s then
    if s.kind == "failed" then
      bedrock.note("BLACKBOX" .. DOT .. s.channel .. " BACKUP FAILED - " .. s.text)
    elseif s.kind == "error" then
      bedrock.note("BLACKBOX" .. DOT .. "CHECK FAILED - " .. s.text)
    end
  end
  for _, st in ipairs(E.states) do
    local n = E.notices[st.name]
    if n and n ~= had[st.name] then bedrock.note("BLACKBOX" .. DOT .. st.name .. " " .. noticeLine(n)) end
  end
  -- a message stands two minutes
  if V.message and bedrock.now() - V.message.at > 120000 then
    V.message = nil
    bedrock.redraw()
  end
end)

-- ---- pieces ------------------------------------------------------------------------

local function showLines(ui, lines)
  local most = 300
  for i, l in ipairs(lines) do
    if i > most then
      ui.text("+" .. (#lines - most) .. " more", "dim")
      break
    end
    local kind, text = l[1], l[2]
    if kind == "section" then
      ui.text(diff.upper(text), "dim")
    elseif kind == "group" then
      ui.text(text)
    elseif kind == "added" then
      ui.text("+ " .. text, "good")
    elseif kind == "removed" then
      ui.text("\u{2212} " .. text, "bad")
    elseif kind == "changed" then
      ui.text("~ " .. text, "warn")
    else
      ui.text(text, "dim")
    end
  end
end

-- What changed from `from` to `to` (nil: the files now), as lines
local function changeLines(name, from, to)
  if not from then return { { "note", "OLDEST BACKUP - Nothing before it", 0 } } end
  local r = E.compare(name, from, to)
  if not r then return { { "note", "CAN'T BE READ", 0 } } end
  if #r.lines == 0 then return { { "note", "No differences", 0 } } end
  return r.lines
end

local function restoreResult(r, b, part)
  if r.ok and r.nothingToDo then
    return "NOTHING TO RESTORE - Your files match this backup", "dim"
  elseif r.ok then
    return "RESTORED - " .. (part == "all" and "From " or (PART_WORDS[part] .. " from ")) .. lowerWhen(b.created) ..
      DOT .. r.written .. (r.written == 1 and " file" or " files"), "good"
  elseif r.problem == "running" then
    return "STAR CITIZEN IS RUNNING - Close it first", "warn"
  elseif r.problem == "empty" then
    return "NOT IN THIS BACKUP - Pick another part", "warn"
  elseif r.problem == "backup" then
    return "NOT RESTORED - Your files could not be backed up first" .. DOT .. tostring(r.why), "bad"
  end
  return "RESTORE FAILED - " .. tostring(r.why), "bad"
end

-- Restoring in two steps: the part, then the button. `at`: the card's id or
-- "notice", where a problem is told. The part first picked as SC Blackbox
-- has it: the keybinds, and with them the settings where the game has none.
local function firstPart(name, parts)
  local settingsThere = false
  for rel in pairs(bedrock.game.files(name) or {}) do
    if diff.lower(rel) == "client\\0\\profiles\\default\\attributes.xml" then settingsThere = true end
  end
  if parts.keybinds > 0 and parts.settings > 0 and not settingsThere then return "all" end
  if parts.keybinds > 0 then return "keybinds" end
  if parts.settings > 0 then return "settings" end
  return "characters"
end

local function restorePanel(ui, name, b, at)
  if not V.parts then
    V.parts = E.partsOf(name, b.id)
    V.firstPart = firstPart(name, V.parts)
  end
  local labels, ids, first = {}, {}, 1
  for _, p in ipairs(PARTS) do
    local n = p == "all" and (V.parts.keybinds + V.parts.settings + V.parts.characters) or V.parts[p]
    if n > 0 then
      labels[#labels + 1] = PART_WORDS[p]
      ids[#ids + 1] = p
      if p == V.firstPart then first = #ids end
    end
  end
  if #ids == 0 then
    ui.text("EMPTY - No files in this backup", "dim")
    return
  end
  ui.gap()
  ui.text("RESTORE TO " .. name .. " - Your current files are backed up first", "warn")
  local pick = ui.chips("part-" .. b.id, labels, first)
  local part = ids[pick] or "all"
  local clicked = ui.buttons({
    { "do:" .. b.id, "Restore " .. PART_WORDS[part]:lower(), "danger" },
    { "cancel:" .. b.id, "Cancel", "plain" },
  })
  if clicked == "do:" .. b.id then
    local r = E.restore(name, b.id, part)
    local text, tone = restoreResult(r, b, part)
    V.restoring, V.parts = nil, nil
    local done = r.ok and not r.nothingToDo
    say(name, done and "page" or at, text, tone)
    if done then
      V.showing, V.lines = nil, nil
      bedrock.note("BLACKBOX" .. DOT .. name .. " " .. text)
    end
    bedrock.redraw()
  elseif clicked == "cancel:" .. b.id then
    V.restoring, V.parts = nil, nil
    bedrock.redraw()
  end
end

local function backupCard(ui, name, list, i)
  local b = list[i]
  local chip, tone
  if b.flag == "reset" then
    chip, tone = "RESET", "bad"
  elseif b.kept then
    chip, tone = "KEPT", "good"
  elseif b.reason == "restored" then
    chip, tone = "RESTORED", "good"
  end
  -- the change in the menus' words, why where it tells something, and the
  -- device: the line breaks between them where it is too long
  local shown = diff.shown(b.summary, b.detail)
  local parts = { shown.change ~= "" and shown.change or "Backup" }
  if shown.more then parts[1] = parts[1] .. "  " .. shown.more end
  local reason = why(b)
  if reason ~= "" then parts[#parts + 1] = reason end
  if shown.device then parts[#parts + 1] = shown.device end
  local key = "b:" .. b.id
  local mine = name .. "/" .. b.id
  -- (faint: no setting of the menus - drawn fainter where Bedrock knows how)
  if not ui.card(key, when(b.created), { subtitle = table.concat(parts, DOT), chip = chip, tone = tone,
    faint = shown.faint }) then return end
  ui.row("Custom binds", b.binds >= 0 and tostring(b.binds) or "\u{2013}")
  if b.counted or b.characters > 0 then ui.row("Characters", tostring(b.characters)) end
  local m = V.message
  if m and m.channel == name and m.id == key then ui.text(m.text, m.tone) end
  local showing = V.showing == mine
  local clicked = ui.buttons({
    { "show:" .. b.id, showing and "Hide changes" or "Show changes", "plain" },
    { "restore:" .. b.id, "Restore", "plain" },
    { "keep:" .. b.id, b.kept and "Don't keep" or "Keep forever", "plain" },
  })
  if clicked == "show:" .. b.id then
    if showing then
      V.showing, V.lines = nil, nil
    else
      V.showing, V.lines = mine, changeLines(name, list[i + 1], b)
    end
    bedrock.redraw()
  elseif clicked == "restore:" .. b.id then
    V.restoring = V.restoring ~= mine and mine or nil
    V.parts = nil
    bedrock.redraw()
  elseif clicked == "keep:" .. b.id then
    local ok, err = B.mark(name, b.id, not b.kept)
    if ok then
      E.list(name, true)
    else
      say(name, key, "NOT CHANGED - " .. tostring(err), "bad")
    end
    bedrock.redraw()
  end
  if V.restoring == mine then restorePanel(ui, name, b, key) end
  if V.showing == mine and V.lines then
    ui.gap()
    showLines(ui, V.lines)
  end
  ui.close()
end

local function noticeBlock(ui, name)
  local n = E.notices[name]
  if not n then return end
  local target = n.restoreTarget
  local mine = name .. "/notice"
  ui.text(noticeLine(n), n.kind == "reset" and "bad" or "warn")
  local showing = V.showing == mine
  local clicked = ui.buttons({
    { "n:restore", "Restore " .. lowerWhen(target.created), "primary" },
    { "n:show", showing and "Hide changes" or "Show changes", "plain" },
    { "n:ignore", "Ignore", "plain" },
  })
  if clicked == "n:restore" then
    V.restoring = V.restoring ~= mine and mine or nil
    V.parts = nil
    bedrock.redraw()
  elseif clicked == "n:show" then
    if showing then
      V.showing, V.lines = nil, nil
    else
      V.showing, V.lines = mine, changeLines(name, n.diffFrom, n.diffTo)
    end
    bedrock.redraw()
  elseif clicked == "n:ignore" then
    E.ignore(name)
    if V.showing == mine then V.showing, V.lines = nil, nil end
    if V.restoring == mine then V.restoring, V.parts = nil, nil end
    bedrock.redraw()
    return
  end
  local m = V.message
  if m and m.channel == name and m.id == "notice" then ui.text(m.text, m.tone) end
  if V.restoring == mine then restorePanel(ui, name, target, "notice") end
  if V.showing == mine and V.lines then showLines(ui, V.lines) end
end

function draw(ui)
  if #E.states == 0 then E.refreshChannels() end
  if #E.states == 0 then
    subtitle = ""
    ui.text("STAR CITIZEN NOT FOUND - Start the game once", "dim")
    return
  end
  local labels = {}
  for i, st in ipairs(E.states) do
    local n = E.notices[st.name]
    labels[i] = st.name .. (n and (DOT .. NOTICE_WORDS[n.kind]) or "")
  end
  local pick = 1
  if #labels > 1 then pick = ui.chips("channel", labels, pickDefaultChannel()) end
  local st = E.states[pick] or E.states[1]
  local name = st.name
  local list = E.list(name)
  subtitle = name .. DOT .. #list .. (#list == 1 and " backup" or " backups") ..
    (list[1] and (DOT .. "Last " .. lowerWhen(list[1].created)) or "")

  noticeBlock(ui, name)
  local notice = E.notices[name]
  if notice then ui.gap() end

  local s = E.status
  if s and s.kind == "failed" and s.channel == name then
    ui.text("BACKUP FAILED - " .. s.text, "bad")
  elseif s and s.kind == "error" then
    ui.text("CHECK FAILED - " .. s.text, "bad")
  end
  local m = V.message
  if m and m.channel == name and m.id == "page" then ui.text(m.text, m.tone) end
  -- the notice's restore is the page's first button while it shows
  local clicked = ui.buttons({ { "backup", "Back up now", notice and "plain" or "primary" } })
  if clicked == "backup" then
    local r = E.backupNow(name)
    if r.status == "created" then
      say(name, "page", "BACKED UP - " .. (r.backup.summary or ""), "good")
    elseif r.status == "unchanged" then
      say(name, "page", "NOTHING CHANGED - Last backup " .. lowerWhen(r.backup.created), "dim")
    elseif r.status == "nofiles" then
      say(name, "page", "NO KEYBIND FILE - Nothing to back up", "warn")
    elseif r.status == "damaged" then
      say(name, "page", fileNames(r.files) .. DOT .. "Not backed up", "warn")
    else
      say(name, "page", "BACKUP FAILED - " .. tostring(r.message), "bad")
    end
    list = E.list(name)
    bedrock.redraw()
  end

  if #list == 0 then
    ui.text("NO BACKUPS YET", "dim")
    return
  end
  for i = 1, #list do backupCard(ui, name, list, i) end
end
