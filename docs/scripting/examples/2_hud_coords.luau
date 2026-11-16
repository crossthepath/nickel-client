-- HUD element: player coordinates, movable in the HUD editor.
local hud = nickel.module({ name = "Coordinates", category = "HUD", description = "Shows X Y Z" })
hud:hud(160, 24)
local background = hud:color("Background", { 0, 0, 0, 0.5 })

hud:on("render2d", function(d: ScreenDraw)
  local x, y, scale, w, h = hud:hudFrame()
  if not (x and y and scale and w and h) then return end
  d:fill(x, y, x + w, y + h, background:get(), 4 * scale)
  local p = player.position()
  local text = "not in a world"
  if p then
    text = string.format("%d  %d  %d", math.floor(p.x), math.floor(p.y), math.floor(p.z))
  end
  d:text(text, x + 6 * scale, y + 4 * scale, 16 * scale, { 1, 1, 1, 1 }, true)
end)
