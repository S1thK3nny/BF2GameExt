// Lua 5.0 chunk run in the shell's and the mission's Lua state by
// menu_navigation.cpp. Split into pieces: MSVC caps a string literal at 16 KB.
static const char kShellPadLua[] =
R"LUA(
-- BF2GameExt: gamepad navigation for the PC menus.
if (GameExt_PadUI) then return end
if ((type(ifs_login) ~= "table") and (type(ifs_pausemenu) ~= "table")) then return end
GameExt_PadUI = 1

local SKIP_TABS = { _tab_mini = 1, _tab_quit = 1, }
local layoutCache = {}
local tagCache = {}
local S = {}
local Configs = {}
local TrigTabs = {} -- screen -> tab row driven by LT/RT only
local bypass = nil
local trigLast = { 0, 0, }
local pulseTime = 0

local function vis(obj)
	if ((not obj) or obj.hidden) then return nil end
	return IFObj_fnGetVis(obj)
end

local function sound(name)
	if (ifelm_shellscreen_fnPlaySound) then ifelm_shellscreen_fnPlaySound(name) end
end

-- Forget what the mouse is over, so Accept acts on the pad focus.
local function ClearMouse()
	gMouseListBox = nil
	gMouseListBoxSlider = nil
	gMouseOverSlider = nil
	gMouseOverHorz = nil
	gMouseOverImage = nil
	gMouseOverButtonWindow = nil
	gMouseOverEditbox = nil
end

-- Run the screen's own Accept as if the mouse were on 'tag'.
local function Activate(this, tag)
	ClearMouse()
	this.CurButton = tag
	if (gCurScreenTable) then gCurScreenTable.CurButton = tag end
	if (not this.Input_Accept) then return end
	bypass = 1
	local ok, err = pcall(this.Input_Accept, this)
	bypass = nil
	if (not ok) then print("GameExt pad: " .. tostring(err)) end
end

-- Which g*TabsLayout built this tab row. Largest match wins.
local function TabLayout(tabs)
	if (layoutCache[tabs]) then return layoutCache[tabs] end
	local best, bestN = nil, 0
	local name, lay
	for name, lay in pairs(_G) do
		local lname = (type(name) == "string") and string.lower(name)
		if (lname and (type(lay) == "table") and (string.find(lname, "tabslayout", 1, 1) or string.find(lname, "tab_layout", 1, 1))) then
			local n = table.getn(lay)
			if ((n > bestN) and (type(lay[1]) == "table") and lay[1].tag) then
				local ok = 1
				local i
				for i = 1, n do
					if ((type(lay[i]) ~= "table") or (not tabs[lay[i].tag])) then
						ok = nil
						break
					end
				end
				if (ok) then
					best = lay
					bestN = n
				end
			end
		end
	end
	layoutCache[tabs] = best
	return best
end

local function RowUsable(tabs, tag)
	local t = tabs[tag]
	return t and (not SKIP_TABS[tag]) and (t.bDimmed ~= 1) and vis(t)
end

-- Visible tab bars, top first: LB/RB drive the first, LT/RT the second.
local function TabRows(this)
	local out = {}
	local names = { "_Tabs", "_Tabs1", "_Tabs2", "_Tabs3", }
	local i
	for i = 1, 4 do
		local tabs = this[names[i]]
		if (tabs and vis(tabs) and (TrigTabs[this] ~= names[i])) then
			local lay = TabLayout(tabs)
			if (lay and (lay ~= gPCMultiPlayerSettingsTabsLayout) and (lay ~= gPCMinimizeQuitTabsLayout)) then
				table.insert(out, tabs)
			end
		end
	end
	return out
end

local function RowFor(this, which)
	local cfg = Configs[this]
	local pages = (cfg and cfg.pages) or TrigTabs[this]
	if ((which == 2) and pages) then
		local tabs = this[pages]
		if (tabs and vis(tabs)) then return tabs end
	end
	return TabRows(this)[which]
end

-- Returns 1, or nil and why not.
local function CycleRow(this, tabs, dir)
	if ((not tabs) or (not vis(tabs))) then return nil, "no visible tab row" end
	local lay = TabLayout(tabs)
	if (not lay) then return nil, "tab row matches no layout" end
	local n = table.getn(lay)
	local cur = nil
	local i
	for i = 1, n do
		local t = tabs[lay[i].tag]
		if (t and t.bSelected) then cur = i end
	end
	if (not cur) then
		if (dir > 0) then cur = 0 else cur = n + 1 end
	end
	i = cur
	local step
	for step = 1, n do
		i = i + dir
		if (i < 1) then i = n end
		if (i > n) then i = 1 end
		if (i == cur) then return nil, "no other usable tab" end
		if (RowUsable(tabs, lay[i].tag)) then
			Activate(this, lay[i].tag)
			return 1
		end
	end
	return nil, "no usable tab"
end

local function ScreenName(this)
	local k, v
	for k, v in pairs(_G) do
		if (v == this) then return tostring(k) end
	end
	return "?"
end

-- Edge-detect the triggers here: the engine's menu input re-fires an analog
-- axis every time its value moves.
local function PollTriggers(this)
	if (not GameExt_PadTriggers) then return end
	local lt, rt = GameExt_PadTriggers()
	local ltDown = (lt == 1) and (trigLast[1] ~= 1)
	local rtDown = (rt == 1) and (trigLast[2] ~= 1)
	trigLast[1] = lt
	trigLast[2] = rt
	if (gCurPopup or ((not ltDown) and (not rtDown))) then return end
	local ok, why = CycleRow(this, RowFor(this, 2), rtDown and 1 or -1)
	if (not ok) then print("GameExt pad: LT/RT did nothing on " .. ScreenName(this) .. ": " .. tostring(why)) end
