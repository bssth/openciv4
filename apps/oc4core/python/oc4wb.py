## openciv4: the WorldBuilder's engine side, which the original's executable draws for
## CvWorldBuilderScreen: its tool palettes (the player's units, buildings and techs; the map's
## improvements, resources, terrain and territory; a unit's or a city's edit panel with its
## promotions, buildings, religions and corporations), its flyout menu and its own buttons (save,
## the modes, exit). The script builds them by sections (addTabSection) and hears what is
## chosen through CvScreensInterface (WorldBuilderOn...BrushSelected, the edit panel's callbacks);
## here they are widgets of its screen named OC4WB..., whose clicks come back to handleInput.

import CvPythonExtensions as _cpe
import oc4host

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
	try:
		_clear(_screen())
	except Exception:
		del _widgets[:]
		_choices.clear()
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
	"""The edit panel's grids: a unit's promotions; a city's buildings, religions, holy cities,
	corporations and headquarters."""
	e = _ctrls['edit']
	if bUnit:
		e.addTabSection(u'Promotions')
	else:
		for name in (u'Buildings', u'Religions', u'Holy cities', u'Corporations', u'Headquarters'):
			e.addTabSection(name)
	e.enabled = True
	_draw()


def refreshWBEditCtrlReligionButtons(b): _draw()
def refreshWBEditCtrlCorporationButtons(b): _draw()
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
	"""The active section's items: (list, index, button art, label, selected)."""
	gc = _gc
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
				out.append((0, i, gc.getUnitInfo(i).getButton(), gc.getUnitInfo(i).getDescription(), i == current))
		elif tab == 1:
			for i in range(gc.getNumBuildingInfos()):
				out.append((0, i, gc.getBuildingInfo(i).getButton(), gc.getBuildingInfo(i).getDescription(), i == current))
		elif tab == 2:
			try:
				team = gc.getTeam(gc.getPlayer(wb.m_iCurrentPlayer).getTeam())
			except Exception:
				team = None
			for i in range(gc.getNumTechInfos()):
				out.append((0, i, gc.getTechInfo(i).getButton(), gc.getTechInfo(i).getDescription(), team is not None and team.isHasTech(i)))
	elif ctrl.kind == 'map':
		try:
			current, currentList = wb.m_iNormalMapCurrentIndexes[tab], wb.m_iNormalMapCurrentList[tab]
		except Exception:
			current, currentList = -1, -1
		sel = lambda l, i: l == currentList and i == current
		if tab == 0:
			for i in range(gc.getNumImprovementInfos()):
				out.append((0, i, gc.getImprovementInfo(i).getButton(), gc.getImprovementInfo(i).getDescription(), sel(0, i)))
		elif tab == 1:
			for i in range(gc.getNumBonusInfos()):
				out.append((0, i, gc.getBonusInfo(i).getButton(), gc.getBonusInfo(i).getDescription(), sel(0, i)))
		elif tab == 2:
			for i in range(gc.getNumTerrainInfos()):
				out.append((0, i, gc.getTerrainInfo(i).getButton(), gc.getTerrainInfo(i).getDescription(), sel(0, i)))
			# (the plot types and the river with the original's own buttons)
			for i, (name, art) in enumerate(((u'Peak', 'WORLDBUILDER_PLOT_TYPE_MOUNTAIN'), (u'Hills', 'WORLDBUILDER_PLOT_TYPE_HILL'),
					(u'Flat', 'WORLDBUILDER_PLOT_TYPE_PLAINS'), (u'Water', 'WORLDBUILDER_PLOT_TYPE_OCEAN'))):
				out.append((1, i, _art(art), name, sel(1, i)))
			k = 0
			for i in range(gc.getNumFeatureInfos()):
				info = gc.getFeatureInfo(i)
				for j in range(info.getNumVarieties()):
					out.append((2, k, info.getButton(), info.getDescription(), sel(2, k)))
					k += 1
			for i in range(gc.getNumRouteInfos()):
				out.append((3, i, gc.getRouteInfo(i).getButton(), gc.getRouteInfo(i).getDescription(), sel(3, i)))
			out.append((3, gc.getNumRouteInfos(), _art('WORLDBUILDER_RIVER_PLACEMENT'), u'River', sel(3, gc.getNumRouteInfos())))
		elif tab == 3:
			for i in range(gc.getMAX_CIV_PLAYERS()):
				p = gc.getPlayer(i)
				if p.isEverAlive():
					civ = gc.getCivilizationInfo(p.getCivilizationType())
					out.append((0, i, civ.getButton(), p.getCivilizationShortDescription(0), sel(0, i)))
	elif ctrl.kind == 'edit':
		plot = getattr(wb, 'm_pActivePlot', 0)
		if not plot or tab < 1:
			return out
		names = ctrl.sections[tab]
		if names == u'Promotions':
			try:
				unit = plot.getUnit(wb.m_iCurrentUnit)
			except Exception:
				unit = None
			for i in range(gc.getNumPromotionInfos()):
				out.append((0, i, gc.getPromotionInfo(i).getButton(), gc.getPromotionInfo(i).getDescription(), unit is not None and unit.isHasPromotion(i)))
		elif plot.isCity():
			city = plot.getPlotCity()
			if names == u'Buildings':
				for i in range(gc.getNumBuildingInfos()):
					out.append((0, i, gc.getBuildingInfo(i).getButton(), gc.getBuildingInfo(i).getDescription(), city.getNumRealBuilding(i) > 0))
			elif names in (u'Religions', u'Holy cities'):
				for i in range(gc.getNumReligionInfos()):
					on = city.isHasReligion(i) if names == u'Religions' else city.isHolyCityByType(i)
					out.append((0, i, gc.getReligionInfo(i).getButton(), gc.getReligionInfo(i).getDescription(), on))
			elif names in (u'Corporations', u'Headquarters'):
				for i in range(gc.getNumCorporationInfos()):
					on = city.isHasCorporation(i) if names == u'Corporations' else city.isHeadquartersByType(i)
					out.append((0, i, gc.getCorporationInfo(i).getButton(), gc.getCorporationInfo(i).getDescription(), on))
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
		name = ctrl.sections[tab]
		if name in (u'Promotions', u'Buildings'):
			si.WorldBuilderOnWBEditBrushSelected((l, i, tab))
		elif name == u'Religions':
			si.WorldBuilderOnWBEditReligionSelected((l, i, tab))
		elif name == u'Holy cities':
			si.WorldBuilderOnWBEditHolyCitySelected((l, i, tab))
		elif name == u'Corporations':
			si.WorldBuilderOnWBEditCorporationSelected((l, i, tab))
		elif name == u'Headquarters':
			si.WorldBuilderOnWBEditHeadquartersSelected((l, i, tab))


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
_TITLES = {'player': 'TXT_KEY_WB_PLAYER_MODE', 'map': 'TXT_KEY_WB_MAP_MODE', 'edit': 'TXT_KEY_WB_WORLD_BUILDER_EDIT_CONTROL'}
_WX, _WY, _WW = 0, 0, 318
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
	if ctrl is not None:
		dark = u'<color=%d,%d,%d>' % _ROW_TEXT
		tabs = []
		for i, name in enumerate(ctrl.sections):
			if not name and ctrl.kind == 'edit' and i == 0:
				name = u'Data'
			try:
				w = int(oc4screens._textWidth(name, 3))
			except Exception:
				w = 9 * len(name)
			tabs.append((name, w + 20))
		# (as wide as its tabs need: the map mode's four do not fit the player mode's width)
		width = max(_WW, 15 + sum(tw + 1 for name, tw in tabs) + 14)
		body_w = width - 2 * _BODY_X
		# what the edit panel's section holds (a unit's or a city's data) goes over its items
		controls = ctrl.kind == 'edit' and ctrl.active < len(ctrl.controls) and ctrl.controls[ctrl.active]
		items = _items(ctrl)
		body_h = (_GRID_H if items else 0) + (_controlsHeight(ctrl.controls[ctrl.active]) if controls else 0) + 6
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
		# the tabs, each as wide as its word
		x = _WX + 15
		for i, (name, tw) in enumerate(tabs):
			tab = _add('OC4WBTabBack%d' % i)
			screen.addPanel(tab, u'', u'', True, False, x, _WY + _TAB_Y, tw, _TAB_H, _cpe.PanelStyles.PANEL_STYLE_MAIN)
			_style(screen, tab, _TAB_SKIN + ('Select' if i == ctrl.active else 'Default'))
			screen.setText(_add('OC4WBTab%d' % i, ('tab', ctrl, i)), 'Background', dark + name + u'</color>', 1 << 2, x + tw / 2, _WY + _TAB_Y + 4, -0.3,
				_cpe.FontTypes.GAME_FONT, general, -1, -1)
			x += tw + 1
		body = _add('OC4WBBody')
		screen.addPanel(body, u'', u'', True, False, _WX + _BODY_X, _WY + _BODY_Y, body_w, body_h, _cpe.PanelStyles.PANEL_STYLE_IN)
		y = _WY + _BODY_Y + 4
		if controls:
			y = _drawControls(screen, ctrl, _WX + _BODY_X + 8, y, body_w - 16)
		if items:
			grid = _add('OC4WBGrid')
			screen.addScrollPanel(grid, u'', _WX + _BODY_X + 2, y - 2, body_w - 4, _GRID_H, _cpe.PanelStyles.PANEL_STYLE_EXTERNAL)
			for k, item in enumerate(items):
				cx, cy = (k % _COLUMNS) * _CELL, (k // _COLUMNS) * _CELL
				if item[4]:
					screen.attachPanelAt(grid, _add('OC4WBSel%d' % k), u'', u'', False, False, _cpe.PanelStyles.PANEL_STYLE_HUD_HELP, cx - 2, cy - 2, _CELL - 1, _CELL - 1,
						general, -1, -1)
				name = 'OC4WBItem%d' % k
				if item[2]:
					screen.setImageButtonAt(_add(name, ('item', ctrl, item)), grid, item[2], cx, cy, _ICON, _ICON, general, -1, -1)
				else:
					screen.setTextAt(_add(name, ('item', ctrl, item)), grid, item[3][:6], 0, cx, cy + 8, -0.3, _cpe.FontTypes.SMALL_FONT, general, -1, -1)
	if _flyout is not None and _flyout.items:
		mx, my = 300, 200
		screen.addPanel(_add('OC4WBFlyout'), u'', u'', True, False, mx, my, 240, 24 * len(_flyout.items) + 12, _cpe.PanelStyles.PANEL_STYLE_MAIN)
		for k, item in enumerate(_flyout.items):
			screen.setText(_add('OC4WBFly%d' % k, ('fly', item)), 'Background', item[0], 0, mx + 10, my + 8 + 24 * k, -0.3, _cpe.FontTypes.GAME_FONT, general, -1, -1)


def _controlsHeight(controls):
	return sum({'label': 18, 'edit': 22, 'button': 28, 'dropdown': 28, 'spinner': 24}.get(c[0], 0) for c in controls) + 6


def _drawControls(screen, ctrl, x, y, width):
	for k, c in enumerate(ctrl.controls[ctrl.active]):
		kind = c[0]
		if kind == 'label':
			screen.setText(_add('OC4WBCtl%d' % k), 'Background', c[1], 0, x, y, -0.3, _cpe.FontTypes.SMALL_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
			y += 18
		elif kind == 'edit':
			screen.setText(_add('OC4WBCtl%d' % k), 'Background', c[1], 0, x, y, -0.3, _cpe.FontTypes.GAME_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
			y += 22
		elif kind == 'button':
			screen.setButtonGFC(_add('OC4WBCtl%d' % k, ('call', c[2], c[3], ())), c[1], '', x, y, width, 24, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1,
				_cpe.ButtonStyles.BUTTON_STYLE_STANDARD)
			y += 28
		elif kind == 'dropdown':
			name = _add('OC4WBDrop%d' % k, ('drop', c[3], c[4]))
			screen.addDropDownBoxGFC(name, x, y, width, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1, _cpe.FontTypes.GAME_FONT)
			for i, choice in enumerate(c[2]):
				screen.addPullDownString(name, choice, i, i, i == c[5])
			y += 28
		elif kind == 'spinner':
			module, function, lo, hi, step, value = c[2], c[3], c[4], c[5], c[6], c[7]
			screen.setText(_add('OC4WBMinus%d' % k, ('spin', ctrl, k, -1)), 'Background', u'  -  ', 0, x, y, -0.3, _cpe.FontTypes.GAME_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
			screen.setText(_add('OC4WBValue%d' % k), 'Background', u'%d' % int(value), 0, x + 50, y, -0.3, _cpe.FontTypes.GAME_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
			screen.setText(_add('OC4WBPlus%d' % k, ('spin', ctrl, k, 1)), 'Background', u'  +  ', 0, x + 110, y, -0.3, _cpe.FontTypes.GAME_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
			y += 24
	return y + 6


# ---------------------------------------------------------------- input

def handleInput(argsList):
	"""A click on one of the palettes' widgets (oc4screens' input tuple: notify, data, ..., the
	function name at 5, the id at 3)."""
	notify, data, function, wid = argsList[0], argsList[1], argsList[5], argsList[3]
	name = '%s%d' % (function, wid) if function in ('OC4WBTab', 'OC4WBItem', 'OC4WBFly', 'OC4WBCtl', 'OC4WBDrop', 'OC4WBMinus', 'OC4WBPlus') else function
	choice = _choices.get(name)
	if choice is None:
		return 1
	what = choice[0]
	if what == 'tab':
		choice[1].active = choice[2]
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
		c = list(ctrl.controls[ctrl.active][k])
		c[7] = max(c[4], min(c[5], c[7] + d * c[6]))
		ctrl.controls[ctrl.active][k] = tuple(c)
		_call(c[2], c[3], (int(c[7]),))
	_draw()
	return 1


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
		'refreshWBEditCtrlCorporationButtons', 'isMouseOverGameSurface', 'setFocusToCVG', 'getWBSaveExtension', 'CyGFlyoutMenu'):
	setattr(_cpe, _name, globals()[_name])
