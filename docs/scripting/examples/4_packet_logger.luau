-- Packet logger: counts incoming packets by name and prints a summary every 5 seconds.
local logger = nickel.module({ name = "Packet Counter", category = "Utility" })
local counts: { [string]: number } = {}
local timer: number? = nil

logger:on("packetReceive", function(e: PacketEvent)
  counts[e.name] = (counts[e.name] or 0) + 1
end)

logger:onEnable(function()
  counts = {}
  timer = client.every(5000, function()
    for name, n in counts do
      chat.print(name .. ": " .. n)
    end
    counts = {}
  end)
end)

logger:onDisable(function()
  if timer then
    client.cancel(timer)
    timer = nil
  end
end)
