-- A small XML reader for the game's settings files: elements and their
-- attributes, after the checks .NET's XmlReader makes (SC Blackbox reads the
-- files with it). A file that fails them - cut off, emptied, garbled - is
-- damaged, and parse returns nil and why.
--
--   xml.parse(data)       the root element, or nil, why
--   xml.attr(el, name)    an attribute's value (one without a prefix), or nil
--
-- An element: { name = qualified, lname = local, attrs = { {q, l, v}, ... },
-- kids = { elements } } - attributes and child elements in file order.

local xml = {}

local byte, find, sub, match = string.byte, string.find, string.sub, string.match

local NAME = "^[A-Za-z_:\128-\255][A-Za-z0-9_:%.%-\128-\255]*"
local WS = "^[ \t\r\n]*"

local ENTITIES = { lt = "<", gt = ">", amp = "&", apos = "'", quot = '"' }

local function validChar(c)
  return c == 0x9 or c == 0xA or c == 0xD or (c >= 0x20 and c <= 0xD7FF) or
    (c >= 0xE000 and c <= 0xFFFD) or (c >= 0x10000 and c <= 0x10FFFF)
end

-- Text with its references replaced; nil where one is not a known entity or
-- a character the file may hold
local function unescape(raw)
  if not find(raw, "&", 1, true) then return raw end
  local parts, i = {}, 1
  while true do
    local a = find(raw, "&", i, true)
    if not a then
      parts[#parts + 1] = sub(raw, i)
      break
    end
    parts[#parts + 1] = sub(raw, i, a - 1)
    local semi = find(raw, ";", a + 1, true)
    if not semi then return nil end
    local ref = sub(raw, a + 1, semi - 1)
    local ch = ENTITIES[ref]
    if not ch then
      local code
      if find(ref, "^#x[0-9A-Fa-f]+$") then
        code = tonumber(sub(ref, 3), 16)
      elseif find(ref, "^#[0-9]+$") then
        code = tonumber(sub(ref, 2))
      end
      if not code or code ~= math.floor(code) or not validChar(code) then return nil end
      ch = utf8.char(math.tointeger(code))
    end
    parts[#parts + 1] = ch
    i = semi + 1
  end
  return table.concat(parts)
end

-- An attribute's value as XmlReader gives it: each line break, tab or
-- carriage return written in it a space
local function attrValue(raw)
  if find(raw, "[\r\n\t]") then
    raw = raw:gsub("\r\n", " "):gsub("[\r\n\t]", " ")
  end
  return unescape(raw)
end

local function localName(q)
  local colon = find(q, ":", 1, true)
  return colon and sub(q, colon + 1) or q
end

-- A prefixed name needs its prefix declared (xmlns:p) on the element or above
local function prefixOk(q, scope)
  local p = match(q, "^([^:]+):[^:]+$")
  if not p then return false end
  return p == "xml" or p == "xmlns" or (scope and scope[p] == true) or false
end

function xml.attr(el, name)
  local attrs = el.attrs
  for i = 1, #attrs do
    local a = attrs[i]
    if a[1] == name then return a[3] end
  end
  return nil
end

-- Comments, processing instructions and white space between elements, from
-- `pos`; the position after them, or nil, why
local function misc(s, pos, inside)
  while true do
    if inside then
      -- text up to the next tag is the caller's
      if byte(s, pos) ~= 60 then return pos end
    else
      pos = select(2, find(s, WS, pos)) + 1
    end
    if find(s, "^<!%-%-", pos) then
      local d = find(s, "--", pos + 4, true)
      if not d then return nil, "a comment is not closed" end
      if byte(s, d + 2) ~= 62 then return nil, "a comment holds '--'" end
      pos = d + 3
    elseif find(s, "^<%?", pos) then
      local e = select(2, find(s, NAME, pos + 2))
      if not e then return nil, "a processing instruction has no name" end
      local target = sub(s, pos + 2, e)
      if target:lower() == "xml" then return nil, "an XML declaration out of place" end
      local close = find(s, "?>", e + 1, true)
      if not close then return nil, "a processing instruction is not closed" end
      if close > e + 1 and not find(s, "^[ \t\r\n]", e + 1) then
        return nil, "a processing instruction's name runs on"
      end
      pos = close + 2
    else
      return pos
    end
  end
end

-- The start tag at `pos` ('<' there): the element, where it ends, whether it
-- is empty (<a/>)
local function startTag(s, pos, scope)
  local e = select(2, find(s, NAME, pos + 1))
  if not e then return nil, "a tag without a name" end
  local q = sub(s, pos + 1, e)
  local el = { name = q, lname = localName(q), attrs = {}, kids = {} }
  pos = e + 1
  local seen = {}
  local declared
  while true do
    local w = select(2, find(s, WS, pos))
    local spaced = w >= pos
    pos = w + 1
    local c = byte(s, pos)
    if c == 62 then -- >
      pos = pos + 1
      break
    elseif c == 47 then -- /
      if byte(s, pos + 1) ~= 62 then return nil, "'/' in a tag" end
      el.empty = true
      pos = pos + 2
      break
    elseif c == nil then
      return nil, "the file ends in a tag"
    end
    if not spaced then return nil, "attributes run together" end
    local ae = select(2, find(s, NAME, pos))
    if not ae then return nil, "an attribute without a name" end
    local an = sub(s, pos, ae)
    local eq = select(2, find(s, "^[ \t\r\n]*=[ \t\r\n]*", ae + 1))
    if not eq then return nil, "an attribute without a value" end
    pos = eq + 1
    local quote = sub(s, pos, pos)
    if quote ~= '"' and quote ~= "'" then return nil, "an attribute's value not in quotes" end
    local close = find(s, quote, pos + 1, true)
    if not close then return nil, "the file ends in an attribute" end
    local raw = sub(s, pos + 1, close - 1)
    if find(raw, "<", 1, true) then return nil, "'<' in an attribute" end
    local v = attrValue(raw)
    if not v then return nil, "a reference that is not known" end
    if seen[an] then return nil, "an attribute twice" end
    seen[an] = true
    local attrs = el.attrs
    attrs[#attrs + 1] = { an, localName(an), v }
    if sub(an, 1, 6) == "xmlns:" then
      if v == "" then return nil, "a prefix bound to nothing" end
      declared = declared or {}
      declared[sub(an, 7)] = true
    end
    pos = close + 1
  end
  if declared then
    scope = setmetatable(declared, { __index = scope })
  end
  if find(q, ":", 1, true) and not prefixOk(q, scope) then return nil, "a prefix not declared" end
  for _, a in ipairs(el.attrs) do
    if find(a[1], ":", 1, true) and not prefixOk(a[1], scope) then return nil, "a prefix not declared" end
  end
  return el, pos, scope
end

-- Text between tags: its references known, no "]]>"
local function textOk(t)
  if find(t, "]]>", 1, true) then return false end
  return not find(t, "&", 1, true) or unescape(t) ~= nil
end

function xml.parse(s)
  if type(s) ~= "string" then return nil, "no data" end
  local pos = 1
  if sub(s, 1, 3) == "\239\187\191" then pos = 4 end
  -- the characters: UTF-8, and none XML forbids (most control characters,
  -- U+FFFE and U+FFFF)
  if not utf8.len(s, pos) then return nil, "not UTF-8" end
  if find(s, "[\0-\8\11\12\14-\31]", pos) or find(s, "\239\191[\190\191]", pos) then
    return nil, "a character XML forbids"
  end

  -- <?xml version="1.0" encoding="..." standalone="..."?>, first or never
  if find(s, "^<%?xml[ \t\r\n]", pos) then
    local close = find(s, "?>", pos, true)
    if not close then return nil, "the declaration is not closed" end
    local d = sub(s, pos + 5, close - 1)
    local q, rest = match(d, "^[ \t\r\n]+version[ \t\r\n]*=[ \t\r\n]*([\"'])1%.0%1(.*)$")
    if not q then return nil, "the declaration has no version 1.0" end
    local q2, r2 = match(rest, "^[ \t\r\n]+encoding[ \t\r\n]*=[ \t\r\n]*([\"'])[A-Za-z][A-Za-z0-9%._%-]*%1(.*)$")
    if q2 then rest = r2 end
    local q3, v3, r3 = match(rest, "^[ \t\r\n]+standalone[ \t\r\n]*=[ \t\r\n]*([\"'])(%w*)%1(.*)$")
    if q3 then
      if v3 ~= "yes" and v3 ~= "no" then return nil, "standalone is yes or no" end
      rest = r3
    end
    if not find(rest, "^[ \t\r\n]*$") then return nil, "the declaration holds more" end
    pos = close + 2
  end

  local why
  pos, why = misc(s, pos, false)
  if not pos then return nil, why end
  if find(s, "^<!DOCTYPE", pos) then return nil, "a DTD" end
  if byte(s, pos) ~= 60 then
    return nil, pos > #s and "no root element" or "text outside the root element"
  end

  -- the prefixes declared at each open element (false: none)
  local root, scope
  root, pos, scope = startTag(s, pos, nil)
  if not root then return nil, pos end
  local stack, scopes = {}, {}
  if not root.empty then
    stack[1], scopes[1] = root, scope or false
  end
  while #stack > 0 do
    local lt = find(s, "<", pos, true)
    if not lt then return nil, "the file ends inside <" .. stack[#stack].name .. ">" end
    if lt > pos and not textOk(sub(s, pos, lt - 1)) then return nil, "bad text" end
    pos = lt
    local c = byte(s, pos + 1)
    if c == 47 then -- </name>
      local e = select(2, find(s, NAME, pos + 2))
      if not e then return nil, "an end tag without a name" end
      local top = stack[#stack]
      if sub(s, pos + 2, e) ~= top.name then return nil, "</" .. sub(s, pos + 2, e) .. "> closes <" .. top.name .. ">" end
      local g = select(2, find(s, "^[ \t\r\n]*>", e + 1))
      if not g then return nil, "an end tag runs on" end
      pos = g + 1
      stack[#stack], scopes[#scopes] = nil, nil
    elseif c == 33 then -- <!-- --> or <![CDATA[ ]]>
      if find(s, "^<!%[CDATA%[", pos) then
        local e = find(s, "]]>", pos + 9, true)
        if not e then return nil, "CDATA is not closed" end
        pos = e + 3
      elseif find(s, "^<!%-%-", pos) then
        pos, why = misc(s, pos, true)
        if not pos then return nil, why end
      else
        return nil, "'<!' in an element"
      end
    elseif c == 63 then -- <?pi?>
      pos, why = misc(s, pos, true)
      if not pos then return nil, why end
    else
      local el, after, sc = startTag(s, pos, scopes[#scopes] or nil)
      if not el then return nil, after end
      local kids = stack[#stack].kids
      kids[#kids + 1] = el
      pos = after
      if not el.empty then
        stack[#stack + 1], scopes[#scopes + 1] = el, sc or false
      end
    end
  end

  -- after the root: comments, processing instructions, white space
  pos, why = misc(s, pos, false)
  if not pos then return nil, why end
  if pos <= #s then
    return nil, byte(s, pos) == 60 and "a second root element" or "text after the root element"
  end
  return root
end

function xml.valid(data)
  return xml.parse(data) ~= nil
end

return xml