end

local function FindTag(root, tag, depth, seen)
	if (depth > 5) then return nil end
	local k, v
	for k, v in pairs(root) do
		if ((type(v) == "table") and (not seen[v])) then
			seen[v] = 1
			if ((v.tag == tag) and v.cp) then return v end
			local r = FindTag(v, tag, depth + 1, seen)
			if (r) then return r end
		end
	end
	return nil
end

local function TagObj(this, root, tag)
	local key = tostring(this) .. tag
	if (tagCache[key] == nil) then
		tagCache[key] = (root and FindTag(root, tag, 0, {})) or false
	end
	return tagCache[key] or nil
end

-- The visible ones of 'tags', as zone items.
local function TagItems(this, root, tags)
	local out = {}
	local i
	for i = 1, table.getn(tags) do
		local obj = TagObj(this, root, tags[i])
		if (obj and vis(obj) and (not obj.bDimmed)) then table.insert(out, { tag = tags[i], obj = obj, }) end
	end
	return out
end
)LUA"
R"LUA(
---------------------------------------------------------------------------
-- Zones: a screen is a list of zones, the pad focus is one item in one zone.
-- cross = "h": left/right changes zone. cross = "v": up/down changes zone.
-- The focus is shown exactly the way mouse hover is.
---------------------------------------------------------------------------

local function FormItems(this, z)
	local out = {}
	local forms = z.forms(this)
	local fi
	for fi = 1, table.getn(forms) do
		local form = forms[fi]
		if (form and form.layout and form.elements) then
			local els = form.layout.elements
			local i
			for i = 1, table.getn(els) do
				local tag = els[i].tag
				local el = form.elements[tag]
				if (el and (not el.hidden)) then
					table.insert(out, { tag = tag, form = form, el = el, obj = (form.text and form.text[tag]) or form.buttons[tag], })
				end
			end
		end
	end
	return out
end

local function RowItems(this, rowname)
	local out = {}
	local tabs = this[rowname]
	if ((not tabs) or (not vis(tabs))) then return out end
	local lay = TabLayout(tabs)
	if (not lay) then return out end
	local i
	for i = 1, table.getn(lay) do
		if (RowUsable(tabs, lay[i].tag)) then
			table.insert(out, { tag = lay[i].tag, obj = tabs[lay[i].tag], })
		end
	end
	return out
end

local function ZoneItems(this, z)
	if (z.kind == "row") then return RowItems(this, z.row) end
	if ((z.kind == "items") or (z.kind == "grid")) then return z.items(this) end
	if (z.kind == "form") then return FormItems(this, z) end
	return {}
end

local function ZoneList(this, z)
	local lb, contents, layout = z.list(this)
	if ((not lb) or (not layout) or (not contents) or (not vis(lb))) then return nil end
	local n = table.getn(contents)
	if (n < 1) then return nil end
	if ((not layout.SelectedIdx) or (layout.SelectedIdx > n)) then
		layout.SelectedIdx = layout.LastSelectedIdx or 1
		if (layout.SelectedIdx > n) then layout.SelectedIdx = 1 end
	end
	return lb, contents, layout
end

local function ZoneCount(this, z)
	if (z.kind == "list") then
		if (ZoneList(this, z)) then return 1 end
		return 0
	end
	if (z.kind == "dropdown") then
		if (vis(z.button(this))) then return 1 end
		return 0
	end
	return table.getn(ZoneItems(this, z))
end

-- The axis that moves inside a zone.
local function Inner(cfg, z)
	if ((z.kind == "list") or (z.kind == "form") or (z.kind == "row")) then return "v" end
	if ((z.kind == "dropdown") or (z.kind == "grid")) then return nil end
	if (cfg.cross == "h") then return "v" end
	return "h"
end

local function St(this)
	local st = S[this]
	if (not st) then
		st = { pos = {}, }
		S[this] = st
	end
	return st
end

local function Unfocus(st)
	if (st.btn) then
		IFButton_fnSelect(st.btn, nil)
		if (gCurHiliteButton == st.btn) then gCurHiliteButton = nil end
		st.btn = nil
	end
	if (st.img) then
		if (gMouseOverImage == st.img) then gMouseOverImage = nil end
		st.img = nil
	end
	if (st.pulse) then
		IFObj_fnSetAlpha(st.pulse, 1)
		st.pulse = nil
	end
	st.list = nil
end

-- Same calls the mouse hover handlers make.
local function FocusObj(this, st, obj, tag)
	if (not obj) then return end
	if ((obj.type == "button") or obj.label or obj.skin) then
		if (obj.tag) then SetCurButtonTable(obj) else IFButton_fnSelect(obj, 1) end
		st.btn = obj
	elseif ((obj.type == "image") or (obj.type == "maskimage")) then
		this.CurButton = tag
		gMouseOverImage = obj
		if (this.UpdateUIMouseOver) then this.UpdateUIMouseOver(this, obj) end
		st.img = obj
		if (not obj.bIsFlashObj) then st.pulse = obj end
	else
		st.pulse = obj
	end
