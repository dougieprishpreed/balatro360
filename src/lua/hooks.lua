-- Balatro360 game hooks (embedded into the executable).
--
-- Adjusts Balatro for the console without modifying its source files:
-- hooks are attached when specific game modules finish loading through
-- require(). Nothing here contains game code; it only sets documented
-- feature flags that the game already supports for its console versions.

local hooks = {}

-- Platform feature flags, applied right after Game:set_globals() runs.
local function apply_console_flags(G)
	G.F_VIDEO_SETTINGS = false   -- window modes are meaningless on a TV
	G.F_EXTERNAL_LINKS = false   -- no web browser
	G.F_CRASH_REPORTS = false
	G.F_DISCORD = false
	G.F_ENGLISH_ONLY = false
	G.F_QUIT_BUTTON = true       -- "Quit" returns to the dashboard
	G.F_RUMBLE = 0.5
	G.F_SAVE_TIMER = 30
	G.F_HIDE_BG = true           -- console versions hide the board when paused
	G.F_SOUND_THREAD = true
	G.F_NO_ERROR_HAND = false    -- show the error screen instead of exiting
	local name = love.system.getUsername and love.system.getUsername()
	if type(name) == "string" and name ~= "" and love._os == "Xbox 360" then
		G.F_DISP_USERNAME = name
	end

	-- User overrides: b360_config.lua in the game folder may return a table
	-- of flags, e.g. return { F_RUMBLE = 1.0 }.
	if love.filesystem.getInfo("b360_config.lua") then
		local ok, cfg = pcall(function() return love.filesystem.load("b360_config.lua")() end)
		if ok and type(cfg) == "table" then
			for k, v in pairs(cfg) do G[k] = v end
		elseif not ok then
			print("b360_config.lua: " .. tostring(cfg))
		end
	end
end

hooks["game"] = function()
	if type(Game) ~= "table" or type(Game.init) ~= "function" then return end
	local orig_init = Game.init
	Game.init = function(self, ...)
		local r = orig_init(self, ...)
		apply_console_flags(self)
		return r
	end
end

-- Performance: Balatro's classes (engine/object.lua, the SNKRX/classic
-- pattern) resolve methods through a chain of __index tables, e.g.
-- Card -> Moveable -> Node -> Object. Plain Lua 5.1 pays for every hop on
-- every method call (LuaJIT mostly optimises it away). Once all game code is
-- loaded, copy inherited methods into each subclass so lookups resolve in
-- one hop. Metatables are untouched, so Object:is() and explicit
-- Parent.method(self) calls keep working, and subclass overrides win.
local function flatten_classes()
	if type(Object) ~= "table" then return 0 end
	local classes = {}
	local function is_class(t)
		return type(t) == "table" and rawget(t, "__index") == t and rawget(t, "super") ~= nil
	end
	for _, v in pairs(_G) do
		if is_class(v) then classes[#classes + 1] = v end
	end
	local copied = 0
	for _, cls in ipairs(classes) do
		local parent = rawget(cls, "super")
		local depth = 0
		while type(parent) == "table" and depth < 32 do
			for k, v in pairs(parent) do
				if type(v) == "function" and type(k) == "string" and k:sub(1, 2) ~= "__"
					and rawget(cls, k) == nil then
					rawset(cls, k, v)
					copied = copied + 1
				end
			end
			parent = rawget(parent, "super")
			depth = depth + 1
		end
	end
	return copied, #classes
end

-- Called by boot.lua after main.lua (and every module it requires) loaded.
function love._b360_postmain()
	local copied, n = flatten_classes()
	print(string.format("[b360] flattened %d methods into %d classes", copied or 0, n or 0))
end

-- Wrap require so hooks run once their module has loaded.
local orig_require = require
function require(name)
	local first = package.loaded[name] == nil
	local result = orig_require(name)
	if first and hooks[name] then
		local h = hooks[name]
		hooks[name] = nil
		h()
	end
	return result
end
