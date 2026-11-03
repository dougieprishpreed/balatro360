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
--     B360_FPS = "auto"       frame rate: 60, 30 or "auto" (60 while the game
--                             keeps up, otherwise a steady 30; the default)
--     B360_SHOW_FPS = true    FPS / frame time overlay in the top left corner
--     B360_LOG_PERF = true    log frame times every 10 s, split into update /
--                             draw / present, with draw call counts
--     B360_PROFILE = true     also log the hottest Lua functions every 30 s
--                             (costs a little speed while enabled)
--     B360_GPU_PROFILE = true log the GPU time of each draw of one frame
--                             every 10 s (xSDK build)
--     B360_HALF_RES_BACKGROUND = false  draw the swirling backgrounds at full
--                             resolution (xSDK build; they're blocks of two
--                             pixels anyway, so half size looks the same)
--     B360_CRT_BLOOM = "original"  the CRT bloom setting's own 49 samples a
--                             pixel instead of 9 (far slower on the console)
--     B360_INTERPOLATE = true at 30 fps, show a frame halfway between each
--                             two (xSDK build; 60 a second, a frame late, and
--                             in-between frames go wrong when cards reorder)
--     B360_SCREENSHOTS = true hold BACK and press RB for a screenshot in
--                             shots\ (xSDK build)
--     B360_RENDER_THREAD = false  draw on the game's thread (xSDK build; also
--                             turns interpolation off)
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
	G.F_VERBOSE = false          -- like Balatro's own console versions
	local name = love.system.getUsername and love.system.getUsername()
	if type(name) == "string" and name ~= "" and love._os == "Xbox 360" then
		G.F_DISP_USERNAME = name
	end

	for k, v in pairs(config()) do
		if type(k) == "string" and k:sub(1, 2) == "F_" then G[k] = v end
	end
end

-- Frame rate (see FramePacer in src/runtime/pacing.cpp).
local function apply_frame_rate(cfg)
	if cfg.B360_INTERPOLATE ~= nil and love.graphics._b360_setInterpolation then
		love.graphics._b360_setInterpolation(cfg.B360_INTERPOLATE and true or false)
	end
	if cfg.B360_SCREENSHOTS and love.graphics._b360_allowScreenshots then
		love.graphics._b360_allowScreenshots(true)
	end
	local setter = love.graphics._b360_setFrameRate
	if not setter then return end
	local fps = cfg.B360_FPS
	if fps == nil then
		if love._os ~= "Xbox 360" then return end   -- host test runs stay unpaced
		fps = "auto"
	end
	local ok, err = pcall(setter, fps)
	if not ok then print("b360_config.lua: B360_FPS: " .. tostring(err)) end
end

