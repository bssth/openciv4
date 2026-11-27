## openciv4: the WorldBuilder's engine side, which the original's executable draws for
## CvWorldBuilderScreen: its tool palettes (the player's units, buildings and techs; the map's
## improvements, resources, terrain and territory; a unit's or a city's edit panel with its
## promotions, buildings, religions and corporations), its flyout menu and its own buttons (save,
## the modes, exit). The script builds them by sections (addTabSection) and hears what is
## chosen through CvScreensInterface (WorldBuilderOn...BrushSelected, the edit panel's callbacks);
## here they are widgets of its screen named OC4WB..., whose clicks come back to handleInput.

import CvPythonExtensions as _cpe
import oc4host
import oc4screens

_gc = _cpe.CyGlobalContext()
_COLUMNS = 7         # items to a row of a palette
_CELL = 35           # from one to the next
_ICON = 31
_widgets = []       # the widgets drawn (deleted and drawn again each time)
_choices = {}       # a widget's name: what its click does


def _u(s):
	if isinstance(s, unicode):
		return s
	try:
		return str(s).decode('utf-8')
	except Exception:
		return unicode(str(s), 'latin-1')


def _screenId():
	import CvScreenEnums
	return CvScreenEnums.WORLDBUILDER_SCREEN


def _screen():
	return _cpe.CyGInterfaceScreen('WorldBuilderScreen', _screenId())


def _wb():
	import CvScreensInterface
	return CvScreensInterface.worldBuilderScreen


def _call(module, function, args):
	try:
		m = __import__(module)
		return getattr(m, function)(args)
	except Exception:
		import traceback
		traceback.print_exc()


class _TabCtrl(object):
	"""A WorldBuilder tool control: its sections (tabs) and, for the edit panel, its controls."""

	def __init__(self, kind):
		self.kind = kind
		self.sections = []
		self.controls = []
		self.active = 0
		self.enabled = False
		self.dead = False

	def setNumColumns(self, n): pass
	def setColumnLength(self, n): pass
	def setCheckBoxState(self, *args): pass

	def addTabSection(self, name):
		self.sections.append(_u(name))
		self.controls.append([])

	def getActiveTab(self): return self.active

	def setActiveTab(self, i):
		self.active = int(i)
		_draw()

	def enable(self, bOn):
		self.enabled = bool(bOn)
		_draw()

	def isEnabled(self): return self.enabled and not self.dead
	def isNone(self): return self.dead

	def destroy(self):
		self.dead = True
		self.enabled = False
		_draw()

	# the edit panel's controls, into its last section
	def _add(self, *c):
		if not self.controls:
			self.addTabSection(u'')
		self.controls[-1].append(c)

	def addSectionLabel(self, szText, iSection=0): self._add('label', _u(szText))
	def addSectionEditCtrl(self, szText, szModule, szFunction, szId, iSection=0): self._add('edit', _u(szText), szModule, szFunction)
	def addSectionButton(self, szText, szModule, szFunction, szId, iSection=0): self._add('button', _u(szText), szModule, szFunction)

	def addSectionDropdown(self, szName, choices, szModule, szFunction, szId, iSection=0, iSelected=0):
		self._add('dropdown', szName, [_u(c) for c in choices], szModule, szFunction, int(iSelected))

	def addSectionSpinner(self, szName, szModule, szFunction, szId, iSection, fMin, fMax, fStep, fValue, *args):
		self._add('spinner', szName, szModule, szFunction, float(fMin), float(fMax), float(fStep), float(fValue))

	def setDropDownSelection(self, szSection, szName, iIndex):
		for section in self.controls:
			for i, c in enumerate(section):
				if c[0] == 'dropdown' and c[1] == szName:
					section[i] = c[:5] + (int(iIndex),)
		_draw()


_ctrls = {'player': _TabCtrl('player'), 'map': _TabCtrl('map'), 'edit': _TabCtrl('edit'), 'advanced': _TabCtrl('advanced')}
_ctrls['edit'].dead = True


def _fresh(kind):
	_ctrls[kind] = _TabCtrl(kind)
	return _ctrls[kind]


def reset():
	"""The WorldBuilder opened or closed: its palettes, edit panel and flyout as new, their widgets
	gone, so each opening starts as the first (the script's interfaceScreen adds the map
	palette's tabs again each time it opens, which the executable's new control takes)."""
	global _flyout
	_clear(_screen())
	for kind in ('player', 'map', 'advanced'):
		_fresh(kind)
	_fresh('edit').dead = True
	_flyout = None


# ---------------------------------------------------------------- the functions the script calls

def initWBToolPlayerControl(): _fresh('player')
def getWBToolNormalPlayerTabCtrl(): return _ctrls['player']
def addWBPlayerControlTabs(): _draw()
def getWBToolNormalMapTabCtrl(): return _ctrls['map']
def setWBInitialCtrlTabPlacement():
	# (the player's palette shown first, as the original opens)
	if not any(c.isEnabled() for c in _ctrls.values()):
		_ctrls['player'].enabled = True
	_draw()
def initWBToolAdvancedStartControl(): _fresh('advanced')
def getWBToolAdvancedStartTabCtrl(): return _ctrls['advanced']
def addWBAdvancedStartControlTabs(): _draw()
def initWBToolEditCtrl(): _fresh('edit')
def getWBToolEditTabCtrl(): return _ctrls['edit']


