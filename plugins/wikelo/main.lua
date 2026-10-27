-- Wikelo: the trades of his emporiums out of the game files, what each one
-- needs against what you have, and what the game's log says of the ones
-- you took. A plugin for Bedrock, by Ravaxx.

local DATA = bedrock.data("wikelo")
local TRADES = DATA.trades

title = "Wikelo"

-- ---- what is kept ------------------------------------------------------------
-- inv       item name -> how many you have
-- done      trade id -> { n = times done, last = when (ms) }
-- missions  the game's mission id -> what the log said of it: the trade, its
--           title, when it was accepted, its parts, how it ended
-- since     from then on a trade done in the game takes its parts from the
--           inventory; before, the log is history alone
-- session   when the game's latest session started, by its log
local S = bedrock.load()
local fresh = S == nil
S = S or {}
S.inv = S.inv or {}
S.done = S.done or {}
S.missions = S.missions or {}
S.since = S.since or bedrock.now()
S.session = S.session or 0
local function save() bedrock.save(S) end
if fresh then save() end

-- ---- the trades, looked up ---------------------------------------------------
local byId, byTitle = {}, {}
local items = {}          -- name -> { name = ..., unit = ..., uses = { trade, ... } }
local itemNames = {}
for _, t in ipairs(TRADES) do
  byId[t.id] = t
  byTitle[t.title:lower()] = t
  for _, w in ipairs(t.wants) do
    local it = items[w.name]
    if not it then
      it = { name = w.name, unit = w.unit, uses = {} }
      items[w.name] = it
      itemNames[#itemNames + 1] = w.name
    end
    it.uses[#it.uses + 1] = t
  end
end
table.sort(itemNames, function(a, b) return a:lower() < b:lower() end)

local function have(name) return S.inv[name] or 0 end

local function setHave(name, n)
  if n and n > 0 then S.inv[name] = n else S.inv[name] = nil end
end

local function amountText(w, n)
  return tostring(n) .. (w.unit and (" " .. w.unit) or "")
end

-- READY, PARTIAL or MISSING, and how many of its parts are in stock in full
local function status(t)
  local full, any = 0, false
  for _, w in ipairs(t.wants) do
    local h = have(w.name)
    if h >= w.amount then full = full + 1 end
    if h > 0 then any = true end
  end
  if full == #t.wants then return "READY", full end
  if any then return "PARTIAL", full end
  return "MISSING", full
end

-- A contract stays accepted from one session of the game to the next: each
-- session's log names it again, with the same mission id, soon after the
-- player is in. One the latest session has not named is over, however the
-- log is silent about it.
local function live(m)
  return m.trade and not m.ended and m.seen == S.session
end

-- the missions of a trade that are accepted and not over
local function running(t)
  local list = {}
  for id, m in pairs(S.missions) do
    if m.trade == t.id and live(m) then list[#list + 1] = m end
  end
  table.sort(list, function(a, b) return (a.accepted or 0) > (b.accepted or 0) end)
  return list
end

-- the trades it comes after, not done yet
local function waitingFor(t)
  local list = {}
  for _, id in ipairs(t.after or {}) do
    if not S.done[id] and byId[id] then list[#list + 1] = byId[id].title end
  end
  return list
end

-- The parts of a trade go: in the game, or by hand. Taken from the
-- inventory as far as it holds them.
local function takeParts(t)
  for _, w in ipairs(t.wants) do setHave(w.name, have(w.name) - w.amount) end
end

local function countDone(t, when)
  local d = S.done[t.id] or { n = 0, last = 0 }
  d.n = d.n + 1
  if when > (d.last or 0) then d.last = when end
  S.done[t.id] = d
end

-- ---- the game's log ------------------------------------------------------------
-- A contract of Wikelo's is known by its title when it is accepted, and by
-- the generator the game names on its markers. The title is the game's own,
-- with Bedrock's marks taken off it ("<EM4>[BP]</EM4>").
local function plainTitle(text)
  text = text:gsub("<(%w+)>.-</%1>", "")
  text = text:gsub("<[^>]*>", "")
  text = text:gsub("^%s+", "")
  text = text:gsub("%s+$", "")
  return text
end

local function mission(id)
  local m = S.missions[id]
  if not m then
    m = { parts = {} }
    S.missions[id] = m
  end
  return m
end

-- A line of the log is new news when it was written a moment ago; older
-- ones - the logs of earlier sessions, the start of this one - are history
local function recent(line, old)
  if old then return false end
  local at = bedrock.time(line)
  return at ~= nil and bedrock.now() - at < 10 * 60 * 1000
end

bedrock.watch("Log started on", function(line, old)
  local at = bedrock.time(line)
  if at and at > S.session then
    S.session = at
    save()
  end
end, true)

bedrock.watch("Contract Accepted:", function(line, old)
  local text = line:match('Added notification "Contract Accepted:%s+(.-): " %[')
  local id = line:match("MissionId: %[([%x%-]+)%]")
  if not text or not id then return end
  local t = byTitle[plainTitle(text):lower()]
  local known = S.missions[id]
  if not t and not known then return end
  local m = mission(id)
  if t and not m.trade then m.trade = t.id end
  m.title = m.title or (t and t.title) or plainTitle(text)
  m.accepted = m.accepted or bedrock.time(line)
  m.seen = S.session
  save()
end, true)

bedrock.watch("generator name [TheCollector]", function(line, old)
  local id, contract = line:match("missionId %[([%x%-]+)%], generator name %[TheCollector%], contract %[([^%]]+)%]")
  if not id or not byId[contract] then return end
  local m = mission(id)
  if m.trade ~= contract or m.seen ~= S.session then
    m.trade = contract
    m.title = byId[contract].title
    m.accepted = m.accepted or bedrock.time(line)
    m.seen = S.session
    save()
  end
end, true)

bedrock.watch("New Objective: Need", function(line, old)
  local got, need, name = line:match('"New Objective: Need (%d+)/(%d+) of (.-): " %[')
  local id = line:match("MissionId: %[([%x%-]+)%]")
  local objective = line:match("ObjectiveId: %[([^%]]+)%]")
  local m = id and S.missions[id]
  if not m or not objective then return end
  local p = m.parts[objective] or {}
  p.name, p.need = name, tonumber(need)
  m.parts[objective] = p
  save()
end, true)

bedrock.watch("<ObjectiveUpserted>", function(line, old)
  local id, objective, state = line:match("mission_id ([%x%-]+) %- objective_id (%S+) %- state ([%u_]+)")
  local m = id and S.missions[id]
  if not m or state ~= "MISSION_OBJECTIVE_STATE_COMPLETED" then return end
  local p = m.parts[objective] or {}
  if not p.handed then
    p.handed = bedrock.time(line) or bedrock.now()
    m.parts[objective] = p
    save()
  end
end, true)

bedrock.watch("<EndMission>", function(line, old)
  local id, how = line:match("MissionId%[([%x%-]+)%].-CompletionType%[(%a+)%]")
  local m = id and S.missions[id]
  if not m or m.ended then return end
  local at = bedrock.time(line) or bedrock.now()
  m.ended, m.endedAt = how:lower(), at
  local t = byId[m.trade or ""]
  if how == "Complete" and t then
    countDone(t, at)
    -- handed in: gone from the inventory too, for a trade done since the
    -- plugin came - one done before that is history
    if at >= S.since then
      takeParts(t)
      m.taken = true
    end
    if recent(line, old) then
      bedrock.note("WIKELO  \u{B7}  " .. t.title .. " DONE" ..
        (m.taken and " - Parts removed from inventory" or ""))
    end
  end
  save()
  bedrock.redraw()
end, true)

-- ---- the window --------------------------------------------------------------
local function find(text, query)
  return query == "" or text:lower():find(query, 1, true) ~= nil
end

local function matches(t, query)
  if query == "" then return true end
  if find(t.title, query) then return true end
  for _, w in ipairs(t.wants) do if find(w.name, query) then return true end end
  for _, g in ipairs(t.gives) do if find(g.name, query) then return true end end
  return false
end

local function joinNames(list, most)
  local names = {}
  for i, x in ipairs(list) do
    if i > most then
      names[#names] = names[#names] .. "  +" .. (#list - most) .. " more"
      break
    end
    names[#names + 1] = (x.amount and x.amount > 1 and (x.amount .. " ") or "") .. x.name
  end
  return table.concat(names, ", ")
end

local function shareText(t)
  local lines = { "Wikelo: " .. t.title, "Needs:" }
  for _, w in ipairs(t.wants) do
    lines[#lines + 1] = "- " .. amountText(w, w.amount) .. " " .. w.name .. " (have " .. have(w.name) .. ")"
  end
  lines[#lines + 1] = "Gives:"
  for _, g in ipairs(t.gives) do lines[#lines + 1] = "- " .. g.amount .. " " .. g.name end
  return table.concat(lines, "\n")
end

local ORDER = { READY = 1, PARTIAL = 2, MISSING = 3 }

local function tradeCard(ui, t)
  local st, full = status(t)
  local live = running(t)
  local chip, tone = st, (st == "READY" and "good") or (st == "PARTIAL" and "warn") or "dim"
  if st == "PARTIAL" then chip = "PARTIAL  " .. full .. " OF " .. #t.wants end
  if #live > 0 then chip, tone = "ACCEPTED", "warn" end
  local d = S.done[t.id]
  local sub = "Gives " .. joinNames(t.gives, 2)
  if d then sub = sub .. "  \u{B7}  Done " .. (d.n == 1 and "once" or (d.n .. " times")) end
  if not ui.card("t:" .. t.id, t.title, { subtitle = sub, chip = chip, tone = tone }) then return end

  local waiting = waitingFor(t)
  if #waiting > 0 then
    ui.text("AFTER " .. table.concat(waiting, ", "):upper() .. " - Wikelo offers it once that one is done", "warn")
  end
  -- what it needs: what you have of each, to change here or in the inventory
  local handed = {}
  for _, m in ipairs(live) do
    for _, p in pairs(m.parts) do
      if p.handed and p.name then handed[p.name] = true end
    end
  end
  ui.text("NEEDS", "dim")
  local changed = false
  for _, w in ipairs(t.wants) do
    local h = have(w.name)
    local label = w.name .. "  \u{B7}  needs " .. amountText(w, w.amount)
    local tone = h >= w.amount and "good" or (h > 0 and "warn" or nil)
    if handed[w.name] then label, tone = label .. "  \u{B7}  HANDED IN", "good" end
    local v = ui.counter("n:" .. t.id .. ":" .. w.name, label, h, tone)
    if v ~= h then
      setHave(w.name, v)
      changed = true
    end
  end
  if changed then save() end
  ui.gap()
  ui.text("GIVES", "dim")
  for _, g in ipairs(t.gives) do ui.row(g.name, g.amount > 1 and (g.amount .. "x") or "") end
  -- what the log says of it
  for _, m in ipairs(live) do
    local all, done = 0, 0
    for _, p in pairs(m.parts) do
      all = all + 1
      if p.handed then done = done + 1 end
    end
    ui.gap()
    ui.text("ACCEPTED IN GAME - " .. (m.accepted and bedrock.date(m.accepted) or "Date not in log") ..
      (all > 0 and ("  \u{B7}  " .. done .. " of " .. all .. " handed in") or ""), "warn")
  end
  if d then
    ui.text("DONE " .. (d.n == 1 and "ONCE" or (d.n .. " TIMES")) .. (d.last and d.last > 0 and (" - Last " .. bedrock.date(d.last)) or ""), "good")
  end
  if t.description and t.description ~= "" then
    ui.gap()
    ui.text(t.description, "dim")
  end
  local clicked = ui.buttons({
    { "done:" .. t.id, "Complete", st == "READY" and "primary" or "plain" },
    { "copy:" .. t.id, "Copy", "plain" },
  })
  if clicked == "done:" .. t.id then
    takeParts(t)
    countDone(t, bedrock.now())
    save()
    bedrock.note("WIKELO  \u{B7}  " .. t.title .. " DONE - Parts removed from inventory")
  elseif clicked == "copy:" .. t.id then
    bedrock.copy(shareText(t))
    bedrock.note("WIKELO  \u{B7}  " .. t.title .. " copied")
  end
  ui.close()
end

local function tradesTab(ui)
  local query = ui.search("find", "Search trades, rewards or items"):lower()
  local list, count = {}, { READY = 0, PARTIAL = 0, MISSING = 0, ACCEPTED = 0 }
  for _, t in ipairs(TRADES) do
    local st = status(t)
    local accepted = #running(t) > 0
    count[st] = count[st] + 1
    if accepted then count.ACCEPTED = count.ACCEPTED + 1 end
    list[#list + 1] = { t = t, st = st, accepted = accepted }
  end
  local filter = ui.chips("filter", {
    "All " .. #TRADES, "Ready " .. count.READY, "Partial " .. count.PARTIAL,
    "Missing " .. count.MISSING, "Accepted " .. count.ACCEPTED,
  }, 1)
  local keep = { nil, "READY", "PARTIAL", "MISSING", "ACCEPTED" }
  table.sort(list, function(a, b)
    if a.accepted ~= b.accepted then return a.accepted end
    if a.st ~= b.st then return ORDER[a.st] < ORDER[b.st] end
    return a.t.title < b.t.title
  end)
  local shown = 0
  for _, e in ipairs(list) do
    local wanted = filter == 1 or (keep[filter] == "ACCEPTED" and e.accepted) or keep[filter] == e.st
    if wanted and matches(e.t, query) then
      tradeCard(ui, e.t)
      shown = shown + 1
    end
  end
  if shown == 0 then
    ui.text(query ~= "" and "NO MATCH - No trade, reward or item with that name" or "None of the trades is in this state", "dim")
  end
end

local function inventoryTab(ui)
  local query = ui.search("findItem", "Search items"):lower()
  local filter = ui.chips("items", { "Still needed", "In stock", "All items" }, 1)
  ui.gap()
  local changed, shown = false, 0
  for _, name in ipairs(itemNames) do
    local it, h = items[name], have(name)
    local short = 0
    for _, t in ipairs(it.uses) do
      for _, w in ipairs(t.wants) do
        if w.name == name and w.amount > h then short = short + 1 end
      end
    end
    local wanted = (filter == 1 and short > 0) or (filter == 2 and h > 0) or filter == 3
    if wanted and find(name, query) then
      local uses = #it.uses == 1 and "1 trade" or (#it.uses .. " trades")
      local v = ui.counter("i:" .. name, name .. "  \u{B7}  " .. uses .. (it.unit and ("  \u{B7}  " .. it.unit) or ""), h)
      if v ~= h then
        setHave(name, v)
        changed = true
      end
      shown = shown + 1
    end
  end
  if changed then save() end
  if shown == 0 then
    ui.text(query ~= "" and "NO MATCH - No item with that name" or
      (filter == 2 and "Nothing in stock" or "Nothing needed"), "dim")
  end
  ui.gap()
  local clicked = ui.buttons({ { "copyInv", "Copy inventory", "plain" } })
  if clicked == "copyInv" then
    local lines = { "Wikelo inventory:" }
    for _, name in ipairs(itemNames) do
      if have(name) > 0 then lines[#lines + 1] = "- " .. have(name) .. " " .. name end
    end
    bedrock.copy(table.concat(lines, "\n"))
    bedrock.note("WIKELO  \u{B7}  Inventory copied")
  end
end

local function logTab(ui)
  local open, ended = {}, {}
  for id, m in pairs(S.missions) do
    if live(m) then open[#open + 1] = m
    elseif m.trade and m.ended then ended[#ended + 1] = m end
  end
  table.sort(open, function(a, b) return (a.accepted or 0) > (b.accepted or 0) end)
  table.sort(ended, function(a, b) return (a.endedAt or 0) > (b.endedAt or 0) end)
  ui.heading("ACCEPTED")
  if #open == 0 then ui.text("None", "dim") end
  for _, m in ipairs(open) do
    local all, done = 0, 0
    for _, p in pairs(m.parts) do
      all = all + 1
      if p.handed then done = done + 1 end
    end
    local key = "m:" .. (m.accepted or 0) .. ":" .. m.trade
    if ui.card(key, m.title or m.trade, {
      subtitle = "Accepted " .. (m.accepted and bedrock.date(m.accepted) or "- date not in log"),
      chip = all > 0 and (done .. " OF " .. all .. " HANDED IN") or "ACCEPTED", tone = "warn" }) then
      local names = {}
      for objective, p in pairs(m.parts) do names[#names + 1] = { objective, p } end
      table.sort(names, function(a, b) return a[1] < b[1] end)
      for _, e in ipairs(names) do
        local p = e[2]
        ui.row((p.need or 1) .. " " .. (p.name or "?"), p.handed and "HANDED IN" or ("have " .. have(p.name or "")),
          p.handed and "good" or nil)
      end
      ui.close()
    end
  end
  ui.heading("DONE AND OVER")
  if #ended == 0 then ui.text("Nothing yet", "dim") end
  for i, m in ipairs(ended) do
    if i > 40 then
      ui.text("+" .. (#ended - 40) .. " more", "dim")
      break
    end
    local word = (m.ended == "complete" and "DONE") or (m.ended == "abandon" and "ABANDONED") or
      (m.ended == "fail" and "FAILED") or m.ended:upper()
    ui.row(m.title or m.trade, word .. "  \u{B7}  " .. (m.endedAt and bedrock.date(m.endedAt) or ""),
      m.ended == "complete" and "good" or "dim")
  end
end

function draw(ui)
  local ready = 0
  for _, t in ipairs(TRADES) do
    if status(t) == "READY" then ready = ready + 1 end
  end
  subtitle = (DATA.game or "?") .. "  \u{B7}  " .. #TRADES .. " trades  \u{B7}  " .. ready .. " ready"
  local tab = ui.tabs("tab", { "Trades", "Inventory", "Game log" }, 1)
  if tab == 1 then tradesTab(ui)
  elseif tab == 2 then inventoryTab(ui)
  else logTab(ui) end
end
