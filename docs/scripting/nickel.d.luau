-- Nickel scripting API: Luau definition file.
-- Source of truth: native/script_engine.hpp (RegisterApi, lines 331-355), native/script_api21.hpp,
-- native/script_draw.hpp, native/script_events.hpp, native/script_modules.hpp, native/script_canvas.hpp,
-- native/script_values.hpp. Load with Frontend::loadDefinitionFile.
--
-- Conventions
-- * Positions returned by the API are native Luau `vector` values (fields .x .y .z, built with vector.create).
--   Functions that take a position accept a vector, a table {x=, y=, z=} or an array {x, y, z} (VectorLike).
-- * A function typed `T?` may return nothing at all (0 values); in Luau that reads as nil.
-- * Colours are tables with numbers 0..1: {r, g, b, a} (array) and/or {r=, g=, b=, a=}; missing parts are 1,
--   anything that is not a table is opaque white.
-- * The events "join", "leave" and "hit" are accepted by mod:on at run time but are NEVER dispatched by the engine;
--   they are deliberately left out of the `on` overloads below so a script relying on them is rejected.

-- ---------------------------------------------------------------- shared value types

type VectorLike = vector | { x: number, y: number, z: number } | { number }

type Color = { number } | { r: number?, g: number?, b: number?, a: number? }

-- A colour as setting:get() returns it: an array {r, g, b, a} (indices 1..4, no named fields).
type ColorValue = { number }

type Face = "Down" | "Up" | "North" | "South" | "West" | "East"

type Category = "Combat" | "Movement" | "Player" | "Visual" | "Utility" | "HUD"

-- Event payloads. Each is a plain table built for the handler call; they are declared as extern types so scripts can
-- annotate handler parameters (`function(e: AttackEvent)`), which strict mode needs to pick the right `on` overload.
-- Every event table carries cancel(); event:cancel() (or event.cancelled = true) only has an effect on attack,
-- packetSend, packetReceive, chat and jump. Handler return values are ignored.
declare extern type AttackEvent with
    target: number
    cancelled: boolean?
    function cancel(self): ()
end
declare extern type PacketEvent with
    id: number
    name: string
    cancelled: boolean?
    function cancel(self): ()
end
declare extern type ChatEvent with
    message: string
    author: string
    type: "Chat"
    cancelled: boolean?
    function cancel(self): ()
end
declare extern type KeyEvent with
    key: string
    pressed: boolean
    cancelled: boolean?
    function cancel(self): ()
end
-- Writable: changed fields are written back into the movement input (forward / strafe clamped to -1..1).
declare extern type MoveEvent with
    forward: number
    strafe: number
    jump: boolean
    sneak: boolean
    sprint: boolean
    cancelled: boolean?
    function cancel(self): ()