def initWBToolEditCtrlTab(bUnit):
	"""The edit window's second tab, as the original's: a unit's promotions, a city's buildings (the
	city's religions, holy cities, corporations and headquarters are rows of its first tab)."""
	e = _ctrls['edit']
	e.unit = bool(bUnit)
	e.addTabSection(_text('TXT_KEY_WB_PROMOTIONS') or u'Promotions' if bUnit else _text('TXT_KEY_WB_BUILDINGS') or u'Buildings')
	e.enabled = True
	_draw()


def refreshWBEditCtrlReligionButtons(b): _draw()
def refreshWBEditCtrlCorporationButtons(b): _draw()
# The diplomacy mode's window (CvWorldBuilderDiplomacyScreen): a tab control, as
# the options screen's (oc4options.CyGTabCtrl, one window of tabs, centred).
_diplomacy = {'ctrl': None}


def initWBDiplomacyCtrl(szName, bStatic=True, bCloseButton=False):
	import oc4options
	_diplomacy['ctrl'] = oc4options.CyGTabCtrl(szName, bStatic, bCloseButton)


def getWBToolDiplomacyTabCtrl(): return _diplomacy['ctrl']


def destroyWBDiplomacyCtrl():
	ctrl, _diplomacy['ctrl'] = _diplomacy['ctrl'], None
	if ctrl is not None:
		ctrl.destroy()


def saveDiplomacySettings(): pass
def setDiplomacySettings(): pass


def isMouseOverGameSurface(): return bool(oc4host.ui_mouse_over_map())
def setFocusToCVG(): pass
def getWBSaveExtension(): return '.CivBeyondSwordWBSave'


class CyGFlyoutMenu(object):
	"""The flyout menu at the mouse (a plot's units, its city, ...)."""

	def __init__(self):
		self.items = []

	def addTextItem(self, szText, szModule, szFunction, iData):
		self.items.append((_u(szText), szModule, szFunction, iData))

	def show(self):
		global _flyout
		_flyout = self
		_draw()

	def destroy(self):
		global _flyout
		if _flyout is self:
			_flyout = None
		_draw()


_flyout = None


# ---------------------------------------------------------------- what a palette holds

def _art(tag):
	"""An interface art's file (CIV4ArtDefines_Interface), or '' (the item shows its name)."""
	try:
		return str(_cpe.CyArtFileMgr().getInterfaceArtInfo(tag).getPath())
	except Exception:
		return ''


