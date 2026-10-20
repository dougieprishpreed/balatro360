-- Balatro360 boot script (embedded into the executable).
--
-- Plays the role of LÖVE's boot.lua: sets up require() through
-- love.filesystem, reads conf.lua, loads main.lua and runs love.run()
-- with LÖVE 11 error handling semantics.

local love = love

love._version = "11.5"
love._version_major = 11
love._version_minor = 5
love._version_revision = 0
love._version_codename = "Mysterious Mysteries"
love._os = love.system.getOS()
love._b360 = true

function love.getVersion()
	return love._version_major, love._version_minor, love._version_revision, love._version_codename
end

function love.isVersionCompatible(...)
	return true
end

-------------------------------------------------------------------------------
-- Command line / arguments
-------------------------------------------------------------------------------

arg = arg or {}
love.arg = {
	parseGameArguments = function(a)
		local out = {}
		for i, v in ipairs(a or {}) do out[i] = v end
		return out
	end,
	parseOptions = function() end,
	getLow = function(a) return 0, a and a[0] end,
}

-------------------------------------------------------------------------------
-- Event handlers (same set as LÖVE 11)
-------------------------------------------------------------------------------

love.handlers = setmetatable({
	keypressed = function(b, s, r) if love.keypressed then return love.keypressed(b, s, r) end end,
	keyreleased = function(b, s) if love.keyreleased then return love.keyreleased(b, s) end end,
	textinput = function(t) if love.textinput then return love.textinput(t) end end,
	textedited = function(t, s, l) if love.textedited then return love.textedited(t, s, l) end end,
	mousemoved = function(x, y, dx, dy, t) if love.mousemoved then return love.mousemoved(x, y, dx, dy, t) end end,
	mousepressed = function(x, y, b, t, c) if love.mousepressed then return love.mousepressed(x, y, b, t, c) end end,
	mousereleased = function(x, y, b, t, c) if love.mousereleased then return love.mousereleased(x, y, b, t, c) end end,
	wheelmoved = function(x, y) if love.wheelmoved then return love.wheelmoved(x, y) end end,
	touchpressed = function(id, x, y, dx, dy, p) if love.touchpressed then return love.touchpressed(id, x, y, dx, dy, p) end end,
	touchreleased = function(id, x, y, dx, dy, p) if love.touchreleased then return love.touchreleased(id, x, y, dx, dy, p) end end,
	touchmoved = function(id, x, y, dx, dy, p) if love.touchmoved then return love.touchmoved(id, x, y, dx, dy, p) end end,
	joystickpressed = function(j, b) if love.joystickpressed then return love.joystickpressed(j, b) end end,
	joystickreleased = function(j, b) if love.joystickreleased then return love.joystickreleased(j, b) end end,
	joystickaxis = function(j, a, v) if love.joystickaxis then return love.joystickaxis(j, a, v) end end,
	joystickhat = function(j, h, v) if love.joystickhat then return love.joystickhat(j, h, v) end end,
	gamepadpressed = function(j, b) if love.gamepadpressed then return love.gamepadpressed(j, b) end end,
	gamepadreleased = function(j, b) if love.gamepadreleased then return love.gamepadreleased(j, b) end end,
	gamepadaxis = function(j, a, v) if love.gamepadaxis then return love.gamepadaxis(j, a, v) end end,
	joystickadded = function(j) if love.joystickadded then return love.joystickadded(j) end end,
	joystickremoved = function(j) if love.joystickremoved then return love.joystickremoved(j) end end,
	focus = function(f) if love.focus then return love.focus(f) end end,
	mousefocus = function(f) if love.mousefocus then return love.mousefocus(f) end end,
	visible = function(v) if love.visible then return love.visible(v) end end,
	quit = function() return end,
	threaderror = function(t, err)
		if love.threaderror then return love.threaderror(t, err) end
	end,
	resize = function(w, h) if love.resize then return love.resize(w, h) end end,
	filedropped = function(f) if love.filedropped then return love.filedropped(f) end end,
	directorydropped = function(d) if love.directorydropped then return love.directorydropped(d) end end,
	lowmemory = function() if love.lowmemory then love.lowmemory() end collectgarbage() collectgarbage() end,
	displayrotated = function(d, o) if love.displayrotated then return love.displayrotated(d, o) end end,
}, {
	__index = function(self, name)
		error("Unknown event: " .. name)
	end,
})

function love.createhandlers() end

