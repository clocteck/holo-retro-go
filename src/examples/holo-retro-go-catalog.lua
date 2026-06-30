local retrogo = require("retrogo")

local function app_dir()
  local cur = app and app.current and app.current() or nil
  local entry = cur and cur.entry or nil
  if type(entry) == "string" and entry ~= "" then
    local dir = entry:gsub("\\", "/"):match("^(.*)/[^/]*$")
    if dir and dir ~= "" then
      return dir
    end
  end
  return "/sd/apps/holo-retro-go"
end

local root = app_dir() .. "/roms"
local rows = {}

local function add(kind, path, size, mtime)
  rows[#rows + 1] = string.format("%s\t%s\t%d\t%d\n", kind, path, size or 0, mtime or 0)
end

local function scan(path)
  local list = file.listdir(path) or {}
  add("D", path, 0, 0)
  for _, item in ipairs(list) do
    local child = item.path or (path .. "/" .. item.name)
    local is_dir = item.is_dir or item.dir or item.directory
    if is_dir then
      scan(child)
    else
      add("F", child, item.size or 0, item.mtime or 0)
    end
  end
end

scan(root)
assert(retrogo.set_catalog(table.concat(rows)))

local info = retrogo.catalog_info()
print(string.format("retrogo catalog: %d entries, %d files", info.entries, info.files))

retrogo.start({ app = "launcher" })