def _items(ctrl):
	"""The active section's items: (list, index, button art, label, selected, help): help the widget
	type and data whose game core help the item shows (the original's palette: a unit's,
	building's, tech's, resource's... help), None its name."""
	gc = _gc
	W = _cpe.WidgetTypes
	wb = _wb()
	tab = ctrl.active
	out = []
	if ctrl.kind == 'player':
		try:
			current = wb.m_iNormalPlayerCurrentIndexes[tab]
		except Exception:
			current = -1
		if tab == 0:
			for i in range(gc.getNumUnitInfos()):
				out.append((0, i, gc.getUnitInfo(i).getButton(), gc.getUnitInfo(i).getDescription(), i == current, (W.WIDGET_PEDIA_JUMP_TO_UNIT, i)))
		elif tab == 1:
			# (as the original's list: a city first, then the buildings, one on: the script
			# places a city for 0 and building i - 1 for i)
			try:
				found = gc.getMissionInfo(_cpe.MissionTypes.MISSION_FOUND).getButton()
			except Exception:
				found = ''
			out.append((0, 0, found, u'City', current == 0, None))
			for i in range(gc.getNumBuildingInfos()):
				out.append((0, i + 1, gc.getBuildingInfo(i).getButton(), gc.getBuildingInfo(i).getDescription(), i + 1 == current, (W.WIDGET_PEDIA_JUMP_TO_BUILDING, i)))
		elif tab == 2:
			try:
				team = gc.getTeam(gc.getPlayer(wb.m_iCurrentPlayer).getTeam())
			except Exception:
				team = None
			for i in range(gc.getNumTechInfos()):
				out.append((0, i, gc.getTechInfo(i).getButton(), gc.getTechInfo(i).getDescription(), team is not None and team.isHasTech(i), (W.WIDGET_PEDIA_JUMP_TO_TECH, i)))
	elif ctrl.kind == 'map':
		try:
			current, currentList = wb.m_iNormalMapCurrentIndexes[tab], wb.m_iNormalMapCurrentList[tab]
		except Exception:
			current, currentList = -1, -1
		sel = lambda l, i: l == currentList and i == current
		if tab == 0:
			# (as the original's list: without the graphical only ones, which the script skips
			# when it counts its way to the improvement placed)
			k = 0
			for i in range(gc.getNumImprovementInfos()):
				info = gc.getImprovementInfo(i)
				if not info.isGraphicalOnly():
					out.append((0, k, info.getButton(), info.getDescription(), sel(0, k), None))
					k += 1
		elif tab == 1:
			for i in range(gc.getNumBonusInfos()):
				out.append((0, i, gc.getBonusInfo(i).getButton(), gc.getBonusInfo(i).getDescription(), sel(0, i), (W.WIDGET_PEDIA_JUMP_TO_BONUS, i)))
		elif tab == 2:
			# (as the original's list, seen in Wine: without the graphical only terrains, the peak
			# and the hill, which the plot types place; each list on rows of its own)
			for i in range(gc.getNumTerrainInfos()):
				if not gc.getTerrainInfo(i).isGraphicalOnly():
					out.append((0, i, gc.getTerrainInfo(i).getButton(), gc.getTerrainInfo(i).getDescription(), sel(0, i), (W.WIDGET_PEDIA_JUMP_TO_TERRAIN, i)))
			# (the plot types and the river with the original's own buttons)
			for i, (name, art) in enumerate(((u'Peak', 'WORLDBUILDER_PLOT_TYPE_MOUNTAIN'), (u'Hills', 'WORLDBUILDER_PLOT_TYPE_HILL'),
					(u'Flat', 'WORLDBUILDER_PLOT_TYPE_PLAINS'), (u'Water', 'WORLDBUILDER_PLOT_TYPE_OCEAN'))):
				out.append((1, i, _art(art), name, sel(1, i), None))
			k = 0
			for i in range(gc.getNumFeatureInfos()):
				info = gc.getFeatureInfo(i)
				for j in range(info.getNumVarieties()):
					# (each variety its own button, as the original's: the forest's leafy, evergreen
					# and snowy ones)
					try:
						button = oc4host.feature_variety_button(i, j) or info.getButton()
					except Exception:
						button = info.getButton()
					out.append((2, k, button, info.getDescription(), sel(2, k), (W.WIDGET_PEDIA_JUMP_TO_FEATURE, i)))
					k += 1
			for i in range(gc.getNumRouteInfos()):
				out.append((3, i, gc.getRouteInfo(i).getButton(), gc.getRouteInfo(i).getDescription(), sel(3, i), None))
			out.append((3, gc.getNumRouteInfos(), _art('WORLDBUILDER_RIVER_PLACEMENT'), u'River', sel(3, gc.getNumRouteInfos()), None))
		elif tab == 3:
			for i in range(gc.getMAX_CIV_PLAYERS()):
				p = gc.getPlayer(i)
				if p.isEverAlive():
					civ = gc.getCivilizationInfo(p.getCivilizationType())
					out.append((0, i, civ.getButton(), p.getCivilizationShortDescription(0), sel(0, i), None))
	elif ctrl.kind == 'edit':
		plot = getattr(wb, 'm_pActivePlot', 0)
		if not plot:
			return out
		if getattr(ctrl, 'unit', False):
			if tab == 1:
				try:
					unit = plot.getUnit(wb.m_iCurrentUnit)
				except Exception:
					unit = None
				for i in range(gc.getNumPromotionInfos()):
					out.append((0, i, gc.getPromotionInfo(i).getButton(), gc.getPromotionInfo(i).getDescription(), unit is not None and unit.isHasPromotion(i), (W.WIDGET_HELP_PROMOTION, i)))
		elif plot.isCity():
			city = plot.getPlotCity()
			if tab == 1:
				for i in range(gc.getNumBuildingInfos()):
					out.append((0, i, gc.getBuildingInfo(i).getButton(), gc.getBuildingInfo(i).getDescription(), city.getNumRealBuilding(i) > 0, (W.WIDGET_PEDIA_JUMP_TO_BUILDING, i)))
			elif tab == 0:
				# (under the city's data, a row each: its religions, its holy cities, its
				# corporations, its headquarters)
				for row in ('religion', 'holy'):
					for i in range(gc.getNumReligionInfos()):
						on = city.isHasReligion(i) if row == 'religion' else city.isHolyCityByType(i)
						out.append((row, i, gc.getReligionInfo(i).getButton(), gc.getReligionInfo(i).getDescription(), on, (W.WIDGET_PEDIA_JUMP_TO_RELIGION, i)))
				for row in ('corp', 'hq'):
					for i in range(gc.getNumCorporationInfos()):
						on = city.isHasCorporation(i) if row == 'corp' else city.isHeadquartersByType(i)
						out.append((row, i, gc.getCorporationInfo(i).getButton(), gc.getCorporationInfo(i).getDescription(), on, (W.WIDGET_PEDIA_JUMP_TO_CORPORATION, i)))
	return out


def _chosen(ctrl, item):
	"""A palette's item clicked: as the original's palette tells the script."""
	import CvScreensInterface as si
	l, i = item[0], item[1]
	tab = ctrl.active
	if ctrl.kind == 'player':
		si.WorldBuilderOnNormalPlayerBrushSelected((l, i, tab))
	elif ctrl.kind == 'map':
		si.WorldBuilderOnNormalMapBrushSelected((l, i, tab))
	elif ctrl.kind == 'advanced':
		si.WorldBuilderOnAdvancedStartBrushSelected((l, i, tab))
	elif ctrl.kind == 'edit':
		calls = {'religion': si.WorldBuilderOnWBEditReligionSelected, 'holy': si.WorldBuilderOnWBEditHolyCitySelected,
			'corp': si.WorldBuilderOnWBEditCorporationSelected, 'hq': si.WorldBuilderOnWBEditHeadquartersSelected}
		if l in calls:
			calls[l]((0, i, tab))
		else:
			si.WorldBuilderOnWBEditBrushSelected((l, i, tab))


# ---------------------------------------------------------------- drawing

def _add(name, choice=None):
	_widgets.append(name)
	if choice is not None:
		_choices[name] = choice
	return name


def _clear(screen):
	for name in _widgets:
		screen.deleteWidget(name)
	del _widgets[:]
	_choices.clear()


def _shown():
	for kind in ('edit', 'advanced', 'player', 'map'):
		c = _ctrls[kind]
		if c.isEnabled() and c.sections:
			return c
	return None