end

local function Focus(this, cfg, st)
	Unfocus(st)
	local z = cfg.zones[st.zone]
	if (not z) then return end
	if (z.kind == "list") then
		st.list = ZoneList(this, z)
		return
	end
	if (z.kind == "dropdown") then
		if (z.isopen(this)) then
			st.list = ZoneList(this, z)
		else
			FocusObj(this, st, z.button(this), z.tag)
		end
		return
	end
	local items = ZoneItems(this, z)
	local n = table.getn(items)
	if (n < 1) then return end
	local p = st.pos[st.zone]
	if (not p) then
		p = 1
		local i
		for i = 1, n do
			if (z.default and (items[i].tag == z.default)) then p = i end
		end
	end
	if (p > n) then p = n end
	st.pos[st.zone] = p
	FocusObj(this, st, items[p].obj, items[p].tag)
end

local function ListCursors(this, cfg, alpha, keep)
	local i
	for i = 1, table.getn(cfg.zones) do
		local z = cfg.zones[i]
		if (z.kind == "list") then
			local lb = z.list(this)
			if (lb and lb.cursor and (lb ~= keep)) then IFObj_fnSetAlpha(lb.cursor, alpha) end
		end
	end
end

local function Deactivate(this)
	local st = S[this]
	if (st and st.active) then
		Unfocus(st)
		ListCursors(this, Configs[this], 1)
		st.active = nil
	end
end

-- Start where the mouse was, else at cfg.start.
local function StartZone(this, cfg, st)
	local hoverTag, hoverList = this.CurButton, gMouseListBox
	local i, p
	for i = 1, table.getn(cfg.zones) do
		local z = cfg.zones[i]
		if (z.kind == "list") then
			if (hoverList and (z.list(this) == hoverList) and ZoneList(this, z)) then return i end
		elseif (hoverTag and (z.kind ~= "dropdown")) then
			local items = ZoneItems(this, z)
			for p = 1, table.getn(items) do
				if (items[p].tag == hoverTag) then
					st.pos[i] = p
					return i
				end
			end
		end
	end
	if (st.zone and cfg.zones[st.zone] and (ZoneCount(this, cfg.zones[st.zone]) > 0)) then return st.zone end
	if (cfg.start and (ZoneCount(this, cfg.zones[cfg.start]) > 0)) then return cfg.start end
	for i = 1, table.getn(cfg.zones) do
		if (ZoneCount(this, cfg.zones[i]) > 0) then return i end
	end
	return nil
end

-- First pad input on a screen only shows the focus.
local function Engage(this, cfg)
	local st = St(this)
	if (st.active) then return st, nil end
	local z = StartZone(this, cfg, st)
	if (not z) then return nil, nil end
	st.zone = z
	st.active = 1
	ClearMouse()
	SetCurButton(nil, this)
	Focus(this, cfg, st)
	return st, 1
end

local function MoveZone(this, cfg, st, dir)
	local n = table.getn(cfg.zones)
	local z = st.zone
	local i
	for i = 1, n do
		z = z + dir
		if ((z < 1) or (z > n)) then return nil end
		local count = ZoneCount(this, cfg.zones[z])
		if (count > 0) then
			st.zone = z
			-- Spilling up/down lands on the near edge of the next zone.
			if (cfg.spill and (Inner(cfg, cfg.zones[z]) == cfg.cross)) then
				if (dir > 0) then st.pos[z] = 1 else st.pos[z] = count end
			end
			sound("shell_select_change")
			Focus(this, cfg, st)
			return 1
		end
	end
	return nil
end

local function MoveList(this, z, dir)
	local lb, contents, layout = ZoneList(this, z)
	if (not lb) then return nil end
	local before = layout.SelectedIdx
	if (dir < 0) then
		ListManager_fnNavUp(lb, contents, layout)
	else
		ListManager_fnNavDown(lb, contents, layout)
	end
	if (layout.SelectedIdx == before) then return nil end
	if (z.moved) then z.moved(this, lb, contents, layout) end
	return 1
end

local function MoveIn(this, cfg, st, dir)
	local z = cfg.zones[st.zone]
	if (z.kind == "list") then return MoveList(this, z, dir) end
	if (z.kind == "dropdown") then return nil end
	local items = ZoneItems(this, z)
	local p = (st.pos[st.zone] or 1) + dir
	if ((p < 1) or (p > table.getn(items))) then return nil end
	st.pos[st.zone] = p
	sound("shell_select_change")
	Focus(this, cfg, st)
	return 1
end

local function Center(obj)
	return (obj.x or 0) + (obj.hotspot_x or ((obj.localpos_r or 0) / 2)), (obj.y or 0)
end

-- A grid zone moves to the nearest item on screen in that direction.
local function MoveGrid(this, cfg, st, z, axis, dir)
	local items = ZoneItems(this, z)
	local cur = items[st.pos[st.zone] or 1]
	if (not cur) then return nil end
	local cx, cy = Center(cur.obj)
	local best, bestCost = nil, nil
	local i
	for i = 1, table.getn(items) do
		local x, y = Center(items[i].obj)
		local cost = nil
		if (axis == "h") then
			if ((x - cx) * dir > 5) then cost = (x - cx) * dir + 4 * math.abs(y - cy) end
		elseif ((y - cy) * dir > 5) then
			cost = (y - cy) * dir + 0.5 * math.abs(x - cx)
		end
		if (cost and ((not bestCost) or (cost < bestCost))) then
			best = i
			bestCost = cost
		end
	end
	if (not best) then return nil end
	st.pos[st.zone] = best
	sound("shell_select_change")
	Focus(this, cfg, st)
	return 1
