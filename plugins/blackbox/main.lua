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

local function lowerWhen(ms)
  local w = when(ms)
  return (w:gsub("^Today", "today"):gsub("^Yesterday", "yesterday"))
end

local REASONS = {
  start = "App started",
  change = "Files changed",
  ["while-playing"] = "While playing",
  ["game-start"] = "Game started",
  ["game-exit"] = "After playing",
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

-- ---- what the window keeps ------------------------------------------------------
local V = {
  showing = nil,      -- backup id whose changes are shown
  lines = nil,        -- ...the lines
  restoring = nil,    -- backup id being restored: parts picked, then confirmed
  restoringAt = nil,  -- ...from its card or from the notice
  parts = nil,        -- ...what it holds
  message = nil,      -- { id = card or "page", text, tone }
  defaultChannel = nil,
}

local function say(id, text, tone)
  V.message = { id = id, text = text, tone = tone }
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
bedrock.every(2, function()
  local ok, err = pcall(E.tick)
  if not ok then
    B.log("Check failed: " .. tostring(err))
    E.status = { kind = "error", text = tostring(err) }
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

local function restoreResult(r, b, part)
  if r.ok and r.nothingToDo then
    return "NOTHING TO RESTORE - Your files match this backup", "dim"
  elseif r.ok then
    return "RESTORED - " .. PART_WORDS[part] .. " from " .. when(b.created) .. DOT .. r.written ..
      (r.written == 1 and " file" or " files"), "good"
  elseif r.problem == "running" then
    return "STAR CITIZEN IS RUNNING - Close it first", "warn"
  elseif r.problem == "empty" then
    return "NOT IN THIS BACKUP - Pick another part", "warn"
  elseif r.problem == "backup" then
    return "NOTHING CHANGED - Your files could not be backed up first: " .. tostring(r.why), "bad"
  end
  return "RESTORE FAILED - " .. tostring(r.why), "bad"
end

-- Restoring in two steps: the part, then the button
local function restorePanel(ui, name, b)
  if not V.parts then V.parts = E.partsOf(name, b.id) end
  local labels, ids = {}, {}
  for _, p in ipairs(PARTS) do
    local n = p == "all" and (V.parts.keybinds + V.parts.settings + V.parts.characters) or V.parts[p]
    if n > 0 then
      labels[#labels + 1] = PART_WORDS[p]
      ids[#ids + 1] = p
    end
  end
  if #ids == 0 then
    ui.text("EMPTY - This backup holds no files", "dim")
    return
  end
  ui.gap()
  ui.text("RESTORE " .. name .. " - Your files now are backed up first", "warn")
  local pick = ui.chips("part-" .. b.id, labels, 1)
  local part = ids[pick] or "all"
  local clicked = ui.buttons({
    { "do:" .. b.id, "Restore " .. PART_WORDS[part]:lower(), "danger" },
    { "cancel:" .. b.id, "Cancel", "plain" },
  })
  if clicked == "do:" .. b.id then
    local r = E.restore(name, b.id, part)
    local text, tone = restoreResult(r, b, part)
    local at = V.restoringAt == "notice" and "page" or ("b:" .. b.id)
    V.restoring, V.parts = nil, nil
    say(r.ok and not r.nothingToDo and "page" or at, text, tone)
    if r.ok and not r.nothingToDo then bedrock.note("BLACKBOX" .. DOT .. name .. " " .. text) end
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
  local sub = (b.summary ~= "" and b.summary or "Backup") .. DOT .. why(b)
  local key = "b:" .. b.id
  if not ui.card(key, when(b.created), { subtitle = sub, chip = chip, tone = tone }) then return end
  if b.detail ~= "" then ui.text(b.detail) end
  ui.row("Custom binds", b.binds >= 0 and tostring(b.binds) or "\u{2013}")
  if b.counted or b.characters > 0 then ui.row("Characters", tostring(b.characters)) end
  if V.message and V.message.id == key then ui.text(V.message.text, V.message.tone) end
  local showing = V.showing == b.id
  local clicked = ui.buttons({
    { "show:" .. b.id, showing and "Hide changes" or "Show changes", "plain" },
    { "restore:" .. b.id, "Restore", "plain" },
    { "keep:" .. b.id, b.kept and "Don't keep" or "Keep", "plain" },
  })
  if clicked == "show:" .. b.id then
    if showing then
      V.showing, V.lines = nil, nil
    else
      V.showing = b.id
      local prev = list[i + 1]
      if prev then
        local r = E.compare(name, prev, b)
        V.lines = r and (#r.lines > 0 and r.lines or { { "note", "No differences", 0 } }) or
          { { "note", "Can't be read", 0 } }
      else
        V.lines = { { "note", "The oldest backup - nothing before it", 0 } }
      end
    end
    bedrock.redraw()
  elseif clicked == "restore:" .. b.id then
    V.restoring = (V.restoring ~= b.id or V.restoringAt ~= "card") and b.id or nil
    V.restoringAt, V.parts = "card", nil
    bedrock.redraw()
  elseif clicked == "keep:" .. b.id then
    local ok, err = B.mark(name, b.id, not b.kept)
    if ok then
      E.list(name, true)
    else
      say(key, "NOT CHANGED - " .. tostring(err), "bad")
    end
    bedrock.redraw()
  end
  if V.restoring == b.id and V.restoringAt == "card" then restorePanel(ui, name, b) end
  if V.showing == b.id and V.lines then
    ui.gap()
    showLines(ui, V.lines)
  end
  ui.close()
end

local function noticeBlock(ui, name)
  local n = E.notices[name]
  if not n then return end
  local target = n.restoreTarget
  local text
  if n.kind == "reset" then
    text = "KEYBINDS RESET - " .. n.before.binds .. " custom binds on " .. when(n.before.created) .. ", " ..
      n.after.binds .. " now"
  elseif n.kind == "nofiles" then
    text = "NO KEYBIND FILE - Restore the backup of " .. lowerWhen(target.created)
  else
    local names = {}
    for _, rel in ipairs(n.files or {}) do names[#names + 1] = rel:match("[^\\]+$") end
    text = "FILE DAMAGED" .. DOT .. table.concat(names, ", ") .. " - Restore the backup of " .. lowerWhen(target.created)
  end
  ui.text(text, n.kind == "reset" and "bad" or "warn")
  local clicked = ui.buttons({
    { "n:restore", "Restore " .. when(target.created), "primary" },
    { "n:show", "Show changes", "plain" },
    { "n:ignore", "Ignore", "plain" },
  })
  if clicked == "n:restore" then
    V.restoring = (V.restoring ~= target.id or V.restoringAt ~= "notice") and target.id or nil
    V.restoringAt, V.parts = "notice", nil
    bedrock.redraw()
  elseif clicked == "n:show" then
    if V.showing == "notice" then
      V.showing, V.lines = nil, nil
    else
      local r = E.compare(name, n.diffFrom, n.diffTo)
      V.showing = "notice"
      V.lines = r and (#r.lines > 0 and r.lines or { { "note", "No differences", 0 } }) or { { "note", "Can't be read", 0 } }
    end
    bedrock.redraw()
  elseif clicked == "n:ignore" then
    E.ignore(name)
    bedrock.redraw()
  end
  if V.restoring == target.id and V.restoringAt == "notice" then restorePanel(ui, name, target) end
  if V.showing == "notice" and V.lines then showLines(ui, V.lines) end
end

function draw(ui)
  if #E.states == 0 then E.refreshChannels() end
  if #E.states == 0 then
    subtitle = ""
    ui.text("NO GAME FOLDER - Start Star Citizen once", "dim")
    return
  end
  local labels = {}
  for i, st in ipairs(E.states) do labels[i] = st.name .. (E.notices[st.name] and "  !" or "") end
  local pick = 1
  if #labels > 1 then pick = ui.chips("channel", labels, pickDefaultChannel()) end
  local st = E.states[pick] or E.states[1]
  local name = st.name
  local list = E.list(name)
  subtitle = name .. DOT .. #list .. (#list == 1 and " backup" or " backups") ..
    (list[1] and (DOT .. "Last " .. lowerWhen(list[1].created)) or "")

  noticeBlock(ui, name)

  if V.message and V.message.id == "page" then ui.text(V.message.text, V.message.tone) end
  local clicked = ui.buttons({ { "backup", "Back up now", "primary" } })
  if clicked == "backup" then
    local r = E.backupNow(name)
    if r.status == "created" then
      say("page", "BACKED UP - " .. (r.backup.summary or ""), "good")
    elseif r.status == "unchanged" then
      say("page", "NOTHING CHANGED - Last backup " .. lowerWhen(r.backup.created), "dim")
    elseif r.status == "nofiles" then
      say("page", "NO KEYBIND FILES - Nothing to back up", "warn")
    elseif r.status == "damaged" then
      say("page", "FILE DAMAGED - " .. r.message, "warn")
    else
      say("page", "BACKUP FAILED - " .. tostring(r.message), "bad")
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