# The tool window, as the original's (measured on it at 1920 x 1080): at the top left, the
# theme's tool window frame and title bar, the tab control's tabs, under them the items in a
# scroll panel, seven to a row.
_WINDOW_INNER = '@GFC_Control_DefaultWindow_Style|GFC_CtrlTheme_Bitmap_Control_Window_Skin_Inner_Default'
_WINDOW_TITLE = '@|SF_CtrlTheme_Civ4_Control_Window_Skin_Tool_TitleBar_Full_Default'
_WINDOW_FRAME = '@SF_CtrlTheme_Civ4_Control_Window_Tool_InFrameBorder_Normal_Style|GFC_CtrlTheme_Bitmap_Control_Border_Skin_Full_Default'
_TITLE_H = 20
_TAB_SKIN = '@GFC_CtrlTheme_Bitmap_Control_Tab_SkinStyle_Top_Begin_BaseLevel_Single|GFC_CtrlTheme_Bitmap_Control_Tab_Skin_Full_'
# The tabs end 13 px from the window's right edge. When they do not fit there (the map mode's),
# as seen in Wine at 1600 x 900: the theme's End_Arrow_Indent (30, Civ4Theme_Tab.thm) is kept
# for two arrow buttons, and the tab after the last whole one shows up to there, narrowed to end
# there, its word cut. The arrows are the theme's tab arrows (12 x 25 each, the art
# Civ4/tab_arrows.tga 14 x 14 in their middle); an arrow with nothing more that way is grey.
_TAB_ARROWS = ('SF_CtrlTheme_Civ4_Control_Tab_Arrow_LeftButton_Style', 'SF_CtrlTheme_Civ4_Control_Tab_Arrow_RightButton_Style')
_TAB_END, _ARROW_INDENT, _ARROW_X, _ARROW_Y, _ARROW_H = 13, 30, (38, 25), 3, 25
_TITLES = {'player': 'TXT_KEY_WB_PLAYER_MODE', 'map': 'TXT_KEY_WB_MAP_MODE', 'edit': 'TXT_KEY_WB_WORLD_BUILDER_EDIT_CONTROL'}
_WX, _WY, _WW = 0, 0, 315   # (seen in Wine at 1600 x 900: 315 wide in player and map mode)
_TAB_Y, _TAB_H = 31, 28
_BODY_X, _BODY_Y = 12, 59
_GRID_H = 162       # four rows and a half show, as the original's
_ROW_TEXT = (40, 28, 16)  # (the colour of the title and the tabs' words: the theme's dark brown)


def _text(key):
	try:
		return _u(_cpe.CyTranslator().getText(key, ()))
	except Exception:
		return u''


def _style(screen, name, style):
	w = screen._s.get(name)
	if w is not None:
		w.style = style


def _draw():
	try:
		if not oc4host.ui_world_builder():
			return
		screen = _screen()
	except Exception:
		return
	_clear(screen)
	ctrl = _shown()
	general = _cpe.WidgetTypes.WIDGET_GENERAL
	if ctrl is not None and ctrl.kind == 'edit':
		_drawEdit(screen, ctrl)
	elif ctrl is not None:
		dark = u'<color=%d,%d,%d>' % _ROW_TEXT
		tabs = []
		for i, name in enumerate(ctrl.sections):
			try:
				w = int(oc4screens._textWidth(name, 3))
			except Exception:
				w = 9 * len(name)
			tabs.append((name, w + 20))
		# (as the original's: as wide as the player mode's; the tabs that do not fit scrolled to
		# with the two arrows at the right, the map mode's)
		width = _WW
		cut = _WX + width - _TAB_END
		scroll = _WX + 15 + sum(tw + 1 for name, tw in tabs) - 1 > cut
		if scroll:
			cut -= _ARROW_INDENT
		first = min(getattr(ctrl, 'tabFirst', 0), len(tabs) - 1) if scroll else 0
		body_w = width - 2 * _BODY_X
		items = _items(ctrl)
		body_h = (_GRID_H if items else 0) + 6
		height = _BODY_Y + body_h + 8
		# (the window: its inside and its frame under the title bar, the title on the bar)
		for name, y, h, style in (('OC4WBWindow', _TITLE_H, height - _TITLE_H, _WINDOW_INNER), ('OC4WBFrame', _TITLE_H, height - _TITLE_H, _WINDOW_FRAME),
				('OC4WBTitleBar', 0, _TITLE_H, _WINDOW_TITLE)):
			screen.addPanel(_add(name), u'', u'', True, False, _WX, _WY + y, width, h, _cpe.PanelStyles.PANEL_STYLE_MAIN)
			_style(screen, name, style)
		title = _text(_TITLES.get(ctrl.kind, ''))
		if title:
			screen.setLabel(_add('OC4WBTitle'), 'Background', u'<font=3b>' + dark + title + u'</color></font>', 1 << 0, _WX + 6, _WY + 1, -0.3,
				_cpe.FontTypes.TITLE_FONT, general, -1, -1)
		# the tabs, each as wide as its word; the one that does not fit whole cut at the arrows
		x = _WX + 15
		more = False
		for i, (name, tw) in enumerate(tabs):
			if i < first:
				continue
			if scroll and x >= cut:
				more = True
				break
			part = scroll and x + tw > cut
			more = more or part
			tab = _add('OC4WBTabBack%d' % i)
			# (the cut one's skin narrowed to the room left, its end showing; its word cut there)
			screen.addPanel(tab, u'', u'', True, False, x, _WY + _TAB_Y, cut - x if part else tw, _TAB_H, _cpe.PanelStyles.PANEL_STYLE_MAIN)
			_style(screen, tab, _TAB_SKIN + ('Select' if i == ctrl.active else 'Default'))
			# (clicking the cut one also moves the strip a tab on, as seen in Wine: Terrains then whole)
			word = _add('OC4WBTab%d' % i, ('tab', ctrl, i, part))
			screen.setText(word, 'Background', dark + name + u'</color>', 1 << 2, x + tw / 2, _WY + _TAB_Y + 4, -0.3,
				_cpe.FontTypes.GAME_FONT, general, -1, -1)
			w = screen._s.get(word)
			if part and w is not None:
				w.clipRect = (x, _WY + _TAB_Y, cut, _WY + _TAB_Y + _TAB_H)
			x += tw + 1
		if scroll:
			for k, d in enumerate((-1, 1)):
				b = _add('OC4WBTabScroll%d' % k, ('tabscroll', ctrl, d, len(tabs)))
				screen.setButtonGFC(b, u'', '', _WX + width - _ARROW_X[k], _WY + _TAB_Y + _ARROW_Y, 14, _ARROW_H, general, -1, -1,
					_cpe.ButtonStyles.BUTTON_STYLE_STANDARD)
				_style(screen, b, _TAB_ARROWS[k])
				screen.enable(b, first > 0 if d < 0 else more)
		body = _add('OC4WBBody')
		screen.addPanel(body, u'', u'', True, False, _WX + _BODY_X, _WY + _BODY_Y, body_w, body_h, _cpe.PanelStyles.PANEL_STYLE_IN)
		if items:
			_drawGrid(screen, ctrl, items, _WX + _BODY_X + 2, _WY + _BODY_Y + 2, body_w - 4, _GRID_H)

	if _flyout is not None and _flyout.items:
		mx, my = 300, 200
		screen.addPanel(_add('OC4WBFlyout'), u'', u'', True, False, mx, my, 240, 24 * len(_flyout.items) + 12, _cpe.PanelStyles.PANEL_STYLE_MAIN)
		for k, item in enumerate(_flyout.items):
			screen.setText(_add('OC4WBFly%d' % k, ('fly', item)), 'Background', item[0], 0, mx + 10, my + 8 + 24 * k, -0.3, _cpe.FontTypes.GAME_FONT, general, -1, -1)