end
)LUA"
R"LUA(
local function FormChanged(item)
	if (item.el.fnChanged) then item.el.fnChanged(item.form, item.el) end
	Form_SetValues(item.form)
	sound("shell_select_change")
end

-- Left/right on a setting: the same change a click or drag makes.
local function FormAdjust(item, dir, wrap)
	local el = item.el
	if (el.control == "slider") then
		local range = el.maxValue - el.minValue
		local step = range / 20
		if (range >= 20) then step = math.max(1, math.floor(step)) end
		el.selValue = Form_ClampValue((el.selValue or el.minValue) + dir * step, el.minValue, el.maxValue)
		FormChanged(item)
	elseif (el.control == "radio") then
		local grp = item.form.radiobuttons[item.tag]
		local n = table.getn(grp)
		local v = ifelem_GetSelectedRadioButton(grp) + dir
		if (wrap and (v > n)) then v = 1 end
		if ((v >= 1) and (v <= n)) then
			ifelem_SelectRadioButton(grp, v)
			sound("shell_select_change")
		end
	elseif (el.values) then
		local n = table.getn(el.values)
		if (n < 1) then return end
		local v = (el.selValue or 1) + dir
		if ((el.control == "button") or wrap) then
			if (v > n) then v = 1 end
			if (v < 1) then v = n end
		end
		if ((v >= 1) and (v <= n) and (v ~= el.selValue)) then
			el.selValue = v
			FormChanged(item)
		end
	end
end

local function DropClose(this, cfg, st, z, cancel)
	local lb, contents, layout = z.list(this)
	if (cancel and st.dropSaved) then layout.SelectedIdx = st.dropSaved end
	ClearMouse()
	gMouseListBox = lb
	layout.CursorIdx = layout.SelectedIdx
	cfg.dropclose(this)
	gMouseListBox = nil
	layout.CursorIdx = nil
	st.dropSaved = nil
	Focus(this, cfg, st)
end

-- Returns 1 when the pad layer consumed the input.
local function Nav(this, cfg, axis, dir)
	if (gCurPopup) then return nil end
	-- The mouse wheel sends Up/Down too: over a list it scrolls that list.
	if ((axis == "v") and gMouseListBox and GameExt_PadDir and (GameExt_PadDir() ~= 1)) then
		Deactivate(this)
		return nil
	end
	local st, fresh
	st, fresh = Engage(this, cfg)
	if (not st) then return nil end
	if (fresh) then return 1 end
	local z = cfg.zones[st.zone]
	if ((z.kind == "dropdown") and z.isopen(this)) then
		if (axis == "v") then MoveList(this, z, dir) end
		return 1
	end
	if ((z.kind == "form") and (axis == "h")) then
		local item = ZoneItems(this, z)[st.pos[st.zone] or 1]
		if (item) then FormAdjust(item, dir) end
		return 1
	end
	if (z.kind == "grid") then
		if ((not MoveGrid(this, cfg, st, z, axis, dir)) and (axis == cfg.cross)) then MoveZone(this, cfg, st, dir) end
		return 1
	end
	if (axis == Inner(cfg, z)) then
		if ((not MoveIn(this, cfg, st, dir)) and cfg.spill and (axis == cfg.cross)) then
			MoveZone(this, cfg, st, dir)
		end
	elseif (axis == cfg.cross) then
		MoveZone(this, cfg, st, dir)
	end
	return 1
end

local function Accept(this, cfg, st)
	local z = cfg.zones[st.zone]
	if (not z) then return end
	if (z.kind == "list") then
		local lb, contents, layout = ZoneList(this, z)
		if (lb and z.accept) then z.accept(this, lb, contents, layout) end
		return
	end
	if (z.kind == "dropdown") then
		if (z.isopen(this)) then
			DropClose(this, cfg, st, z, nil)
		else
			local lb, contents, layout = z.list(this)
			st.dropSaved = layout and layout.SelectedIdx
			Activate(this, z.tag)
			Focus(this, cfg, st)
		end
		return
	end
	local items = ZoneItems(this, z)
	local item = items[st.pos[st.zone] or 1]
	if (not item) then return end
	if (z.kind == "form") then
		FormAdjust(item, 1, 1)
		return
	end
	Activate(this, item.tag)
	if (S[this] and S[this].active) then Focus(this, cfg, st) end
end

local function PadActive(this)
	local st = S[this]
	return st and st.active and (not gCurPopup) and (not bypass)
end

-- The pad's own button, not the right mouse button, which is Misc too.
local function PadDown(n)
	return GameExt_PadButton and (GameExt_PadButton(n) == 1)
end

local function Shortcut(scr, name, tag, n)
	if ((not tag) or scr[name]) then return end
	scr[name] = function(this)
		if ((PadActive(this) or PadDown(n)) and (not gCurPopup)) then Activate(this, tag) end
	end
end

