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
_PAGE = 48          # items a page of a palette shows
_COLUMNS = 6
_CELL = 36
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
		self.page = 0
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
		self.page = 0
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
			for i, name in enumerate((u'Peak', u'Hills', u'Flat', u'Water')):
				out.append((1, i, '', name, sel(1, i)))
			k = 0
			for i in range(gc.getNumFeatureInfos()):
				info = gc.getFeatureInfo(i)
				for j in range(info.getNumVarieties()):
					out.append((2, k, info.getButton(), info.getDescription(), sel(2, k)))
					k += 1
			for i in range(gc.getNumRouteInfos()):
				out.append((3, i, gc.getRouteInfo(i).getButton(), gc.getRouteInfo(i).getDescription(), sel(3, i)))
			out.append((3, gc.getNumRouteInfos(), '', u'River', sel(3, gc.getNumRouteInfos())))
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


def _top(screen):
	"""Where the palette starts: under the script's own side menu (setSideMenu: its buttons, then
	the bottom panel of the player's or the map's pull downs, 45 or 85 high), as the original
	places its tab control; not over those pull downs."""
	top = 86
	try:
		for name in ('WorldBuilderBackgroundPanel', 'WorldBuilderBackgroundBottomPanel'):
			w = screen._s.get(name)
			if w is not None and w.shown and w.mode == 'abs':
				top = max(top, w.y + w.h + 2)
	except Exception:
		pass
	return top


def _draw():
	try:
		if not oc4host.ui_world_builder():
			return
		screen = _screen()
	except Exception:
		return
	_clear(screen)
	ctrl = _shown()
	W, H = screen.getXResolution(), screen.getYResolution()
	try:
		width = _wb().iScreenWidth
	except Exception:
		width = 228
	x0, y0 = W - width, _top(screen)
	if ctrl is not None:
		screen.addPanel(_add('OC4WBPalette'), u'', u'', True, False, x0, y0, width, H - y0 - 4, _cpe.PanelStyles.PANEL_STYLE_MAIN)
		y = y0 + 6
		# the sections' tabs
		x = x0 + 6
		for i, name in enumerate(ctrl.sections):
			if not name and ctrl.kind == 'edit' and i == 0:
				name = u'Data'
			label = (u'<color=255,255,0>%s</color>' % name) if i == ctrl.active else name
			screen.setText(_add('OC4WBTab%d' % i, ('tab', ctrl, i)), 'Background', label, 0, x, y, -0.3, _cpe.FontTypes.SMALL_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
			try:
				w = _cpe.CyInterface().determineWidth(name)
			except Exception:
				w = 9 * len(name)
			x += 12 + max(int(w), 9 * len(name))
			if i + 1 < len(ctrl.sections) and x + 9 * len(ctrl.sections[i + 1]) > x0 + width - 6:
				x = x0 + 6
				y += 18
		y += 24
		if ctrl.kind == 'edit' and ctrl.active < len(ctrl.controls) and ctrl.controls[ctrl.active]:
			y = _drawControls(screen, ctrl, x0 + 8, y, width - 16)
		items = _items(ctrl)
		if items:
			pages = (len(items) + _PAGE - 1) // _PAGE
			ctrl.page = max(0, min(ctrl.page, pages - 1))
			shown = items[ctrl.page * _PAGE:(ctrl.page + 1) * _PAGE]
			for k, item in enumerate(shown):
				cx = x0 + 8 + (k % _COLUMNS) * _CELL
				cy = y + (k // _COLUMNS) * _CELL
				name = 'OC4WBItem%d' % k
				if item[4]:
					screen.addPanel(_add('OC4WBSel%d' % k), u'', u'', False, False, cx - 2, cy - 2, _CELL, _CELL, _cpe.PanelStyles.PANEL_STYLE_HUD_HELP)
				if item[2]:
					screen.setImageButton(_add(name, ('item', ctrl, item)), item[2], cx, cy, _CELL - 4, _CELL - 4, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
				else:
					screen.setText(_add(name, ('item', ctrl, item)), 'Background', item[3][:6], 0, cx, cy + 8, -0.3, _cpe.FontTypes.SMALL_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
			y += ((len(shown) + _COLUMNS - 1) // _COLUMNS) * _CELL + 6
			if pages > 1:
				screen.setText(_add('OC4WBPrev', ('page', ctrl, -1)), 'Background', u'<<', 0, x0 + 10, y, -0.3, _cpe.FontTypes.GAME_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
				screen.setText(_add('OC4WBPage'), 'Background', u'%d / %d' % (ctrl.page + 1, pages), 0, x0 + width / 2 - 20, y, -0.3, _cpe.FontTypes.GAME_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
				screen.setText(_add('OC4WBNext', ('page', ctrl, 1)), 'Background', u'>>', 0, x0 + width - 34, y, -0.3, _cpe.FontTypes.GAME_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
	if _flyout is not None and _flyout.items:
		mx, my = 300, 200
		screen.addPanel(_add('OC4WBFlyout'), u'', u'', True, False, mx, my, 240, 24 * len(_flyout.items) + 12, _cpe.PanelStyles.PANEL_STYLE_MAIN)
		for k, item in enumerate(_flyout.items):
			screen.setText(_add('OC4WBFly%d' % k, ('fly', item)), 'Background', item[0], 0, mx + 10, my + 8 + 24 * k, -0.3, _cpe.FontTypes.GAME_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)


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
		choice[1].page = 0
	elif what == 'page':
		choice[1].page += choice[2]
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
