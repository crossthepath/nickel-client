-- Config-style module: turns built-in modules on with chosen settings, restores them when turned off.
local preset = nickel.module({ name = "PvP Preset", category = "Utility", description = "Applies a PvP setup" })
local names = { "Reach Range", "Nametags" }
local before: { [string]: boolean } = {}

preset:onEnable(function()
  for _, name in names do
    local m = nickel.modules.get(name)
    if m then
      before[name] = m:isEnabled()
      m:setEnabled(true)
    else
      chat.print("No module named " .. name)
    end
  end
  local nametags = nickel.modules.get("Nametags")
  if nametags then
    for _, settingName in nametags:settings() do
      local s = nametags:setting(settingName)
      if s then
        chat.print(settingName .. " (" .. s:type() .. ") = " .. tostring(s:get()))
      end
    end
  end
end)

preset:onDisable(function()
  for name, was in before do
    local m = nickel.modules.get(name)
    if m then m:setEnabled(was) end
  end
  before = {}
end)
