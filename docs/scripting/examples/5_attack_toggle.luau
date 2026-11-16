-- Toggle module reacting to attack: blocks hits on players named in a list, notifies on others.
local guard = nickel.module({ name = "Friend Guard", category = "Combat", key = "G" })
local friends = guard:text("Friends", "Steve,Alex")

guard:on("attack", function(e: AttackEvent)
  local target = world.entity(e.target)
  if target and target.isPlayer and target.name then
    for friend in string.gmatch(friends:get(), "[^,]+") do
      if friend == target.name then
        e:cancel()
        chat.print("Not hitting " .. friend)
        return
      end
    end
  end
end)

guard:onEnable(function() client.notify("Friend Guard", "On") end)
guard:onDisable(function() client.notify("Friend Guard", "Off") end)