def _item(screen, ctrl, item, k, parent, cx, cy):
	"""One item: its button (on the highlight when chosen), its help as the original's palette
	shows it under the mouse: the game's help of what it places, else its name as a link."""
	general = _cpe.WidgetTypes.WIDGET_GENERAL
	# (the player mode's Technologies: a tech the player knows framed in yellow, as the original's,
	# seen in Wine; an image check box that is on with the highlight BUTTON_HILITE_SQUARE
	# (Button_SELECT.dds), the art CvWorldBuilderScreen.py gives its tool buttons' check boxes)
	known = item[4] and ctrl.kind == 'player' and ctrl.active == 2
	if item[4] and not known:
		screen.attachPanelAt(parent, _add('OC4WBSel%d' % k), u'', u'', False, False, _cpe.PanelStyles.PANEL_STYLE_HUD_HELP, cx - 2, cy - 2, _CELL - 1, _CELL - 1,
			general, -1, -1)
	name = 'OC4WBItem%d' % k
	if known and item[2]:
		screen.addCheckBoxGFCAt(parent, _add(name, ('item', ctrl, item)), item[2], _art('BUTTON_HILITE_SQUARE'), cx, cy, _ICON, _ICON, general, -1, -1,
			_cpe.ButtonStyles.BUTTON_STYLE_LABEL)
		screen.setState(name, True)
	elif item[2]:
		screen.setImageButtonAt(_add(name, ('item', ctrl, item)), parent, item[2], cx, cy, _ICON, _ICON, general, -1, -1)
	else:
		screen.setTextAt(_add(name, ('item', ctrl, item)), parent, item[3][:6], 0, cx, cy + 8, -0.3, _cpe.FontTypes.SMALL_FONT, general, -1, -1)
	w = screen._s.get(name)
	if w is not None:
		if item[5]:
			w.helpWidget = (int(item[5][0]), int(item[5][1]), -1)
		else:
			w.help = u'<link=literal>' + _u(item[3]) + u'</link>'


def _gridPlaces(items, columns):
	"""Each item's column and row: a section's lists each start a row of their own, as the
	original's Terrains tab (seen in Wine: the terrains, the plot types, the features, then the
	routes and the river, each on new rows)."""
	places, row, col, last = [], 0, 0, None
	for item in items:
		if last is not None and (item[0] != last or col >= columns):
			row, col = row + 1, 0
		places.append((col, row))
		col, last = col + 1, item[0]
	return places


def _drawGrid(screen, ctrl, items, x, y, w, h, k0=0):
	"""Items in a scroll panel: seven to a row beside its scroll bar, or, when they fit without
	one, as many as fit (the original's: the map mode's improvements eight)."""
	grid = _add('OC4WBGrid%d' % k0)
	screen.addScrollPanel(grid, u'', x, y, w, h, _cpe.PanelStyles.PANEL_STYLE_EXTERNAL)
	columns = max(_COLUMNS, (w - 6) / _CELL)
	places = _gridPlaces(items, columns)
	if places and (places[-1][1] + 1) * _CELL > h - 6:
		columns = _COLUMNS
		places = _gridPlaces(items, columns)
	for k, item in enumerate(items):
		_item(screen, ctrl, item, k0 + k, grid, places[k][0] * _CELL, places[k][1] * _CELL)


# The edit window (a unit's or a city's), as the original's (measured in Wine at 1600 x 900): in
# the middle of the screen, the tool window's frame and a title bar with a close box; two tabs;
# its controls 22 in, one under the other: an edit box, a pull down or a spin box 24 high, 8
# apart; a label 21 high; a button 26; under a city's, its religions, holy cities, corporations
# and headquarters, a row each, 34 apart. A spin box is as wide as its longest value needs.
_EDIT_W = {True: 338, False: 310}  # a unit's, a city's
_EDIT_TITLE_H, _EDIT_TAB_Y, _EDIT_BODY_Y, _EDIT_TOP = 26, 37, 65, 78
_EDIT_ROW, _EDIT_ICON = 34, 30
_SPIN_UP = 'SF_CtrlTheme_Civ4_Control_SpinBox_UpButton_Style'
_SPIN_DOWN = 'SF_CtrlTheme_Civ4_Control_SpinBox_DownButton_Style'
_CONTROL_H = {'label': 21, 'edit': 24, 'dropdown': 24, 'spinner': 24, 'button': 26}


