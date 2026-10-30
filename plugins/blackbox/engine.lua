-- When to back up, backing up, pruning and restoring - SC Blackbox's
-- BackupEngine, Watcher, RestoreEngine and the notices of its AppHost, with
-- the custom characters as a part of their own. The files and the backups'
-- folder are Bedrock's (bedrock.game, bedrock.backups): the same folder and
-- form as SC Blackbox, so each reads the other's backups.

local diff = bedrock.require("diff")

local G, B = bedrock.game, bedrock.backups

local E = {}

E.SETTLE_MS = 6000        -- files unchanged this long before a backup
E.QUIET_MS = 300000       -- ...while Star Citizen runs: a change left alone this long
E.AFTER_EXIT_MS = 8000    -- after Star Citizen closes
E.YIELD_MS = 20000        -- while SC Blackbox runs: it backs up first
E.REFRESH_MS = 30000      -- the lists read again (SC Blackbox's backups)

local XML = { keybinds = true, settings = true }
local CHARS = { characters = true }

-- ---- what is kept between runs of Bedrock -----------------------------------
-- ignored  channel -> the id of the reset backup whose notice was ignored
-- oldest   channel -> the id of its oldest backup, marked kept once
local S = bedrock.load() or {}
S.ignored = S.ignored or {}
S.oldest = S.oldest or {}
local function save() bedrock.save(S) end

-- ---- channels ----------------------------------------------------------------
-- E.states: one per channel, in the game's order:
--   name, aliases, stamp (of the files when last seen), changedAt, pending,
--   list (its backups, newest first), listAt, notice, dismissed (stamp)
E.states = {}
E.byName = {}
E.notices = {}
E.status = nil            -- { kind, channel, text } of the latest event
E.running = nil           -- Star Citizen, as last seen

local function now() return bedrock.now() end

function E.state(name)
  return E.byName[name]
end

-- The channels beside the game's folder; true where they changed
function E.refreshChannels()
  local list = G.channels() or {}
  local states, byName, changed = {}, {}, #list ~= #E.states
  for i, c in ipairs(list) do
    local st = E.byName[c.name]
    if not st then
      st = { name = c.name, fresh = true }
      changed = true
    elseif E.states[i] ~= st then
      changed = true
    end
    st.aliases = c.aliases or {}
    states[#states + 1] = st
    byName[c.name] = st
  end
  E.states, E.byName = states, byName
  return changed
end

-- A channel's backups, newest first, read again when older than REFRESH_MS
-- or `again`
function E.list(name, again)
  local st = E.byName[name]
  if not st then return {} end
  if again or not st.list or now() - (st.listAt or 0) > E.REFRESH_MS then
    st.list = B.list(name) or {}
    st.listAt = now()
  end
  return st.list
end

-- ---- backing up (BackupEngine.Run) -------------------------------------------

-- The characters a backup stands for: those of the newest backup from
-- `from` on (list index) that counts them or has some - Bedrock's backups
-- count them, SC Blackbox's leave them out. None where there is no such one.
local function baselineChars(name, list, from)
  for i = from or 1, #list do
    local b = list[i]
    if b.counted or (b.characters or 0) > 0 then
      local files = B.files(name, b.id)
      return diff.set(files or {}, CHARS), b
    end
  end
  return diff.set({}, CHARS), nil
end
E.baselineChars = baselineChars

local function findBackup(list, id)
  for i, b in ipairs(list) do
    if b.id == id then return b, i end
  end
  return nil
end
E.findBackup = findBackup

-- Keeps the newest `keep` backups, the newest of each of the last
-- `keepdistinct` keybind sets, and every kept one (SnapshotStore.Prune)
function E.prune(name)
  local where = B.where()
  local list = E.list(name, true)
  local keep = {}
  for i = 1, math.min(where.keep, #list) do keep[list[i].id] = true end
  local distinct, n = {}, 0
  for _, b in ipairs(list) do
    if not distinct[b.keybindhash] and n < where.keepdistinct then
      distinct[b.keybindhash] = true
      n = n + 1
      keep[b.id] = true
    end
  end
  for _, b in ipairs(list) do
    if b.kept then keep[b.id] = true end
  end
  local removed = 0
  for _, b in ipairs(list) do
    if not keep[b.id] then
      local ok, why = B.remove(name, b.id)
      if ok then
        B.log("Removed old backup " .. name .. "/" .. b.id)
        removed = removed + 1
      else
        B.log("Could not remove old backup " .. name .. "/" .. b.id .. ": " .. tostring(why))
      end
    end
  end
  if removed > 0 then E.list(name, true) end
  return removed
end

-- One backup of a channel, unless nothing changed since the latest:
-- { status = created | unchanged | nofiles | damaged | failed, backup (the
-- new one, or the latest), previous, looksReset, message }
-- opt: allowDamaged, restoredFrom, restoredParts
function E.run(name, reason, opt)
  opt = opt or {}
  local files, why = G.files(name)
  if not files then return { status = "failed", message = why or "" } end
  local current = diff.set(files)
  local xml = diff.set(files, XML)
  if #xml.list == 0 then return { status = "nofiles" } end
  -- a file Star Citizen is still writing (or a broken one) is not worth keeping
  local bad = diff.damaged(xml)
  if #bad > 0 and not opt.allowDamaged then
    return { status = "damaged", message = table.concat(bad, ", "), files = bad }
  end

  local hashes = bedrock.hashes(files)
  local list = E.list(name, true)
  local latest = list[1]
  local base = baselineChars(name, list, 1)
  local chars = diff.set(files, CHARS)
  local charsSame = diff.same(chars, base)
  if latest and latest.allhash == hashes.all and charsSame then
    return { status = "unchanged", backup = latest }
  end
  local before
  if latest then
    local bf = B.files(name, latest.id)
    if bf then before = diff.set(bf, XML) end
    -- the game also saves a few values by itself while it runs: when nothing
    -- else changed, the latest backup still stands
    if before and charsSame and diff.onlyGameStateDiffers(before, xml) then
      return { status = "unchanged", backup = latest }
    end
  end

  local info = {
    reason = reason,
    restoredfrom = opt.restoredFrom or "",
    restoredparts = opt.restoredParts or "",
    summary = "",
    detail = "",
    flag = "",
    kept = false,
    binds = diff.countBinds(current),
  }
  local looksReset = false
  if not latest then
    -- the first backup of a channel is kept forever
    info.summary = "First backup"
    info.kept = true
  else
    local ok = pcall(function()
      if not before then error("the latest backup can't be read") end
      local d = diff.compare(diff.merged(before, base, CHARS), current)
      info.summary = #d.lines == 0 and "Minor file changes" or diff.summary(d)
      info.detail = diff.detail(d)
    end)
    if not ok then
      info.summary = "Changed"
      info.detail = ""
    end
    if reason ~= "restored" and latest.binds >= 10 and info.binds * 2 < latest.binds and latest.binds - info.binds >= 10 then
      info.flag = "reset"
      looksReset = true
    end
  end

  local id, why2 = B.write(name, files, info)
  if not id then
    B.log("Backup of " .. name .. " failed: " .. tostring(why2))
    return { status = "failed", message = why2 or "" }
  end
  B.log("Backup " .. name .. "/" .. id .. " (" .. reason .. "): " .. info.summary .. ", " .. info.binds .. " binds")
  if not latest then
    S.oldest[name] = id
    save()
  end
  E.prune(name)
  local backup = findBackup(E.list(name), id) or { id = id, created = now(), binds = info.binds, summary = info.summary }
  return { status = "created", backup = backup, previous = latest, looksReset = looksReset, info = info }
end

-- ---- notices (AppHost) ---------------------------------------------------------
-- E.notices[channel]: { kind = reset | nofiles | damaged, before, after,
-- restoreTarget, diffFrom, diffTo (nil: the files now), filesProblem, files }

local function setNotice(name, notice)
  E.notices[name] = notice
  bedrock.redraw()
end

local function resetNotice(before, after)
  return { kind = "reset", before = before, after = after, restoreTarget = before, diffFrom = before, diffTo = after }
end

local function dismissed(st)
  return st.dismissed ~= nil and st.dismissed == st.stamp
end

local function onChecked(st, reason, r)
  local name = st.name
  local current = E.notices[name]
  if r.status == "created" then
    E.status = { kind = "saved", channel = name, text = r.backup.summary or "" }
    if r.looksReset and r.previous and reason ~= "before-restore" and reason ~= "restored" then
      setNotice(name, resetNotice(r.previous, r.backup))
    elseif current and current.filesProblem then
      setNotice(name, nil)
    elseif current and current.diffTo and (r.backup.binds or 0) * 2 >= (current.restoreTarget.binds or 0) then
      setNotice(name, nil)   -- the binds are back
    end
  elseif r.status == "unchanged" then
    if current and current.filesProblem then setNotice(name, nil) end
  elseif r.status == "nofiles" then
    local latest = E.list(name)[1]
    if latest and not dismissed(st) then
      setNotice(name, { kind = "nofiles", filesProblem = true, restoreTarget = latest, diffFrom = latest })
    end
  elseif r.status == "damaged" then
    B.log(name .. ": can't read " .. (r.message or ""))
    local latest = E.list(name)[1]
    if latest and not E.running and not dismissed(st) then
      setNotice(name, { kind = "damaged", filesProblem = true, restoreTarget = latest, diffFrom = latest,
        files = r.files })
    end
  elseif r.status == "failed" then
    E.status = { kind = "failed", channel = name, text = r.message or "" }
  end
  bedrock.redraw()
end

-- A reset noticed earlier, neither restored nor ignored, shows again after a
-- start, as long as the binds are still missing
function E.showOpenResets()
  for _, st in ipairs(E.states) do
    if not E.notices[st.name] then
      local all = E.list(st.name)
      local i
      for k, b in ipairs(all) do
        if b.flag == "reset" and b.reason ~= "before-restore" then
          i = k
          break
        end
      end
      if i and i + 1 <= #all then
        local reset, before = all[i], all[i + 1]
        local restored = false
        for k = 1, i - 1 do
          if all[k].reason == "restored" then restored = true end
        end
        if reset.id ~= S.ignored[st.name] and not restored and all[1].binds * 2 < before.binds then
          setNotice(st.name, resetNotice(before, reset))
        end
      end
    end
  end
end

function E.ignore(name)
  local st = E.byName[name]
  local n = E.notices[name]
  if not st then return end
  st.dismissed = st.stamp
  if n and n.diffTo then
    S.ignored[name] = n.diffTo.id
    save()
  end
  setNotice(name, nil)
end

-- The oldest backup of each channel is kept forever: marked once, so a mark
-- taken off by hand stays off
function E.keepOldest()
  for _, st in ipairs(E.states) do
    if not S.oldest[st.name] then
      local list = E.list(st.name, true)
      local oldest = list[#list]
      if oldest then
        local ok = oldest.kept or B.mark(st.name, oldest.id, true)
        if ok then
          S.oldest[st.name] = oldest.id
          save()
          if not oldest.kept then
            B.log("Kept forever: " .. st.name .. "/" .. oldest.id .. " (the oldest)")
            E.list(st.name, true)
          end
        end
      end
    end
  end
end

-- ---- watching (Watcher) --------------------------------------------------------

function E.check(st, reason)
  st.stamp = G.stamp(st.name)
  st.pending = false
  local r = E.run(st.name, reason)
  onChecked(st, reason, r)
  return r
end

function E.checkAll(reason)
  for _, st in ipairs(E.states) do E.check(st, reason) end
end

local started = false
local startAt, gameStartAt, exitAt, refreshAt

-- every 2 s: at the first after Bedrock started a backup ("start"); when
-- Star Citizen starts ("game-start"), a moment after it closes ("game-exit"),
-- and when the files changed and were then left alone: QUIET_MS while the
-- game runs ("while-playing"), so that a slider moved again and again makes
-- one backup, SETTLE_MS when it does not ("change"). While SC Blackbox runs,
-- each waits at least YIELD_MS, so that it backs up first and this finds
-- nothing new.
function E.tick()
  local t = now()
  local yield = G.blackbox()
  local running = G.running()
  if not started then
    started = true
    E.running = running
    E.refreshChannels()
    for _, st in ipairs(E.states) do
      st.stamp = G.stamp(st.name)
      st.fresh = nil
    end
    startAt = yield and t + E.YIELD_MS or t
    refreshAt = t + E.REFRESH_MS
  elseif refreshAt and t >= refreshAt then
    refreshAt = t + E.REFRESH_MS
    E.refreshChannels()
    E.keepOldest()
    for _, st in ipairs(E.states) do
      if st.fresh then
        st.fresh = nil
        E.check(st, "start")
      else
        E.list(st.name)
      end
    end
    bedrock.redraw()
  end
  if startAt and t >= startAt then
    startAt = nil
    -- (before the first prune: the oldest stays)
    E.keepOldest()
    E.checkAll("start")
    E.showOpenResets()
  end

  if running ~= E.running then
    E.running = running
    if running then
      exitAt = nil
      gameStartAt = yield and t + E.YIELD_MS or t
    else
      gameStartAt = nil
      exitAt = t + (yield and E.YIELD_MS or E.AFTER_EXIT_MS)
    end
    bedrock.redraw()
  end
  if gameStartAt and t >= gameStartAt then
    gameStartAt = nil
    E.checkAll("game-start")
  end
  if exitAt and t >= exitAt then
    exitAt = nil
    E.checkAll("game-exit")
  end

  local settle = (E.running and E.QUIET_MS) or (yield and E.YIELD_MS) or E.SETTLE_MS
  for _, st in ipairs(E.states) do
    local stamp = G.stamp(st.name)
    if stamp ~= st.stamp then
      st.stamp = stamp
      st.changedAt = t
      st.pending = true
    elseif st.pending and not exitAt and t - st.changedAt >= settle then
      -- (once the game closed, its exit check takes what is pending)
      E.check(st, E.running and "while-playing" or "change")
    end
  end
end

-- By hand: the result of the check
function E.backupNow(name)
  local st = E.byName[name]
  if not st then return nil end
  return E.check(st, "manual")
end

-- ---- restoring (RestoreEngine) ---------------------------------------------------
-- part: all, keybinds, settings or characters. { ok, nothingToDo, written,
-- before, after, problem = running | unreadable | empty | backup | write, why }

function E.restore(name, id, part)
  if G.running() then return { problem = "running" } end
  local files, why = B.files(name, id)
  if not files then return { problem = "unreadable", why = why } end
  local pick, n = {}, 0
  for rel, f in pairs(files) do
    local p = diff.partOf(rel)
    if p and (part == "all" or p == part) then
      pick[rel] = f.data
      n = n + 1
    end
  end
  if n == 0 then return { problem = "empty" } end

  local nowSet = diff.set(G.files(name) or {})
  local differs = false
  for rel, data in pairs(pick) do
    if diff.get(nowSet, rel) ~= data then
      differs = true
      break
    end
  end
  if not differs then return { ok = true, nothingToDo = true } end

  -- the files as they are first, broken or not, so the restore can be undone
  local before = E.run(name, "before-restore", { allowDamaged = true })
  if before.status == "failed" then return { problem = "backup", why = before.message } end
  local written, why2 = G.restore(name, pick)
  if not written then
    B.log("Restore of " .. name .. " from " .. id .. " failed: " .. tostring(why2))
    return { problem = why2 == "Star Citizen is running" and "running" or "write", why = why2, before = before.backup }
  end
  local after = E.run(name, "restored", { allowDamaged = true, restoredFrom = id, restoredParts = part })
  B.log("Restored " .. name .. " from " .. id .. " (" .. part .. ", " .. written .. " files)")
  local st = E.byName[name]
  if st then
    st.stamp = G.stamp(name)
    st.pending = false
  end
  E.notices[name] = nil
  E.status = { kind = "restored", channel = name, text = id }
  return { ok = true, written = written, before = before.backup, after = after.backup }
end

-- The parts a backup holds: { keybinds = n, settings = n, characters = n }
function E.partsOf(name, id)
  local files = B.files(name, id)
  local parts = { keybinds = 0, settings = 0, characters = 0 }
  for rel in pairs(files or {}) do
    local p = diff.partOf(rel)
    if p then parts[p] = parts[p] + 1 end
  end
  return parts
end

-- ---- comparing ---------------------------------------------------------------------

-- What changed from backup `from` to backup `to` (nil: the files now).
-- Characters count where `to` has them: against those `from` stands for.
function E.compare(name, from, to)
  local list = E.list(name)
  local toFiles = to and B.files(name, to.id) or G.files(name)
  local fromFiles = from and B.files(name, from.id) or {}
  if not toFiles or not fromFiles then return nil end
  local b = diff.set(toFiles)
  local a = diff.set(fromFiles, XML)
  local withChars = to == nil or to.counted or (to.characters or 0) > 0
  if withChars then
    local base = diff.set({}, CHARS)
    if from then
      local _, i = findBackup(list, from.id)
      base = baselineChars(name, list, i or #list + 1)
    end
    a = diff.merged(a, base, CHARS)
  else
    b = diff.set(toFiles, XML)
  end
  return diff.compare(a, b)
end

return E
