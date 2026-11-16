-- Chat command: ".dist to <x> <y> <z>" prints the distance to a point; ".dist here" saves the position.
nickel.command({
  name = "dist",
  description = "Distance to a point",
  subcommands = {
    to = {
      args = { { "x", "number" }, { "y", "number" }, { "z", "number" } },
      run = function(x, y, z)
        local p = player.position()
        if not p then
          chat.print("Not in a world")
          return
        end
        local d = vector.magnitude(vector.create(x, y, z) - p)
        chat.print(string.format("Distance: %.1f blocks", d))
      end,
    },
    here = {
      run = function()
        local p = player.position()
        if p then
          storage.set("saved", { x = p.x, y = p.y, z = p.z })
          chat.print("Saved")
        end
      end,
    },
  },
})
