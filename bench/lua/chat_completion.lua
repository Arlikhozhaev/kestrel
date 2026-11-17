-- Open-loop body for wrk2. Constant rate is wrk2's -R, not a closed-loop thread count.
-- The content bytes match bench/prompt.py: "k" + 8-digit id + "x" padding.
-- A repeated id is an exact-match hit after warmup. A fresh id is a miss.
-- Paraphrases are not hits. This script does not generate them.

local function env_number(name, default)
  if os == nil or os.getenv == nil then
    error("wrk2 lua cannot read the environment")
  end
  local value = os.getenv(name)
  if value == nil or value == "" then
    return default
  end
  return tonumber(value)
end

local hit_ratio = env_number("KESTREL_HIT_RATIO", 1)
local payload = env_number("KESTREL_PAYLOAD", 128)
local pool = env_number("KESTREL_POOL", 32)
local uniq = env_number("KESTREL_UNIQ_BASE", 1000000)

if payload < 9 or payload > 8192 then
  error("KESTREL_PAYLOAD must be 9..8192")
end
if pool < 1 then
  error("KESTREL_POOL must be >= 1")
end

wrk.method = "POST"
wrk.headers["Content-Type"] = "application/json"
wrk.headers["Host"] = "127.0.0.1"

local function content(id)
  local prefix = string.format("k%08d", id)
  if payload == #prefix then
    return prefix
  end
  return prefix .. string.rep("x", payload - #prefix)
end

local function request_body(id)
  return '{"model":"m","messages":[{"role":"user","content":"' .. content(id) .. '"}]}'
end

function request()
  local id
  if hit_ratio >= 1 then
    id = 0
  elseif hit_ratio <= 0 then
    uniq = uniq + 1
    id = uniq
  elseif math.random() < hit_ratio then
    id = math.random(0, pool - 1)
  else
    uniq = uniq + 1
    id = uniq
  end
  return wrk.format(nil, "/v1/chat/completions", nil, request_body(id))
end