-- Optional performance overlay / logging, for testing on hardware.
local function install_perf_tools(cfg)
	local log_perf = cfg.B360_LOG_PERF or cfg.B360_PROFILE
	if cfg.B360_PROFILE and love.graphics._b360_startProfiler then
		love.graphics._b360_startProfiler(1, type(cfg.B360_PROFILE) == "number" and cfg.B360_PROFILE or 30)
	end
	if cfg.B360_GPU_PROFILE and love.graphics._b360_gpuProfile then
		love.graphics._b360_gpuProfile(type(cfg.B360_GPU_PROFILE) == "number" and cfg.B360_GPU_PROFILE or 10)
	end
	local window = type(cfg.B360_LOG_PERF) == "number" and cfg.B360_LOG_PERF or 10
	if not (cfg.B360_SHOW_FPS or log_perf) then return end
	local graphics = love.graphics
	local timer = love.timer
	local present = graphics.present
	local frame_info = graphics._b360_getFrameInfo
	local last = timer.getTime()
	local shown_ms, acc, worst, frames, window_start = 0, 0, 0, 0, last
	local upd, drw, pres, calls, verts = 0, 0, 0, 0, 0
	local font

	-- love.run looks these up every frame, so wrapping them measures the
	-- game's update and draw (Lua plus the C++ work they cause).
	local update, draw = love.update, love.draw
	if log_perf and update and draw then
		love.update = function(...)
			local t = timer.getTime()
			update(...)
			upd = upd + timer.getTime() - t
		end
		love.draw = function(...)
			local t = timer.getTime()
			draw(...)
			drw = drw + timer.getTime() - t
		end
	end

	graphics.present = function(...)
		local now = timer.getTime()
		local ms = (now - last) * 1000
		last = now
		acc = acc + ms
		frames = frames + 1
		if ms > worst then worst = ms end
		shown_ms = shown_ms * 0.9 + ms * 0.1
		if log_perf then
			local st = graphics.getStats()
			calls = calls + st.drawcalls
			verts = verts + (st.b360_vertices or 0)
		end
		if now - window_start >= window then
			if log_perf then
				local vb, _, lerp = 1, 0, false
				if frame_info then vb, _, lerp = frame_info() end
				print(string.format("[perf] %d frames, avg %.1f ms (%.1f fps, target %d%s), worst %.1f ms, Lua heap %.1f MB",
					frames, acc / frames, 1000 * frames / acc, vb == 2 and 30 or 60, lerp and ", shown at 60 interpolated" or "",
					worst, collectgarbage("count") / 1024))
				print(string.format("[perf]   per frame: update %.1f ms, draw %.1f ms, present %.1f ms, %d draw calls, %d vertices",
					upd * 1000 / frames, drw * 1000 / frames, pres * 1000 / frames, calls / frames, verts / frames))
			end
			acc, worst, frames, window_start = 0, 0, 0, now
			upd, drw, pres, calls, verts = 0, 0, 0, 0, 0
		end
		if cfg.B360_SHOW_FPS then
			font = font or graphics.newFont("resources/fonts/m6x11plus.ttf", 24)
			graphics.push("all")
			graphics.setCanvas()
			graphics.setShader()
			graphics.origin()
			local text
			if frame_info then
				local vb, cpu, lerp = frame_info()
				if lerp then
					-- Each game frame and one halfway to it are shown.
					text = string.format("%d FPS (game %d)  %.1f ms  CPU %.1f ms  [30>60]", 2 * timer.getFPS(),
						timer.getFPS(), shown_ms, cpu)
				else
					text = string.format("%d FPS  %.1f ms  CPU %.1f ms  [%s]", timer.getFPS(), shown_ms, cpu,
						vb == 2 and "30" or "60")
				end
			else
				text = string.format("%d FPS  %.1f ms", timer.getFPS(), shown_ms)
			end
			graphics.setColor(0, 0, 0, 0.6)
			graphics.rectangle("fill", 8, 8, font:getWidth(text) + 16, font:getHeight() + 8)
			graphics.setColor(1, 1, 1, 1)
			graphics.print(text, font, 16, 12)
			graphics.pop()
		end
		local t = timer.getTime()
		local r = present(...)
		pres = pres + timer.getTime() - t
		return r
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
	-- Fonts load on first use (Balatro creates one per language, 60+ MB in
	-- total). Load the current language's font while the game is still
	-- starting up, instead of when the main menu first shows text. The
	-- main menu's profile button checks the gamertag's glyphs in the
	-- "all1" font (GoNotoCurrent, 14.5 MB, 0.76 s from USB), so load that
	-- one now too when the gamertag is shown.
	if type(Game.set_language) == "function" then
		local orig_set_language = Game.set_language
		Game.set_language = function(self, ...)
			local r = orig_set_language(self, ...)
			local font = self.LANG and self.LANG.font and self.LANG.font.FONT
			if font then pcall(font.getWidth, font, "0") end
			local all1 = self.F_DISP_USERNAME and self.LANGUAGES and self.LANGUAGES.all1
			font = all1 and all1.font and all1.font.FONT
			if font then pcall(font.getWidth, font, "0") end
			return r
		end
	end
end

