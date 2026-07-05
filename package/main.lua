local FALLBACK_APP_ID = "holo-retro-go"

local function app_id_from_dir(dir)
  dir = type(dir) == "string" and dir:gsub("\\", "/") or ""
  local id = dir:match("([^/]+)$")
  if id and id ~= "" then
    return id
  end
  return FALLBACK_APP_ID
end

local function resolve_app_dir()
  local cur = app and app.current and app.current() or nil
  local entry = cur and cur.entry or nil
  if type(entry) == "string" and entry ~= "" then
    entry = entry:gsub("\\", "/")
    local dir = entry:match("^(.*)/[^/]*$")
    if dir and dir ~= "" then
      return dir
    end
  end
  return "/sd/apps/" .. FALLBACK_APP_ID
end

local function normalize_route_base(route, fallback)
  route = type(route) == "string" and route or ""
  if route == "" then
    route = fallback or ("/" .. FALLBACK_APP_ID)
  end
  route = route:gsub("\\", "/")
  if route:sub(1, 1) ~= "/" then
    route = "/" .. route
  end
  while #route > 1 and route:sub(-1) == "/" do
    route = route:sub(1, -2)
  end
  return route
end

local APP_DIR = resolve_app_dir()
local ROUTE_BASE = normalize_route_base(
  app and app.route_base and app.route_base() or nil,
  "/" .. app_id_from_dir(APP_DIR)
)
local MODULE_DIR = APP_DIR .. "/modules"

local APP = {
  VERSION = "2026-07-01-runtime-light-v16",
  APP_DIR = APP_DIR,
  MODULE_DIR = MODULE_DIR,
  ROM_ROOT = "/sd/roms/md",
  LEGACY_ROM_ROOT = APP_DIR .. "/roms",
  ROUTE_BASE = ROUTE_BASE,
  API_PREFIX = ROUTE_BASE .. "/api",
  CHUNK_SIZE = 64 * 1024,
  MAX_ROM_FILE_SIZE = 32 * 1024 * 1024,
  POLL_DELAY_MS = 200,
  STATUS_POLL_DELAY_MS = 200,
  EXIT_POLL_DELAY_MS = 200,
  AXIS_THRESHOLD = 0.60,
  routes = {},
  web_ready = false,
  rom_list_cache = {},
  rom_list_cache_ready = false,
  AUTO_SELECT_MODULE = false,
  DEFAULT_MODULE_ID = "nes",
  AUDIO_EQ = {
    md = {
      low = { type = "peak", freq = 240, gain = 6.5, q = 0.65 },
      mid = { type = "peak", freq = 1200, gain = -4.0, q = 0.65 },
    },
  },
  MODULES = {
    {
      id = "nes",
      title = "Retro Core",
      detail = "Holocubic classic cores",
      path = MODULE_DIR .. "/retrogo.so",
    },
    {
      id = "md",
      title = "Mega Drive",
      detail = "Holocubic optimized",
      path = MODULE_DIR .. "/gwenesis.so",
    },
  },
}

local function log(...)
  print("[retrogo_app]", ...)
end

log("boot", APP.VERSION, APP.ROUTE_BASE, APP.APP_DIR)

local function has_bit(mask, bit)
  if type(mask) ~= "number" or type(bit) ~= "number" or bit == 0 then
    return false
  end
  local r = mask % (bit * 2)
  return r >= bit and r < bit * 2
end

local function to_bool(v)
  if type(v) == "boolean" then
    return v
  end
  if type(v) == "number" then
    return v ~= 0
  end
  if type(v) == "string" then
    local s = v:lower()
    return s == "1" or s == "true" or s == "on" or s == "yes" or s == "connected"
  end
  return false
end

local function read_gamepad_state()
  if not gamepad or not gamepad.state then
    return nil
  end

  local ok, state = pcall(function()
    return gamepad.state()
  end)
  if not ok or type(state) ~= "table" then
    return nil
  end

  return state
end

local function to_number(v)
  if type(v) == "number" then
    return v
  end
  if type(v) == "string" then
    local n = tonumber(v)
    if n then
      return n
    end
  end
  return nil
end

local function any_true(state, names)
  for _, key in ipairs(names) do
    if to_bool(state[key]) then
      return true
    end
  end
  return false
end

local function first_true_name(state, names)
  for _, key in ipairs(names) do
    if to_bool(state[key]) then
      return key
    end
  end
  return nil
end

