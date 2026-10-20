-- Prints values that must match LuaJIT 2.1 exactly (see expected_luajit.txt,
-- generated with: luajit tests/rng/main.lua > tests/rng/expected_luajit.txt).
local out = {}
local function p(...) local t={} for i=1,select('#',...) do t[#t+1]=string.format("%.17g", (select(i,...))) end out[#out+1]=table.concat(t," ") end
p(math.random(), math.random(), math.random(1,10))
for _, s in ipairs({0, 1, 1234.5, 0.123456789, 98765.4321, -5, 1e10}) do
  math.randomseed(s); p(math.random(), math.random(), math.random(10), math.random(3, 17), math.random(1, 1000000))
end
p(math.log(1e7, 10), math.log(8, 2), math.log(100, 5), math.log(1000, 10), math.log(1e6*10, 10))
-- Balatro-like pseudohash
local function pseudohash(str) local num = 1 for i=#str, 1, -1 do num = ((1.1239285023/num)*string.byte(str, i)*math.pi + math.pi*i)%1 end return num end
p(pseudohash("Joker1ABCDEFG"), pseudohash("rarity1"), pseudohash("seed"))
local v = math.abs(tonumber(string.format("%.13f", (2.134453429141+0.5123*1.72431234)%1)))
p(v)
print(table.concat(out, "\n"))
if love and love.event then love.event.quit() end
