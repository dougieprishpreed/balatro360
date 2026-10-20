-- Balatro360 game hooks (embedded into the executable).
--
-- Adjusts Balatro for the console without modifying its source files:
-- hooks are attached when specific game modules finish loading through
-- require(). Nothing here contains game code; it only sets documented
-- feature flags that the game already supports for its console versions.

local hooks = {}

-- b360_config.lua in the game folder may return a table of settings:
--   F_* keys override the game's feature flags (e.g. F_RUMBLE = 1.0),
--   B360_* keys are runtime options:
--     B360_SHOW_FPS = true    FPS / frame time overlay in the top left corner
--     B360_LOG_PERF = true    log average/worst frame time every 10 s
--     B360_SAFE_AREA = 0.9    shrink the picture for TVs that crop the edges
local user_config
local function config()
	if user_config then return user_config end
	user_config = {}
	if love.filesystem.getInfo("b360_config.lua") then
		local ok, cfg = pcall(function() return love.filesystem.load("b360_config.lua")() end)
		if ok and type(cfg) == "table" then
			user_config = cfg
		elseif not ok then
			print("b360_config.lua: " .. tostring(cfg))
		end
	end
	return user_config
end

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

	for k, v in pairs(config()) do
		if type(k) == "string" and k:sub(1, 2) == "F_" then G[k] = v end
	end
end

-- Optional performance overlay / logging, for testing on hardware.
local function install_perf_tools(cfg)
	if not (cfg.B360_SHOW_FPS or cfg.B360_LOG_PERF) then return end
	local graphics = love.graphics
	local present = graphics.present
	local last = love.timer.getTime()
	local shown_ms, acc, worst, frames, window_start = 0, 0, 0, 0, last
	local font
	graphics.present = function(...)
		local now = love.timer.getTime()
		local ms = (now - last) * 1000
		last = now
		acc = acc + ms
		frames = frames + 1
		if ms > worst then worst = ms end
		shown_ms = shown_ms * 0.9 + ms * 0.1
		if now - window_start >= 10 then
			if cfg.B360_LOG_PERF then
				print(string.format("[perf] %d frames, avg %.1f ms (%.1f fps), worst %.1f ms, Lua heap %.1f MB",
					frames, acc / frames, 1000 * frames / acc, worst, collectgarbage("count") / 1024))
			end
			acc, worst, frames, window_start = 0, 0, 0, now
		end
		if cfg.B360_SHOW_FPS then
			font = font or graphics.newFont("resources/fonts/m6x11plus.ttf", 24)
			graphics.push("all")
			graphics.setCanvas()
			graphics.setShader()
			graphics.origin()
			local text = string.format("%d FPS  %.1f ms", love.timer.getFPS(), shown_ms)
			graphics.setColor(0, 0, 0, 0.6)
			graphics.rectangle("fill", 8, 8, font:getWidth(text) + 16, font:getHeight() + 8)
			graphics.setColor(1, 1, 1, 1)
			graphics.print(text, font, 16, 12)
			graphics.pop()
		end
		return present(...)
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
	local cfg = config()
	if type(cfg.B360_SAFE_AREA) == "number" and love.graphics._b360_setSafeArea then
		love.graphics._b360_setSafeArea(cfg.B360_SAFE_AREA)
	end
	install_perf_tools(cfg)
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
