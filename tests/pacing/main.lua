-- Frame pacing test: busy-waits a scripted amount of "game" time per frame.
-- Run with the null renderer and its emulated vsync (docs/HOST_TESTING.md):
--   B360_NULL_VSYNC=1 B360_GAME_DIR=tests/pacing B360_SAVE_DIR=/tmp/p out/host-null/b360
local phases = {
	{ 3, 0.010, "light (10 ms)" },
	{ 4, 0.022, "heavy (22 ms)" },
	{ 6, 0.008, "light (8 ms)" },
	{ 6, 0.015, "borderline (15 ms, misses now and then)" },
	{ 4, 0.040, "very heavy (40 ms)" },
}
local start, phase, frames, phase_start = nil, 1, 0, nil
local last

function love.load()
	love.graphics._b360_setFrameRate("auto")
end

function love.update(dt)
	local now = love.timer.getTime()
	start = start or now
	phase_start = phase_start or now
	local p = phases[phase]
	if now - phase_start > p[1] then
		local vb = love.graphics._b360_getFrameInfo()
		print(string.format("phase %d %s: %d frames in %.1f s (%.1f fps), ends at interval %d",
			phase, p[3], frames, now - phase_start, frames / (now - phase_start), vb))
		phase, frames, phase_start = phase + 1, 0, now
		if not phases[phase] then love.event.quit() return end
		p = phases[phase]
	end
	frames = frames + 1
	local work = p[2]
	if phase == 4 and frames % 7 == 0 then work = 0.020 end
	local t = love.timer.getTime()
	while love.timer.getTime() - t < work do end
end

function love.draw() end
