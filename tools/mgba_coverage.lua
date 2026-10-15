-- Function-entry coverage for mGBA (development build; needs emu:setBreakpoint).
-- Tools > Scripting > File > Load script.
-- Reads build/coverage/entries.txt (made by `python tools/coverage.py prep`), sets a
-- breakpoint on each entry, records which fired and removes it on first hit.
--
-- If the game is too slow, run in SLICES: write "k n" in build/coverage/slice.txt
-- (k = 0..n-1). Only entries with index % n == k get a breakpoint, so each session
-- has n times fewer. Output goes to hits_k_of_n.txt; merge with `coverage.py report`.
-- relative to mGBA's working dir; use an absolute path if mGBA cannot find it
local DIR = "build/coverage/"

if not emu.setBreakpoint then
  console:error("this mGBA has no emu:setBreakpoint (stable 0.10.x lacks it). Use a development build: https://mgba.io/downloads.html#development-downloads")
  return
end

local K, N = 0, 1
local sf = io.open(DIR .. "slice.txt", "r")
if sf then
  local k, n = sf:read("*l"):match("(%d+)%s+(%d+)")
  if k and tonumber(n) > 0 then K, N = tonumber(k), tonumber(n) end
  sf:close()
end
local OUT = DIR .. string.format("hits_%d_of_%d.txt", K, N)

local hits, ids = {}, {}
local total, nhit, frames = 0, 0, 0

local function flush()
  local f = io.open(OUT, "w")
  if not f then console:error("cannot write " .. OUT); return end
  for a in pairs(hits) do f:write(string.format("0x%08X\n", a)) end
  f:close()
end

local fh = io.open(DIR .. "entries.txt", "r")
if not fh then console:error("missing entries.txt (run: python tools/coverage.py prep)"); return end
local idx = 0
for line in fh:lines() do
  local a = tonumber(line)
  if a then
    if idx % N == K then
      total = total + 1
      -- one-shot: remove the breakpoint right away so hot functions cost one Lua call, ever
      ids[a] = emu:setBreakpoint(function()
        if not hits[a] then
          hits[a] = true; nhit = nhit + 1
          emu:clearBreakpoint(ids[a])
        end
      end, a)
    end
    idx = idx + 1
  end
end
fh:close()
console:log(string.format("coverage slice %d/%d: %d breakpoints -> %s", K, N, total, OUT))

-- ---------------------------------------------------------------------------
-- Optional bot: create build/coverage/bot.txt (optional content: "<seed> <minutes>")
-- and the script drives the game by itself (random but menu-aware), so nobody has
-- to play. Ends by flushing hits and logging; stop early with the console/close.
-- ---------------------------------------------------------------------------
local KEY = {A = 0, B = 1, SELECT = 2, START = 3, RIGHT = 4, LEFT = 5, UP = 6, DOWN = 7, R = 8, L = 9}
local bot, seed, max_frames = false, 1234, 60 * 60 * 30
local bf = io.open(DIR .. "bot.txt", "r")
if bf then
  bot = true
  local sd, mins = (bf:read("*l") or ""):match("(%d+)%s+(%d+)")
  if sd then seed, max_frames = tonumber(sd), tonumber(mins) * 60 * 60 end
  bf:close()
  math.randomseed(seed)
  console:log(string.format("bot: ON seed=%d for %d frames (~%d min game time)", seed, max_frames, max_frames // 3600))
end
local function mask(...) local m = 0 for _, k in ipairs({...}) do m = m | (1 << KEY[k]) end return m end
local cur, hold_until, last_nhit, plateau = 0, 0, 0, 0
local function bot_step()
  if frames < 1500 then
    -- boot/title/menu: mash START, then A, so file select and the first level start
    if frames % 40 < 6 then cur = mask("START") elseif frames % 40 >= 20 and frames % 40 < 26 then cur = mask("A") else cur = 0 end
    return
  end
  if frames < hold_until then emu:setKeys(cur) return end
  hold_until = frames + 10 + math.random(0, 50)
  local r, m = math.random(), 0
  if     r < 0.45 then m = mask("RIGHT")
  elseif r < 0.65 then m = mask("LEFT")
  elseif r < 0.73 then m = mask("UP")
  elseif r < 0.81 then m = mask("DOWN") end
  if math.random() < 0.5  then m = m | mask("A") end               -- jump / confirm
  if math.random() < 0.35 then m = m | mask("B") end               -- inhale / attack
  if math.random() < 0.08 then m = m | mask("R") end
  if math.random() < 0.05 then m = m | mask("L") end
  if math.random() < 0.04 then m = m | mask("START") end           -- pause menu now and then
  if math.random() < 0.02 then m = m | mask("SELECT") end
  cur = m
end

callbacks:add("frame", function()
  frames = frames + 1
  if bot then
    bot_step()
    emu:setKeys(cur)
    if frames >= max_frames then
      flush(); console:log("bot: finished, hits written to " .. OUT); bot = false; emu:setKeys(0)
    end
  end
  if frames % 600 == 0 then
    flush()
    console:log(string.format("coverage: %d / %d hit", nhit, total))
  end
end)
callbacks:add("shutdown", flush)