local function Attach(scr, cfg)
	if (type(scr) ~= "table") then return end
	Configs[scr] = cfg
	local orig = {}
	local names = { "Enter", "Update", "HandleMouse", "Input_KeyDown", "Input_Accept", "Input_Back",
		"Input_GeneralUp", "Input_GeneralDown", "Input_GeneralLeft", "Input_GeneralRight", }
	local i
	for i = 1, table.getn(names) do orig[names[i]] = scr[names[i]] end

	scr.Enter = function(this, bFwd)
		Deactivate(this)
		if (orig.Enter) then return orig.Enter(this, bFwd) end
	end
	scr.Update = function(this, fDt)
		if (orig.Update) then orig.Update(this, fDt) end
		local st = S[this]
		if (st and st.active) then
			pulseTime = pulseTime + (fDt or 0)
			if (st.pulse) then IFObj_fnSetAlpha(st.pulse, 0.6 + 0.4 * math.cos(pulseTime * 6)) end
			gCurHiliteListbox = st.list
			ListCursors(this, cfg, 0.3, st.list)
		end
	end
	scr.HandleMouse = function(this, x, y)
		Deactivate(this)
		if (orig.HandleMouse) then return orig.HandleMouse(this, x, y) end
	end
	-- Typed keys call Input_Accept themselves with their own CurButton.
	scr.Input_KeyDown = function(this, key)
		Deactivate(this)
		if (orig.Input_KeyDown) then return orig.Input_KeyDown(this, key) end
	end
	scr.Input_Accept = function(this, a)
		if (PadActive(this)) then return Accept(this, cfg, S[this]) end
		if (orig.Input_Accept) then return orig.Input_Accept(this, a) end
	end
	scr.Input_Back = function(this, a)
		local st = S[this]
		if (PadActive(this)) then
			local z = cfg.zones[st.zone]
			if (z and (z.kind == "dropdown") and z.isopen(this)) then
				DropClose(this, cfg, st, z, 1)
				return
			end
			-- B steps out to the zone that leads into this one, then leaves.
			if (cfg.backzone and (st.zone ~= cfg.backzone) and (ZoneCount(this, cfg.zones[cfg.backzone]) > 0)) then
				st.zone = cfg.backzone
				sound("shell_select_change")
				Focus(this, cfg, st)
				return
			end
		end
		if (orig.Input_Back) then return orig.Input_Back(this, a) end
	end
	-- View: the Ctrl+click of a list.
	local origSelect = scr.Input_Select
	scr.Input_Select = function(this, a)
		local st = S[this]
		if (PadActive(this)) then
			local z = cfg.zones[st.zone]
			if (z and (z.kind == "list") and z.select) then
				local lb, contents, layout = ZoneList(this, z)
				if (lb) then z.select(this, lb, contents, layout) end
				return
			end
		end
		if (origSelect) then return origSelect(this, a) end
	end
	local function dir(name, axis, d)
		scr[name] = function(this, a)
			if (Nav(this, cfg, axis, d)) then return end
			if (orig[name]) then return orig[name](this, a) end
		end
	end
	dir("Input_GeneralUp", "v", -1)
	dir("Input_GeneralDown", "v", 1)
	dir("Input_GeneralLeft", "h", -1)
	dir("Input_GeneralRight", "h", 1)

	Shortcut(scr, "Input_Misc", cfg.misc, 2)
	Shortcut(scr, "Input_Misc2", cfg.misc2, 3)
end
)LUA"
R"LUA(
---------------------------------------------------------------------------
-- Screen layouts
---------------------------------------------------------------------------

-- A mouse click on the row under the pad cursor, through the screen's own
-- Accept, so 1.3 and other shell mods keep their own click rules.
local function ClickList(this, lb, layout, ctrl)
	ClearMouse()
	gMouseListBox = lb
	layout.CursorIdx = layout.SelectedIdx
	local oldCtrl, oldShift = this.bCtrl, this.bShift
	this.bCtrl = ctrl
	this.bShift = nil
	this.CurButton = nil
	if (gCurScreenTable) then gCurScreenTable.CurButton = nil end
	bypass = 1
	local ok, err = pcall(this.Input_Accept, this)
	bypass = nil
	if (not ok) then print("GameExt pad: " .. tostring(err)) end
	gMouseListBox = nil
	this.bCtrl = oldCtrl
	this.bShift = oldShift
	layout.CursorIdx = nil
	if (lb.Contents) then ListManager_fnFillContents(lb, lb.Contents, layout) end
end

local function ListClick(this, lb, contents, layout) ClickList(this, lb, layout, nil) end
local function ListCtrlClick(this, lb, contents, layout) ClickList(this, lb, layout, 1) end

-- 1.3 builds one checkbox per era (custom_BuildEraList) and renames the stock
-- ones, so the tag is always read off the checkbox itself.
local function EraItems(this, out)
	local boxes = {}
	local i
	if (custom_BuildEraList) then
		local list = custom_BuildEraList(this)
		for i = 1, table.getn(list) do table.insert(boxes, list[i].box) end
	else
		boxes = { this.Era_C_box, this.Era_G_box, }
	end
	for i = 1, table.getn(boxes) do
		local b = boxes[i]
		if (b and vis(b) and b.Check_Era and b.Check_Era.tag) then
			table.insert(out, { tag = b.Check_Era.tag, obj = b.Check_Era, })
		end
	end
end

