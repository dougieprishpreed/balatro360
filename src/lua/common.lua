-- Balatro360 common setup, run in every Lua state (main and love.thread).

-- require() searches the game directory through love.filesystem, like LÖVE.
local function love_loader(name)
	local path = name:gsub("%.", "/")
	local candidates = { path .. ".lua", path .. "/init.lua" }
	for _, file in ipairs(candidates) do
		if love.filesystem.getInfo(file, "file") then
			local chunk, err = love.filesystem.load(file)
			if not chunk then error(err, 0) end
			return chunk
		end
	end
	return "\n\tno '" .. path .. ".lua' in the game directory"
end
table.insert(package.loaders, 2, love_loader)

-- Network modules don't exist on the console; give the game harmless stubs.
package.preload["https"] = function()
	return { request = function() return 0, "", {} end }
end
package.preload["luasteam"] = function()
	return { init = function() return false end }
end
