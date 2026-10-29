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
-- First start: the player's Balatro.exe
-------------------------------------------------------------------------------

-- Balatro's loading bar, drawn as its boot_timer() draws it
-- (functions/misc_functions.lua).
local function loading_bar(progress)
	local w, h = love.window.getMode()
	love.graphics.setCanvas()
	love.graphics.push()
	love.graphics.setShader()
	love.graphics.clear(0, 0, 0, 1)
	love.graphics.setColor(0.6, 0.8, 0.9, 1)
	if progress > 0 then love.graphics.rectangle("fill", w/2 - 150, h/2 - 15, progress*300, 30, 5) end
	love.graphics.setColor(1, 1, 1, 1)
	love.graphics.setLineWidth(3)
	love.graphics.rectangle("line", w/2 - 150, h/2 - 15, 300, 30, 5)
	love.graphics.pop()
	love.graphics.present()
	love.graphics.setLineWidth(1)
end

-- Unpacks a Balatro.exe (or Balatro.love) copied beside the executable into
-- the game folder, then deletes it (src/runtime/game_unpack.h). It looks like
-- Balatro's own start-up: its loading bar fills to where the game's first
-- step puts it (0.1) and the game carries on from there.
local function unpack_game()
	local step = love.filesystem._b360_unpackGame
	if not step then return end
	local progress = step(0.05)
	while progress and progress < 1 do
		loading_bar(0.1 * progress)
		progress = step(0.05)
	end
	if progress then loading_bar(0.1) end
end

-- " " to "~" in 8x8 pixels, a row a byte, the low bit leftmost: the public
-- domain font8x8_basic (Daniel Hepper, from IBM's VGA font). The game's own
-- font is among the files that are missing.
local GLYPHS =
	"0000000000000000183c3c1818001800363600000000000036367f367f3636000c3e031e301f0c00006333180c666300" ..
	"1c361c6e3b336e000606030000000000180c0606060c1800060c1818180c060000663cff3c660000000c0c3f0c0c0000" ..
	"00000000000c0c060000003f0000000000000000000c0c006030180c060301003e63737b6f673e000c0e0c0c0c0c3f00" ..
	"1e33301c06333f001e33301c30331e00383c36337f3078003f031f3030331e001c06031f33331e003f3330180c0c0c00" ..
	"1e33331e33331e001e33333e30180e00000c0c00000c0c00000c0c00000c0c06180c0603060c180000003f00003f0000" ..
	"060c1830180c06001e3330180c000c003e637b7b7b031e000c1e33333f3333003f66663e66663f003c66030303663c00" ..
	"1f36666666361f007f46161e16467f007f46161e16060f003c66030373667c003333333f333333001e0c0c0c0c0c1e00" ..
	"7830303033331e006766361e366667000f06060646667f0063777f7f6b63630063676f7b736363001c36636363361c00" ..
	"3f66663e06060f001e3333333b1e38003f66663e366667001e33070e38331e003f2d0c0c0c0c1e003333333333333f00" ..
	"33333333331e0c006363636b7f7763006363361c1c3663003333331e0c0c1e007f6331184c667f001e06060606061e00" ..
	"03060c18306040001e18181818181e00081c36630000000000000000000000ff0c0c18000000000000001e303e336e00" ..
	"0706063e66663b0000001e3303331e003830303e33336e0000001e333f031e001c36060f06060f0000006e33333e301f" ..
	"0706366e666667000c000e0c0c0c1e00300030303033331e070666361e3667000e0c0c0c0c0c1e000000337f7f6b6300" ..
	"00001f333333330000001e3333331e0000003b66663e060f00006e33333e307800003b6e66060f0000003e031e301f00" ..
	"080c3e0c0c2c18000000333333336e0000003333331e0c000000636b7f7f3600000063361c36630000003333333e301f" ..
	"00003f190c263f00380c0c070c0c38001818180018181800070c0c380c0c07006e3b000000000000"

-- love.graphics.points() for lines of text in GLYPHS, each centred on cx,
-- scale pixels to a glyph pixel.
local function text_points(lines, cx, y, scale)
	local points = {}
	for i, line in ipairs(lines) do
		local x = cx - #line * 4 * scale
		for c = 1, #line do
			local glyph = ((line:byte(c) or 32) - 32) * 16
			for row = 0, 7 do
				local bits = tonumber(GLYPHS:sub(glyph + row * 2 + 1, glyph + row * 2 + 2), 16) or 0
				for col = 0, 7 do
					if bits % 2 == 1 then
						points[#points + 1] = x + ((c - 1) * 8 + col + 0.5) * scale
						points[#points + 1] = y + ((i - 1) * 12 + row + 0.5) * scale
					end
					bits = math.floor(bits / 2)
				end
			end
		end
	end
	return points
end

-- Without the game's files: says what to copy, until START or BACK.
local function missing_game()
	local lines = {
		"Balatro360 needs your own copy of Balatro.",
		"",
		"Copy Balatro.exe from your Steam folder (or Balatro.love",
		"from the Mac version) into the folder that holds default.xex,",
		"then start Balatro360 again.",
		"",
		"Press START or BACK to quit.",
	}
	local w, h = love.graphics.getDimensions()
	local scale = 2
	local points = text_points(lines, w / 2, h / 2 - #lines * 6 * scale, scale)
	love.graphics.reset()
	return function()
		love.event.pump()
		for e, a, b in love.event.poll() do
			if e == "quit" then return 1 end
			if e == "gamepadpressed" and (b == "start" or b == "back") then return 1 end
			if e == "keypressed" and a == "escape" then return 1 end
		end
		love.graphics.clear(0, 0, 0, 1)
		love.graphics.setColor(1, 1, 1, 1)
		love.graphics.setPointSize(scale)
		love.graphics.points(points)
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
	unpack_game()
	if not love.filesystem.getInfo("main.lua") then
		func = missing_game()
		return
	end
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