def _gapAfter(controls, n):
	if controls[n][0] == 'label' or n + 1 >= len(controls):
		return 0
	return 6 if controls[n + 1][0] == 'button' else 8


def _controlsHeight(controls):
	return sum(_CONTROL_H.get(c[0], 0) + _gapAfter(controls, n) for n, c in enumerate(controls))


def _spinWidth(c):
	"""(a spin box: as wide as the longer of its lowest and highest values, 35 + 11 a figure)"""
	return 35 + 11 * max(len('%d' % int(c[4])), len('%d' % int(c[5])))


def _itemsOf(ctrl, tab):
	active, ctrl.active = ctrl.active, tab
	try:
		return _items(ctrl)
	finally:
		ctrl.active = active


def _drawEdit(screen, ctrl):
	general = _cpe.WidgetTypes.WIDGET_GENERAL
	dark = u'<color=%d,%d,%d>' % _ROW_TEXT
	unit = bool(getattr(ctrl, 'unit', False))
	controls = ctrl.controls[0] if ctrl.controls else []
	rows = [] if unit else _itemsOf(ctrl, 0)
	nrows = len(set(item[0] for item in rows))
	height = _EDIT_TOP + _controlsHeight(controls) + (6 + (nrows - 1) * _EDIT_ROW + _EDIT_ICON + 18 if nrows else 24)
	width = _EDIT_W[unit]
	try:
		RW, RH = screen.getXResolution(), screen.getYResolution()
	except Exception:
		RW, RH = 1600, 900
	wx, wy = (RW - width) / 2, max(0, (RH - height) / 2)
	for name, y, h, style in (('OC4WBWindow', _EDIT_TITLE_H, height - _EDIT_TITLE_H, _WINDOW_INNER), ('OC4WBFrame', _EDIT_TITLE_H, height - _EDIT_TITLE_H, _WINDOW_FRAME),
			('OC4WBTitleBar', 0, _EDIT_TITLE_H, _WINDOW_TITLE)):
		screen.addPanel(_add(name), u'', u'', True, False, wx, wy + y, width, h, _cpe.PanelStyles.PANEL_STYLE_MAIN)
		_style(screen, name, style)
	screen.setLabel(_add('OC4WBTitle'), 'Background', u'<font=3b>' + dark + _text(_TITLES['edit']) + u'</color></font>', 1 << 0, wx + 10, wy + 4, -0.3,
		_cpe.FontTypes.TITLE_FONT, general, -1, -1)
	screen.setText(_add('OC4WBClose', ('close',)), 'Background', u'<font=3>' + dark + u'x</color></font>', 1 << 2, wx + width - 18, wy + 4, -0.3,
		_cpe.FontTypes.TITLE_FONT, general, -1, -1)
	# the tabs
	x = wx + 13
	for i, name in enumerate(ctrl.sections):
		try:
			tw = int(oc4screens._textWidth(name, 3)) + 20
		except Exception:
			tw = 9 * len(name) + 20
		tab = _add('OC4WBTabBack%d' % i)
		screen.addPanel(tab, u'', u'', True, False, x, wy + _EDIT_TAB_Y, tw, _TAB_H, _cpe.PanelStyles.PANEL_STYLE_MAIN)
		_style(screen, tab, _TAB_SKIN + ('Select' if i == ctrl.active else 'Default'))
		screen.setText(_add('OC4WBTab%d' % i, ('tab', ctrl, i)), 'Background', dark + name + u'</color>', 1 << 2, x + tw / 2, wy + _EDIT_TAB_Y + 4, -0.3,
			_cpe.FontTypes.GAME_FONT, general, -1, -1)
		x += tw + 1
	screen.addPanel(_add('OC4WBBody'), u'', u'', True, False, wx + 11, wy + _EDIT_BODY_Y, width - 22, height - _EDIT_BODY_Y - 10, _cpe.PanelStyles.PANEL_STYLE_IN)
	cx, cw = wx + 22, width - 44
	if ctrl.active == 0:
		y = _drawControls(screen, ctrl, controls, cx, wy + _EDIT_TOP, cw) + 6
		row, k, prev = -1, 0, None
		for item in rows:
			if item[0] != prev:
				row, k, prev = row + 1, 0, item[0]
			_item(screen, ctrl, item, 1000 + row * 100 + k, 'Background', cx - 1 + k * _EDIT_ROW, y + row * _EDIT_ROW)
			k += 1
	else:
		items = _itemsOf(ctrl, ctrl.active)
		if items:
			_drawGrid(screen, ctrl, items, wx + 13, wy + _EDIT_BODY_Y + 4, width - 26, height - _EDIT_BODY_Y - 18)