end
-- Writable: `boost` (starts at 0) is added to the jump after the game computes it.
-- UNSURE: unit of boost (added to the float at +0x1c of the jump hook's 8th argument).
declare extern type JumpEvent with
    boost: number
    cancelled: boolean?
    function cancel(self): ()
end
declare extern type HurtEvent with
    amount: number
    health: number
    cancelled: boolean?
    function cancel(self): ()
end
declare extern type DimensionEvent with
    dimension: "Overworld" | "Nether" | "TheEnd"
    cancelled: boolean?
    function cancel(self): ()
end
declare extern type ScreenEvent with
    name: string
    previous: string
    cancelled: boolean?
    function cancel(self): ()
end
declare extern type ContainerOpenEvent with
    type: "container"
    cancelled: boolean?
    function cancel(self): ()
end
declare extern type TeleportEvent with
    position: vector
    pearl: boolean
    cancelled: boolean?
    function cancel(self): ()
end

declare extern type Entity with
    id: number -- runtime id
    type: string -- "player" or "entity.<n>" (n = ActorType & 0xff); no type names
    name: string? -- players: name tag; others: the type's translated display name; absent when empty
    isPlayer: boolean
    isLocal: boolean
    teammate: boolean -- always false
    bot: boolean -- always false
    health: number? -- absent when the health attribute could not be read
    position: vector -- feet when the bounding box was read, else the movement-state position
    eyePosition: vector
    velocity: vector
    boxMin: vector?
    boxMax: vector?
end

type EntityQuery = { players: boolean?, radius: number?, type: string? }

type TargetQuery = { range: number?, playersOnly: boolean? }

declare extern type LookTarget with
    type: "block" | "entity" | "none"
    block: vector? -- block hits only
    position: vector? -- block hits only (same value as block)
    face: Face?
end

declare extern type RaycastHit with
    type: ("block" | "entity" | "none")? -- absent for an unknown hit kind
    block: vector?
    face: Face?
    position: vector? -- absent for "none"
end

declare extern type Block with
    name: string -- full name such as "minecraft:stone"; "" when unknown
    runtimeId: number
    isAir: boolean
    solid: boolean
    liquid: boolean
    position: vector
    states: { [string]: number | string }
end

declare extern type Item with
    id: string -- always namespaced, e.g. "minecraft:diamond_sword"
    name: string -- the identifier as read (may already be namespaced)
    count: number
    durability: number? -- only for damageable items
    maxDurability: number?
end

type SimInput = {
    strafe: number?,
    forward: number?,
    yaw: number?,
    pitch: number?,
    jump: boolean?,
    sneak: boolean?,
    sprint: boolean?,
}

declare extern type SimTick with
    position: vector
    velocity: vector
    onGround: boolean
    landed: boolean
    collided: boolean
    outcome: "Normal" | "Teleport" | "Unreliable" | "UnloadedChunk" | "ImmobileOrNotReady" | "Mounted" | "InvalidInput"
end

declare extern type ProjectileHit with
    position: vector
    ticks: number
    entity: number?
end

declare extern type PathStatus with
    state: "Idle" | "Planning" | "Moving" | "Acting" | "Paused"
    process: "None" | "Goto" | "Wander" | "Follow" | "Mine" | "Tunnel" | "Chop" | "Build"
    detail: string
    remaining: number
end

type ServerInfo = { ["local"]: boolean, address: string?, port: number? }

type RotationMode = "camera" | "visible" | "silent" | "packet"
type RotationPriority = "ambient" | "aim" | "combat" | "action" | "view"
type RotationOptions = { mode: RotationMode?, priority: RotationPriority?, smooth: number? }
type AimOptions = { predict: number?, mode: RotationMode?, priority: RotationPriority?, smooth: number? }

type UseItemOptions = {
    slot: number?,
    ghost: boolean?,
    disable: boolean?,
    waitForAim: boolean?,
    aimTolerance: number?,
    switchDelay: number?,
    restoreDelay: number?,
    done: ((ok: boolean) -> ())?,
}

type ModuleOptions = {
    name: string,
    category: Category,
    description: string?,
    key: string?,
    activation: ("Toggle" | "Hold" | "OneShot")?,
}

-- One argument: { "name", "type", { choices }? , optional = bool?, min = n?, max = n? }
-- type: "string" (default), "text" (rest of the line), "number", "integer", "boolean", "choice".
type CommandArg = { [number]: any, optional: boolean?, min: number?, max: number? }

type SubcommandSpec = {
    description: string?,
    args: { CommandArg }?,
    subcommands: { [string]: SubcommandSpec }?,
    run: ((...any) -> ())?,
}

type CommandSpec = {
    name: string,
    description: string?,
    args: { CommandArg }?,
    subcommands: { [string]: SubcommandSpec }?,
    run: ((...any) -> ())?,
}

type TextOptions = { font: ("Regular" | "SemiBold" | "Bold" | "ExtraBold")?, align: ("Left" | "Center" | "Right")? }

type InvResult = "Submitted" | "Unavailable" | "Busy" | "InvalidContainer" | "InvalidSlot" | "InvalidAmount" | "IncompatibleItems" | "NotArmor" | "Empty" | "InventoryFull" | "ContainerFull"

-- ---------------------------------------------------------------- drawing (render2d / render3d handlers only)

declare extern type ScreenDraw with
    function text(self, text: string, x: number, y: number, size: number?, color: Color?, shadow: boolean?, options: TextOptions?): ()
    function fill(self, x1: number, y1: number, x2: number, y2: number, color: Color?, rounding: number?): ()
    function rect(self, x1: number, y1: number, x2: number, y2: number, color: Color?, thickness: number?): ()
    function line(self, x1: number, y1: number, x2: number, y2: number, color: Color?, thickness: number?): ()
    function circle(self, x: number, y: number, radius: number, color: Color?, filled: boolean?, thickness: number?): ()
    function gradient(self, x1: number, y1: number, x2: number, y2: number, topLeft: Color?, topRight: Color?, bottomRight: Color?, bottomLeft: Color?): ()
    function worldToScreen(self, position: VectorLike): (number?, number?)
    function size(self): (number, number)
end

declare extern type WorldDraw with
    function box(self, min: VectorLike, max: VectorLike, color: Color?, thickness: number?): ()
    function filledBox(self, min: VectorLike, max: VectorLike, color: Color?): ()
    function line(self, from: VectorLike, to: VectorLike, color: Color?, thickness: number?): ()
    function entityBox(self, id: number, color: Color?, thickness: number?): boolean
end

-- ---------------------------------------------------------------- settings

declare extern type BooleanSetting with
    function get(self): boolean
    function set(self, value: boolean): ()
    function name(self): string
    function type(self): string -- "boolean"
end

declare extern type NumberSetting with
    function get(self): number
    function set(self, value: number): ()
    function name(self): string
    function type(self): string -- "number" or "integer"
end

declare extern type ChoiceSetting with
    function get(self): string
    function set(self, value: string): ()
    function name(self): string
    function type(self): string -- "choice"
end

declare extern type ColorSetting with
    function get(self): ColorValue
    function set(self, value: Color): ()
    function name(self): string
    function type(self): string -- "color"
end

declare extern type TextSetting with
    function get(self): string
    function set(self, value: string): ()
    function name(self): string
    function type(self): string -- "text"
end

declare extern type KeySetting with
    function get(self): string -- key name, "None" when unbound
    function set(self, value: string): ()
    function name(self): string
    function type(self): string -- "key"
end

-- A setting of any module reached through nickel.modules.get(name):setting(name). type() is the internal kind:
-- "bool", "number", "range", "enum", "color", "gradient", "list", "pattern", "text" or "key".
declare extern type Setting with
    function get(self): any
    function set(self, value: any, high: number?): ()
    function name(self): string
    function type(self): string
end

-- ---------------------------------------------------------------- modules

declare extern type BuiltinModule with
    function name(self): string
    function category(self): string
    function isEnabled(self): boolean
    function setEnabled(self, on: boolean): ()
    function toggle(self): ()
    function settings(self): { string }
    function setting(self, nameOrId: string): Setting?
end

declare extern type Module with
    -- settings: only while the script loads
    function boolean(self, name: string, default: boolean?): BooleanSetting
    function number(self, name: string, default: number, min: number, max: number, step: number?): NumberSetting
    function integer(self, name: string, default: number, min: number, max: number, step: number?): NumberSetting
    function choice(self, name: string, options: { string }): ChoiceSetting
    function color(self, name: string, default: Color?): ColorSetting
    function text(self, name: string, default: string?): TextSetting
    function key(self, name: string, default: string?): KeySetting
    -- HUD element: only while the script loads
    function hud(self, width: number, height: number): ()
    function hudFrame(self): (number?, number?, number?, number?, number?)

    function onEnable(self, handler: () -> ()): ()
    function onDisable(self, handler: () -> ()): ()
    function isEnabled(self): boolean
    function setEnabled(self, on: boolean): ()

    -- not while the script loads
    function rotate(self, yaw: number, pitch: number, options: RotationOptions?): ()
    function aimAt(self, target: VectorLike | number, options: AimOptions?): boolean
    function useItem(self, options: UseItemOptions?): ()

    function on(self, event: "update", handler: () -> ()): ()
    function on(self, event: "tick", handler: () -> ()): ()
    function on(self, event: "render2d", handler: (draw: ScreenDraw) -> ()): ()
    function on(self, event: "render3d", handler: (draw: WorldDraw) -> ()): ()
    function on(self, event: "attack", handler: (event: AttackEvent) -> ()): ()
    function on(self, event: "packetSend", handler: (event: PacketEvent) -> ()): ()
    function on(self, event: "packetReceive", handler: (event: PacketEvent) -> ()): ()
    function on(self, event: "chat", handler: (event: ChatEvent) -> ()): ()
    function on(self, event: "key", handler: (event: KeyEvent) -> ()): ()
    function on(self, event: "move", handler: (event: MoveEvent) -> ()): ()
    function on(self, event: "jump", handler: (event: JumpEvent) -> ()): ()
    function on(self, event: "hurt", handler: (event: HurtEvent) -> ()): ()
    function on(self, event: "death", handler: () -> ()): ()
    function on(self, event: "respawn", handler: () -> ()): ()
    function on(self, event: "dimension", handler: (event: DimensionEvent) -> ()): ()
    function on(self, event: "screen", handler: (event: ScreenEvent) -> ()): ()
    function on(self, event: "containerOpen", handler: (event: ContainerOpenEvent) -> ()): ()
    function on(self, event: "containerClose", handler: () -> ()): ()
    function on(self, event: "pathEnd", handler: () -> ()): ()
    function on(self, event: "teleport", handler: (event: TeleportEvent) -> ()): ()
end

-- ---------------------------------------------------------------- globals

declare nickel: {
    module: (options: ModuleOptions) -> Module, -- only while loading
    command: (spec: CommandSpec) -> (), -- only while loading
    modules: {
        list: () -> { string },
        get: (name: string) -> BuiltinModule?,
    },
}

declare player: {
    inWorld: () -> boolean,
    position: () -> vector?,
    eyePosition: () -> vector?,
    velocity: () -> vector?,
    rotation: () -> (number, number),
    anglesTo: (target: VectorLike) -> (number?, number?),
    health: () -> number?,
    name: () -> string,
    runtimeId: () -> number?,
    actorUniqueId: () -> number?,
    onGround: () -> boolean,
    isSprinting: () -> boolean,
    isSneaking: () -> boolean,
    inWater: () -> boolean,
    inLava: () -> boolean,
    fallDistance: () -> number,
    gameMode: () -> "Survival" | "Creative" | "Adventure" | "Spectator" | "Default" | "Undefined",
    slot: () -> number,
    setSlot: (slot: number) -> (),
    setVelocity: (velocity: VectorLike) -> (),
    addVelocity: (velocity: VectorLike) -> (),
    teleport: (feet: VectorLike) -> (boolean, string?),
    jump: () -> boolean,
    swing: () -> (),
    lookingAt: () -> LookTarget?,
    attack: (id: number) -> boolean?,
    interact: (id: number) -> boolean?,
    startBreak: (position: VectorLike, face: Face?) -> (boolean?, boolean?),
    continueBreak: (position: VectorLike, face: Face?) -> (boolean?, boolean?),
    stopBreak: (position: VectorLike) -> (),
    useItemOn: (position: VectorLike, face: Face?) -> (),
    releaseItem: () -> (),
    simulate: (input: SimInput?, ticks: number?) -> { SimTick }?,
}

declare world: {
    entities: (options: (EntityQuery | boolean)?) -> { Entity },
    entity: (id: number) -> Entity?,
    target: (options: TargetQuery?) -> Entity?,
    dimension: () -> ("Overworld" | "Nether" | "TheEnd")?,
    time: () -> nil,
    block: (position: VectorLike) -> Block?,
    raycast: (from: VectorLike, to: VectorLike, liquids: boolean?) -> RaycastHit?,
    isLoaded: (position: VectorLike) -> boolean?,
    findBlocks: (name: string, radius: number?) -> { vector }?,
    placeBlock: (position: VectorLike) -> boolean?,
}

declare chat: {
    print: (text: string) -> (),
    send: (text: string) -> (),
    command: (command: string) -> (),
}

declare client: {
    notify: (title: string, text: string?) -> (),
    time: () -> number,
    tick: () -> number,
    after: (ms: number, callback: () -> ()) -> number,
    every: (ms: number, callback: () -> ()) -> number,
    cancel: (id: number) -> boolean,
    isKeyDown: (key: string) -> boolean,
    server: () -> ServerInfo,
    ping: () -> number?,
    transport: () -> "RakNet" | "NetherNet" | "None",
}

declare storage: {
    get: (key: string) -> any,
    set: (key: string, value: any) -> (),
}

declare inventory: {
    item: (slot: number) -> Item?,
    size: () -> number,
    held: () -> Item?,
    offhand: () -> Item?,
    armor: (index: number) -> Item?,
    find: (name: string) -> number?,
    hotbarFind: (name: string) -> number?,
    swap: (a: number, b: number) -> (boolean, InvResult),
    drop: (slot: number, count: number?) -> (boolean, InvResult),
    equip: (slot: number) -> (boolean, InvResult),
    toOffhand: (slot: number) -> (boolean, InvResult),
}

declare container: {
    isOpen: () -> boolean,
    -- UNSURE: the full list of names (protocol container types, see reference.md); "None" when nothing is open
    type: () -> string,
    size: () -> number,
    item: (slot: number) -> Item?,
    items: () -> { Item? },
    take: (slot: number, count: number?) -> (boolean, InvResult),
    put: (inventorySlot: number, count: number?) -> (boolean, InvResult),
    close: () -> (),
}

declare path: {
    goto: (position: VectorLike, radius: number?) -> (),
    follow: (name: string, distance: number?) -> (),
    mine: (blocks: { string }, count: number?) -> (),
    wander: (radius: number?) -> (),
    stop: () -> (),
    pause: () -> (),
    resume: () -> (),
    isActive: () -> boolean,
    status: () -> PathStatus,
}

type ProjectileItem = "bow" | "arrow" | "snowball" | "egg" | "blue_egg" | "brown_egg" | "ender_pearl" | "fishing_rod" | "fishing_hook" | "minecraft:bow" | "minecraft:arrow" | "minecraft:snowball" | "minecraft:egg" | "minecraft:blue_egg" | "minecraft:brown_egg" | "minecraft:ender_pearl" | "minecraft:fishing_rod" | "minecraft:fishing_hook"

declare projectile: {
    trace: (item: ProjectileItem, yaw: number, pitch: number) -> ProjectileHit?,
    aim: (item: ProjectileItem, target: VectorLike, tolerance: number?) -> (number?, number?),
}

-- Libraries lib/<name>.luau of the scripts folder; only while the script loads.
declare function require(name: string): any