local function MapMoved(this, lb, contents, layout)
	local idx = layout.SelectedIdx
	if (missionselect_name_listboxL_layout) then missionselect_name_listboxL_layout.SelectedIdx = idx end
	if (pc_missionselect_name_listboxL_layout) then pc_missionselect_name_listboxL_layout.SelectedIdx = idx end
	if ((this == ifs_missionselect) and ifs_missionselect_fnSetMapPreview) then
		ifs_missionselect_fnSetMapPreview(this)
	elseif (ifs_missionselect_pcMulti_fnSetMapPreview) then
		ifs_missionselect_pcMulti_fnSetMapPreview(this)
	end
end

local loggedItems = {}

-- Every visible tagged widget directly inside a container.
local function ChildItems(this, root)
	local out = {}
	if (not root) then return out end
	local found = ""
	local k, v
	for k, v in pairs(root) do
		if ((type(v) == "table") and v.tag and v.cp) then
			found = found .. " " .. tostring(v.tag)
			if (vis(v) and (not v.bDimmed)) then
				table.insert(out, { tag = v.tag, obj = v, })
			else
				found = found .. "(hidden)"
			end
		end
	end
	if (not loggedItems[root]) then
		loggedItems[root] = 1
		print("GameExt pad: map buttons" .. found)
	end
	return out
end

local function MissionSelectLayout()
	return {
		cross = "h",
		start = 2,
		misc = "_map_add",
		misc2 = "Launch",
		zones = {
			{ kind = "row", row = "_Tabs2", },
			{ kind = "list", accept = ListClick, select = ListCtrlClick, moved = MapMoved,
				list = function(this) return this.MapListbox, missionselect_listbox_contents, ifs_mspc_MapList_layout end, },
			-- Modes sit above the eras in one column.
			{ kind = "items", items = function(this)
				local out = {}
				local boxes = this.mode_checkbox
				local i
				if (boxes) then
					for i = 1, table.getn(boxes) do
						local cb = boxes[i].checkbox
						if (cb and vis(boxes[i])) then table.insert(out, { tag = cb.tag or ("check_mode" .. i), obj = cb, }) end
					end
				end
				EraItems(this, out)
				return out
			end, },
			{ kind = "list", accept = ListClick, select = ListCtrlClick,
				list = function(this) return this.PlayListbox, gPickedMapList, ifs_mspc_PlayList_layout end, },
			{ kind = "grid", default = "_map_add", items = function(this) return ChildItems(this, this.map_buttons) end, },
		},
	}
end

local function LoginLayout()
	return {
		cross = "v",
		start = 1,
		dropclose = ifs_login_fnClickDropDownButtons,
		zones = {
			{ kind = "dropdown", tag = "_profile_dropdown",
				button = function(this) return this.profile_button end,
				isopen = function(this) return this.bProfileDropBoxesOpen end,
				list = function(this) return this.listbox, ifs_login_listbox_contents, ifs_login_listbox_layout end, },
			{ kind = "dropdown", tag = "_diff_dropdown",
				button = function(this) return this.diff_button end,
				isopen = function(this) return this.bDiffDropBoxesOpen end,
				list = function(this) return this.diff_listbox, ifs_login_diff_listbox_contents, ifs_login_diff_listbox_layout end, },
			{ kind = "items", default = "_accept", items = function(this)
				local out = {}
				local order = { { "_back", this.Helptext_Back }, { "_delete", this.Helptext_Delete },
					{ "_create", this.Helptext_Create }, { "_accept", this.Helptext_Accept }, }
				local i
				for i = 1, 4 do
					if (vis(order[i][2])) then table.insert(out, { tag = order[i][1], obj = order[i][2], }) end
				end
				return out
			end, },
		},
	}
end

-- A Form_CreateVertical settings list plus the row of buttons under it.
local function FormLayout(formsFn, buttons, pages)
	local zones = { { kind = "form", forms = formsFn, }, }
	if (buttons) then
		table.insert(zones, { kind = "items", items = function(this) return TagItems(this, this, buttons) end, })
	end
	return { cross = "v", spill = 1, start = 1, pages = pages, zones = zones, }
end

local function FormOf(container)
	if (container and vis(container)) then return container.form end
	return nil
end

---------------------------------------------------------------------------
-- Every screen
---------------------------------------------------------------------------

local function Chain(scr, name, fn, fallback)
	local orig = scr[name]
	local fall = scr[fallback]
	scr[name] = function(this, a)
		if (fn(this)) then return end
		if (orig) then return orig(this, a) end
		if (fall) then return fall(this, a) end
	end
end

local function ButtonsByY(this)
	local out = {}
	local k, b
	for k, b in pairs(this.buttons) do
		if ((type(b) == "table") and b.tag and b.cp and (not b.bDimmed) and vis(b)) then table.insert(out, b) end
	end
	table.sort(out, function(a, c) return (a.y or 0) < (c.y or 0) end)
	return out
end

-- Up/down on a plain button screen. Moves by position when the link graph
-- did nothing (mod screens), and selects like a mouse hover either way.
local function Vertical(scr, name, d)
	local orig = scr[name]
	if (not orig) then return end
	scr[name] = function(this, a)
		local before, oldBtn = this.CurButton, gCurHiliteButton
		local r = orig(this, a)
		if (gCurPopup or gMouseListBox or (not this.buttons) or (not before) or (not this.buttons[before])) then return r end
		if (this.CurButton == before) then
			local list = ButtonsByY(this)
			local n = table.getn(list)
			local i, cur
			for i = 1, n do
				if (list[i].tag == before) then cur = i end
			end
			if (cur and list[cur + d]) then
				SetCurButton(list[cur + d].tag, this)
				sound("shell_select_change")
			end
		elseif (this.CurButton and this.buttons[this.CurButton]) then
			if (oldBtn and (oldBtn ~= this.buttons[this.CurButton])) then IFButton_fnSelect(oldBtn, nil) end
			SetCurButton(this.CurButton, this)
		end
		return r
	end