-------------------------------------------------------------------------------
-- Default love.run / error handler (games usually override love.run)
-------------------------------------------------------------------------------

function love.run()
	if love.load then love.load(love.arg.parseGameArguments(arg), arg) end
	if love.timer then love.timer.step() end
	local dt = 0
	return function()
		if love.event then
			love.event.pump()
			for name, a, b, c, d, e, f in love.event.poll() do
				if name == "quit" then
					if not love.quit or not love.quit() then
						return a or 0
					end
				end
				love.handlers[name](a, b, c, d, e, f)
			end
		end
		if love.timer then dt = love.timer.step() end
		if love.update then love.update(dt) end
		if love.graphics and love.graphics.isActive() then
			love.graphics.origin()
			love.graphics.clear(love.graphics.getBackgroundColor())
			if love.draw then love.draw() end
			love.graphics.present()
		end
		if love.timer then love.timer.sleep(0.001) end
	end
end

local function error_printer(msg, layer)
	local trace = debug.traceback("Error: " .. tostring(msg), 1 + (layer or 1)):gsub("\n[^\n]+$", "")
	print(trace)
	return trace
end

-- Fallback error screen used when the game does not define its own.
function love.errorhandler(msg)
	msg = tostring(msg)
	local trace = error_printer(msg, 2)
	if not love.graphics then return end
	love.graphics.reset()
	local ok, font = pcall(love.graphics.setNewFont, 14)
	love.graphics.setColor(1, 1, 1, 1)
	local text = "Error\n\n" .. trace .. "\n\nPress START or BACK to quit."
	text = text:gsub("\t", "    ")
	return function()
		love.event.pump()
		for e, a, b in love.event.poll() do
			if e == "quit" then return 1 end
			if e == "gamepadpressed" and (b == "start" or b == "back") then return 1 end
			if e == "keypressed" and a == "escape" then return 1 end
		end
		love.graphics.clear(0.35, 0.1, 0.1, 1)
		love.graphics.printf(text, 70, 70, love.graphics.getWidth() - 140)
		love.graphics.present()
		if love.timer then love.timer.sleep(0.1) end
	end
end

-------------------------------------------------------------------------------
-- Boot
-------------------------------------------------------------------------------

local function run_conf()
	local c = {
		title = "Untitled",
		version = love._version,
		window = { width = 800, height = 600, x = nil, y = nil, minwidth = 1, minheight = 1,
			fullscreen = true, fullscreentype = "desktop", display = 1, vsync = 1, msaa = 0,
			borderless = false, resizable = false, centered = true, highdpi = false, usedpiscale = true },
		modules = {},
		audio = { mixwithsystem = true, mic = false },
		console = false,
		identity = false,
		appendidentity = false,
		externalstorage = false,
		accelerometerjoystick = true,
		gammacorrect = false,
	}
	if love.filesystem.getInfo("conf.lua") then
		local ok, err = pcall(require, "conf")
		if not ok then print("Error loading conf.lua: " .. tostring(err)) end
	end
	if love.conf then
		local ok, err = pcall(love.conf, c)
		if not ok then print("Error in love.conf: " .. tostring(err)) end
	end
	if c.window and c.window.title then c.title = c.window.title end
	love.window.setTitle(c.title or "Untitled")
	love.conf_table = c
end

local function boot()
	run_conf()
	if love._b360_hooks then love._b360_hooks() end
	require("main")
	if love._b360_postmain then love._b360_postmain() end
end

local inerror = false
local func

local function deferErrhand(...)
	-- Prefer a game-defined love.errhand (Balatro defines one) over the
	-- default love.errorhandler above.
	local errhand = love.errhand or love.errorhandler
	if inerror then
		-- Error inside the error handler: just print it.
		error_printer(..., 2)
		func = nil
		return
	end
	inerror = true
	local ok, result = pcall(errhand, ...)
	inerror = false
	if ok and type(result) == "function" then
		func = result
	else
		if not ok then error_printer(result, 1) end
		-- Old style (LÖVE 0.10) handlers loop by themselves and return
		-- when the user quits.
		func = nil
	end
end

local function earlyinit()
	local ok = xpcall(boot, deferErrhand)
	if not ok then return end
	local main
	ok, main = xpcall(love.run, deferErrhand)
	if ok then func = main end
end

func = earlyinit
while func do
	local _, retval = xpcall(func, deferErrhand)
	if retval then
		if love.audio then love.audio.stop() end
		return retval
	end
end

return 1