local function names_from_mask(mask, defs)
  local names = {}
  mask = tonumber(mask) or 0
  for _, def in ipairs(defs or {}) do
    if has_bit(mask, def[2]) then
      names[#names + 1] = def[1]
    end
  end
  if #names == 0 then
    return "-"
  end
  return table.concat(names, "+")
end

local function connected_state(state)
  if not state then
    return false
  end
  if state.connected ~= nil then
    return to_bool(state.connected)
  end
  if state.started ~= nil then
    return to_bool(state.started)
  end
  return true
end

local function axis_greater_than(value, threshold)
  return type(value) == "number" and value >= math.abs(threshold or APP.AXIS_THRESHOLD)
end

local function axis_less_than(value, threshold)
  return type(value) == "number" and value <= -math.abs(threshold or APP.AXIS_THRESHOLD)
end

local function read_raw_gamepad_mask(state)
  local raw_mask = 0
  for _, key in ipairs({
      "buttons_mask",
      "buttons",
      "mask",
      "button_mask",
      "button",
    }) do
    raw_mask = to_number(state[key]) or raw_mask
    if raw_mask ~= 0 then
      break
    end
  end
  if raw_mask == 0 and type(state.buttons) == "table" then
    raw_mask = to_number(state.buttons.mask) or to_number(state.buttons.value) or 0
  end
  return raw_mask
end

local function build_gamepad_mask(state, retrogo_mask)
  local raw_mask = read_raw_gamepad_mask(state)

  local lx = state.lx or state.analog_x or state.left_x
  local ly = state.ly or state.analog_y or state.left_y

  local up = has_bit(raw_mask, gamepad and gamepad.BTN_UP or 0) or
      any_true(state, { "dpad_up", "up_pressed", "up_key" })
  local down = has_bit(raw_mask, gamepad and gamepad.BTN_DOWN or 0) or
      any_true(state, { "dpad_down", "down_pressed", "down_key" })
  local left = has_bit(raw_mask, gamepad and gamepad.BTN_LEFT or 0) or
      any_true(state, { "dpad_left", "left_pressed", "left_key" })
  local right = has_bit(raw_mask, gamepad and gamepad.BTN_RIGHT or 0) or
      any_true(state, { "dpad_right", "right_pressed", "right_key" })

  if not up and not down then
    up = axis_greater_than(ly)
    down = axis_less_than(ly)
  end
  if not left and not right then
    left = axis_less_than(lx)
    right = axis_greater_than(lx)
  end

  if up and down then
    up = false
    down = false
  end
  if left and right then
    left = false
    right = false
  end

  local mask = 0
  if up then
    mask = mask + retrogo_mask.BTN_UP
  end
  if down then
    mask = mask + retrogo_mask.BTN_DOWN
  end
  if left then
    mask = mask + retrogo_mask.BTN_LEFT
  end
  if right then
    mask = mask + retrogo_mask.BTN_RIGHT
  end

  local raw_select_pressed = has_bit(raw_mask, gamepad and gamepad.BTN_SELECT or 0)
  local raw_start_pressed = has_bit(raw_mask, gamepad and gamepad.BTN_START or 0)
  local raw_menu_pressed = has_bit(raw_mask, gamepad and gamepad.BTN_MENU or 0)
  local xbox_menu_pressed = any_true(state, { "menu" })
  local xbox_view_pressed = any_true(state, { "view" })
  local start_alias_pressed = xbox_menu_pressed or any_true(state, { "start", "btn_start" })
  local menu_alias_pressed = xbox_view_pressed or any_true(state, { "menu_btn", "option", "btn_menu" })

  local a_pressed = has_bit(raw_mask, gamepad and gamepad.BTN_A or 0) or
      any_true(state, { "a", "btn_a", "button_a" })
  local b_pressed = has_bit(raw_mask, gamepad and gamepad.BTN_B or 0) or
      any_true(state, { "b", "btn_b", "button_b" })
  local select_pressed = (raw_select_pressed and not xbox_view_pressed) or
      any_true(state, { "select", "btn_select", "back" })
  local start_pressed = raw_start_pressed or start_alias_pressed
  local x_pressed = has_bit(raw_mask, gamepad and gamepad.BTN_X or 0) or
      any_true(state, { "x", "btn_x", "button_x" })
  local y_pressed = has_bit(raw_mask, gamepad and gamepad.BTN_Y or 0) or
      any_true(state, { "y", "btn_y", "button_y" })
  local l_pressed = has_bit(raw_mask, gamepad and gamepad.BTN_L or 0) or
      any_true(state, { "l", "lb", "l1", "left_shoulder", "btn_l", "button_l", "button_lb" })
  local r_pressed = has_bit(raw_mask, gamepad and gamepad.BTN_R or 0) or
      any_true(state, { "r", "rb", "r1", "right_shoulder", "btn_r", "btn_rb", "btn_r1", "button_r", "button_rb", "button_r1" })
  local rt_pressed = has_bit(raw_mask, gamepad and (gamepad.BTN_RT or gamepad.BTN_R2 or gamepad.BTN_ZR or 0) or 0) or
      any_true(state, { "rt", "r2", "right_trigger", "trigger_right", "right_trigger_pressed", "btn_rt", "btn_r2", "button_rt", "button_r2" })
  local home_pressed = has_bit(raw_mask, gamepad and gamepad.BTN_HOME or 0) or
      any_true(state, { "xbox", "home", "btn_home", "guide", "system" })
  local menu_pressed = menu_alias_pressed or (raw_menu_pressed and not start_pressed)

  if a_pressed then
    mask = mask + retrogo_mask.BTN_A
  end
  if b_pressed then
    mask = mask + retrogo_mask.BTN_B
  end
  if select_pressed then
    mask = mask + retrogo_mask.BTN_SELECT
  end
  if start_pressed then
    mask = mask + retrogo_mask.BTN_START
  end
  if x_pressed or (APP.MODULE_ID == "md" and (r_pressed or rt_pressed)) then
    mask = mask + retrogo_mask.BTN_X
  end
  if y_pressed then
    mask = mask + retrogo_mask.BTN_Y
  end
  if l_pressed then
    mask = mask + retrogo_mask.BTN_L
  end
  if r_pressed then
    mask = mask + retrogo_mask.BTN_R
  end
  if home_pressed then
    mask = mask + retrogo_mask.BTN_HOME
  end
  if menu_pressed then
    mask = mask + retrogo_mask.BTN_MENU
  end

  return mask
end

local function now_ms()
  if type(millis) == "function" then
    return tonumber(millis()) or 0
  end
  if tmr and type(tmr.now) == "function" then
    return math.floor((tonumber(tmr.now()) or 0) / 1000)
  end
  if os and type(os.clock) == "function" then
    return math.floor(os.clock() * 1000)
  end
  return 0
end

local function sleep_ms(ms)
  ms = tonumber(ms) or 0
  if ms < 1 then
    ms = 1
  end
  if sleep then
    sleep(ms)
    return true
  end
  if tmr and tmr.delay then
    tmr.delay(ms * 1000)
    return true
  end
  if task and task.delay then
    task.delay(ms)
    return true
  end
  return false
end

local function text_or(value, fallback)
  if value == nil then
    return fallback or ""
  end
  local text = tostring(value)
  if text == "" then
    return fallback or ""
  end
  return text
end

local function split_path(path)
  local parts = {}
  for part in text_or(path, ""):gmatch("[^/]+") do
    parts[#parts + 1] = part
  end
  return parts
end

local function url_decode(text)
  text = text_or(text, "")
  text = text:gsub("+", " ")
  text = text:gsub("%%(%x%x)", function(hex)
    return string.char(tonumber(hex, 16))
  end)
  return text
end

local function parse_query(query)
  local out = {}
  for pair in text_or(query, ""):gmatch("([^&]+)") do
    local key, value = pair:match("^([^=]*)=(.*)$")
    if not key then
      key = pair
      value = ""
    end
    out[url_decode(key)] = url_decode(value)
  end
  return out
end

local function json_escape(text)
  text = text_or(text, "")
  text = text:gsub("\\", "\\\\")
  text = text:gsub("\"", "\\\"")
  text = text:gsub("\n", "\\n")
  text = text:gsub("\r", "\\r")
  text = text:gsub("\t", "\\t")
  return "\"" .. text .. "\""
end

local function table_is_array(value)
  local max = 0
  local count = 0
  for key, _ in pairs(value) do
    if type(key) ~= "number" or key < 1 or math.floor(key) ~= key then
      return false
    end
    if key > max then
      max = key
    end
    count = count + 1
  end
  return max == count
end

local function encode_json(value)
  if json and json.encode then
    local ok, body = pcall(function()
      return json.encode(value)
    end)
    if ok and body then
      return body
    end
  end
  if sjson and sjson.encode then
    local ok, body = pcall(function()
      return sjson.encode(value)
    end)
    if ok and body then
      return body
    end
  end
  local kind = type(value)
  if kind == "nil" then
    return "null"
  end
  if kind == "boolean" then
    return value and "true" or "false"
  end
  if kind == "number" then
    return tostring(value)
  end
  if kind == "string" then
    return json_escape(value)
  end
  if kind ~= "table" then
    return json_escape(tostring(value))
  end
  local out = {}
  if table_is_array(value) then
    for i = 1, #value do
      out[#out + 1] = encode_json(value[i])
    end
    return "[" .. table.concat(out, ",") .. "]"
  end
  for key, item in pairs(value) do
    out[#out + 1] = json_escape(key) .. ":" .. encode_json(item)
  end
  return "{" .. table.concat(out, ",") .. "}"
end

local function json_response(status, value)
  return {
    status = status or "200 OK",
    type = "application/json; charset=utf-8",
    headers = {
      ["cache-control"] = "no-store",
      ["connection"] = "close",
    },
    body = encode_json(value),
  }
end

local function text_response(status, content_type, body, headers)
  headers = type(headers) == "table" and headers or {}
  headers["cache-control"] = headers["cache-control"] or "no-store"
  headers["connection"] = headers["connection"] or "close"
  return {
    status = status or "200 OK",
    type = content_type or "text/plain; charset=utf-8",
    headers = headers,
    body = text_or(body, ""),
  }
end

local function error_response(status, message)
  return json_response(status or "400 Bad Request", {
    ok = false,
    error = text_or(message, "request failed"),
  })
end

local function normalize_absolute_path(path)
  path = text_or(path, ""):gsub("\\", "/")
  if path == "" then
    return nil, "empty path"
  end
  if path:sub(1, 1) ~= "/" then
    path = "/" .. path
  end
  local parts = {}
  for _, part in ipairs(split_path(path)) do
    if part == "" or part == "." then
      -- skip
    elseif part == ".." then
      if #parts == 0 then
        return nil, "path out of range"
      end
      table.remove(parts)
    else
      parts[#parts + 1] = part
    end
  end
  return "/" .. table.concat(parts, "/")
end

local function path_is_under(path, root)
  return path == root or path:sub(1, #root + 1) == (root .. "/")
end

local function normalize_rom_path(path)
  path = text_or(path, "")
  if path == "" then
    return APP.ROM_ROOT
  end
  path = path:gsub("\\", "/")
  if path:sub(1, 1) ~= "/" then
    path = APP.ROM_ROOT .. "/" .. path
  end
  local normalized, err = normalize_absolute_path(path)
  if not normalized then
    return nil, err
  end
  if not path_is_under(normalized, APP.ROM_ROOT) then
    return nil, "path must stay under " .. APP.ROM_ROOT
  end
  return normalized
end

local function basename(path)
  local normalized = normalize_absolute_path(path or "") or text_or(path, "")
  if normalized == "/" then
    return ""
  end
  local parts = split_path(normalized)
  return parts[#parts] or ""
end

local function dirname(path)
  local normalized = normalize_absolute_path(path or "")
  if not normalized or normalized == "/" then
    return "/"
  end
  local parts = split_path(normalized)
  table.remove(parts)
  if #parts == 0 then
    return "/"
  end
  return "/" .. table.concat(parts, "/")
end

local function stat_is_dir(st)
  return st and (st.is_dir or st.dir or st.directory or st.type == "dir") and true or false
end

local function ensure_dir(path)
  if not file or not file.mkdir then
    return false, "file API unavailable"
  end
  local normalized, err = normalize_absolute_path(path)
  if not normalized then
    return false, err
  end
  local current = ""
  for _, part in ipairs(split_path(normalized)) do
    current = current .. "/" .. part
    local st = file.stat and file.stat(current) or nil
    if st then
      if not stat_is_dir(st) then
        return false, "not a directory: " .. current
      end
    else
      local ok = file.mkdir(current)
      if not ok then
        st = file.stat and file.stat(current) or nil
        if not stat_is_dir(st) then
          return false, "mkdir failed: " .. current
        end
      end
    end
  end
  return true
end

local function rom_item_path(parent, item)
  if type(item) == "table" then
    return item.path or item.fullpath or item.full_path or (parent .. "/" .. tostring(item.name or item[1] or ""))
  end
  return parent .. "/" .. tostring(item)
end

local function rom_item_is_dir(path, item)
  if type(item) == "table" then
    if item.is_dir ~= nil then return item.is_dir end
    if item.dir ~= nil then return item.dir end
    if item.directory ~= nil then return item.directory end
    if item.type == "dir" or item.category == "dir" then return true end
  end
  local st = file and file.stat and file.stat(path) or nil
  return st and (st.is_dir or st.dir or st.directory) or false
end

local function rom_item_size(path, item)
  if type(item) == "table" then
    return item.size or item.file_size or 0
  end
  local st = file and file.stat and file.stat(path) or nil
  return st and (st.size or st.file_size or 0) or 0
end

local function rom_listdir(path)
  if file and file.listdir then
    return file.listdir(path) or {}
  end
  if sd and sd.listdir then
    return sd.listdir(path) or {}
  end
  return {}
end

local function scan_rom_list(path, seen)
  if seen[path] then
    return
  end
  seen[path] = true
  for _, item in ipairs(rom_listdir(path)) do
    local child = rom_item_path(path, item)
    if child ~= path and child ~= "" then
      if rom_item_is_dir(child, item) then
        scan_rom_list(child, seen)
      else
        APP.rom_list_cache[#APP.rom_list_cache + 1] = {
          name = basename(child),
          path = child,
          size = tonumber(rom_item_size(child, item)) or 0,
        }
      end
    end
  end
end

local function refresh_rom_list_cache()
  ensure_dir(APP.ROM_ROOT)
  APP.rom_list_cache = {}
  APP.rom_list_cache_ready = false
  scan_rom_list(APP.ROM_ROOT, {})
  APP.rom_list_cache_ready = true
  log("rom list cache", #APP.rom_list_cache, APP.ROM_ROOT)
  return true
end

local function list_rom_files()
  local items = {}
  if not APP.rom_list_cache_ready then
    refresh_rom_list_cache()
  end
  if not APP.rom_list_cache_ready then
    return items
  end
  for _, item in ipairs(APP.rom_list_cache or {}) do
    items[#items + 1] = {
      name = item.name,
      path = item.path,
      size = item.size or 0,
    }
  end
  table.sort(items, function(a, b)
    return text_or(a.name, ""):lower() < text_or(b.name, ""):lower()
  end)
  return items
end

local function write_request_body_to_file(req, fd, base_offset, total_size)
  local written = 0
  while true do
    local chunk = req.getbody()
    if not chunk then
      break
    end
    local n = #chunk
    if n > 0 then
      if (base_offset + written + n) > total_size then
        return nil, "request body exceeds total size"
      end
      local ok = fd:write(chunk)
      if not ok then
        return nil, "file write failed"
      end
      written = written + n
    end
  end
  if fd.flush then
    fd:flush()
  end
  return written
end

local function prepare_upload_file(path, offset, total)
  if total < 0 then
    return nil, "invalid total size"
  end
  if total > APP.MAX_ROM_FILE_SIZE then
    return nil, "file too large"
  end
  if offset < 0 or offset > total then
    return nil, "invalid offset"
  end
  local ok_parent, parent_err = ensure_dir(dirname(path))
  if not ok_parent then
    return nil, parent_err
  end
  local st = file.stat and file.stat(path) or nil
  if stat_is_dir(st) then
    return nil, "target path is directory"
  end
  if offset == 0 then
    return file.open(path, "w+")
  end
  if not st then
    return nil, "resume target missing"
  end
  if offset > (st.size or st.file_size or 0) then
    return nil, "offset beyond file size"
  end
  local fd = file.open(path, "r+")
  if not fd then
    return nil, "open for update failed"
  end
  local pos = fd:seek("set", offset)
  if not pos then
    fd:close()
    return nil, "seek failed"
  end
  return fd
end

APP.HTML = [==[
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Retro-Go ROM Upload</title>
<style>
:root{--bg:#f8fafc;--surface:#ffffff;--line:#d8e0ea;--text:#111827;--muted:#5b6472;--primary:#2563eb;--primary-dark:#1d4ed8;--ok:#15803d;--error:#b91c1c}
*{box-sizing:border-box}
html,body{min-height:100%}
body{margin:0;background:var(--bg);color:var(--text);font:16px/1.5 system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}
main{width:min(720px,100%);margin:0 auto;padding:32px 16px 48px}
header{display:flex;align-items:flex-end;justify-content:space-between;gap:16px;margin-bottom:20px}
.eyebrow{margin:0 0 4px;color:var(--muted);font-size:13px;font-weight:700;text-transform:uppercase}
h1{margin:0;font-size:30px;line-height:1.15}
.path{color:var(--muted);font-size:14px;word-break:break-all}
.card{background:var(--surface);border:1px solid var(--line);border-radius:8px;padding:20px;box-shadow:0 12px 28px rgba(15,23,42,.08)}
.drop{display:flex;align-items:center;justify-content:center;min-height:136px;margin:16px 0;border:1px dashed #9aa8bb;border-radius:8px;background:#fdfefe;text-align:center;transition:border-color .18s ease,background-color .18s ease}
.drop.drag{border-color:var(--primary);background:#eff6ff}
.drop strong{display:block;margin-bottom:4px}
.drop span{color:var(--muted);font-size:14px}
label{display:block;margin:0 0 8px;font-weight:700}
input[type=file]{display:block;width:100%;min-height:48px;padding:10px;border:1px solid var(--line);border-radius:8px;background:#fff;color:var(--text)}
.actions{display:flex;align-items:center;gap:12px;margin-top:16px}
button{min-height:48px;padding:0 18px;border:0;border-radius:8px;background:var(--primary);color:#fff;font:700 16px/1 system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;cursor:pointer}
button:hover{background:var(--primary-dark)}
button:focus-visible,input:focus-visible{outline:3px solid rgba(37,99,235,.32);outline-offset:2px}
button:disabled{opacity:.5;cursor:not-allowed}
.status{min-height:24px;color:var(--muted);font-size:14px}
.status.ok{color:var(--ok)}
.status.err{color:var(--error)}
.bar{height:10px;margin-top:16px;overflow:hidden;border-radius:999px;background:#e6edf5}
.bar span{display:block;width:0;height:100%;background:var(--primary);transition:width .18s ease}
.list{margin-top:18px}
.list h2{margin:0 0 10px;font-size:18px}
.file{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:10px 0;border-top:1px solid var(--line)}
.file:first-of-type{border-top:0}
.file-name{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.file-size{color:var(--muted);font-variant-numeric:tabular-nums}
.empty{color:var(--muted);padding:8px 0}
@media (max-width:520px){main{padding-top:24px}header{display:block}h1{font-size:26px}.card{padding:16px}.actions{display:block}button{width:100%;margin-top:12px}.status{margin-top:10px}}
</style>
</head>
<body>
<main>
  <header>
    <div>
      <p class="eyebrow">Retro-Go</p>
      <h1>ROM Upload</h1>
      <div class="path" id="romPath">__ROM_ROOT__</div>
    </div>
  </header>
  <section class="card" aria-label="ROM upload">
    <label for="fileInput">ROM files</label>
    <input id="fileInput" type="file" multiple>
    <div class="drop" id="dropzone">
      <div><strong>Drop files here</strong><span>NES, GB, GBC, SMS, PCE, MD and other supported ROMs</span></div>
    </div>
    <div class="actions">
      <button id="uploadBtn" type="button">Upload</button>
      <div class="status" id="status" aria-live="polite">Ready</div>
    </div>
    <div class="bar" aria-hidden="true"><span id="bar"></span></div>
    <div class="list">
      <h2>ROMs</h2>
      <div id="romList" class="empty">Loading...</div>
    </div>
  </section>
</main>
<script>
const CONFIG_BASE="__APP_BASE__";
const pathBase=location.pathname.replace(/\/api\/.*$/,"").replace(/\/$/,"");
const BASE=pathBase||CONFIG_BASE;
let serverInfo={rom_root:"__ROM_ROOT__",chunk_size:65536,max_file_size:33554432};
const qs=id=>document.getElementById(id);
const statusEl=qs("status");
const bar=qs("bar");
const input=qs("fileInput");
const dropzone=qs("dropzone");
const uploadBtn=qs("uploadBtn");
function api(path){return BASE+"/api"+path}
function esc(text){return String(text||"").replace(/[&<>"']/g,ch=>({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;","'":"&#39;"}[ch]))}
function fmtSize(bytes){bytes=Number(bytes)||0;if(bytes<1024)return bytes+" B";if(bytes<1048576)return (bytes/1024).toFixed(1)+" KB";return (bytes/1048576).toFixed(1)+" MB"}
function setStatus(text,kind){statusEl.textContent=text;statusEl.className="status "+(kind||"")}
function setProgress(done,total){const pct=total>0?Math.max(0,Math.min(100,Math.round(done*100/total))):0;bar.style.width=pct+"%"}
async function parseJson(res){const text=await res.text();let data={};try{data=text?JSON.parse(text):{}}catch(_){throw new Error(text||res.statusText)}if(!res.ok||data.ok===false)throw new Error(data.error||res.statusText);return data}
async function loadInfo(){const data=await parseJson(await fetch(api("/info"),{cache:"no-store"}));serverInfo=data;qs("romPath").textContent=data.rom_root||serverInfo.rom_root}
async function loadList(){const data=await parseJson(await fetch(api("/list"),{cache:"no-store"}));const items=data.items||[];const target=qs("romList");if(!items.length){target.className="empty";target.textContent=data.pending?"Scanning ROMs...":"No ROMs yet";if(data.pending)setTimeout(()=>loadList().catch(()=>{}),1000);return}target.className="";target.innerHTML=items.map(item=>`<div class="file"><div class="file-name" title="${esc(item.name)}">${esc(item.name)}</div><div class="file-size">${fmtSize(item.size)}</div></div>`).join("")}
function selectedFiles(files){return Array.from(files||input.files||[]).filter(file=>file&&file.name)}
async function uploadOne(file,index,totalFiles){if(file.size>serverInfo.max_file_size)throw new Error(file.name+" is too large");let offset=0;const chunkSize=serverInfo.chunk_size||65536;const safeName=file.name.replace(/[\\/]/g,"_");const path=(serverInfo.rom_root||"__ROM_ROOT__")+"/"+safeName;while(offset<file.size||file.size===0){const end=file.size===0?0:Math.min(offset+chunkSize,file.size);setStatus(`Uploading ${index}/${totalFiles}: ${file.name}`,"");const res=await fetch(api("/upload")+"?path="+encodeURIComponent(path)+"&offset="+offset+"&total="+file.size,{method:"PUT",body:file.size===0?new Blob([]):file.slice(offset,end)});const data=await parseJson(res);const next=data.next_offset||end;if(file.size>0&&next<=offset)throw new Error("Upload did not advance");offset=next;setProgress(offset,file.size);if(file.size===0||data.done)break}}
async function uploadFiles(files){files=selectedFiles(files);if(!files.length){setStatus("Select files first","err");return}uploadBtn.disabled=true;setProgress(0,1);try{for(let i=0;i<files.length;i++){await uploadOne(files[i],i+1,files.length)}setStatus("Upload complete","ok");setProgress(1,1);input.value="";await loadList()}catch(err){setStatus(err.message||"Upload failed","err")}finally{uploadBtn.disabled=false}}
uploadBtn.onclick=()=>uploadFiles();
["dragenter","dragover"].forEach(name=>dropzone.addEventListener(name,ev=>{ev.preventDefault();dropzone.classList.add("drag")}));
["dragleave","drop"].forEach(name=>dropzone.addEventListener(name,ev=>{ev.preventDefault();dropzone.classList.remove("drag")}));
dropzone.addEventListener("drop",ev=>uploadFiles(ev.dataTransfer&&ev.dataTransfer.files));
loadInfo().then(loadList).catch(err=>setStatus(err.message||"WebUI failed","err"));
</script>
</body>
</html>
]==]

local function request_route_base(req)
  local uri = text_or(req and req.uri, "")
  uri = uri:match("^([^?]*)") or uri
  if uri == "" then
    return APP.ROUTE_BASE
  end
  while #uri > 1 and uri:sub(-1) == "/" do
    uri = uri:sub(1, -2)
  end
  local api_pos = uri:find("/api/", 1, true)
  if api_pos then
    uri = uri:sub(1, api_pos - 1)
  end
  if uri == "" then
    return APP.ROUTE_BASE
  end
  return uri
end

function APP.render_upload_html(route_base)
  local html = APP.HTML:gsub("__APP_BASE__", function() return route_base or APP.ROUTE_BASE end)
  html = html:gsub("__ROM_ROOT__", function() return APP.ROM_ROOT end)
  return html
end

function APP.route_redirect(req)
  local base = request_route_base(req)
  return text_response("302 Found", "text/plain; charset=utf-8", "", {
    ["location"] = base .. "/",
  })
end

function APP.route_index(req)
  return text_response("200 OK", "text/html; charset=utf-8", APP.render_upload_html(request_route_base(req)))
end

function APP.route_favicon()
  return text_response("204 No Content", "image/x-icon", "")
end

function APP.api_info()
  return json_response("200 OK", {
    ok = true,
    version = APP.VERSION,
    route_base = APP.ROUTE_BASE,
    rom_root = APP.ROM_ROOT,
    chunk_size = APP.CHUNK_SIZE,
    max_file_size = APP.MAX_ROM_FILE_SIZE,
    rom_list_ready = APP.rom_list_cache_ready and true or false,
  })
end

function APP.api_list()
  local items = list_rom_files()
  return json_response("200 OK", {
    ok = true,
    rom_root = APP.ROM_ROOT,
    pending = not APP.rom_list_cache_ready,
    items = items,
  })
end

function APP.api_upload(req)
  local q = parse_query(req.query)
  local path, err = normalize_rom_path(q.path or q.name or "")
  if not path or path == APP.ROM_ROOT then
    return error_response("400 Bad Request", err or "missing file path")
  end
  local offset = to_number(q.offset) or 0
  local total = to_number(q.total) or -1
  local fd, open_err = prepare_upload_file(path, offset, total)
  if not fd then
    return error_response(open_err == "file too large" and "413 Payload Too Large" or "400 Bad Request", open_err or "open failed")
  end
  local written, write_err = write_request_body_to_file(req, fd, offset, total)
  fd:close()
  if not written then
    return error_response("400 Bad Request", write_err or "write failed")
  end
  if total > 0 and written == 0 and offset < total then
    return error_response("400 Bad Request", "empty upload chunk")
  end
  local next_offset = offset + written
  local st = file.stat and file.stat(path) or nil
  if not st or stat_is_dir(st) then
    return error_response("500 Internal Server Error", "write result invalid")
  end
  if next_offset >= total then
    APP.rom_list_cache_ready = false
  end
  return json_response("200 OK", {
    ok = true,
    path = path,
    name = basename(path),
    next_offset = next_offset,
    total = total,
    done = next_offset >= total,
    size = st.size or st.file_size or 0,
  })
end

function APP.register_route(method, route, handler)
  local err = httpd.dynamic(method, route, handler)
  if err then
    log("httpd route failed", route, tostring(err))
    return false
  end
  APP.routes[#APP.routes + 1] = { method = method, route = route }
  return true
end

function APP.register_web_routes()
  local get = httpd.GET or "GET"
  local put = httpd.PUT or "PUT"
  APP.register_route(get, APP.ROUTE_BASE, APP.route_redirect)
  APP.register_route(get, APP.ROUTE_BASE .. "/", APP.route_index)
  APP.register_route(get, APP.ROUTE_BASE .. "/favicon.ico", APP.route_favicon)
  APP.register_route(get, APP.API_PREFIX .. "/info", APP.api_info)
  APP.register_route(get, APP.API_PREFIX .. "/list", APP.api_list)
  APP.register_route(put, APP.API_PREFIX .. "/upload", APP.api_upload)
end

function APP.unregister_routes()
  if not httpd or not httpd.unregister then
    APP.routes = {}
    return
  end
  for i = #APP.routes, 1, -1 do
    local item = APP.routes[i]
    pcall(function()
      httpd.unregister(item.method, item.route)
    end)
  end
  APP.routes = {}
end

function APP.stop_web(reason)
  APP.unregister_routes()
  if app and app.set_webui then
    pcall(function()
      app.set_webui(false)
    end)
  end
  APP.web_ready = false
  log("web stopped", reason or "")
end

local function start_rom_web()
  if not httpd or not httpd.dynamic then
    log("httpd unavailable, ROM upload WebUI disabled")
    return false
  end
  local previous = rawget(_G, "HOLO_RETRO_GO_WEB")
  if previous and previous.stop then
    pcall(function()
      previous.stop("reload")
    end)
  end
  ensure_dir(APP.ROM_ROOT)
  if httpd.start then
    pcall(function()
      httpd.start({
        webroot = "/sd",
        auto_index = httpd.INDEX_NONE,
        max_handlers = 64,
      })
    end)
  end
  APP.register_web_routes()
  _G.HOLO_RETRO_GO_WEB = {
    stop = function(reason)
      APP.stop_web(reason)
    end,
  }
  if app and app.set_webui then
    pcall(function()
      app.set_webui(true)
    end)
  end
  APP.web_ready = true
  log("web ready", APP.ROUTE_BASE .. "/", APP.ROM_ROOT)
  return true
end

local function lv_style(obj, props)
  if not obj or type(props) ~= "table" then
    return
  end
  local part = rawget(_G, "LV_PART_MAIN") or 0
  local state = rawget(_G, "LV_STATE_DEFAULT") or 0
  local selector = part + state
  if props.bg ~= nil and lv_obj_set_style_bg_color then
    pcall(lv_obj_set_style_bg_color, obj, props.bg, selector)
  end
  if props.bg_opa ~= nil and lv_obj_set_style_bg_opa then
    pcall(lv_obj_set_style_bg_opa, obj, props.bg_opa, selector)
  end
  if props.border ~= nil and lv_obj_set_style_border_color then
    pcall(lv_obj_set_style_border_color, obj, props.border, selector)
  end
  if props.border_w ~= nil and lv_obj_set_style_border_width then
    pcall(lv_obj_set_style_border_width, obj, props.border_w, selector)
  end
  if props.radius ~= nil and lv_obj_set_style_radius then
    pcall(lv_obj_set_style_radius, obj, props.radius, selector)
  end
  if props.pad ~= nil and lv_obj_set_style_pad_all then
    pcall(lv_obj_set_style_pad_all, obj, props.pad, selector)
  end
  if props.color ~= nil and lv_obj_set_style_text_color then
    pcall(lv_obj_set_style_text_color, obj, props.color, selector)
  end
  if props.font and lv_obj_set_style_text_font then
    local font = rawget(_G, props.font)
    if font then
      pcall(lv_obj_set_style_text_font, obj, font, selector)
    end
  end
  if props.align ~= nil and lv_obj_set_style_text_align then
    pcall(lv_obj_set_style_text_align, obj, props.align, selector)
  end
end

local function lv_disable_scroll(obj)
  local flag = rawget(_G, "LV_OBJ_FLAG_SCROLLABLE")
  if obj and flag and lv_obj_clear_flag then
    pcall(lv_obj_clear_flag, obj, flag)
  end
end

local function lv_label(parent, text, x, y, w, h, color, font, align)
  if not lv_label_create then
    return nil
  end
  local label = lv_label_create(parent)
  if lv_obj_set_pos then pcall(lv_obj_set_pos, label, x, y) end
  if lv_obj_set_size then pcall(lv_obj_set_size, label, w, h) end
  if lv_label_set_text then pcall(lv_label_set_text, label, text or "") end
  if lv_label_set_long_mode then
    pcall(lv_label_set_long_mode, label, rawget(_G, "LV_LABEL_LONG_CLIP") or 0)
  end
  lv_style(label, {
    color = color or 0xFFFFFF,
    font = font,
    align = align or rawget(_G, "LV_TEXT_ALIGN_CENTER") or 1,
  })
  return label
end

local function relative_path_under(root, path)
  local normalized_root = normalize_absolute_path(root or "") or text_or(root, "")
  local normalized_path = normalize_absolute_path(path or "") or text_or(path, "")
  if normalized_path == normalized_root then
    return ""
  end
  local prefix = normalized_root .. "/"
  if normalized_path:sub(1, #prefix) == prefix then
    return normalized_path:sub(#prefix + 1)
  end
  return basename(normalized_path)
end

local function collect_rom_files(root, path, out, seen)
  local normalized = normalize_absolute_path(path or "") or path
  if not normalized or normalized == "" or seen[normalized] then
    return
  end
  seen[normalized] = true
  for _, item in ipairs(rom_listdir(normalized)) do
    local child = normalize_absolute_path(rom_item_path(normalized, item)) or rom_item_path(normalized, item)
    if child ~= normalized and child ~= "" then
      if rom_item_is_dir(child, item) then
        collect_rom_files(root, child, out, seen)
      else
        local rel = relative_path_under(root, child)
        if rel ~= "" then
          out[#out + 1] = {
            src = child,
            rel = rel,
            size = tonumber(rom_item_size(child, item)) or 0,
          }
        end
      end
    end
  end
end

local function format_mb(bytes)
  return string.format("%.2f MB", (tonumber(bytes) or 0) / (1024 * 1024))
end

local function make_rom_sync_ui(total_files, total_bytes)
  if not lv_scr_act or not lv_obj_create or not lv_label_create then
    return nil
  end
  local root = lv_scr_act()
  local overlay = lv_obj_create(root)
  if lv_obj_set_pos then pcall(lv_obj_set_pos, overlay, 0, 0) end
  if lv_obj_set_size then pcall(lv_obj_set_size, overlay, 320, 240) end
  lv_disable_scroll(overlay)
  lv_style(overlay, { bg = 0x000000, bg_opa = 180, border_w = 0, radius = 0, pad = 0 })

  local panel = lv_obj_create(overlay)
  if lv_obj_set_pos then pcall(lv_obj_set_pos, panel, 30, 54) end
  if lv_obj_set_size then pcall(lv_obj_set_size, panel, 260, 132) end
  lv_disable_scroll(panel)
  lv_style(panel, { bg = 0x202428, bg_opa = 255, border = 0x3A4652, border_w = 1, radius = 8, pad = 0 })

  local title = lv_label(panel, "ROM Syncing", 0, 14, 260, 22, 0xFFFFFF, "LV_FONT_MONTSERRAT_16")
  local detail = lv_label(panel, string.format("0 / %d files", total_files), 18, 42, 224, 18, 0xB0B0B0, "LV_FONT_MONTSERRAT_12")
  local bytes = lv_label(panel, "Moved 0.00 MB / Total " .. format_mb(total_bytes), 18, 88, 224, 18, 0xD6D6D6, "LV_FONT_MONTSERRAT_12")
  local result = lv_label(panel, "", 18, 108, 224, 16, 0x7F8790, "LV_FONT_MONTSERRAT_12")
  local bar = nil
  if lv_bar_create and lv_bar_set_range and lv_bar_set_value then
    bar = lv_bar_create(panel)
    if lv_obj_set_pos then pcall(lv_obj_set_pos, bar, 18, 68) end
    if lv_obj_set_size then pcall(lv_obj_set_size, bar, 224, 12) end
    pcall(lv_bar_set_range, bar, 0, 1000)
    pcall(lv_bar_set_value, bar, 0, rawget(_G, "LV_ANIM_OFF") or 0)
  end
  return {
    root = overlay,
    panel = panel,
    title = title,
    detail = detail,
    bytes = bytes,
    result = result,
    bar = bar,
  }
end

local function update_rom_sync_ui(ui, done_files, total_files, processed_bytes, moved_bytes, total_bytes, message, summary)
  if not ui then
    return
  end
  if ui.detail and lv_label_set_text then
    pcall(lv_label_set_text, ui.detail, string.format("%d / %d files  %s", done_files, total_files, message or ""))
  end
  if ui.bytes and lv_label_set_text then
    pcall(lv_label_set_text, ui.bytes, string.format("Moved %s / Total %s", format_mb(moved_bytes), format_mb(total_bytes)))
  end
  if ui.result and summary and lv_label_set_text then
    pcall(lv_label_set_text, ui.result, summary)
  end
  if ui.bar and lv_bar_set_value then
    local progress = 0
    if (tonumber(total_bytes) or 0) > 0 then
      progress = math.floor(((tonumber(processed_bytes) or 0) * 1000) / total_bytes)
    elseif (tonumber(total_files) or 0) > 0 then
      progress = math.floor(((tonumber(done_files) or 0) * 1000) / total_files)
    end
    if progress < 0 then progress = 0 end
    if progress > 1000 then progress = 1000 end
    pcall(lv_bar_set_value, ui.bar, progress, rawget(_G, "LV_ANIM_OFF") or 0)
  end
  sleep_ms(15)
end

local function close_rom_sync_ui(ui)
  if not ui then
    return
  end
  sleep_ms(120)
  if ui.root and lv_obj_del then
    pcall(lv_obj_del, ui.root)
  end
end

local function sync_legacy_roms()
  ensure_dir(APP.ROM_ROOT)
  if APP.LEGACY_ROM_ROOT == APP.ROM_ROOT or not file or not file.stat or not file.rename then
    return true
  end
  local legacy_st = file.stat(APP.LEGACY_ROM_ROOT)
  if not stat_is_dir(legacy_st) then
    return true
  end

  local files = {}
  collect_rom_files(APP.LEGACY_ROM_ROOT, APP.LEGACY_ROM_ROOT, files, {})
  if #files == 0 then
    return true
  end

  local moved = 0
  local skipped = 0
  local failed = 0
  local moved_bytes = 0
  local processed_bytes = 0
  local total_bytes = 0
  for _, item in ipairs(files) do
    total_bytes = total_bytes + (tonumber(item.size) or 0)
  end
  local ui = make_rom_sync_ui(#files, total_bytes)
  update_rom_sync_ui(ui, 0, #files, 0, 0, total_bytes, "Preparing", "")

  for i, item in ipairs(files) do
    local dst = APP.ROM_ROOT .. "/" .. item.rel
    local action = "Moving"
    local item_size = tonumber(item.size) or 0
    if file.stat(dst) then
      skipped = skipped + 1
      action = "Skipped"
      log("rom sync skip exists", item.src, dst)
    else
      ensure_dir(dirname(dst))
      if file.rename(item.src, dst) then
        moved = moved + 1
        moved_bytes = moved_bytes + item_size
        log("rom sync moved", item.src, dst)
      else
        failed = failed + 1
        action = "Failed"
        log("rom sync failed", item.src, dst)
      end
    end
    processed_bytes = processed_bytes + item_size
    update_rom_sync_ui(
      ui,
      i,
      #files,
      processed_bytes,
      moved_bytes,
      total_bytes,
      action .. " " .. basename(item.src),
      string.format("Moved %d  Skipped %d  Failed %d", moved, skipped, failed)
    )
  end

  APP.rom_list_cache_ready = false
  close_rom_sync_ui(ui)
  log("rom sync done", "moved", moved, "skipped", skipped, "failed", failed)
  return failed == 0
end

local function selector_gamepad_status(state, phase)
  local status_connected = 0x34D17A
  local status_connecting = 0x3C95FF
  local status_waiting = 0xF2A23A
  if phase == "connected" then
    return "Pad Connect", status_connected
  end
  if phase == "connecting" then
    return "BT CONNECTING", status_connecting
  end
  if phase == "waiting" then
    return "BT NOT CONNECTED", status_waiting
  end
  if not gamepad or not gamepad.state then
    return "BT NOT CONNECTED", status_waiting
  end
  if not state then
    return "BT NOT CONNECTED", status_waiting
  end
  if state.connecting ~= nil and to_bool(state.connecting) then
    return "BT CONNECTING", status_connecting
  end
  if state.connected ~= nil then
    if to_bool(state.connected) then
      return "Pad Connect", status_connected
    end
    return "BT NOT CONNECTED", status_waiting
  end
  if state.started ~= nil then
    if to_bool(state.started) then
      return "BT NOT CONNECTED", status_waiting
    end
    return "BT NOT CONNECTED", status_waiting
  end
  return "Pad Connect", status_connected
end

local function selector_update_gamepad_status(ui, state, phase)
  if not ui or not ui.gamepad_status or not lv_label_set_text then
    return
  end
  local text, color = selector_gamepad_status(state, phase)
  if text ~= ui.gamepad_status_text then
    pcall(lv_label_set_text, ui.gamepad_status, text)
    lv_style(ui.gamepad_status, { color = color })
    ui.gamepad_status_text = text
  end
end

local function selector_make_ui()
  if not lv_scr_act or not lv_obj_create or not lv_label_create then
    log("selector ui unavailable")
    return nil
  end

  local root = lv_scr_act()
  local overlay = lv_obj_create(root)
  if lv_obj_set_pos then pcall(lv_obj_set_pos, overlay, 0, 0) end
  if lv_obj_set_size then pcall(lv_obj_set_size, overlay, 320, 240) end
  lv_disable_scroll(overlay)
  lv_style(overlay, { bg = 0x000000, bg_opa = 255, border_w = 0, radius = 0, pad = 0 })

  local rows = {
    {
      title = lv_label(overlay, "", 70, 64, 180, 22, 0xFFFFFF, "LV_FONT_MONTSERRAT_16"),
      detail = lv_label(overlay, "", 70, 86, 180, 16, 0x9A9A9A, "LV_FONT_MONTSERRAT_12"),
    },
    {
      title = lv_label(overlay, "", 70, 114, 180, 22, 0xFFFFFF, "LV_FONT_MONTSERRAT_16"),
      detail = lv_label(overlay, "", 70, 136, 180, 16, 0x9A9A9A, "LV_FONT_MONTSERRAT_12"),
    },
  }

  local gamepad_status = lv_label(overlay, "", 0, 186, 320, 18, 0xF2A23A, "LV_FONT_MONTSERRAT_12")
  local message = lv_label(overlay, "", 0, 212, 320, 16, 0x484848, "LV_FONT_MONTSERRAT_12")

  return {
    root = overlay,
    rows = rows,
    gamepad_status = gamepad_status,
    message = message,
  }
end

local function selector_row_text(row, mod, selected)
  if not row then
    return
  end
  if lv_label_set_text then
    pcall(lv_label_set_text, row.title, mod.title)
    pcall(lv_label_set_text, row.detail, mod.detail)
  end
  if selected then
    lv_style(row.title, { color = 0xFFFFFF })
    lv_style(row.detail, { color = 0xD0D0D0 })
  else
    lv_style(row.title, { color = 0x5F5F5F })
    lv_style(row.detail, { color = 0x444444 })
  end
end

local function selector_render(ui, index, status_text)
  if not ui then
    return
  end
  selector_row_text(ui.rows[1], APP.MODULES[1], index == 1)
  selector_row_text(ui.rows[2], APP.MODULES[2], index == 2)
  if ui.message and lv_label_set_text then
    pcall(lv_label_set_text, ui.message, status_text or "")
  end
end

local function key_event_is_press(evt_type)
  if not key then
    return true
  end
  return evt_type == key.START or evt_type == key.SHORT or evt_type == key.LONG_START
end

local function selector_gamepad_nav(state)
  if not connected_state(state) then
    return {}
  end
  local raw_mask = read_raw_gamepad_mask(state)
  local lx = state.lx or state.analog_x or state.left_x
  local ly = state.ly or state.analog_y or state.left_y
  return {
    left = has_bit(raw_mask, gamepad and gamepad.BTN_LEFT or 0) or
        any_true(state, { "dpad_left", "left_pressed" }) or axis_less_than(lx),
    right = has_bit(raw_mask, gamepad and gamepad.BTN_RIGHT or 0) or
        any_true(state, { "dpad_right", "right_pressed" }) or axis_greater_than(lx),
    confirm = has_bit(raw_mask, gamepad and gamepad.BTN_A or 0) or
        has_bit(raw_mask, gamepad and gamepad.BTN_START or 0) or
        has_bit(raw_mask, gamepad and gamepad.BTN_MENU or 0) or
        any_true(state, { "a", "btn_a", "start", "btn_start", "menu" }),
    home = has_bit(raw_mask, gamepad and gamepad.BTN_HOME or 0) or
        any_true(state, { "xbox", "home", "btn_home", "guide", "system" }),
  }
end

local function edge(now, prev, name)
  return now[name] and not prev[name]
end

local function selector_cleanup(ui, key_codes)
  if key and key.off then
    for _, code in ipairs(key_codes or {}) do
      if code then
        pcall(function() key.off(code) end)
      end
    end
  end
  if ui and ui.root and lv_obj_del then
    pcall(lv_obj_del, ui.root)
  end
end

local function choose_module()
  local index = 1
  local chosen = false
  local canceled = false
  local ui = selector_make_ui()
  local bt_phase = nil
  local key_codes = {}

  local function move(delta)
    index = index + delta
    if index < 1 then index = #APP.MODULES end
    if index > #APP.MODULES then index = 1 end
    selector_render(ui, index)
    log("selector", APP.MODULES[index].id, APP.MODULES[index].path)
  end

  local function confirm()
    chosen = true
    selector_render(ui, index, "LOADING " .. APP.MODULES[index].title)
  end

  local function cancel()
    canceled = true
    selector_render(ui, index, "EXIT")
  end

  selector_render(ui, index)
  selector_update_gamepad_status(ui, read_gamepad_state())

  if gamepad and gamepad.start then
    pcall(function()
      if gamepad.off then gamepad.off() end
      gamepad.start({ clear_bonds = false, debug = false })
    end)
  end

  if gamepad and gamepad.on then
    local function set_bt_phase(phase)
      bt_phase = phase
      selector_update_gamepad_status(ui, read_gamepad_state(), bt_phase)
    end
    pcall(function()
      if gamepad.EVT_CONNECTING then
        gamepad.on(gamepad.EVT_CONNECTING, function()
          set_bt_phase("connecting")
        end)
      end
      if gamepad.EVT_CONNECT_FAILED then
        gamepad.on(gamepad.EVT_CONNECT_FAILED, function()
          set_bt_phase("waiting")
        end)
      end
      if gamepad.EVT_CONNECTED then
        gamepad.on(gamepad.EVT_CONNECTED, function()
          set_bt_phase("connected")
        end)
      end
      if gamepad.EVT_DISCONNECTED then
        gamepad.on(gamepad.EVT_DISCONNECTED, function()
          set_bt_phase("waiting")
        end)
      end
    end)
  end

  if key and key.on then
    local function bind(code, fn)
      if not code then return end
      key_codes[#key_codes + 1] = code
      pcall(function()
        key.on(code, function(evt_type)
          if key_event_is_press(evt_type) then
            fn()
          end
        end)
      end)
    end
    bind(key.LEFT, function() move(-1) end)
    bind(key.RIGHT, function() move(1) end)
    bind(key.UP, confirm)
    bind(key.DOWN, confirm)
    bind(key.HOME, cancel)
  end

  local prev = selector_gamepad_nav(read_gamepad_state())
  while not chosen and not canceled do
    if app and app.exiting and app.exiting() then
      canceled = true
      break
    end
    local state = read_gamepad_state()
    selector_update_gamepad_status(ui, state, bt_phase)
    local nav = selector_gamepad_nav(state)
    if edge(nav, prev, "left") then
      move(-1)
    elseif edge(nav, prev, "right") then
      move(1)
    elseif edge(nav, prev, "confirm") then
      confirm()
    elseif edge(nav, prev, "home") then
      cancel()
    end
    prev = nav
    if not sleep_ms(30) then
      break
    end
  end

  local cleanup_ui = ui
  ui = nil
  selector_cleanup(cleanup_ui, key_codes)

  if canceled then
    if app and app.exit then
      pcall(function() app.exit() end)
    end
    return nil
  end
  return APP.MODULES[index]
end

local function choose_module_async(on_selected)
  if not tmr or not tmr.create then
    log("tmr unavailable, selector uses blocking fallback")
    return false
  end

  local index = 1
  local finished = false
  local ui = selector_make_ui()
  local bt_phase = nil
  local key_codes = {}
  local selector_timer = nil
  local prev = selector_gamepad_nav(read_gamepad_state())

  local function stop_selector_timer()
    if not selector_timer then
      return
    end
    pcall(function()
      selector_timer:stop()
    end)
    pcall(function()
      selector_timer:unregister()
    end)
    selector_timer = nil
  end

  local function finish(selected, exit_app)
    if finished then
      return
    end
    finished = true
    stop_selector_timer()
    local cleanup_ui = ui
    ui = nil
    selector_cleanup(cleanup_ui, key_codes)
    if type(on_selected) == "function" then
      on_selected(selected)
    end
    if exit_app and app and app.exit then
      pcall(function() app.exit() end)
    end
  end

  local function move(delta)
    if finished then
      return
    end
    index = index + delta
    if index < 1 then index = #APP.MODULES end
    if index > #APP.MODULES then index = 1 end
    selector_render(ui, index)
    log("selector", APP.MODULES[index].id, APP.MODULES[index].path)
  end

  local function confirm()
    if finished then
      return
    end
    selector_render(ui, index, "LOADING " .. APP.MODULES[index].title)
    finish(APP.MODULES[index], false)
  end

  local function cancel(exit_app)
    if finished then
      return
    end
    selector_render(ui, index, "EXIT")
    finish(nil, exit_app)
  end

  selector_render(ui, index)
  selector_update_gamepad_status(ui, read_gamepad_state())
  log("selector ready", APP.MODULES[index].id, APP.MODULES[index].path)

  if gamepad and gamepad.start then
    pcall(function()
      if gamepad.off then gamepad.off() end
      gamepad.start({ clear_bonds = false, debug = false })
    end)
  end

  if gamepad and gamepad.on then
    local function set_bt_phase(phase)
      bt_phase = phase
      selector_update_gamepad_status(ui, read_gamepad_state(), bt_phase)
    end
    pcall(function()
      if gamepad.EVT_CONNECTING then
        gamepad.on(gamepad.EVT_CONNECTING, function()
          set_bt_phase("connecting")
        end)
      end
      if gamepad.EVT_CONNECT_FAILED then
        gamepad.on(gamepad.EVT_CONNECT_FAILED, function()
          set_bt_phase("waiting")
        end)
      end
      if gamepad.EVT_CONNECTED then
        gamepad.on(gamepad.EVT_CONNECTED, function()
          set_bt_phase("connected")
        end)
      end
      if gamepad.EVT_DISCONNECTED then
        gamepad.on(gamepad.EVT_DISCONNECTED, function()
          set_bt_phase("waiting")
        end)
      end
    end)
  end

  if key and key.on then
    local function bind(code, fn)
      if not code then return end
      key_codes[#key_codes + 1] = code
      pcall(function()
        key.on(code, function(evt_type)
          if key_event_is_press(evt_type) then
            fn()
          end
        end)
      end)
    end
    bind(key.LEFT, function() move(-1) end)
    bind(key.RIGHT, function() move(1) end)
    bind(key.UP, confirm)
    bind(key.DOWN, confirm)
    bind(key.HOME, function() cancel(true) end)
  end

  selector_timer = tmr.create()
  local ok, err = pcall(function()
    selector_timer:alarm(30, tmr.ALARM_AUTO or 1, function()
      if finished then
        return
      end
      if app and app.exiting and app.exiting() then
        cancel(false)
        return
      end
      local state = read_gamepad_state()
      selector_update_gamepad_status(ui, state, bt_phase)
      local nav = selector_gamepad_nav(state)
      if edge(nav, prev, "left") then
        move(-1)
      elseif edge(nav, prev, "right") then
        move(1)
      elseif edge(nav, prev, "confirm") then
        confirm()
      elseif edge(nav, prev, "home") then
        cancel(true)
      end
      prev = nav
    end)
  end)
  if not ok then
    log("selector timer failed", tostring(err))
    selector_cleanup(ui, key_codes)
    ui = nil
    selector_timer = nil
    return false
  end

  return true
end

local function module_by_id(id)
  if type(id) ~= "string" or id == "" then
    return nil
  end
  for _, mod in ipairs(APP.MODULES) do
    if mod.id == id then
      return mod
    end
  end
  return nil
end

local function select_module()
  if APP.AUTO_SELECT_MODULE then
    local selected = module_by_id(APP.DEFAULT_MODULE_ID) or APP.MODULES[1]
    if selected then
      log("auto selected", selected.id, selected.path)
    end
    return selected
  end
  return choose_module()
end

local function start_selected_module(selected_module)
if not selected_module then
  log("module selection canceled")
  APP.stop_web("module selection canceled")
  return
end

APP.MODULE_ID = selected_module.id
APP.MODULE_PATH = selected_module.path
log("selected", selected_module.id, APP.MODULE_PATH)

local ok_module, retrogo = pcall(require, APP.MODULE_PATH)
if not ok_module or type(retrogo) ~= "table" then
  log("require failed", tostring(retrogo))
  APP.stop_web("require failed")
  return
end

log("module", tostring(retrogo.VERSION), tostring(retrogo.RETRO_GO_CORE))

local audio_eq = APP.AUDIO_EQ and APP.AUDIO_EQ[APP.MODULE_ID]
if type(audio_eq) == "table" and type(retrogo.set_audio_eq) == "function" then
  local ok_eq, eq_result, eq_err = pcall(function()
    return retrogo.set_audio_eq(audio_eq)
  end)
  if ok_eq and eq_result then
    log("set_audio_eq ok", APP.MODULE_ID)
  else
    log("set_audio_eq failed", tostring(eq_err or eq_result))
  end
end

local retrogo_button_defs = {
  { "A", retrogo.BTN_A },
  { "B", retrogo.BTN_B },
  { "SELECT", retrogo.BTN_SELECT },
  { "START", retrogo.BTN_START },
  { "UP", retrogo.BTN_UP },
  { "DOWN", retrogo.BTN_DOWN },
  { "LEFT", retrogo.BTN_LEFT },
  { "RIGHT", retrogo.BTN_RIGHT },
  { "X", retrogo.BTN_X },
  { "Y", retrogo.BTN_Y },
  { "L", retrogo.BTN_L },
  { "R", retrogo.BTN_R },
  { "HOME", retrogo.BTN_HOME },
  { "MENU", retrogo.BTN_MENU },
}

local function simple_pad_text(mask)
  local defs
  if APP.MODULE_ID == "md" then
    defs = {
      { "up", retrogo.BTN_UP },
      { "down", retrogo.BTN_DOWN },
      { "left", retrogo.BTN_LEFT },
      { "right", retrogo.BTN_RIGHT },
      { "a", retrogo.BTN_A },
      { "b", retrogo.BTN_X },
      { "c", retrogo.BTN_B },
      { "y", retrogo.BTN_Y },
      { "l", retrogo.BTN_L },
      { "select", retrogo.BTN_SELECT },
      { "start", retrogo.BTN_START },
      { "menu", retrogo.BTN_MENU },
      { "home", retrogo.BTN_HOME },
    }
  else
    defs = {
      { "up", retrogo.BTN_UP },
      { "down", retrogo.BTN_DOWN },
      { "left", retrogo.BTN_LEFT },
      { "right", retrogo.BTN_RIGHT },
      { "a", retrogo.BTN_A },
      { "b", retrogo.BTN_B },
      { "x", retrogo.BTN_X },
      { "y", retrogo.BTN_Y },
      { "l", retrogo.BTN_L },
      { "r", retrogo.BTN_R },
      { "select", retrogo.BTN_SELECT },
      { "start", retrogo.BTN_START },
      { "menu", retrogo.BTN_MENU },
      { "home", retrogo.BTN_HOME },
    }
  end

  local names = {}
  for _, def in ipairs(defs) do
    if has_bit(mask, def[2]) then
      names[#names + 1] = def[1]
    end
  end
  if #names == 0 then
    return nil
  end
  return "pad " .. table.concat(names, " ")
end

local function log_pad_mask(mask)
  local text = simple_pad_text(mask)
  if text then
    print(text)
  end
end

local function gamepad_button_defs()
  if not gamepad then
    return {}
  end
  return {
    { "A", gamepad.BTN_A },
    { "B", gamepad.BTN_B },
    { "SELECT", gamepad.BTN_SELECT },
    { "START", gamepad.BTN_START },
    { "UP", gamepad.BTN_UP },
    { "DOWN", gamepad.BTN_DOWN },
    { "LEFT", gamepad.BTN_LEFT },
    { "RIGHT", gamepad.BTN_RIGHT },
    { "X", gamepad.BTN_X },
    { "Y", gamepad.BTN_Y },
    { "L", gamepad.BTN_L },
    { "R", gamepad.BTN_R },
    { "HOME", gamepad.BTN_HOME },
    { "MENU", gamepad.BTN_MENU },
  }
end

local function debug_input_aliases(state)
  if type(state) ~= "table" then
    return "-"
  end
  local aliases = {}
  local checks = {
    { "SELECT", { "select", "btn_select", "back" } },
    { "START", { "start", "btn_start", "menu" } },
    { "HOME", { "xbox", "home", "btn_home", "guide", "system" } },
    { "MENU", { "view", "menu_btn", "option", "btn_menu" } },
  }
  for _, check in ipairs(checks) do
    local name = first_true_name(state, check[2])
    if name then
      aliases[#aliases + 1] = check[1] .. ":" .. name
    end
  end
  if #aliases == 0 then
    return "-"
  end
  return table.concat(aliases, ",")
end

APP.stop_web("runtime")

local function retrogo_info()
  local ok, info = pcall(function()
    return retrogo.info()
  end)
  if ok and type(info) == "table" then
    return info
  end
  return nil
end

local function wait_retrogo_stopped(timeout_ms)
  local deadline = now_ms() + (timeout_ms or 1000)
  while now_ms() < deadline do
    local info = retrogo_info()
    if info and not info.running then
      return true
    end
    if not sleep_ms(20) then
      break
    end
  end
  return false
end

local function start_retrogo(app_name, rom_path, flags)
  log("start call", app_name or "launcher", rom_path or "")
  local started, start_err = retrogo.start({
    app = app_name or "launcher",
    rom = rom_path or "",
    flags = flags or 0,
  })
  if not started then
    log("start failed", tostring(start_err))
    return false
  end
  log("started", app_name or "launcher", rom_path or "")
  return true
end

if not start_retrogo("launcher", "", 0) then
  return
end

local function sync_input_mask(mask)
  local next_mask = tonumber(mask) or 0
  if next_mask < 0 then
    next_mask = 0
  end
  local ok, err = pcall(function()
    return retrogo.set_input_mask(next_mask)
  end)
  if not ok then
    log("set_input_mask failed", tostring(err))
  end
end

local current_mask = 0
local exit_to_home_requested = false
local runtime_timer = nil
local exit_timer = nil
local pending_restart = nil
local runtime_tick_busy = false
local next_status_poll_ms = 0
local next_exit_poll_ms = 0

local function stop_timer(timer_obj)
  if not timer_obj then
    return
  end
  pcall(function()
    timer_obj:stop()
  end)
  pcall(function()
    timer_obj:unregister()
  end)
end

local function stop_runtime_timer()
  if runtime_timer then
    stop_timer(runtime_timer)
    runtime_timer = nil
  end
end

local function finish_exit_to_home()
  if exit_timer then
    local timer = exit_timer
    exit_timer = nil
    stop_timer(timer)
  end
  stop_runtime_timer()
  if gamepad and gamepad.off then
    pcall(function()
      gamepad.off()
    end)
  end
  if app and app.exit then
    local ok, result, err = pcall(function()
      return app.exit()
    end)
    log("app.exit", tostring(ok), tostring(result), tostring(err))
    if not ok or result == nil then
      log("app.exit failed", tostring(err or result))
    end
  end
end

local function schedule_exit_to_home()
  if exit_timer then
    return
  end
  if not tmr or not tmr.create then
    finish_exit_to_home()
    return
  end
  exit_timer = tmr.create()
  local ok, err = pcall(function()
    exit_timer:alarm(80, tmr.ALARM_SINGLE or 0, finish_exit_to_home)
  end)
  if ok then
    log("app.exit scheduled")
  else
    log("app.exit schedule failed", tostring(err))
    finish_exit_to_home()
  end
end

local function request_exit_to_home(reason)
  if exit_to_home_requested then
    return
  end
  exit_to_home_requested = true
  pending_restart = nil
  log("exit to home", reason or "")
  APP.stop_web("exit")
  sync_input_mask(0)
  current_mask = 0
  local stop_ok, stop_result, stop_err = pcall(function()
    return retrogo.stop()
  end)
  log("retrogo.stop", tostring(stop_ok), tostring(stop_result), tostring(stop_err))
  log("retrogo stopped", tostring(wait_retrogo_stopped(1500)))
  schedule_exit_to_home()
end

local function update_gamepad_input(force)
  local state = read_gamepad_state()
  local next_mask = 0
  local raw_mask = 0
  if connected_state(state) then
    raw_mask = read_raw_gamepad_mask(state)
    next_mask = build_gamepad_mask(state, retrogo)
  else
    next_mask = 0
  end
  if has_bit(next_mask, retrogo.BTN_HOME) then
    log_pad_mask(next_mask)
    request_exit_to_home("GAMEPAD_HOME")
    return
  end
  if force or next_mask ~= current_mask then
    sync_input_mask(next_mask)
    if next_mask ~= 0 then
      log_pad_mask(next_mask)
    end
    current_mask = next_mask
  end
end

local gamepad_service_started = false
local gamepad_poll_while_running = true

local function register_cb(evt, cb)
  if not gamepad or not gamepad.on or not evt then
    return
  end
  pcall(function()
    gamepad.on(evt, cb)
  end)
end

local function start_gamepad()
  if not gamepad or not gamepad.start or not gamepad.off then
    log("gamepad module unavailable, skip BLE")
    return false
  end
  if gamepad_service_started then
    return true
  end

  pcall(function()
    gamepad.off()
  end)

  local ok, started, err = pcall(function()
    return gamepad.start({
      clear_bonds = false,
      debug = false,
    })
  end)
  if not ok or not started then
    log("gamepad.start failed", tostring(err or started))
    return false
  end

  register_cb(gamepad.EVT_UPDATE, function()
    update_gamepad_input(false)
  end)
  register_cb(gamepad.EVT_CONNECTING, function()
    log("BLE pairing")
  end)
  register_cb(gamepad.EVT_CONNECT_FAILED, function()
    sync_input_mask(0)
    current_mask = 0
  end)
  register_cb(gamepad.EVT_CONNECTED, function()
    update_gamepad_input(true)
  end)
  register_cb(gamepad.EVT_DISCONNECTED, function()
    sync_input_mask(0)
    current_mask = 0
  end)

  update_gamepad_input(true)
  gamepad_service_started = true
  gamepad_poll_while_running = true
  log("gamepad service started")
  return true
end

start_gamepad()

if app and app.set_home_exit then
  pcall(function()
    app.set_home_exit(true)
  end)
  log("HOME default exit enabled")
end

if key and key.on and key.HOME then
  pcall(function()
    key.on(key.HOME, function(evt_type, ts_ms)
      log("HOME event", tostring(evt_type), tostring(ts_ms))
      if not key or evt_type == key.START or evt_type == key.SHORT or
          evt_type == key.LONG_START or evt_type == key.LONG_REPEAT or evt_type == key.EXIT then
        request_exit_to_home("HOME:" .. tostring(evt_type))
      end
    end)
  end)
else
  log("key HOME handler unavailable")
end

local function schedule_restart(info)
  if exit_to_home_requested or (info and info.stop_requested) then
    pending_restart = nil
    return
  end
  sync_input_mask(0)
  current_mask = 0
  local app_name = info.app or "launcher"
  local rom_path = info.rom or ""
  local flags = info.boot_flags or 0
  if app_name == "desktop" or app_name == "__desktop" then
    request_exit_to_home("module requested desktop")
    return
  end
  log("restart request", app_name, rom_path, flags)
  pending_restart = {
    app_name = app_name,
    rom_path = rom_path,
    flags = flags,
    due_ms = now_ms() + 80,
  }
end

local function runtime_tick()
  if runtime_tick_busy or exit_to_home_requested then
    return
  end
  runtime_tick_busy = true
  local ok, err = pcall(function()
    local now = now_ms()
    if now >= next_exit_poll_ms then
      next_exit_poll_ms = now + APP.EXIT_POLL_DELAY_MS
      if app and app.exiting and app.exiting() then
        request_exit_to_home("app.exiting")
        return
      end
    end
    if exit_to_home_requested then
      return
    end
    if pending_restart then
      if now >= (pending_restart.due_ms or 0) then
        if exit_to_home_requested then
          pending_restart = nil
          return
        end
        local req = pending_restart
        pending_restart = nil
        if not start_retrogo(req.app_name, req.rom_path, req.flags) then
          request_exit_to_home("restart failed")
        end
      end
      return
    end
    if not gamepad_service_started or gamepad_poll_while_running then
      update_gamepad_input(false)
    end
    if exit_to_home_requested then
      return
    end
    if now >= next_status_poll_ms then
      next_status_poll_ms = now + APP.STATUS_POLL_DELAY_MS
      local info = retrogo_info()
      if info and not info.running then
        schedule_restart(info)
      end
    end
  end)
  runtime_tick_busy = false
  if not ok then
    log("runtime tick failed", tostring(err))
    request_exit_to_home("runtime tick failed")
  end
end

local function start_runtime_timer()
  if not tmr or not tmr.create then
    log("tmr unavailable, runtime loop disabled")
    return false
  end
  runtime_timer = tmr.create()
  local ok, err = pcall(function()
    runtime_timer:alarm(APP.POLL_DELAY_MS, tmr.ALARM_AUTO or 1, runtime_tick)
  end)
  if not ok then
    log("runtime timer failed", tostring(err))
    runtime_timer = nil
    return false
  end
  log("runtime timer started", APP.POLL_DELAY_MS)
  return true
end

if not start_runtime_timer() then
  if app and app.exiting and app.exiting() then
    request_exit_to_home("app.exiting")
  end
end
end

sync_legacy_roms()
start_rom_web()

if APP.AUTO_SELECT_MODULE then
  start_selected_module(select_module())
elseif not choose_module_async(start_selected_module) then
  start_selected_module(choose_module())
end
