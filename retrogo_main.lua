local APP = {
  VERSION = "2026-06-15-minimal-selector-v2",
  ROM_ROOT = "/sd/roms",
  POLL_DELAY_MS = 16,
  AXIS_THRESHOLD = 0.60,
  MODULES = {
    {
      id = "retro-core",
      title = "Retro Core",
      detail = "Holocubic optimized",
      path = "/sd/modules/retrogo.so",
    },
    {
      id = "gwenesis",
      title = "Mega Drive",
      detail = "Holocubic optimized",
      path = "/sd/modules/gwenesis.so",
    },
  },
}

local function log(...)
  print("[retrogo_app]", ...)
end

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

  local up = any_true(state, { "dpad_up", "up", "up_pressed", "up_key", "n", "north" }) or
      axis_greater_than(ly)
  local down = any_true(state, { "dpad_down", "down", "down_pressed", "down_key", "s", "south" }) or
      axis_less_than(ly)
  local left = any_true(state, { "dpad_left", "left", "left_pressed", "left_key", "w", "west" }) or
      axis_less_than(lx)
  local right = any_true(state, { "dpad_right", "right", "right_pressed", "right_key", "e", "east" }) or
      axis_greater_than(lx)


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
      any_true(state, { "r", "rb", "r1", "right_shoulder", "btn_r", "button_r", "button_rb" })
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
  if x_pressed then
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
        any_true(state, { "dpad_left", "left", "left_pressed", "west" }) or axis_less_than(lx),
    right = has_bit(raw_mask, gamepad and gamepad.BTN_RIGHT or 0) or
        any_true(state, { "dpad_right", "right", "right_pressed", "east" }) or axis_greater_than(lx),
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
    bind(key.MENU, confirm)
    bind(key.HOME, cancel)
  end

  local prev = {}
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

local selected_module = choose_module()
if not selected_module then
  log("module selection canceled")
  return
end

APP.MODULE_PATH = selected_module.path
log("selected", selected_module.id, APP.MODULE_PATH)

local ok_module, retrogo = pcall(require, APP.MODULE_PATH)
if not ok_module or type(retrogo) ~= "table" then
  log("require failed", tostring(retrogo))
  return
end

log("module", tostring(retrogo.VERSION), tostring(retrogo.RETRO_GO_CORE))

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

local function add_row(rows, kind, path, size, mtime)
  rows[#rows + 1] = string.format("%s\t%s\t%d\t%d\n", kind, path, tonumber(size) or 0, tonumber(mtime) or 0)
end

local function item_path(parent, item)
  if type(item) == "table" then
    return item.path or item.fullpath or item.full_path or (parent .. "/" .. tostring(item.name or item[1] or ""))
  end
  return parent .. "/" .. tostring(item)
end

local function item_is_dir(path, item)
  if type(item) == "table" then
    if item.is_dir ~= nil then return item.is_dir end
    if item.dir ~= nil then return item.dir end
    if item.directory ~= nil then return item.directory end
    if item.type == "dir" or item.category == "dir" then return true end
  end
  local st = file and file.stat and file.stat(path) or nil
  return st and (st.is_dir or st.dir or st.directory) or false
end

local function item_size(path, item)
  if type(item) == "table" then
    return item.size or item.file_size or 0
  end
  local st = file and file.stat and file.stat(path) or nil
  return st and (st.size or st.file_size or 0) or 0
end

local function listdir(path)
  if file and file.listdir then
    return file.listdir(path) or {}
  end
  if sd and sd.listdir then
    return sd.listdir(path) or {}
  end
  return {}
end

local function scan(path, rows, seen)
  if seen[path] then
    return
  end
  seen[path] = true
  add_row(rows, "D", path, 0, 0)
  for _, item in ipairs(listdir(path)) do
    local child = item_path(path, item)
    if child ~= path and child ~= "" then
      if item_is_dir(child, item) then
        scan(child, rows, seen)
      else
        add_row(rows, "F", child, item_size(child, item), 0)
      end
    end
  end
end

local rows = {}
scan(APP.ROM_ROOT, rows, {})
local blob = table.concat(rows)
log("catalog bytes", #blob, "rows", #rows)
log("set_catalog call")
local ok, set_err = retrogo.set_catalog(blob)
if not ok then
  log("set_catalog failed", tostring(set_err))
  return
end
log("set_catalog ok")

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

local function request_exit_to_home(reason)
  if exit_to_home_requested then
    return
  end
  exit_to_home_requested = true
  log("exit to home", reason or "")
  sync_input_mask(0)
  current_mask = 0
  local stop_ok, stop_result, stop_err = pcall(function()
    return retrogo.stop()
  end)
  log("retrogo.stop", tostring(stop_ok), tostring(stop_result), tostring(stop_err))
  log("retrogo stopped", tostring(wait_retrogo_stopped(1500)))
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
    log("input mask", next_mask,
        "mapped", names_from_mask(next_mask, retrogo_button_defs),
        "raw", raw_mask,
        "raw_buttons", names_from_mask(raw_mask, gamepad_button_defs()),
        "aliases", debug_input_aliases(state))
    request_exit_to_home("GAMEPAD_HOME")
    return
  end
  if force or next_mask ~= current_mask then
    sync_input_mask(next_mask)
    if next_mask ~= 0 then
      log("input mask", next_mask,
          "mapped", names_from_mask(next_mask, retrogo_button_defs),
          "raw", raw_mask,
          "raw_buttons", names_from_mask(raw_mask, gamepad_button_defs()),
          "aliases", debug_input_aliases(state))
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

while true do
  if app and app.exiting and app.exiting() then
    request_exit_to_home("app.exiting")
    break
  end
  if exit_to_home_requested then
    break
  end
  if not gamepad_service_started or gamepad_poll_while_running then
    update_gamepad_input(false)
  end
  local info = retrogo_info()
  if info and not info.running then
    sync_input_mask(0)
    current_mask = 0
    local app_name = info.app or "launcher"
    local rom_path = info.rom or ""
    local flags = info.boot_flags or 0
    log("restart request", app_name, rom_path, flags)
    sleep_ms(80)
    if not start_retrogo(app_name, rom_path, flags) then
      break
    end
  end
  if not sleep_ms(APP.POLL_DELAY_MS) then
    break
  end
end
