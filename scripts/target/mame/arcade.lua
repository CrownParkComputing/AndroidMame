-- license:BSD-3-Clause
-- copyright-holders:MAMEdev Team

---------------------------------------------------------------------------
--
--   arcade.lua
--
--   ARCADE64 target wrapper.
--   Use make SUBTARGET=arcade to build.
--
---------------------------------------------------------------------------

dofile(MAME_DIR .. "scripts/target/mame/mame.lua")

function createProjects_mame_arcade(_target, _subtarget)
	createProjects_mame_mame(_target, _subtarget)
end

function linkProjects_mame_arcade(_target, _subtarget)
	linkProjects_mame_mame(_target, _subtarget)
end