end

-- The cursor hides while the pad is in use and comes back when the mouse moves.
local cursorHidden = nil

local function PadCursor()
	if ((not GameExt_MouseMoved) or (not ScriptCB_EnableCursor)) then return end
	if (GameExt_MouseMoved() == 1) then
		if (cursorHidden) then
			cursorHidden = nil
			ScriptCB_EnableCursor(1)
		end
	elseif (GameExt_PadAny() == 1) then
		ScriptCB_EnableCursor(0)
		cursorHidden = 1
	end
end

local name, scr
for name, scr in pairs(_G) do
	if ((type(scr) == "table") and (scr.type == "screen")) then
		Chain(scr, "Input_LTrigger2", function(this) return CycleRow(this, RowFor(this, 1), -1) end, "Input_LTrigger")
		Chain(scr, "Input_RTrigger2", function(this) return CycleRow(this, RowFor(this, 1), 1) end, "Input_RTrigger")
		local origUpdate = scr.Update
		-- Some stock Updates fail every frame (the shell's career screen indexes
		-- a quit button it never built). The pad still has to work there, so
		-- ours runs regardless and the error is raised again afterwards.
		scr.Update = function(this, fDt)
			local ok, err = true, nil
			if (origUpdate) then ok, err = pcall(origUpdate, this, fDt) end
			PollTriggers(this)
			PadCursor()
			if (not ok) then error(err, 0) end
		end
		Vertical(scr, "Input_GeneralUp", -1)
		Vertical(scr, "Input_GeneralDown", 1)
	end
end

if (ifs_mspc_MapList_layout) then
	Attach(ifs_missionselect, MissionSelectLayout())
	Attach(ifs_missionselect_pcMulti, MissionSelectLayout())
end
if (ifs_login_listbox_layout and ifs_login_fnClickDropDownButtons) then Attach(ifs_login, LoginLayout()) end
if (Form_CreateVertical) then
	-- Settings pages column on the left, B steps back out to it.
	Attach(ifs_instant_options, {
		cross = "h", start = 2, backzone = 1, pages = "_Tabs3",
		zones = {
			{ kind = "row", row = "_Tabs3", },
			{ kind = "form", forms = function(this)
				return { this.screens and ifs_io_PageName and FormOf(this.screens[ifs_io_PageName]) }
			end, },
		},
	})
	Attach(ifs_opt_general, FormLayout(function(this) return { FormOf(this.formcontainergeneral) } end,
		{ "_back", "reset", "_ok", }))
	Attach(ifs_opt_mp, FormLayout(function(this) return { FormOf(this.formcontainer) } end,
		{ "_cancel", "_reset", "_ok", }))
	Attach(ifs_opt_pcvideo, FormLayout(function(this)
		return { FormOf(this.formcontainergeneral), FormOf(this.formcontainercustom) }
	end, { "cancel", "autodetect", "apply", }))
	-- Settings, then the bindings list (A opens the press-a-key popup the
	-- way a click does), then the buttons.
	if (ifs_opt_pckeyboard_listbox_layout) then
		Attach(ifs_opt_pccontrols, {
			cross = "v", spill = 1, start = 1,
			zones = {
				{ kind = "form", forms = function(this) return { FormOf(this.formcontainer) } end, },
				{ kind = "list", accept = ListClick,
					list = function(this)
						return this.listbox, ifs_opt_pckeyboard_listbox_contents, ifs_opt_pckeyboard_listbox_layout
					end, },
				{ kind = "items", items = function(this) return TagItems(this, this, { "cancel", "reset", "apply", }) end, },
			},
		})
	end
end
)LUA"
R"LUA(
---------------------------------------------------------------------------
-- Face-button shortcuts: X and Y press a button by tag, the first of the
-- candidates the screen shows.
---------------------------------------------------------------------------

local function FirstTag(this, tags)
	local i
	for i = 1, table.getn(tags) do
		local obj = TagObj(this, this, tags[i])
		if (obj and vis(obj) and (not obj.bDimmed)) then return tags[i] end
	end
	return nil
end

-- B presses the screen's Cancel, which reloads the profile as it was on
-- entry; the plain Back keeps the changes. Without changes Cancel is hidden
-- and B backs out as before.
local function PadShortcuts(scr, x, y, b)
	if (type(scr) ~= "table") then return end
	local function wrap(name, n, tags)
		if (not tags) then return end
		local orig = scr[name]
		scr[name] = function(this, a)
			if (PadDown(n) and (not gCurPopup) and (not bypass)) then
				local tag = FirstTag(this, tags)
				if (tag) then return Activate(this, tag) end
			end
			if (orig) then return orig(this, a) end
		end
	end
	wrap("Input_Misc", 2, x)
	wrap("Input_Misc2", 3, y)
	wrap("Input_Back", 1, b)
end