def _drawControls(screen, ctrl, controls, x, y, width):
	general = _cpe.WidgetTypes.WIDGET_GENERAL
	font = _cpe.FontTypes.GAME_FONT
	for k, c in enumerate(controls):
		kind = c[0]
		if kind == 'label':
			screen.setText(_add('OC4WBCtl%d' % k), 'Background', c[1], 0, x - 3, y + 1, -0.3, font, general, -1, -1)
		elif kind == 'edit':
			name = _add('OC4WBEdit%d' % k, ('edit', c[2], c[3]))
			screen.addEditBoxGFC(name, x, y, width, 24, general, -1, -1, font)
			screen.setEditBoxString(name, c[1])
		elif kind == 'button':
			screen.setButtonGFC(_add('OC4WBCtl%d' % k, ('call', c[2], c[3], ())), c[1], '', x - 1, y, width + 2, 26, general, -1, -1,
				_cpe.ButtonStyles.BUTTON_STYLE_STANDARD)
		elif kind == 'dropdown':
			name = _add('OC4WBDrop%d' % k, ('drop', c[3], c[4]))
			screen.addDropDownBoxGFC(name, x, y, width, general, -1, -1, font)
			for i, choice in enumerate(c[2]):
				screen.addPullDownString(name, choice, i, i, i == c[5])
		elif kind == 'spinner':
			# (an edit box with the value, its up and down buttons at its right)
			sw = _spinWidth(c)
			name = _add('OC4WBEdit%d' % k, ('spinedit', ctrl, k))
			screen.addEditBoxGFC(name, x, y, sw - 21, 24, general, -1, -1, font)
			screen.setEditBoxString(name, u'%d' % int(c[7]))
			w = screen._s.get(name)
			if w is not None:
				w.allowed = u'-0123456789'
			for part, d, style, by in (('OC4WBPlus%d', 1, _SPIN_UP, y), ('OC4WBMinus%d', -1, _SPIN_DOWN, y + 12)):
				b = _add(part % k, ('spin', ctrl, k, d))
				screen.setButtonGFC(b, u'', '', x + sw - 21, by, 21, 12, general, -1, -1, _cpe.ButtonStyles.BUTTON_STYLE_STANDARD)
				_style(screen, b, style)
		y += _CONTROL_H.get(kind, 0) + _gapAfter(controls, k)
	return y


def editDone(name, text):
	"""Enter in one of the edit window's edit boxes: the script told, as the original's."""
	choice = _choices.get(name)
	if choice is None:
		return
	if choice[0] == 'edit':
		_call(choice[1], choice[2], (_u(text),))
	elif choice[0] == 'spinedit':
		ctrl, k = choice[1], choice[2]
		c = list(ctrl.controls[0][k])
		try:
			value = int(text)
		except ValueError:
			value = int(c[7])
		c[7] = max(c[4], min(c[5], value))
		ctrl.controls[0][k] = tuple(c)
		_call(c[2], c[3], (int(c[7]),))
	_draw()


# ---------------------------------------------------------------- input

def handleInput(argsList):
	"""A click on one of the palettes' widgets (oc4screens' input tuple: notify, data, ..., the
	function name at 5, the id at 3)."""
	notify, data, function, wid = argsList[0], argsList[1], argsList[5], argsList[3]
	name = '%s%d' % (function, wid) if function in ('OC4WBTab', 'OC4WBItem', 'OC4WBFly', 'OC4WBCtl', 'OC4WBDrop', 'OC4WBMinus', 'OC4WBPlus', 'OC4WBEdit', 'OC4WBTabScroll') else function
	choice = _choices.get(name)
	if choice is None:
		return 1
	what = choice[0]
	if what == 'tab':
		choice[1].active = choice[2]
		if len(choice) > 3 and choice[3]:
			choice[1].tabFirst = getattr(choice[1], 'tabFirst', 0) + 1
	elif what == 'tabscroll':
		ctrl = choice[1]
		ctrl.tabFirst = max(0, min(choice[3] - 1, getattr(ctrl, 'tabFirst', 0) + choice[2]))
	elif what == 'item':
		_chosen(choice[1], choice[2])
	elif what == 'fly':
		item = choice[1]
		global _flyout
		_flyout = None
		_call(item[1], item[2], (item[3],))
	elif what == 'call':
		_call(choice[1], choice[2], choice[3])
	elif what == 'drop':
		if notify == 11:  # NOTIFY_LISTBOX_ITEM_SELECTED
			_call(choice[1], choice[2], (int(data),))
	elif what == 'spin':
		ctrl, k, d = choice[1], choice[2], choice[3]
		c = list(ctrl.controls[0][k])
		c[7] = max(c[4], min(c[5], c[7] + d * c[6]))
		ctrl.controls[0][k] = tuple(c)
		_call(c[2], c[3], (int(c[7]),))
	elif what in ('edit', 'spinedit'):
		return 1  # (a click into it: it takes the keys; Enter tells the script, editDone)
	elif what == 'close':
		_closeEdit()
	_draw()
	return 1


def _closeEdit():
	"""The edit window's close box: the window gone, the palette of the mode back."""
	_ctrls['edit'].destroy()
	wb = _wb()
	try:
		wb.m_bCtrlEditUp = False
		(_ctrls['map'] if wb.m_bNormalMap else _ctrls['player']).enabled = True
	except Exception:
		_ctrls['player'].enabled = True


