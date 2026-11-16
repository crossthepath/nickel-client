-- Entity ESP: a label over every entity within a radius.
local esp = nickel.module({ name = "Label ESP", category = "Visual", description = "Names over nearby entities" })
local radius = esp:number("Radius", 32, 4, 128, 1)
local playersOnly = esp:boolean("Players only", false)
local color = esp:color("Color", { 1, 1, 1, 1 })

esp:on("render2d", function(d: ScreenDraw)
  local list = world.entities({ players = playersOnly:get(), radius = radius:get() })
  for _, e in list do
    if not e.isLocal then
      local top = e.boxMax or e.eyePosition
      local x, y = d:worldToScreen(top + vector.create(0, 0.5, 0))
      if x and y then
        local label = e.name or e.type
        if e.health then
          label = label .. "  " .. math.floor(e.health)
        end
        d:text(label, x, y, 14, color:get(), true, { align = "Center" })
      end
    end
  end
end)