local RESET = { "reset", "_reset", "autodetect", }
local OK = { "_ok", "ok", "_apply", "apply", }
local CANCEL = { "_cancel", "cancel", "_back", }
PadShortcuts(ifs_opt_general, RESET, OK, CANCEL)
PadShortcuts(ifs_opt_mp, RESET, OK, CANCEL)
PadShortcuts(ifs_opt_pcvideo, RESET, OK, CANCEL)
PadShortcuts(ifs_opt_sound, RESET, OK, CANCEL)
PadShortcuts(ifs_opt_pcsound, RESET, OK, CANCEL)
-- Pad presses are bindings on the controls screen, so only B here.
PadShortcuts(ifs_opt_pccontrols, nil, nil, CANCEL)

---------------------------------------------------------------------------
-- Galactic Conquest. Every GC screen on PC only acts on Accept when the mouse
-- is on its _accept button, so a pad A is that button, as on console. The
-- stick and d-pad pick planets on the galaxy map, X ends the turn, LT/RT
-- switch the tabs and LB/RB keep zooming.
---------------------------------------------------------------------------

local gcEndTurn = nil
local gcAccepting = nil
-- Menu lists: A picks the highlighted entry there.
local GC_LISTS = { ifs_freeform_battle_mode = 1, ifs_freeform_cheat = 1, ifs_freeform_menu = 1,
	ifs_freeform_pickscenario = 1, ifs_freeform_rise_newload = 1, }

local function GCPickPlanet(this)
	local main = ifs_freeform_main
	if ((not main.joystick) or this.displayTimer or gCurPopup) then return end
	if (ScriptCB_IsPopupOpen and ScriptCB_IsPopupOpen()) then return end
	-- A held stick keeps steering a fleet move, as on console.
	if (this.fleetSelected) then main.movePressed = nil end
	local old = main.planetNext
	local read = ScriptCB_ReadLeftstick
	ScriptCB_ReadLeftstick = GameExt_PadStick
	local ok, err = pcall(main.UpdateNextPlanet, main)
	ScriptCB_ReadLeftstick = read
	if (not ok) then print("GameExt pad: " .. tostring(err)) return end
	local nxt = main.planetNext
	if (nxt == old) then return end
	-- What the stock Update does when the mouse picks a planet.
	this.planetRollover = nil
	if (ifs_planet_name_sound and main.planetTeam[nxt] and (main.planetTeam[nxt] > 0)) then
		main:PlayVoice(string.format(ifs_planet_name_sound, main.playerSide, nxt))
	end
	if (not this.fleetSelected) then main:SelectPlanet(this.info, nxt) end
	ifelm_shellscreen_fnPlaySound(this.selectSound)
	this:UpdateAction()
end

local function none() end

local function GCScreen(scr, endTurn)
	if (type(scr) ~= "table") then return end
	-- The purchase screens build their buttons when a game starts, so the
	-- button is looked for on each press. Only the press itself counts: a
	-- screen entered from it may call its own Accept while A is still down.
	local origAccept = scr.Input_Accept
	if (origAccept) then
		scr.Input_Accept = function(this, j)
			if (gcAccepting) then return origAccept(this, j) end
			if (PadDown(0) and (not bypass) and (not gCurPopup) and FindTag(this, "_accept", 0, {})) then
				this.CurButton = "_accept"
			end
			gcAccepting = 1
			local ok, err = pcall(origAccept, this, j)
			gcAccepting = nil
			if (not ok) then print("GameExt pad: " .. tostring(err)) end
		end
	end
	if (not endTurn) then return end
	if (type(ifs_freeform_tab_layout) == "table") then TrigTabs[scr] = "_Tabs" end
	-- Up/down would only walk the mouse buttons here.
	scr.Input_GeneralUp = none
	scr.Input_GeneralDown = none
	local origMisc = scr.Input_Misc
	scr.Input_Misc = function(this, j)
		if (PadDown(2) and (not gCurPopup)) then
			if (not this.displayTimer) then endTurn(this) end
			return
		end
		if (origMisc) then return origMisc(this, j) end
	end
end

if ((type(ifs_freeform_main) == "table") and GameExt_PadStick) then
	local name, scr
	for name, scr in pairs(_G) do
		if ((type(name) == "string") and string.find(name, "^ifs_freeform_") and (type(scr) == "table")
				and (scr.type == "screen") and (not GC_LISTS[name]) and (scr ~= ifs_freeform_fleet) and (scr ~= ifs_freeform_purchase_tech)
				and (scr ~= ifs_freeform_purchase_unit)) then
			GCScreen(scr)
		end
	end
	-- The purchase screens end the turn from the map, through its own button.
	local function viaMap(this)
		gcEndTurn = 1
		Activate(this, "_tab_fleet")
	end
	GCScreen(ifs_freeform_purchase_tech, viaMap)
	GCScreen(ifs_freeform_purchase_unit, viaMap)

	local scr = ifs_freeform_fleet
	if ((type(scr) == "table") and ifs_freeform_main.UpdateNextPlanet) then
		GCScreen(scr, function(this) Activate(this, "_next") end)
		-- The d-pad picks planets here; LB/RB zoom.
		scr.Input_GeneralLeft = none
		scr.Input_GeneralRight = none
		scr.Input_DPadUp = none
		scr.Input_DPadDown = none
		local origUpdate = scr.Update
		scr.Update = function(this, fDt)
			if (gcEndTurn) then
				gcEndTurn = nil
				if (not this.displayTimer) then Activate(this, "_next") end
			end
			GCPickPlanet(this)
			if (origUpdate) then return origUpdate(this, fDt) end
		end
	end
end
)LUA";