-- Two things Balatro keeps for the rest of a run, drawing and updating
-- them every frame although nothing shows: a PC never notices, but on the
-- console a run got slower ante by ante (by ante 8, twice the draw calls
-- and a third more Lua work in every screen than at ante 1).
--
-- Each round's "Cash Out" button is a UIBox of its own whose major is the
-- round summary (functions/common_events.lua); removing the summary leaves
-- it in G.I.UIBOX, off screen. A UIBox removed here takes the UIBoxes
-- aligned to it along.
hooks["engine/ui"] = function()
	if type(UIBox) ~= "table" or type(UIBox.remove) ~= "function" then return end
	local remove = UIBox.remove
	UIBox.remove = function(self, ...)
		local r = remove(self, ...)
		local list = G and G.I and G.I.UIBOX
		if list then
			local minors
			for _, box in ipairs(list) do
				if box ~= self and box.config and box.config.major == self then
					minors = minors or {}
					minors[#minors + 1] = box
				end
			end
			if minors then
				for _, box in ipairs(minors) do
					if not box.REMOVED then box:remove() end
				end
			end
		end
		return r
	end
end

-- Each defeated blind leaves its dissolve particles (blind.lua,
-- Blind:defeat) among the run-long Blind's children and in G.I.MOVEABLE,
-- emptied after a second. Remove the last defeat's before the next.
hooks["blind"] = function()
	if type(Blind) ~= "table" or type(Blind.defeat) ~= "function" then return end
	local defeat = Blind.defeat
	Blind.defeat = function(self, ...)
		local old = self.children and self.children.particles
		if type(old) == "table" and type(old.remove) == "function" and not old.REMOVED then
			old:remove()
			self.children.particles = nil
		end
		return defeat(self, ...)
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

-- The swirling backgrounds (background.fs in a run, splash.fs on the title
-- screen) cover the screen in blocks of about two pixels, and are each
-- frame's most costly draw after the CRT pass: draw them at half size.
local HALF_RES_SHADERS = { ["background.fs"] = true, ["splash.fs"] = true }

-- The CRT effect's bloom (Settings > Graphics) sums a 7x7 grid of samples
-- for every pixel of the screen: 49 texture reads in a loop, about 75 ms a
-- frame on the console. These 9 reads, every other one of the grid's inner
-- 5x5, weighted to give the same total, look nearly the same for a soft
-- glow. B360_CRT_BLOOM = "original" keeps the game's loop.
-- Each tap is the game's: the darkest channel past 0.6, stretched to 0..1
-- (max(2.5*c - 1.5, 0) per channel, then the minimum, is the same thing).
-- Its weight is the game's 2 - |i+j|/6 for the tap's place in the grid,
-- times 79.33/15.33 (the 7x7's weights over these 9's), over 9.
local BLOOM_TAPS = {
	{ -2, -2, "0.766506" }, { 0, -2, "0.958132" }, { 2, -2, "1.149758" },
	{ -2, 0, "0.958132" }, { 0, 0, "1.149758" }, { 2, 0, "0.958132" },
	{ -2, 2, "1.149758" }, { 0, 2, "0.958132" }, { 2, 2, "0.766506" },
}
local function bloom_code()
	local lines = {
		"        MY_HIGHP_OR_MEDIUMP vec4 samp;",
		"        MY_HIGHP_OR_MEDIUMP float glow = 0.0;",
	}
	for _, t in ipairs(BLOOM_TAPS) do
		lines[#lines + 1] = string.format("        samp = Texel(tex, tc + vec2(%.4f, %.4f));", 0.0015 * t[1], 0.0015 * t[2])
		lines[#lines + 1] = "        glow += max(2.5*min(min(samp.r, samp.g), samp.b) - 1.5, 0.)*" .. t[3] .. ";"
	end
	lines[#lines + 1] = "        col = vec4(glow);"
	return table.concat(lines, "\n") .. "\n"
end

local function fast_bloom(source)
	local first = source:find("MY_HIGHP_OR_MEDIUMP float bloom_dist", 1, true)
	local last_s, last_e = source:find("col /= float(BLOOM_AMT*BLOOM_AMT);", 1, true)
	if not first or not last_s or last_s < first then return nil end
	-- Back to the start of the line, so the replacement keeps its indent.
	while first > 1 and source:sub(first - 1, first - 1) ~= "\n" do first = first - 1 end
	return source:sub(1, first - 1) .. bloom_code() .. source:sub(last_e + 1)
end

local function install_shader_hooks(cfg)
	local graphics = love.graphics
	local half_res = cfg.B360_HALF_RES_BACKGROUND ~= false and graphics._b360_setHalfResolution
	local bloom = cfg.B360_CRT_BLOOM ~= "original"
	local new_shader = graphics.newShader
	graphics.newShader = function(code, ...)
		local file = type(code) == "string" and not code:find("\n") and code:match("([^/]+)$")
		if file == "CRT.fs" and bloom then
			local source = love.filesystem.read(code)
			local patched = source and fast_bloom(source)
			if patched then
				code = patched
			else
				print("[b360] CRT.fs: the bloom loop wasn't found; drawing the game's own")
			end
		end
		local shader = new_shader(code, ...)
		if half_res and file and HALF_RES_SHADERS[file] then
			graphics._b360_setHalfResolution(shader, true)
		end
		return shader
	end
end

-- Called by boot.lua after main.lua (and every module it requires) loaded.
function love._b360_postmain()
	local copied, n = flatten_classes()
	print(string.format("[b360] flattened %d methods into %d classes", copied or 0, n or 0))
	local cfg = config()
	if type(cfg.B360_SAFE_AREA) == "number" and love.graphics._b360_setSafeArea then
		love.graphics._b360_setSafeArea(cfg.B360_SAFE_AREA)
	end
	apply_frame_rate(cfg)
	install_perf_tools(cfg)
	install_shader_hooks(cfg)
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