def widgetClicked(widget, iData1, iData2):
	"""The WorldBuilder's own buttons (WIDGET_WB_...), as the executable answers them."""
	import CvScreensInterface as si
	W = _cpe.WidgetTypes
	calls = {
		W.WIDGET_WB_ALL_PLOTS_BUTTON: si.WorldBuilderAllPlotsCB, W.WIDGET_WB_LANDMARK_BUTTON: si.WorldBuilderLandmarkCB,
		W.WIDGET_WB_ERASE_BUTTON: si.WorldBuilderEraseCB, W.WIDGET_WB_EXIT_BUTTON: si.WorldBuilderExitCB,
		W.WIDGET_WB_UNIT_EDIT_BUTTON: si.WorldBuilderToggleUnitEditCB, W.WIDGET_WB_CITY_EDIT_BUTTON: si.WorldBuilderToggleCityEditCB,
		W.WIDGET_WB_NORMAL_PLAYER_TAB_MODE_BUTTON: si.WorldBuilderNormalPlayerTabModeCB, W.WIDGET_WB_NORMAL_MAP_TAB_MODE_BUTTON: si.WorldBuilderNormalMapTabModeCB,
		W.WIDGET_WB_REVEAL_TAB_MODE_BUTTON: si.WorldBuilderRevealTabModeCB, W.WIDGET_WB_DIPLOMACY_MODE_BUTTON: si.WorldBuilderDiplomacyModeCB,
		W.WIDGET_WB_REVEAL_ALL_BUTTON: si.WorldBuilderRevealAllCB, W.WIDGET_WB_UNREVEAL_ALL_BUTTON: si.WorldBuilderUnRevealAllCB,
	}
	if widget == W.WIDGET_WB_SAVE_BUTTON:
		_save()
	elif widget == W.WIDGET_WB_LOAD_BUTTON:
		_cpe.CyInterface().addImmediateMessage(u'Loading a scenario: start it from the command line (--map <file>.CivBeyondSwordWBSave)', '')
	elif widget in calls:
		calls[widget]()
	_draw()


def widgetHelp(widget, iData1, iData2):
	"""The help the original shows for the WorldBuilder's own buttons (WIDGET_WB_...); u'' for any
	other widget. Each was seen in the running original under Wine (every button hovered) and is the
	game's text that says exactly that. Where two keys say the same in English (Save,
	Load, Player Mode, Map Mode) the one named for the button or its action is used; the screen
	can't tell them apart."""
	W = _cpe.WidgetTypes
	keys = {
		W.WIDGET_WB_SAVE_BUTTON: 'TXT_KEY_ACTION_SAVE', W.WIDGET_WB_LOAD_BUTTON: 'TXT_KEY_ACTION_LOAD',
		W.WIDGET_WB_ALL_PLOTS_BUTTON: 'TXT_KEY_WB_CHANGE_ALL_PLOTS', W.WIDGET_WB_ERASE_BUTTON: 'TXT_KEY_WB_ERASE_PLOTS',
		W.WIDGET_WB_LANDMARK_BUTTON: 'TXT_KEY_WB_LANDMARK_PLOTS', W.WIDGET_WB_EXIT_BUTTON: 'TXT_KEY_WB_EXIT',
		W.WIDGET_WB_UNIT_EDIT_BUTTON: 'TXT_KEY_WB_EDIT_UNIT', W.WIDGET_WB_CITY_EDIT_BUTTON: 'TXT_KEY_WB_EDIT_CITY',
		W.WIDGET_WB_NORMAL_PLAYER_TAB_MODE_BUTTON: 'TXT_KEY_WB_NORMAL_PLAYER', W.WIDGET_WB_NORMAL_MAP_TAB_MODE_BUTTON: 'TXT_KEY_WB_NORMAL_MAP',
		W.WIDGET_WB_REVEAL_TAB_MODE_BUTTON: 'TXT_KEY_WB_REVEAL_TILE', W.WIDGET_WB_REVEAL_ALL_BUTTON: 'TXT_KEY_WB_REVEAL_ALL',
		W.WIDGET_WB_UNREVEAL_ALL_BUTTON: 'TXT_KEY_WB_UNREVEAL_ALL', W.WIDGET_WB_REGENERATE_MAP: 'TXT_KEY_WB_REGENERATE_MAP',
		W.WIDGET_WB_DIPLOMACY_MODE_BUTTON: 'TXT_KEY_WB_DIPLOMACY',
	}
	key = keys.get(widget)
	return _text(key) if key else u''


def _save():
	"""The world as a WorldBuilder save (CvWBInterface.writeDesc), into the original's folder of
	them: Documents\\My Games\\Beyond the Sword\\Saves\\WorldBuilder, named by the time."""
	import os, time
	import CvWBInterface
	try:
		folder = os.path.join(os.path.dirname(os.path.dirname(oc4host.profile_path())), 'Beyond the Sword', 'Saves', 'WorldBuilder')
		if not os.path.isdir(folder):
			os.makedirs(folder)
		path = os.path.join(folder, time.strftime('WB-%Y%m%d-%H%M%S') + getWBSaveExtension())
		CvWBInterface.writeDesc((path,))
		_cpe.CyInterface().addImmediateMessage(u'Saved: %s' % _u(path), '')
	except Exception, e:
		_cpe.CyInterface().addImmediateMessage(u'Not saved: %s' % _u(e), '')


# The functions the script imports from CvPythonExtensions.
for _name in ('initWBToolPlayerControl', 'getWBToolNormalPlayerTabCtrl', 'addWBPlayerControlTabs', 'getWBToolNormalMapTabCtrl',
		'setWBInitialCtrlTabPlacement', 'initWBToolAdvancedStartControl', 'getWBToolAdvancedStartTabCtrl', 'addWBAdvancedStartControlTabs',
		'initWBToolEditCtrl', 'getWBToolEditTabCtrl', 'initWBToolEditCtrlTab', 'refreshWBEditCtrlReligionButtons',
		'refreshWBEditCtrlCorporationButtons', 'initWBDiplomacyCtrl', 'getWBToolDiplomacyTabCtrl', 'destroyWBDiplomacyCtrl',
		'saveDiplomacySettings', 'setDiplomacySettings', 'isMouseOverGameSurface', 'setFocusToCVG', 'getWBSaveExtension', 'CyGFlyoutMenu'):
	setattr(_cpe, _name, globals()[_name])
