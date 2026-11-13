## openciv4: the engine's Python screens, CyGInterfaceScreen and CyInterface.
##
## In the original game Civ4BeyondSword.exe draws the screens the scripts describe through
## CyGInterfaceScreen (CvMainInterface.py, the advisors, the Civilopedia, ...). Here each
## screen keeps its widgets as the scripts set them (positions, attachments, texts, art, the
## widget type and data the DLL acts on) and gives the host a flat display list to draw
## whenever something changed; the host sends the input back through CvScreensInterface.
## CyInterface asks the host's interface (selection, dirty bits, city screen).
##
## Coordinates are screen pixels. setX / addX place a widget absolutely, setXAt / addXAt
## relative to the widget it is attached to, attachX into its parent's flow (top to bottom,
## or left to right in a horizontal panel). A widget shows when it and the widgets it is
## attached to are shown; scroll panels clip what they hold.
##
## Tables, list boxes, scroll panels and multi-line texts in a box of their own clip what they
## hold and scroll with the mouse wheel. A click on a pull down opens its list over the
## screens; a click on a row of a table or a list box selects it (Ctrl adds or takes one,
## Shift a range) and the script hears NOTIFY_LISTBOX_ITEM_SELECTED.

import time
import oc4host
import CvPythonExtensions as _cpe

_FONT_LEFT, _FONT_RIGHT, _FONT_CENTER = 1 << 0, 1 << 1, 1 << 2

_version = [1]          # bumped on every change the display list shows
_screens = {}           # python file id -> _Screen
_resolution = [1024, 768]
_logged = set()
_citySelection = [()]   # the cities a table's rows selected last
_drop = [None, 0]       # the pull down whose list is open (file id, name), and how far that list is scrolled
_areas = []             # what scrolls under the mouse, back to front: (x0, y0, x1, y1, file id, name, step, down, sideways)
_bars = {}              # the scroll bars: (file id, name, part) -> (x0, y0, x1, y1)
_dropRects = {}         # where each pull down is: (file id, name) -> (x0, y0, x1, y1)


def _changed():
	_version[0] += 1


beforeDisplay = [] # (called before the display list is made: oc4options lays its window out)


def _log(what):
	if what not in _logged:
		_logged.add(what)
		oc4host.unimplemented(what)


def _text(value):
	if value is None:
		return u''
	if isinstance(value, unicode):
		return value
	if isinstance(value, str):
		try:
			return value.decode('utf-8')
		except UnicodeDecodeError:
			return value.decode('latin-1')
	return unicode(value)


def _style(enum, value):
	"""A style the scripts pass as a number: the name of its value (PANEL_STYLE_EMPTY), as the theme knows it."""
	try:
		return str(enum.values[int(value)])
	except (KeyError, ValueError, TypeError, AttributeError):
		return _str(value)


def _str(value):
	if value is None:
		return ''
	if isinstance(value, unicode):
		return value.encode('utf-8')
	return str(value)


def _int(value, default=-1):
	try:
		return int(value)
	except (TypeError, ValueError):
		return default


class _Widget(object):
	__slots__ = ('name', 'kind', 'parent', 'mode', 'x', 'y', 'z', 'w', 'h', 'style', 'pstyle', 'text', 'help', 'tex', 'tex2',
		'wt', 'd1', 'd2', 'opt', 'flags', 'font', 'shown', 'enabled', 'state', 'hittest', 'seq', 'vertical', 'scroll', 'children',
		'bars', 'lists', 'columns', 'rows', 'headers', 'iconw', 'iconh', 'items', 'selected', 'color', 'size', 'defw', 'defh', 'points',
		'offset', 'extent', 'picked', 'hoffset', 'hextent', 'cells', 'sort')

	def __init__(self, name, kind):
		self.name = name
		self.kind = kind
		self.parent = None
		self.mode = 'abs'      # abs / rel (to the parent) / flow (in the parent's layout)
		self.x = self.y = self.w = self.h = 0
		self.z = 0.0
		self.style = ''
		self.pstyle = 0
		self.text = u''
		self.help = u''
		self.tex = self.tex2 = ''
		self.wt, self.d1, self.d2, self.opt = -1, -1, -1, False
		self.flags = _FONT_LEFT
		self.font = 1
		self.shown = True
		self.enabled = True
		self.state = False
		self.hittest = 0
		self.seq = 0
		self.vertical = True
		self.scroll = False
		self.children = []
		self.bars = None       # stacked bars: [[percent, (r, g, b, a)], ...]
		self.lists = None      # multi lists: [[(texture, wt, d1, d2, option, disabledTexture), ...], ...]
		self.columns = None    # tables: [[header, width, right], ...]
		self.rows = None       # tables: [{column: (text, icon, wt, d1, d2, justify)}, ...]
		self.headers = True
		self.iconw = self.iconh = 0
		self.items = None      # list boxes and pull downs: [(text, wt, d1, d2, justify), ...]
		self.selected = -1     # list boxes and pull downs: the item; tables: the row a Shift range starts from
		self.picked = frozenset()  # tables: the selected rows
		self.offset = 0        # scrolled by, in pixels
		self.extent = 0        # what it holds is as high, in pixels
		self.hoffset = 0       # scroll panels: scrolled sideways by
		self.hextent = 0       # scroll panels: what they hold is as wide
		self.color = None
		self.size = 0
		self.defw = self.defh = 0
		self.points = None     # lines
		self.cells = None      # tables: {(row, column): the name of the control attached there}
		self.sort = None       # tables that sort: (column, descending) the rows are shown by, or (-1, False)


class _Screen(object):
	def __init__(self, name, fileId):
		self.name = name
		self.id = fileId
		self.widgets = {}
		self.order = []        # roots, in creation order
		self.seq = 0
		self.shown = False
		self.main = False
		self.dims = None
		self.helpArea = None   # (x, y, width, minWidth, expandRight, expandDown)
		self.helpText = u''
		self.minimap = None
		self.mm = None         # its own minimap's layers (not the main interface's): see _Minimap
		self.hideLists = {}
		self.endTurn = None    # (text, shown)
		self.closeOnEscape = True

	def add(self, name, kind, parent=None, mode='abs'):
		name = _str(name)
		if not name:
			# (no name: a widget of its own each time, as the engine makes one; the civics screen's
			# headings, the foreign advisor's leader buttons)
			self.seq += 1
			name = '\x01%d' % self.seq
		old = self.widgets.get(name)
		place = None
		if old is not None:
			# (made again under its name: it keeps its place among the others, before and after
			# it, as the engine changes a widget it has; the tech screen's title, a label kept
			# as it is, stays over its top bar, a panel made again)
			siblings = self.widgets[old.parent].children if old.parent in self.widgets else self.order
			place = (old.seq, old.parent, siblings.index(name) if name in siblings else None)
			self.remove(name)
		w = _Widget(name, kind)
		if place is None:
			self.seq += 1
			w.seq = self.seq
		else:
			w.seq = place[0]
		parent = _str(parent) if parent else None
		if parent in (None, '', 'Background') or parent not in self.widgets:
			w.parent = None
			if place is not None and place[1] is None and place[2] is not None:
				self.order.insert(place[2], name)
			else:
				self.order.append(name)
			if mode == 'flow':
				mode = 'abs'
		else:
			w.parent = parent
			children = self.widgets[parent].children
			if place is not None and place[1] == parent and place[2] is not None:
				children.insert(place[2], name)
			else:
				children.append(name)
		w.mode = mode
		self.widgets[name] = w
		_changed()
		return w

	def remove(self, name):
		w = self.widgets.pop(name, None)
		if w is None:
			return
		for c in list(w.children):
			self.remove(c)
		if w.parent and w.parent in self.widgets:
			p = self.widgets[w.parent]
			if name in p.children:
				p.children.remove(name)
		elif name in self.order:
			self.order.remove(name)
		_changed()

	def get(self, name):
		return self.widgets.get(_str(name))


_mmVersion = [0]


class _Minimap(object):
	"""A screen's own minimap (the military advisor's, the replay's): the owners' colours over
	the terrain that strong, the plots coloured ((x, y) -> RGBA packed, r lowest) and flashing
	((x, y) -> (RGBA, until or None)), a replay's map size (None: the game's)."""
	def __init__(self):
		self.mode = -1
		self.territory = 0.0
		self.colors = {}
		self.flash = {}
		self.size = None
		self.touch()

	def touch(self):
		_mmVersion[0] += 1
		self.version = _mmVersion[0]


def _packColor(iColor, fAlpha):
	"""A colour info and an opacity as RGBA8 in one number (r lowest); None for none."""
	if iColor < 0:
		return None
	try:
		c = _cpe.CyGlobalContext().getColorInfo(iColor).getColor()
	except Exception:
		return None
	a = int(max(0.0, min(1.0, c.a * float(fAlpha))) * 255)
	if a <= 0:
		return None
	return int(c.r * 255) | (int(c.g * 255) << 8) | (int(c.b * 255) << 16) | (a << 24)


def shownScreens():
	"""The screens shown over the main interface (their file ids): the ones the engine updates
	every frame."""
	return [s.id for s in _screens.values() if s.shown and not s.main]


def minimapOverlay(version):
	"""The shown screen's own minimap, when it changed since that version: (version, width,
	height, the game's map, territory, [(x, y, RGBA)], [(x, y, RGBA)], the whole map (a replay's));
	None if unchanged, () for none (sizes 0: the game's map)."""
	for s in sorted(_screens.values(), key=lambda s: -s.id):
		if s.shown and not s.main and s.minimap is not None and s.mm is not None:
			mm = s.mm
			if mm.version == version:
				return None
			m = _cpe.CyMap()
			same = mm.size is None or mm.size == (m.getGridWidth(), m.getGridHeight())
			w, h = (0, 0) if same else mm.size
			# (on the game's map, where the window shows them: a mod's viewport, AND2's, moves them)
			vx, vy = (lambda x: x), (lambda y: y)
			if same and hasattr(m, 'getViewportXFromMapX'):
				vx, vy = m.getViewportXFromMapX, m.getViewportYFromMapY
			return (mm.version, w, h, same, mm.territory, [(vx(x), vy(y), c) for (x, y), c in mm.colors.items()],
				[(vx(x), vy(y), f[0]) for (x, y), f in mm.flash.items()], mm.size is not None)
	return () if version != 0 else None


def _screenOf(fileId, name):
	s = _screens.get(fileId)
	if s is None:
		s = _screens[fileId] = _Screen(name, fileId)
	return s


def _set(w, attr, value):
	if getattr(w, attr) != value:
		setattr(w, attr, value)
		_changed()


_heads = {}   # a leaderhead widget's name -> [leader, the gesture last asked of it, how many have been]


def _headAction(s, name, action, leader=None):
	"""A gesture asked of a leader's head (LeaderheadAction): kept for the widget of that name,
	made again or not yet made, so the host sees each one once."""
	head = _heads.get(name)
	if head is None or (leader is not None and head[0] != leader):
		head = _heads[name] = [leader if leader is not None else -1, -1, 0]
	head[1], head[2] = action, head[2] + 1
	w = s.get(name) if s is not None else None
	if w is not None and w.kind == 'leaderhead':
		_set(w, 'text', u'%s %d %d' % ((w.text.split() or [u'-1'])[0], action, head[2]))


_LAUNCH_PAD = 'art/interface/screens/spaceship/launchpad.nif'
_LAUNCH_PAD_KFM = 'art/interface/screens/spaceship/launchpad.kfm'
_spaceship = {'project': -1, 'zoom': None, 'appear': set(), 'launched': False}
# Its parts: the KFM's event codes from their base (none at the base; a part's preview, its
# coming on and on: + 5, + 10, + 11, ten more each next one or each next kind), and their cameras.
_SPACESHIP_PARTS = {
	'PROJECT_SS_THRUSTERS': (2000, 'count', 1600), 'PROJECT_SS_ENGINE': (3000, 'count', 1300),
	'PROJECT_SS_COCKPIT': (4000, 'kind', 1100), 'PROJECT_SS_DOCKING_BAY': (5000, 'kind', 1200),
	'PROJECT_SS_LIFE_SUPPORT': (6000, 'kind', 1400), 'PROJECT_SS_STASIS_CHAMBER': (7000, 'kind', 1500),
	'PROJECT_SS_CASING': (8100, 'casing', 100),
}


def _spaceshipPart(iProject):
	try:
		t = _cpe.CyGlobalContext().getProjectInfo(_int(iProject)).getType()
	except Exception:
		return (None, None, None)
	return _SPACESHIP_PARTS.get(t, (None, None, None))


def _spaceshipLayers():
	"""The launch pad's clips for the active team's ship: "camera=...;code:mode;..."."""
	gc = _cpe.CyGlobalContext()
	team = gc.getTeam(_cpe.CyGame().getActiveTeam())
	out = ['camera=Spacecraft Camera']
	any_part = False
	for p in range(gc.getNumProjectInfos()):
		base, kind, camera = _spaceshipPart(p)
		if base is None:
			continue
		any_part = True
		count = team.getProjectCount(p)
		instances = range(5) if kind == 'casing' else [0]
		for n in instances:
			b = base + 100 * n if kind == 'casing' else base
			out.append('%d:e' % b)
		for n in range(count):
			art = team.getProjectArtType(p, n)
			pending = art < 0
			if pending:
				art = max(team.getProjectDefaultArtType(p), 0)
			if kind == 'count':
				first = base + 10 * n
			elif kind == 'casing':
				first = base + 100 * n + 10 * art
			else:
				first = base + 10 * art
			if pending:
				out.append('%d:l' % (first + 5))
			elif (p, n) in _spaceship['appear']:
				out.append('%d:o' % (first + 10))
			else:
				out.append('%d:e' % (first + 11))
	if not any_part:
		return ''
	# The camera's clip first: it has the parts' alphas too (at 0), which the parts' own clips
	# then set (the original plays it under them).
	zoom = _spaceship['zoom']
	if _spaceship['launched']:
		camera = ['1001:o']
		out += ['3031:l', '3041:l']
	elif zoom is not None:
		base, kind, code = _spaceshipPart(zoom)
		camera = []
		if code is not None:
			if kind == 'casing':
				code += 100 * max(team.getProjectCount(zoom) - 1, 0)
			camera = ['%d:o' % code]
	else:
		camera = ['1000:l']
	return ';'.join(out[:1] + camera + out[1:])


class CyGInterfaceScreen(object):
	"""The scripts' view of a screen; every instance of the same file id is the same screen."""

	def __init__(self, szName, iPythonFileID):
		self._s = _screenOf(_int(iPythonFileID, 0), _str(szName))

	def __getattr__(self, name):
		if name.startswith('__'):
			raise AttributeError(name)
		_log('CyGInterfaceScreen.' + name)
		return lambda *args, **kwargs: 0

	# ---------------------------------------------------------------- the screen

	def getXResolution(self):
		return _resolution[0]

	def getYResolution(self):
		return _resolution[1]

	def centerX(self, iX):
		return int(iX) + (_resolution[0] - 1024) / 2

	def centerY(self, iY):
		return int(iY) + (_resolution[1] - 768) / 2

	def setDimensions(self, iX, iY, iWidth, iHeight):
		if self._s.dims != (int(iX), int(iY), int(iWidth), int(iHeight)):
			self._s.dims = (int(iX), int(iY), int(iWidth), int(iHeight))
			_changed()

	def showScreen(self, bState=0, bPassInput=False):
		if not self._s.shown:
			self._s.shown = True
			_changed()

	def hideScreen(self):
		if self._s.shown:
			self._s.shown = False
			_forget(self._s)
			_changed()
			oc4host.screen_closed(self._s.id)

	def isActive(self):
		return self._s.shown or self._s.main

	def setMainInterface(self, bMain):
		self._s.main = bool(bMain)
		_changed()

	def getPythonFileID(self):
		return self._s.id

	def getCurrentTime(self):
		return time.clock()

	def setCloseOnEscape(self, bCloseOnEscape):
		self._s.closeOnEscape = bool(bCloseOnEscape)

	def setForcedRedraw(self, b): pass
	def isRequiredForcedRedraw(self): return False
	def setRenderInterfaceOnly(self, b): pass
	def getRenderInterfaceOnly(self): return False
	def setScreenGroup(self, iGroup): pass
	def getScreenGroup(self): return 0
	def setSound(self, szSound): pass
	def setSoundId(self, iSound): pass
	def showWindowBackground(self, bShow): pass
	def setPersistent(self, b): pass
	def isPersistent(self): return False
	def setAlwaysShown(self, b): pass
	def isAlwaysShown(self): return False
	def setDying(self, b): pass
	def setShowFor(self, iMilliseconds): pass
	def setExitText(self, *args): pass
	def enableWorldSounds(self, b): pass
	def setFocus(self, szName): pass
	def setActivation(self, szName, activation): pass
	def setToolTipAlignment(self, szName, eAlign): pass
	def setViewMin(self, *args): pass
	def markRenderTexturesDirty(self): pass
	# ---------------------------------------------------------------- the spaceship screen
	# The launch pad (art/interface/screens/spaceship/launchpad.nif) behind the screen's panel:
	# its KFM's clips as layers, each part by its count or kind (A, B, C), the camera's.

	def setSpaceShip(self, iProject):
		_spaceship['project'] = _int(iProject)
		_spaceship['zoom'] = None
		_spaceship['appear'] = set()
		_spaceship['launched'] = False
		self._spaceshipScene()

	def addSpaceShipWidgetGFC(self, szName, iX, iY, iWidth, iHeight, iProject, iArtType, eWidgetType=-1, iData1=-1, iData2=-1):
		# (a part's own picture in its panel: the launch pad shows them all)
		self._spaceshipScene()

	def spaceShipCanChangeType(self, iProject):
		team = _cpe.CyGlobalContext().getTeam(_cpe.CyGame().getActiveTeam())
		return _spaceshipPart(iProject)[1] in ('kind', 'casing') and team.getProjectCount(_int(iProject)) > 0

	def spaceShipChangeType(self, iProject):
		team = _cpe.CyGlobalContext().getTeam(_cpe.CyGame().getActiveTeam())
		team.setProjectDefaultArtType(_int(iProject), (max(team.getProjectDefaultArtType(_int(iProject)), 0) + 1) % 3)
		self._spaceshipScene()

	def spaceShipZoom(self, iProject):
		_spaceship['zoom'] = _int(iProject)
		self._spaceshipScene()

	def spaceShipFinalize(self):
		# The parts built given their kinds (the defaults chosen), shown as they come on.
		gc = _cpe.CyGlobalContext()
		team = gc.getTeam(_cpe.CyGame().getActiveTeam())
		for p in range(gc.getNumProjectInfos()):
			if not gc.getProjectInfo(p).isSpaceship():
				continue
			for n in range(team.getProjectCount(p)):
				if team.getProjectArtType(p, n) < 0:
					team.setProjectArtType(p, n, max(team.getProjectDefaultArtType(p), 0))
					_spaceship['appear'].add((p, n))
		_spaceship['project'] = -1
		self._spaceshipScene()

	def spaceShipLaunch(self):
		gc = _cpe.CyGlobalContext()
		victory = -1
		for p in range(gc.getNumProjectInfos()):
			if gc.getProjectInfo(p).isSpaceship():
				victory = gc.getProjectInfo(p).getVictoryPrereq()
		if victory >= 0:
			oc4host.spaceship_launch(victory)
		_spaceship['launched'] = True
		self._spaceshipScene()

	def _spaceshipScene(self):
		layers = _spaceshipLayers()
		if not layers:
			return
		w = self._image("model", "OC4SpaceShipScene", None, _LAUNCH_PAD, 0, 0, self.getXResolution(), self.getYResolution(), -1, -1, -1, 'abs')
		w.tex2 = _LAUNCH_PAD_KFM
		w.text = u'0 0 1 1||' + layers
		# (behind the screen's own widgets)
		if w.name in self._s.order:
			self._s.order.remove(w.name)
			self._s.order.insert(0, w.name)
		w.seq = 0

	# ---------------------------------------------------------------- showing, moving

	def show(self, szName):
		w = self._s.get(szName)
		if w is not None:
			_set(w, 'shown', True)

	def hide(self, szName):
		w = self._s.get(szName)
		if w is not None:
			_set(w, 'shown', False)

	def enable(self, szName, bEnable):
		w = self._s.get(szName)
		if w is not None:
			_set(w, 'enabled', bool(bEnable))

	def setState(self, szName, bState):
		w = self._s.get(szName)
		if w is not None:
			_set(w, 'state', bool(bState))

	def getCheckBoxState(self, szName):
		w = self._s.get(szName)
		return bool(w and w.state)

	def setStyle(self, szName, szStyle):
		w = self._s.get(szName)
		if w is not None:
			_set(w, 'style', _str(szStyle))

	def setHitTest(self, szName, hitTest):
		w = self._s.get(szName)
		if w is not None:
			_set(w, 'hittest', _int(hitTest, 0))

	def moveItem(self, szName, fX, fY, fZ):
		w = self._s.get(szName)
		if szName == 'EndTurnText' or (w is None and _str(szName) == 'EndTurnText'):
			w = self._s.get('EndTurnText') or self._s.add('EndTurnText', 'endturn')
		if w is not None:
			if (w.x, w.y, w.z) != (int(fX), int(fY), float(fZ)):
				w.x, w.y, w.z = int(fX), int(fY), float(fZ)
				_changed()

	def _restack(self, szName, front):
		w = self._s.get(szName)
		if w is not None:
			self._s.seq += 1
			w.seq = self._s.seq if front else -self._s.seq
			_changed()

	def moveToFront(self, szName): self._restack(szName, True)
	def moveForward(self, szName): self._restack(szName, True)
	def moveToBack(self, szName): self._restack(szName, False)
	def moveBackward(self, szName): self._restack(szName, False)

	def deleteWidget(self, pszName):
		self._s.remove(_str(pszName))

	def registerHideList(self, szNames, iSize, iID):
		self._s.hideLists[int(iID)] = [_str(n) for n in list(szNames)[:int(iSize)]]

	def hideList(self, iID):
		for n in self._s.hideLists.get(int(iID), []):
			self.hide(n)

	# ---------------------------------------------------------------- panels

	def addPanel(self, szName, title, helpText, bVerticalLayout, bScrollable, iX, iY, iWidth, iHeight, eStyle):
		w = self._s.add(szName, 'panel')
		w.text, w.help, w.vertical, w.scroll = _text(title), _text(helpText), bool(bVerticalLayout), bool(bScrollable)
		w.x, w.y, w.w, w.h, w.pstyle = int(iX), int(iY), int(iWidth), int(iHeight), _style(_cpe.PanelStyles, eStyle)

	def addScrollPanel(self, szName, title, iX, iY, iWidth, iHeight, eStyle):
		w = self._s.add(szName, 'panel')
		w.text, w.scroll = _text(title), True
		w.x, w.y, w.w, w.h, w.pstyle = int(iX), int(iY), int(iWidth), int(iHeight), _style(_cpe.PanelStyles, eStyle)

	def attachPanel(self, szAttachTo, szName, title, helpText, bVerticalLayout, bScrollable, eStyle):
		w = self._s.add(szName, 'panel', szAttachTo, 'flow')
		w.text, w.help, w.vertical, w.scroll, w.pstyle = _text(title), _text(helpText), bool(bVerticalLayout), bool(bScrollable), _style(_cpe.PanelStyles, eStyle)

	def attachPanelAt(self, szAttachTo, szName, title, helpText, bVerticalLayout, bScrollable, eStyle, iX, iY, iWidth, iHeight,
			eWidgetType=-1, iData1=-1, iData2=-1):
		w = self._s.add(szName, 'panel', szAttachTo, 'rel')
		w.text, w.help, w.vertical, w.scroll, w.pstyle = _text(title), _text(helpText), bool(bVerticalLayout), bool(bScrollable), _style(_cpe.PanelStyles, eStyle)
		w.x, w.y, w.w, w.h = int(iX), int(iY), int(iWidth), int(iHeight)
		w.wt, w.d1, w.d2 = _int(eWidgetType), _int(iData1), _int(iData2)

	def setPanelSize(self, szName, iX, iY, iWidth, iHeight):
		w = self._s.get(szName)
		if w is not None and (w.x, w.y, w.w, w.h) != (int(iX), int(iY), int(iWidth), int(iHeight)):
			w.x, w.y, w.w, w.h = int(iX), int(iY), int(iWidth), int(iHeight)
			_changed()

	def setPanelColor(self, szName, iRed, iGreen, iBlue):
		w = self._s.get(szName)
		if w is not None:
			_set(w, 'color', (int(iRed) / 255.0, int(iGreen) / 255.0, int(iBlue) / 255.0, 1.0))

	def attachSeparator(self, szAttachTo, szName, bVertical, fLength=0):
		w = self._s.add(szName, 'separator', szAttachTo, 'flow')
		w.vertical, w.size = bool(bVertical), int(fLength)

	# ---------------------------------------------------------------- pictures and buttons

	def _image(self, kind, szName, szAttachTo, szTexture, iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, mode):
		w = self._s.add(szName, kind, szAttachTo, mode)
		w.tex = _str(szTexture)
		w.x, w.y, w.w, w.h = int(iX), int(iY), int(iWidth), int(iHeight)
		w.wt, w.d1, w.d2 = _int(eWidgetType), _int(iData1), _int(iData2)
		return w

	def addDDSGFC(self, szName, szTexture, iX, iY, iWidth, iHeight, eWidgetType=-1, iData1=-1, iData2=-1):
		self._image('image', szName, None, szTexture, iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, 'abs')

	def addDDSGFCAt(self, szName, szAttachTo, szTexture, iX, iY, iWidth, iHeight, eWidgetType=-1, iData1=-1, iData2=-1, bOption=False):
		self._image('image', szName, szAttachTo, szTexture, iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, 'rel').opt = bool(bOption)

	def changeDDSGFC(self, szName, szTexture):
		w = self._s.get(szName)
		if w is not None:
			_set(w, 'tex', _str(szTexture))

	def addDrawControl(self, szName, szTexture, iX, iY, iWidth, iHeight, eWidgetType=-1, iData1=-1, iData2=-1):
		self._image('image', szName, None, szTexture, iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, 'abs')

	def addDrawControlAt(self, szName, szAttachTo, szTexture, iX, iY, iWidth, iHeight, eWidgetType=-1, iData1=-1, iData2=-1):
		self._image('image', szName, szAttachTo, szTexture, iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, 'rel')

	def changeDrawControl(self, szName, szTexture):
		self.changeDDSGFC(szName, szTexture)

	def setImageButton(self, szName, szTexture, iX, iY, iWidth, iHeight, eWidgetType=-1, iData1=-1, iData2=-1):
		self._image('imagebutton', szName, None, szTexture, iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, 'abs')

	def setImageButtonAt(self, szName, szAttachTo, szTexture, iX, iY, iWidth, iHeight, eWidgetType=-1, iData1=-1, iData2=-1):
		self._image('imagebutton', szName, szAttachTo, szTexture, iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, 'rel')

	def changeImageButton(self, szName, szTexture):
		self.changeDDSGFC(szName, szTexture)

	def attachImageButton(self, szAttachTo, szName, szTexture, eSize, eWidgetType=-1, iData1=-1, iData2=-1, bOption=False):
		size = {0: 46, 1: 32, 2: 24, 3: 16}.get(_int(eSize, 1), 32)
		w = self._image('imagebutton', szName, szAttachTo, szTexture, 0, 0, size, size, eWidgetType, iData1, iData2, 'flow')
		w.opt = bool(bOption)

	def setButtonGFC(self, szName, szText, szTexture, iX, iY, iWidth, iHeight, eWidgetType=-1, iData1=-1, iData2=-1, eStyle=0):
		w = self._image('button', szName, None, szTexture, iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, 'abs')
		w.text, w.pstyle = _text(szText), _style(_cpe.ButtonStyles, eStyle)

	def attachButtonGFC(self, szAttachTo, szName, szText, eWidgetType=-1, iData1=-1, iData2=-1, eStyle=0):
		w = self._image('button', szName, szAttachTo, '', 0, 0, 0, 24, eWidgetType, iData1, iData2, 'flow')
		w.text, w.pstyle = _text(szText), _style(_cpe.ButtonStyles, eStyle)

	def addCheckBoxGFC(self, szName, szTexture, szHiliteTexture, iX, iY, iWidth, iHeight, eWidgetType=-1, iData1=-1, iData2=-1, eStyle=0):
		w = self._image('checkbox', szName, None, szTexture, iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, 'abs')
		w.tex2, w.pstyle = _str(szHiliteTexture), _style(_cpe.ButtonStyles, eStyle)

	def addCheckBoxGFCAt(self, szAttachTo, szName, szTexture, szHiliteTexture, iX, iY, iWidth, iHeight, eWidgetType=-1, iData1=-1, iData2=-1,
			eStyle=0, bOption=False):
		w = self._image('checkbox', szName, szAttachTo, szTexture, iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, 'rel')
		w.tex2, w.pstyle, w.opt = _str(szHiliteTexture), _style(_cpe.ButtonStyles, eStyle), bool(bOption)

	def attachCheckBoxGFC(self, szAttachTo, szName, szTexture, szHiliteTexture, iWidth, iHeight, eWidgetType=-1, iData1=-1, iData2=-1, eStyle=0):
		w = self._image('checkbox', szName, szAttachTo, szTexture, 0, 0, iWidth, iHeight, eWidgetType, iData1, iData2, 'flow')
		w.tex2, w.pstyle = _str(szHiliteTexture), _style(_cpe.ButtonStyles, eStyle)

	# 3D widgets (leaderheads, units, buildings, plots): their button art until there is a view.
	def _graphic(self, szName, szAttachTo, iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, tex=''):
		return self._image('graphic', szName, szAttachTo, tex, iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, 'rel' if szAttachTo else 'abs')

	def addLeaderheadGFC(self, szName, eLeader, eAttitude, iX, iY, iWidth, iHeight, eWidgetType=-1, iData1=-1, iData2=-1):
		"""A leader's head: the host draws the art's model in its scene, greeting, then resting in
		the idle of its attitude, and the gestures asked of it (text: "attitude action count")."""
		name, leader = _str(szName), _int(eLeader)
		head = _heads.get(name)
		if head is None or head[0] != leader:
			head = _heads[name] = [leader, -1, 0]
		w = self._image('leaderhead', szName, None, str(leader), iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, 'abs')
		w.text = u'%d %d %d' % (_int(eAttitude, -1), head[1], head[2])

	def performLeaderheadAction(self, szName, eAction):
		_headAction(self._s, _str(szName), _int(eAction, -1))

	def setLeaderheadMood(self, szName, eAttitude):
		w = self._s.get(_str(szName))
		if w is not None and w.kind == 'leaderhead':
			parts = w.text.split()
			_set(w, 'text', u'%d %s %s' % (_int(eAttitude, -1), parts[1], parts[2]))

	def _model(self, szName, art, button, iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, fXRot, fZRot, fScale, bAnimate):
		"""A model on its own (the host draws the art's NIF, in its KFM's idle, turned so):
		text "xrot zrot scale animate|button" (the button where the model cannot be drawn)."""
		nif = kfm = ''
		try:
			nif = _str(art.getNIF())
			kfm = _str(art.getKFM())
		except Exception:
			pass
		if not nif:
			return self._graphic(szName, None, iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, button)
		w = self._image('model', szName, None, nif, iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, 'abs')
		w.tex2 = kfm
		w.text = u'%g %g %g %d|%s' % (float(fXRot), float(fZRot), float(fScale), 1 if bAnimate else 0, _str(button))
		return w

	def _unitArt(self, iUnit, pUnit=None):
		gc = _cpe.CyGlobalContext()
		info = gc.getUnitInfo(iUnit)
		era, style = -1, -1
		try:
			if pUnit is not None:
				owner = gc.getPlayer(pUnit.getOwner())
				era = owner.getCurrentEra()
				style = gc.getCivilizationInfo(owner.getCivilizationType()).getUnitArtStyleType()
		except Exception:
			pass
		try:
			return info.getArtInfo(0, era, style)
		except Exception:
			return None

	def addUnitGraphicGFC(self, szName, iUnit, iX, iY, iWidth, iHeight, eWidgetType=-1, iData1=-1, iData2=-1, fXRot=0, fZRot=0, fScale=1, bAnimate=True):
		if _int(iUnit) < 0:
			return self._graphic(szName, None, iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, '')
		button = _cpe.CyGlobalContext().getUnitInfo(_int(iUnit)).getButton()
		self._model(szName, self._unitArt(_int(iUnit)), button, iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, fXRot, fZRot, fScale, bAnimate)

	def addSpecificUnitGraphicGFC(self, szName, pUnit, iX, iY, iWidth, iHeight, eWidgetType=-1, iData1=-1, iData2=-1, fXRot=0, fZRot=0, fScale=1, bAnimate=True):
		button, art = '', None
		try:
			button = pUnit.getButton()
			art = self._unitArt(pUnit.getUnitType(), pUnit)
		except Exception:
			pass
		self._model(szName, art, button, iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, fXRot, fZRot, fScale, bAnimate)

	def addBuildingGraphicGFC(self, szName, iBuilding, iX, iY, iWidth, iHeight, eWidgetType=-1, iData1=-1, iData2=-1, fXRot=0, fZRot=0, fScale=1, bAnimate=True):
		if _int(iBuilding) < 0:
			return self._graphic(szName, None, iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, '')
		info = _cpe.CyGlobalContext().getBuildingInfo(_int(iBuilding))
		art = None
		try:
			art = info.getArtInfo()
		except Exception:
			pass
		self._model(szName, art, info.getButton(), iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, fXRot, fZRot, fScale, bAnimate)

	def addReligionMovieWidgetGFC(self, szName, szFile, iX, iY, iWidth, iHeight, eWidgetType=-1, iData1=-1, iData2=-1):
		"""A religion's founding "movie": a NIF scene with its own camera, played."""
		w = self._image('model', szName, None, _str(szFile), iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, 'abs')
		w.tex2 = ''
		w.text = u'0 0 1 1|'
		return w
	def addPlotGraphicGFC(self, *args): _log('CyGInterfaceScreen.addPlotGraphicGFC')
	# (the models are the engine's; their buttons stand in for them, as for units and buildings)
	def addImprovementGraphicGFC(self, szName, iImprovement, iX, iY, iWidth, iHeight, eWidgetType=-1, iData1=-1, iData2=-1, fXRot=0, fZRot=0,
			fScale=1, bAnimate=True):
		if _int(iImprovement) < 0:
			return self._graphic(szName, None, iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, '')
		info = _cpe.CyGlobalContext().getImprovementInfo(_int(iImprovement))
		art = None
		try:
			art = _cpe.CyArtFileMgr().getImprovementArtInfo(info.getArtDefineTag())
		except Exception:
			pass
		self._model(szName, art, info.getButton(), iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, fXRot, fZRot, fScale, bAnimate)

	def addBonusGraphicGFC(self, szName, iBonus, iX, iY, iWidth, iHeight, eWidgetType=-1, iData1=-1, iData2=-1, fXRot=0, fZRot=0, fScale=1,
			bAnimate=True):
		if _int(iBonus) < 0:
			return self._graphic(szName, None, iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, '')
		info = _cpe.CyGlobalContext().getBonusInfo(_int(iBonus))
		art = None
		try:
			art = info.getArtInfo()
		except Exception:
			pass
		self._model(szName, art, info.getButton(), iX, iY, iWidth, iHeight, eWidgetType, iData1, iData2, fXRot, fZRot, fScale, bAnimate)
	def addFlagWidgetGFC(self, szName, iX, iY, iWidth, iHeight, iOwner, eWidgetType=-1, iData1=-1, iData2=-1):
		"""A player's flag (the main interface's by the minimap): its civilization's flag picture,
		an emblem on a banner in the player's colour; a white flag's picture is the whole flag in
		its own colours (its alpha is no mask: "opaque:")."""
		w = self._s.add(szName, 'flag')
		w.x, w.y, w.w, w.h = int(iX), int(iY), int(iWidth), int(iHeight)
		w.wt, w.d1, w.d2 = _int(eWidgetType), _int(iData1), _int(iData2)
		try:
			gc = _cpe.CyGlobalContext()
			player = gc.getPlayer(int(iOwner))
			civ = gc.getCivilizationInfo(player.getCivilizationType())
			art = _cpe.CyArtFileMgr().getCivilizationArtInfo(civ.getArtDefineTag())
			w.tex = ('opaque:' if art is not None and art.isWhiteFlag() else '') + _str(civ.getFlagTexture())
			c = gc.getColorInfo(gc.getPlayerColorInfo(player.getPlayerColor()).getColorTypePrimary()).getColor()
			w.color = (c.r, c.g, c.b, 1.0)
		except Exception:
			pass
	def addModelGraphicGFC(self, *args): _log('CyGInterfaceScreen.addModelGraphicGFC')
	def playMovie(self, szFile, iX=0, iY=0, iWidth=0, iHeight=0, fZ=0):
		"""A movie (Bink): played by the host, over the whole screen without a size; the screen
		hears NOTIFY_MOVIE_DONE at its end. Where the host cannot play it, a wonder's own model in
		its place, turning."""
		f = _str(szFile).replace('/', '\\').lower()
		if not f:
			return
		if oc4host.movie_playable(f):
			sized = _int(iWidth) > 0 and _int(iHeight) > 0
			w = self._s.add('OC4Movie', 'movie', None, 'abs' if sized else 'full')
			if sized:
				w.x, w.y, w.w, w.h = int(iX), int(iY), int(iWidth), int(iHeight)
			w.tex = f
			return
		if _int(iWidth) <= 0:
			return
		gc = _cpe.CyGlobalContext()
		for b in range(gc.getNumBuildingInfos()):
			info = gc.getBuildingInfo(b)
			try:
				if _str(info.getMovie()).replace('/', '\\').lower() == f:
					self.addBuildingGraphicGFC('OC4MovieStandIn', b, iX, iY, iWidth, iHeight, -1, -1, -1, -20, 30, 1.0, True)
					return
			except Exception:
				pass

	# ---------------------------------------------------------------- texts

	def _label(self, kind, szName, szAttachTo, szText, uiFlags, fX, fY, fZ, eFont, eType, iData1, iData2, mode):
		w = self._s.get(szName)
		text, flags, font = _text(szText), _int(uiFlags, _FONT_LEFT), _int(eFont, 1)
		if w is not None and w.kind == kind and (w.parent or None) == (_str(szAttachTo) if szAttachTo and _str(szAttachTo) in self._s.widgets else None):
			# (the same label again, as updates do every frame: only what changed)
			new = (text, flags, int(fX), int(fY), float(fZ), font, _int(eType), _int(iData1), _int(iData2))
			if (w.text, w.flags, w.x, w.y, w.z, w.font, w.wt, w.d1, w.d2) != new:
				w.text, w.flags, w.x, w.y, w.z, w.font, w.wt, w.d1, w.d2 = new
				_changed()
			w.mode = mode if w.parent else 'abs'
			# (set again it is shown again: the interface hides its labels, then sets the ones it wants)
			if not w.shown:
				w.shown = True
				_changed()
			return w
		w = self._s.add(szName, kind, szAttachTo, mode)
		w.text, w.flags, w.font = text, flags, font
		w.x, w.y, w.z = int(fX), int(fY), float(fZ)
		w.wt, w.d1, w.d2 = _int(eType), _int(iData1), _int(iData2)
		return w

	def setLabel(self, szName, szAttachTo, szText, uiFlags, fX, fY, fZ, eFont=1, eType=-1, iData1=-1, iData2=-1):
		self._label('label', szName, szAttachTo, szText, uiFlags, fX, fY, fZ, eFont, eType, iData1, iData2, 'abs')

	def setLabelAt(self, szName, szAttachTo, szText, uiFlags, fX, fY, fZ, eFont=1, eType=-1, iData1=-1, iData2=-1):
		self._label('label', szName, szAttachTo, szText, uiFlags, fX, fY, fZ, eFont, eType, iData1, iData2, 'rel')

	def setText(self, szName, szAttachTo, szText, uiFlags, fX, fY, fZ, eFont=1, eType=-1, iData1=-1, iData2=-1):
		self._label('text', szName, szAttachTo, szText, uiFlags, fX, fY, fZ, eFont, eType, iData1, iData2, 'abs')

	def setTextAt(self, szName, szAttachTo, szText, uiFlags, fX, fY, fZ, eFont=1, eType=-1, iData1=-1, iData2=-1):
		self._label('text', szName, szAttachTo, szText, uiFlags, fX, fY, fZ, eFont, eType, iData1, iData2, 'rel')

	def setHelpLabel(self, szName, szAttachTo, szText, uiFlags, fX, fY, fZ, eFont, szHelpText):
		self._label('label', szName, szAttachTo, szText, uiFlags, fX, fY, fZ, eFont, -1, -1, -1, 'abs').help = _text(szHelpText)

	def modifyLabel(self, szName, szText, uiFlags):
		w = self._s.get(szName)
		if w is not None:
			_set(w, 'text', _text(szText))
			_set(w, 'flags', _int(uiFlags, w.flags))

	def modifyString(self, szName, szText, uiFlags):
		self.modifyLabel(szName, szText, uiFlags)

	def attachLabel(self, szAttachTo, szName, szText):
		w = self._s.add(szName, 'label', szAttachTo, 'flow')
		w.text = _text(szText)

	def attachTextGFC(self, szAttachTo, szName, text, eFont=1, eType=-1, iData1=-1, iData2=-1):
		w = self._s.add(szName, 'text', szAttachTo, 'flow')
		w.text, w.font, w.wt, w.d1, w.d2 = _text(text), _int(eFont, 1), _int(eType), _int(iData1), _int(iData2)

	def addMultilineText(self, szName, szText, iX, iY, iWidth, iHeight, eType=-1, iData1=-1, iData2=-1, iJustify=_FONT_LEFT):
		w = self._s.add(szName, 'multiline')
		w.text, w.x, w.y, w.w, w.h = _text(szText), int(iX), int(iY), int(iWidth), int(iHeight)
		w.wt, w.d1, w.d2, w.flags = _int(eType), _int(iData1), _int(iData2), _int(iJustify, _FONT_LEFT)

	def attachMultilineText(self, szAttachTo, szName, szText, eType=-1, iData1=-1, iData2=-1, iJustify=_FONT_LEFT):
		w = self._s.add(szName, 'multiline', szAttachTo, 'flow')
		w.text, w.wt, w.d1, w.d2, w.flags = _text(szText), _int(eType), _int(iData1), _int(iData2), _int(iJustify, _FONT_LEFT)

	# The engine's own: the end of turn message ("EndTurnText") and the end turn button
	# ("EndTurnButton", its state "Red" or "Green").
	def _endTurn(self, szName):
		kind = 'endbutton' if _str(szName) == 'EndTurnButton' else 'endturn'
		w = self._s.get(szName) or self._s.add(szName, kind)
		if w.kind != kind:
			w.kind = kind
			_changed()
		return w

	def setEndTurnState(self, szName, szText):
		_set(self._endTurn(szName), 'text', _text(szText))

	def showEndTurn(self, szName):
		_set(self._endTurn(szName), 'shown', True)

	def hideEndTurn(self, szName):
		w = self._s.get(szName)
		if w is not None:
			_set(w, 'shown', False)

	def setHelpTextArea(self, fWidth, eFont, fX, fY, fZ, bFloating, szArtFile, bExpandRight, bExpandDown, uiFlags, iMinWidth):
		self._s.helpArea = (int(fX), int(fY), int(fWidth), int(iMinWidth), bool(bExpandRight), bool(bExpandDown))

	def setHelpTextString(self, szString):
		self._s.helpText = _text(szString)
		_changed()

	# ---------------------------------------------------------------- bars

	def addStackedBarGFC(self, szName, iX, iY, iWidth, iHeight, iNumBars, eWidgetType=-1, iData1=-1, iData2=-1):
		w = self._s.add(szName, 'bar')
		w.x, w.y, w.w, w.h = int(iX), int(iY), int(iWidth), int(iHeight)
		w.bars = [[0.0, (0.5, 0.5, 0.5, 1.0)] for _ in range(max(1, int(iNumBars)))]
		w.wt, w.d1, w.d2 = _int(eWidgetType), _int(iData1), _int(iData2)

	def addStackedBarGFCAt(self, szName, szAttachTo, iX, iY, iWidth, iHeight, iNumBars, eWidgetType=-1, iData1=-1, iData2=-1):
		w = self._s.add(szName, 'bar', szAttachTo, 'rel')
		w.x, w.y, w.w, w.h = int(iX), int(iY), int(iWidth), int(iHeight)
		w.bars = [[0.0, (0.5, 0.5, 0.5, 1.0)] for _ in range(max(1, int(iNumBars)))]
		w.wt, w.d1, w.d2 = _int(eWidgetType), _int(iData1), _int(iData2)

	def _bar(self, szName, iBar):
		w = self._s.get(szName)
		if w is None or w.bars is None:
			return None
		while len(w.bars) <= int(iBar):
			w.bars.append([0.0, (0.5, 0.5, 0.5, 1.0)])
		return w.bars[int(iBar)]

	def setBarPercentage(self, szName, iBar, fPercent):
		b = self._bar(szName, iBar)
		if b is not None and b[0] != float(fPercent):
			b[0] = float(fPercent)
			_changed()

	def setStackedBarColorsRGB(self, szName, iBar, iRed, iGreen, iBlue, iAlpha=255):
		b = self._bar(szName, iBar)
		if b is not None:
			c = (int(iRed) / 255.0, int(iGreen) / 255.0, int(iBlue) / 255.0, int(iAlpha) / 255.0)
			if b[1] != c:
				b[1] = c
				_changed()

	def setStackedBarColorsAlpha(self, szName, iBar, eColor, fAlpha):
		b = self._bar(szName, iBar)
		if b is not None:
			c = _color(eColor, float(fAlpha))
			if b[1] != c:
				b[1] = c
				_changed()

	def setStackedBarColors(self, szName, iBar, eColor):
		self.setStackedBarColorsAlpha(szName, iBar, eColor, 1.0)

	# ---------------------------------------------------------------- multi lists (button grids)

	def addMultiListControlGFC(self, szName, helpText, iX, iY, iWidth, iHeight, numLists, defaultWidth, defaultHeight, eStyle=0):
		w = self._s.add(szName, 'multilist')
		w.help, w.x, w.y, w.w, w.h = _text(helpText), int(iX), int(iY), int(iWidth), int(iHeight)
		w.lists = [[] for _ in range(max(1, int(numLists)))]
		w.defw, w.defh, w.pstyle = int(defaultWidth), int(defaultHeight), _style(_cpe.TableStyles, eStyle)

	def addMultiListControlGFCAt(self, szName, szAttachTo, iX, iY, iWidth, iHeight, numLists, defaultWidth, defaultHeight, eStyle=0):
		w = self._s.add(szName, 'multilist', szAttachTo, 'rel')
		w.x, w.y, w.w, w.h = int(iX), int(iY), int(iWidth), int(iHeight)
		w.lists = [[] for _ in range(max(1, int(numLists)))]
		w.defw, w.defh, w.pstyle = int(defaultWidth), int(defaultHeight), _style(_cpe.TableStyles, eStyle)

	def attachMultiListControlGFC(self, szAttachTo, szName, helpText, numLists, defaultWidth, defaultHeight, eStyle=0):
		w = self._s.add(szName, 'multilist', szAttachTo, 'flow')
		w.help = _text(helpText)
		w.lists = [[] for _ in range(max(1, int(numLists)))]
		w.defw, w.defh, w.pstyle = int(defaultWidth), int(defaultHeight), _style(_cpe.TableStyles, eStyle)

	def appendMultiListButton(self, szAttachTo, szTexture, listId, eWidgetType=-1, iData1=-1, iData2=-1, bOption=False):
		w = self._s.get(szAttachTo)
		if w is None or w.lists is None:
			return
		while len(w.lists) <= int(listId):
			w.lists.append([])
		w.lists[int(listId)].append([_str(szTexture), _int(eWidgetType), _int(iData1), _int(iData2), bool(bOption), None])
		_changed()

	def clearMultiList(self, szName):
		w = self._s.get(szName)
		if w is not None and w.lists is not None and any(w.lists):
			w.lists = [[] for _ in w.lists]
			_changed()

	def disableMultiListButton(self, szName, iListId, iIndexId, szTexture):
		w = self._s.get(szName)
		try:
			w.lists[int(iListId)][int(iIndexId)][5] = _str(szTexture) or '-'
			_changed()
		except (AttributeError, IndexError, TypeError):
			pass

	def selectMultiList(self, szName, iListID): pass
	def enableMultiListPulse(self, szName, bEnable, listId, iIndexId): pass

	# ---------------------------------------------------------------- tables

	def addTableControlGFC(self, szName, numColumns, iX, iY, iWidth, iHeight, bIncludeHeaders, bDrawGrid, iconWidth, iconHeight, style=0):
		w = self._s.add(szName, 'table')
		w.x, w.y, w.w, w.h = int(iX), int(iY), int(iWidth), int(iHeight)
		w.columns = [[u'', 0, False] for _ in range(max(1, int(numColumns)))]
		w.rows = []
		w.headers, w.iconw, w.iconh, w.pstyle = bool(bIncludeHeaders), int(iconWidth), int(iconHeight), _style(_cpe.TableStyles, style)

	def addTableControlGFCWithHelp(self, szName, numColumns, iX, iY, iWidth, iHeight, bIncludeHeaders, bDrawGrid, iconWidth, iconHeight, style,
			szHelpText):
		self.addTableControlGFC(szName, numColumns, iX, iY, iWidth, iHeight, bIncludeHeaders, bDrawGrid, iconWidth, iconHeight, style)
		self._s.get(szName).help = _text(szHelpText)

	def attachTableControlGFC(self, szAttachTo, szName, numColumns, bIncludeHeaders, bDrawGrid, iconWidth, iconHeight, style=0):
		self.addTableControlGFC(szName, numColumns, 0, 0, 0, 0, bIncludeHeaders, bDrawGrid, iconWidth, iconHeight, style)
		w = self._s.widgets.pop(_str(szName))
		self._s.order.remove(w.name)
		v = self._s.add(szName, 'table', szAttachTo, 'flow')
		v.columns, v.rows, v.headers, v.iconw, v.iconh, v.pstyle = w.columns, w.rows, w.headers, w.iconw, w.iconh, w.pstyle

	def setTableColumnHeader(self, szName, iColumn, header, iWidth):
		w = self._s.get(szName)
		if w is not None and w.columns is not None:
			while len(w.columns) <= int(iColumn):
				w.columns.append([u'', 0, False])
			w.columns[int(iColumn)][0:2] = [_text(header), int(iWidth)]
			_changed()

	def setTableColumnRightJustify(self, szName, iCol):
		w = self._s.get(szName)
		if w is not None and w.columns is not None and int(iCol) < len(w.columns):
			w.columns[int(iCol)][2] = True

	def appendTableRow(self, szName):
		w = self._s.get(szName)
		if w is None or w.rows is None:
			return 0
		w.rows.append({})
		_changed()
		return len(w.rows) - 1

	def setTableNumRows(self, szName, numRows):
		w = self._s.get(szName)
		if w is not None and w.rows is not None:
			n = int(numRows)
			w.rows = w.rows[:n] + [{} for _ in range(max(0, n - len(w.rows)))]
			_changed()

	def getTableNumRows(self, szName):
		w = self._s.get(szName)
		return len(w.rows) if w is not None and w.rows is not None else 0

	def getTableNumColumns(self, szName):
		w = self._s.get(szName)
		return len(w.columns) if w is not None and w.columns is not None else 0

	def setTableText(self, szName, iColumn, iRow, text, szIcon='', eWidgetType=-1, iData1=-1, iData2=-1, iJustify=_FONT_LEFT):
		w = self._s.get(szName)
		if w is None or w.rows is None:
			return
		while len(w.rows) <= int(iRow):
			w.rows.append({})
		w.rows[int(iRow)][int(iColumn)] = (_text(text), _str(szIcon), _int(eWidgetType), _int(iData1), _int(iData2), _int(iJustify, _FONT_LEFT))
		_changed()

	def setTableInt(self, szName, iColumn, iRow, text, szIcon='', eWidgetType=-1, iData1=-1, iData2=-1, iJustify=_FONT_LEFT):
		self.setTableText(szName, iColumn, iRow, text, szIcon, eWidgetType, iData1, iData2, iJustify)

	def setTableDate(self, szName, iColumn, iRow, text, szIcon='', eWidgetType=-1, iData1=-1, iData2=-1, iJustify=_FONT_LEFT):
		self.setTableText(szName, iColumn, iRow, text, szIcon, eWidgetType, iData1, iData2, iJustify)

	def getTableText(self, szName, iColumn, iRow):
		w = self._s.get(szName)
		try:
			return w.rows[int(iRow)][int(iColumn)][0]
		except (AttributeError, IndexError, KeyError, TypeError):
			return u''

	def attachControlToTableCell(self, szControl, szTable, iRow, iColumn):
		t, c = self._s.get(szTable), self._s.get(szControl)
		if t is not None and c is not None and t.rows is not None:
			if t.cells is None:
				t.cells = {}
			t.cells[(int(iRow), int(iColumn))] = c.name
			c.mode = 'cell'
			_changed()

	def setTableRowHeight(self, szName, iRow, iHeight): pass
	def enableSort(self, szName):
		w = self._s.get(szName)
		if w is not None and w.rows is not None and w.sort is None:
			w.sort = (-1, False)
			_changed()
	def enableSelect(self, szName, bSelect): pass
	def enableGridlines(self, *args): pass

	def selectRow(self, szName, iRow, bSelected):
		w = self._s.get(szName)
		if w is None:
			return
		if w.rows is not None:
			picked = set(w.picked)
			if bSelected:
				picked.add(int(iRow))
			else:
				picked.discard(int(iRow))
			_set(w, 'picked', frozenset(picked))
		else:
			_set(w, 'selected', int(iRow) if bSelected else -1)

	def isRowSelected(self, szName, iRow):
		w = self._s.get(szName)
		if w is None:
			return False
		return int(iRow) in w.picked if w.rows is not None else w.selected == int(iRow)

	# ---------------------------------------------------------------- list boxes, pull downs, edit boxes

	def addListBoxGFC(self, szName, helpText, iX, iY, iWidth, iHeight, eStyle=0):
		w = self._s.add(szName, 'listbox')
		w.help, w.x, w.y, w.w, w.h, w.items = _text(helpText), int(iX), int(iY), int(iWidth), int(iHeight), []

	def attachListBoxGFC(self, szAttachTo, szName, helpText, eStyle=0):
		w = self._s.add(szName, 'listbox', szAttachTo, 'flow')
		w.help, w.items = _text(helpText), []

	def appendListBoxString(self, szAttachTo, item, eType=-1, iData1=-1, iData2=-1, iJustify=_FONT_LEFT):
		w = self._s.get(szAttachTo)
		if w is not None and w.items is not None:
			w.items.append((_text(item), _int(eType), _int(iData1), _int(iData2), _int(iJustify, _FONT_LEFT)))
			_changed()

	appendListBoxStringNoUpdate = appendListBoxString

	def prependListBoxString(self, szAttachTo, item, eType=-1, iData1=-1, iData2=-1, iJustify=_FONT_LEFT):
		w = self._s.get(szAttachTo)
		if w is not None and w.items is not None:
			w.items.insert(0, (_text(item), _int(eType), _int(iData1), _int(iData2), _int(iJustify, _FONT_LEFT)))
			_changed()

	def setListBoxStringGFC(self, szName, iIndex, item, eType=-1, iData1=-1, iData2=-1, iJustify=_FONT_LEFT):
		w = self._s.get(szName)
		if w is not None and w.items is not None and 0 <= int(iIndex) < len(w.items):
			w.items[int(iIndex)] = (_text(item), _int(eType), _int(iData1), _int(iData2), _int(iJustify, _FONT_LEFT))
			_changed()

	def setSelectedListBoxStringGFC(self, szName, iIndex):
		w = self._s.get(szName)
		if w is not None:
			_set(w, 'selected', int(iIndex))

	def clearListBoxGFC(self, szName):
		w = self._s.get(szName)
		if w is not None and w.items:
			w.items = []
			_changed()

	def updateListBox(self, szName): pass

	def addDropDownBoxGFC(self, szName, iX, iY, iWidth, eWidgetType=-1, iData1=-1, iData2=-1, eFontType=1):
		w = self._s.add(szName, 'dropdown')
		w.x, w.y, w.w, w.h, w.items = int(iX), int(iY), int(iWidth), 24, []
		w.wt, w.d1, w.d2, w.font = _int(eWidgetType), _int(iData1), _int(iData2), _int(eFontType, 1)

	def attachDropDownBoxGFC(self, szAttachTo, szName, bEnabled=True):
		w = self._s.add(szName, 'dropdown', szAttachTo, 'flow')
		w.w, w.h, w.items = 150, 24, []

	def addPullDownString(self, szName, szString, iType, iData, bSelected):
		w = self._s.get(szName)
		if w is not None and w.items is not None:
			w.items.append((_text(szString), _int(iType), _int(iData), -1, 0))
			if bSelected or w.selected < 0:
				w.selected = len(w.items) - 1
			_changed()

	def getSelectedPullDownID(self, szName):
		w = self._s.get(szName)
		return w.selected if w is not None else -1

	def getPullDownData(self, szName, iIndex):
		w = self._s.get(szName)
		try:
			return w.items[int(iIndex)][2]
		except (AttributeError, IndexError, TypeError):
			return -1

	def getPullDownType(self, szName, iIndex):
		w = self._s.get(szName)
		try:
			return w.items[int(iIndex)][1]
		except (AttributeError, IndexError, TypeError):
			return -1

	def addEditBoxGFC(self, szName, iX, iY, iWidth, iHeight, eWidgetType=-1, iData1=-1, iData2=-1, eFont=1):
		w = self._s.add(szName, 'edit')
		w.x, w.y, w.w, w.h = int(iX), int(iY), int(iWidth), int(iHeight)

	def setEditBoxString(self, szName, szString):
		w = self._s.get(szName)
		if w is not None:
			_set(w, 'text', _text(szString))

	def getEditBoxString(self, szName):
		w = self._s.get(szName)
		return w.text if w is not None else u''

	def setEditBoxMaxCharCount(self, *args): pass
	def setEditBoxTextColor(self, *args): pass

	def addSlider(self, szName, iX, iY, iWidth, iHeight, iDefault, iMin, iMax, eWidgetType=-1, iData1=-1, iData2=-1, bIsVertical=False):
		w = self._s.add(szName, 'slider')
		w.x, w.y, w.w, w.h, w.size = int(iX), int(iY), int(iWidth), int(iHeight), int(iDefault)

	# ---------------------------------------------------------------- lines

	def addLineGFC(self, szDrawCtrlName, szName, iStartX, iStartY, iEndX, iEndY, eColor):
		w = self._s.add(szName, 'line', szDrawCtrlName, 'rel')
		w.points = (int(iStartX), int(iStartY), int(iEndX), int(iEndY))
		w.color = _color(eColor, 1.0)

	def removeLineGFC(self, szDrawCtrlName, szName):
		self._s.remove(_str(szName))

	# ---------------------------------------------------------------- the minimap

	def initMinimap(self, iLeft, iRight, iTop, iBottom, fZ):
		r = (int(iLeft), int(iTop), int(iRight), int(iBottom))
		if self._s.minimap != r:
			self._s.minimap = r
			_changed()

	def _mm(self):
		if self._s.mm is None:
			self._s.mm = _Minimap()
		return self._s.mm

	def updateMinimapSection(self, *args): pass
	def updateMinimapVisibility(self): pass

	def updateMinimap(self, fDelta=0):
		# (flashes that have run their time go)
		mm = self._s.mm
		if mm is not None:
			now = time.clock()
			gone = [k for k, f in mm.flash.items() if f[1] is not None and f[1] <= now]
			for k in gone:
				del mm.flash[k]
			if gone:
				mm.touch()

	def updateMinimapColorFromMap(self, eMode, fAlpha):
		mm = self._mm()
		mm.territory = float(fAlpha)
		mm.touch()

	def setMinimapMode(self, eMode):
		self._mm().mode = _int(eMode)

	def setMinimapColor(self, eMode, iX, iY, iColor, fAlpha):
		mm = self._mm()
		key = (int(iX), int(iY))
		packed = _packColor(_int(iColor), fAlpha)
		if packed is None:
			if mm.colors.pop(key, None) is None:
				return
		else:
			mm.colors[key] = packed
		mm.touch()

	def setMinimapMap(self, replayInfo, iLeft, iRight, iTop, iBottom, fZ):
		"""The replay's: its map's size; its terrain is the game's when it is this game's map."""
		self.initMinimap(iLeft, iRight, iTop, iBottom, fZ)
		mm = self._mm()
		try:
			mm.size = (int(replayInfo.getMapWidth()), int(replayInfo.getMapHeight()))
		except Exception:
			mm.size = None
		mm.touch()

	def setMinimapNoRender(self, *args): pass
	def setMinimapSectionOverride(self, *args): pass
	def bringMinimapToFront(self): pass

	def minimapClearAllFlashingTiles(self):
		mm = self._s.mm
		if mm is not None and mm.flash:
			mm.flash.clear()
			mm.touch()

	def minimapFlashPlot(self, iX, iY, iColor, fSeconds):
		packed = _packColor(_int(iColor), 1.0)
		if packed is not None:
			mm = self._mm()
			mm.flash[(int(iX), int(iY))] = (packed, time.clock() + float(fSeconds) if fSeconds > 0 else None)
			mm.touch()
	def markMinimapTexturePlotDirty(self, *args): pass
	def renderMinimapWorldTexture(self): pass

	def updateAppropriateCitySelection(self, szName='', iNumRows=0, iColumn=1):
		"""The interface's selected cities become the cities of the table's selected rows (a row's
		city: its WIDGET_ZOOM_CITY cell, else the city named in that column)."""
		w = self._s.get(szName)
		if w is None or w.rows is None or not w.picked:
			return
		game = _cpe.CyGame()
		player = _cpe.CyGlobalContext().getPlayer(game.getActivePlayer())
		zoom = int(_cpe.WidgetTypes.WIDGET_ZOOM_CITY)
		cities = []
		for row in sorted(w.picked):
			if row >= min(len(w.rows), int(iNumRows)):
				continue
			city = None
			for cell in w.rows[row].values():
				if cell[2] == zoom and cell[3] >= 0:
					city = _cpe.CyGlobalContext().getPlayer(cell[3]).getCity(cell[4])
					break
			if city is None or city.isNone():
				name = w.rows[row].get(int(iColumn), (u'',))[0]
				(loop, it) = player.firstCity(False)
				while loop is not None and not loop.isNone():
					if loop.getName() and loop.getName() in name:
						city = loop
						break
					(loop, it) = player.nextCity(it, False)
			if city is not None and not city.isNone():
				cities.append((city.getOwner(), city.getID()))
		if not cities or tuple(cities) == _citySelection[0] and all(oc4host.ui_city_selected(o, i) for o, i in cities):
			return
		_citySelection[0] = tuple(cities)
		oc4host.ui_clear_cities()
		for owner, cid in cities:
			oc4host.ui_add_city(owner, cid, False)


def _color(eColor, fAlpha):
	try:
		c = _cpe.CyGlobalContext().getColorInfo(int(eColor)).getColor()
		return (c.r, c.g, c.b, c.a * fAlpha)
	except Exception:
		return (0.5, 0.5, 0.5, fAlpha)


# -------------------------------------------------------------------- the host's side

_TEXT_HEIGHT = {0: 22, 1: 18, 2: 16, 3: 18, 4: 18}  # by FontTypes (TITLE, GAME, SMALL, MENU, MENU_HIGHLIGHT)


class _Point(object):
	__slots__ = ('x', 'y')

	def __init__(self, x, y):
		self.x, self.y = x, y


_mouse = [0, 0]


def setMouse(x, y):
	_mouse[0], _mouse[1] = int(x), int(y)


def setResolution(w, h):
	if [w, h] != _resolution:
		_resolution[0], _resolution[1] = int(w), int(h)
		_changed()


def version():
	return _version[0]


_LIST_ROW = 20   # a list box's row, and a step of the wheel in what scrolls by pixels
_DROP_ROW = 22   # a row of a pull down's open list
_heights = {}    # (text, width, font) -> how high it is


def _origin(s):
	"""Where a screen's corner is (setDimensions), kept on the display as the engine keeps its
	windows: BUG's finance advisor asks for 50 pixels above the middle of a 768 high screen."""
	if s.dims is None:
		return (0, 0)
	x, y, w, h = s.dims
	return (max(0, min(x, _resolution[0] - w)), max(0, min(y, _resolution[1] - h)))


def _clipTo(clip, rect):
	"""The part of a rectangle inside a clip (None: no clip)."""
	if clip is not None:
		rect = (max(rect[0], clip[0]), max(rect[1], clip[1]), min(rect[2], clip[2]), min(rect[3], clip[3]))
	return (rect[0], rect[1], max(rect[0], rect[2]), max(rect[1], rect[3]))


def _textHeight(text, width, font=1):
	"""How high a text is when it wraps at a width, in the font the host draws it in."""
	key = (text, int(width), int(font))
	h = _heights.get(key)
	if h is None:
		try:
			h = int(oc4host.ui_text_height(text, int(width), int(font)))
		except Exception:
			h = (text.count(u'\n') + 1 + len(text) * 7 / max(1, int(width))) * 16
		if len(_heights) > 2000:
			_heights.clear()
		_heights[key] = h
	return h


_BAR = 16   # a scroll bar's width


def _scrolls(w, x0, y0, x1, y1, fileId, step, out, sideways=False):
	"""What a widget holds (w.extent high; w.hextent wide, when it scrolls sideways too) in its
	box: kept scrolled within it; where it is larger the wheel scrolls it there (Shift: sideways)
	and a scroll bar on its edge shows how far, as the engine's."""
	high, wide = y1 - y0, x1 - x0
	offset = max(0, min(w.offset, w.extent - high))
	hoffset = max(0, min(w.hoffset, w.hextent - wide)) if sideways else 0
	if (offset, hoffset) != (w.offset, w.hoffset):
		w.offset, w.hoffset = offset, hoffset
		_changed()
	down, across = w.extent > high > 0, sideways and w.hextent > wide > 0
	if down or across:
		_areas.append((x0, y0, x1, y1, fileId, w.name, step, down, across))
	# (over what the widget holds: nearer than anything of its screen)
	if down:
		_bars[(fileId, w.name, -2)] = (x1 - _BAR, y0, x1, y1)
		out.append(((fileId, w.name, -2), 'scrollbar', x1 - _BAR, y0, x1, y1, None, '', u'', '', '', 0, 0, False, True, 0, None, -1, -1, -1, False,
			-1000.0, w.seq, float(offset) / w.extent, float(high) / w.extent))
	if across:
		right = x1 - (_BAR if down else 0)
		_bars[(fileId, w.name, -3)] = (x0, y1 - _BAR, right, y1)
		out.append(((fileId, w.name, -3), 'scrollbar', x0, y1 - _BAR, right, y1, None, '', u'', '', '', 0, 0, False, True, 0, None, -1, -1, -1, False,
			-1000.0, w.seq, float(hoffset) / w.hextent, float(wide) / w.hextent))


def _size(s, w):
	"""Width and height of a widget placed by the flow of its parent."""
	if w.mode == 'full':
		return _resolution[0], _resolution[1]
	if w.kind in ('label', 'text'):
		# (a text, setText, is in the theme's size 3 whatever font it names; a label in its own)
		return max(w.w, _textWidth(w.text, 3 if w.kind == 'text' else w.font)), _TEXT_HEIGHT.get(w.font, 18)
	if w.kind == 'multiline':
		# (one placed with a height, addMultilineText, is that high: a longer text scrolls in it)
		if w.mode != 'flow' and w.h > 0:
			return w.w, w.h
		lines = max(1, w.text.count(u'\n') + 1 + len(w.text) / 60)
		return w.w, max(w.h, lines * 16)
	if w.kind == 'separator':
		# (as long as it is given, across its parent's flow: a vertical one in a row of leaders)
		return (8, w.size or 8) if w.vertical else (w.size or 8, 8)
	if w.kind == 'panel' and w.mode == 'flow' and not w.h:
		# (what flows in it: one under the other, or side by side in a horizontal one; what is
		# placed in it, attachXAt, down to its lowest)
		held = [cw for cw in [s.widgets.get(c) for c in w.children] if cw is not None and cw.shown]
		heights = [_size(s, cw)[1] for cw in held if cw.mode == 'flow']
		height = 8 + (sum([h + 2 for h in heights]) if w.vertical else max(heights + [0]))
		for cw in held:
			if cw.mode == 'rel':
				height = max(height, cw.y + _size(s, cw)[1] + 4)
		return w.w, height
	if w.kind == 'button' and not w.w:
		return len(w.text) * 8 + 16, w.h or 24
	return w.w, w.h


def _display_widget(s, w, ox, oy, clip, flow, out, cell=None):
	"""Adds a widget (and what is attached to it) to the display list; one attached to a
	table's cell only there (cell: that cell's corner)."""
	if not w.shown or (w.mode == 'cell') != (cell is not None):
		return
	if cell is not None:
		x, y = cell
	elif w.mode == 'abs':
		# (from the screen's corner)
		sx, sy = _origin(s)
		x, y = w.x + sx, w.y + sy
	elif w.mode == 'rel':
		x, y = ox + w.x, oy + w.y
	elif w.mode == 'full':
		x, y = 0, 0
	else:
		x, y = flow[0], flow[1]
	width, height = _size(s, w)
	if w.mode == 'flow':
		# (one of no width of its own takes the rest of its parent's; a text then wraps there)
		if not width:
			width = max(0, flow[3] - (x - flow[4]))
		if w.kind == 'multiline' and width:
			height = max(w.h, _textHeight(w.text, width, w.font))
		if flow[2]:
			flow[1] += height + 2
		else:
			flow[0] += width + 4
	# (a text attached at a place in its parent, setTextAt, starts there whatever its justification:
	# AND2's civics screen puts its centred headings and strategy lines where their left is meant)
	if w.kind in ('label', 'text', 'endturn') and not (w.kind == 'text' and w.mode == 'rel'):
		if w.flags & _FONT_RIGHT:
			x -= width
		elif w.flags & _FONT_CENTER:
			x -= width / 2
	base = (s.id, w.name)
	kind = w.kind
	if kind == 'multilist':
		bw, bh = max(1, w.defw), max(1, w.defh)
		row = 0
		placed = []
		for listId, buttons in enumerate(w.lists or []):
			col = 0
			for index, b in enumerate(buttons):
				if width and col and (col + 1) * bw > width:
					col, row = 0, row + 1
				placed.append((listId, index, b, col * bw, row * bh))
				col += 1
			if buttons:
				row += 1
		box, top = clip, y
		if height:
			box = _clipTo(clip, (x, y, x + width, y + height))
			w.extent = row * bh
			# (a row a notch of the wheel)
			_scrolls(w, x, y, x + width, y + height, s.id, max(1, bh / 3), out)
			top = y - w.offset
		for listId, index, b, dx, dy in placed:
			bx, by = x + dx, top + dy
			if height and (by + bh <= y or by >= y + height):
				continue
			out.append((base + ((listId << 16) | index,), 'imagebutton', bx, by, bx + bw, by + bh, box, w.style, u'', b[5] if b[5] and b[5] != '-' else b[0],
				'', 1, 0, False, b[5] is None and w.enabled, 0, None, b[1], b[2], b[3], b[4], w.z, w.seq))
		return
	if kind == 'table':
		# (the theme's cell height, GFC_Control_Size_ListItem; taller for a table whose cells have pictures)
		pictures = [1 for row in w.rows for cell in row.values() if cell and cell[1]]
		rowHeight = max(24, w.iconh) if pictures else 24
		# (its rows scroll under its headers, in its box)
		box = _clipTo(clip, (x, y, x + width, y + height)) if height else clip
		top = y
		if w.headers:
			cx = x
			for ci, c in enumerate(w.columns):
				# (a table that sorts: each header a part of its own, -100 - column, that a click sorts by)
				out.append((base + (-100 - ci if w.sort is not None else -1,), 'header', cx, top, cx + c[1], top + rowHeight, box, w.style, c[0], '', '', 2, _FONT_RIGHT if c[2] else _FONT_LEFT,
					False, True, 0, None, -1, -1, -1, False, w.z, w.seq))
				cx += c[1]
			top += rowHeight
		body = _clipTo(box, (x, top, x + width, y + height)) if height else box
		w.extent = len(w.rows) * rowHeight
		_scrolls(w, x, top, x + width, y + height if height else top + w.extent, s.id, rowHeight, out)
		cy = top - w.offset
		order = range(len(w.rows))
		if w.sort is not None and w.sort[0] >= 0:
			column = w.sort[0]
			order.sort(key=lambda ri: _sortKey(w.rows[ri].get(column)), reverse=w.sort[1])
		for ri in order:
			r = w.rows[ri]
			if height and (cy + rowHeight <= top or cy >= y + height):
				cy += rowHeight
				continue
			cx = x
			for ci, c in enumerate(w.columns):
				cell = r.get(ci)
				if cell is not None:
					flags = _FONT_RIGHT if c[2] else cell[5]
					out.append((base + ((ri << 8) | ci,), 'cell', cx, cy, cx + c[1], cy + rowHeight, body, w.style, cell[0], cell[1], '', 2, flags,
						ri in w.picked, True, 0, None, cell[2], cell[3], cell[4], len(cell) > 6 and cell[6], w.z, w.seq))
				# (a control attached to the cell: there, scrolling with its row)
				control = s.widgets.get(w.cells.get((ri, ci))) if w.cells else None
				if control is not None:
					_display_widget(s, control, cx, cy, body, [cx, cy, True, c[1], cx], out, (cx, cy))
				cx += c[1]
			cy += rowHeight
		return
	if kind == 'listbox':
		box = _clipTo(clip, (x, y, x + width, y + height)) if height else clip
		items = w.items or []
		w.extent = len(items) * _LIST_ROW
		_scrolls(w, x, y, x + width, y + height if height else y + w.extent, s.id, _LIST_ROW, out)
		cy = y - w.offset
		for i, item in enumerate(items):
			if not height or (cy + _LIST_ROW > y and cy < y + height):
				out.append((base + (i,), 'cell', x, cy, x + width, cy + _LIST_ROW, box, w.style, item[0], '', '', 2, item[4], w.selected == i, True, 0, None,
					item[1], item[2], item[3], False, w.z, w.seq))
			cy += _LIST_ROW
		return
	if kind == 'multiline' and w.mode != 'flow' and width > 0 and height > 0:
		# (in a box of its own: clipped there, and it scrolls when it is longer)
		w.extent = _textHeight(w.text, width, w.font)
		_scrolls(w, x, y, x + width, y + height, s.id, _LIST_ROW, out)
		out.append((base + (-1,), kind, x, y - w.offset, x + width, y - w.offset + max(height, w.extent), _clipTo(clip, (x, y, x + width, y + height)),
			w.style if w.style else w.pstyle, w.text, w.tex, w.tex2, w.font, w.flags, w.state, w.enabled, w.hittest, w.color, w.wt, w.d1, w.d2, w.opt,
			w.z, w.seq))
		return
	if kind == 'dropdown':
		_dropRects[(s.id, w.name)] = (x, y, x + width, y + height)
		text = w.items[w.selected][0] if w.items and 0 <= w.selected < len(w.items) else u''
		out.append((base + (-1,), 'dropdown', x, y, x + width, y + height, clip, w.style, text, '', '', w.font, _FONT_LEFT, False, w.enabled,
			w.hittest, None, w.wt, w.d1, w.d2, w.opt, w.z, w.seq))
		return
	if kind == 'line':
		x0, y0, x1, y1 = w.points
		out.append((base + (-1,), 'line', ox + x0, oy + y0, ox + x1, oy + y1, clip, '', u'', '', '', 0, 0, False, True, 4, w.color, -1, -1, -1, False,
			w.z, w.seq))
		return
	if kind == 'bar':
		start = 0.0
		for i, (pct, color) in enumerate(w.bars):
			# stacked: each bar starts where the one before ended (BTS fills food/production this way)
			out.append((base + (i,), 'bar', x, y, x + width, y + height, clip, w.style, u'', '', '', 0, 0, False, True, w.hittest, color,
				w.wt, w.d1, w.d2, w.opt, w.z, w.seq + i * 0.001, start, pct, i == len(w.bars) - 1))
			start += pct
		return
	color = w.color
	flags = w.flags
	if kind == 'text' and w.mode == 'rel':
		flags = (flags & ~(_FONT_CENTER | _FONT_RIGHT)) | _FONT_LEFT
	out.append((base + (-1,), kind, x, y, x + width, y + height, clip, w.style if w.style else w.pstyle, w.text, w.tex, w.tex2, w.font, flags,
		w.state, w.enabled, w.hittest, color, w.wt, w.d1, w.d2, w.opt, w.z, w.seq))
	if w.children:
		inner, left, top = clip, x, y
		scrolls = kind == 'panel' and w.scroll and width > 0 and height > 0
		if scrolls:
			inner, left, top = _clipTo(clip, (x, y, x + width, y + height)), x - w.hoffset, y - w.offset
		first = len(out)
		childFlow = [left + 4, top + 4 + (18 if w.text else 0), w.vertical, max(0, width - 8), left + 4]
		for c in w.children:
			cw = s.widgets.get(c)
			if cw is not None:
				_display_widget(s, cw, left, top, inner, childFlow, out)
		if scrolls:
			held = [i for i in out[first:] if i[1] != 'scrollbar']
			# (the margin below and right of what it holds only once that overflows: a strip of
			# buttons as big as its panel, the WorldBuilder's load/save one, has no scroll bars)
			extent = max([i[5] for i in held] + [childFlow[1]]) - top
			hextent = max([i[4] for i in held] + [left]) - left
			w.extent = extent + 4 if extent > height else extent
			w.hextent = hextent + 4 if hextent > width else hextent
			_scrolls(w, x, y, x + width, y + height, s.id, _LIST_ROW, out, True)


def dump(path):
	"""Writes every widget of the shown screens (name, kind, place, style, art) to a file."""
	f = open(path, 'w')
	for item in display():
		f.write('%s\t%s\t%s\t(%d,%d)-(%d,%d)\tstyle=%s\ttex=%s\ttext=%s\twidget=%d,%d,%d\n' % (item[0][0], item[0][1], item[1], item[2], item[3], item[4],
			item[5], item[7], item[9], _text(item[8]).encode('utf-8')[:60], item[17], item[18], item[19]))
	f.close()


def display():
	"""The display list: what the shown screens draw, back to front.

	Each item: ((file id, widget name, part), kind, x0, y0, x1, y1, clip, style, text, texture,
	texture2, font, flags, state, enabled, hit test, colour, widget type, data1, data2, option,
	z, order[, bar start, bar percent]).
	"""
	for f in beforeDisplay:
		try:
			f()
		except Exception:
			import traceback
			traceback.print_exc()
	out = []
	del _areas[:]
	_dropRects.clear()
	_bars.clear()
	for s in sorted(_screens.values(), key=lambda s: (not s.main, s.id)):
		if not (s.main or s.shown):
			continue
		items = []
		# (a screen's widgets are placed from its own corner, setDimensions: the 1024x768 screens
		# centred on a larger one)
		ox, oy = _origin(s)
		for name in s.order:
			w = s.widgets.get(name)
			if w is not None:
				_display_widget(s, w, ox, oy, None, [ox, oy, True, 0, 0], items)
		# farther first (GFC's z: smaller is nearer), then in the order they came
		items.sort(key=lambda i: (-i[21], i[22]))
		out.extend(items)
		# (the minimap over the main interface, under the screens shown over it)
		if s.minimap is not None and s.main:
			l, t, r, b = s.minimap
			out.append(((s.id, '__minimap', -1), 'minimap', l, t, r, b, None, '', u'', '', '', 0, 0, False, True, 0, None, -1, -1, -1, False, 0, 0))
		# (a screen's own, over what it shows: the military advisor's, the replay's; placed from
		# the screen's corner as its widgets)
		elif s.minimap is not None and s.mm is not None:
			l, t, r, b = s.minimap
			out.append(((s.id, '__minimap', -1), 'minimap', l + ox, t + oy, r + ox, b + oy, None, '__screenmap', u'', '', '', 0, 0, False, True, 0, None,
				-1, -1, -1, False, 0, 0))
	_dropList(out)
	return out


def _dropList(out):
	"""The open pull down's list, over everything: under the pull down (over it where the
	screen ends first), as wide as its widest item, scrolling when it is long."""
	key = _drop[0]
	s = _screens.get(key[0]) if key else None
	w = s.get(key[1]) if s is not None else None
	r = _dropRects.get(key) if key else None
	if w is None or r is None or not w.items or not (s.main or s.shown):
		_drop[0] = None
		return
	n = len(w.items)
	width = r[2] - r[0]
	for item in w.items:
		width = max(width, _textWidth(item[0]) + 24)
	below, above = _resolution[1] - r[3] - 12, r[1] - 12
	rows = min(n, max(1, max(below, above) // _DROP_ROW))
	height = rows * _DROP_ROW + 8
	y0 = r[3] if below >= height or below >= above else r[1] - height
	x0 = max(0, min(r[0], _resolution[0] - width))
	top = y0 + 4
	box = (x0, top, x0 + width, top + rows * _DROP_ROW)
	_drop[1] = max(0, min(_drop[1], (n - rows) * _DROP_ROW))
	if n > rows:
		_areas.append(box + (-1, '', _DROP_ROW, True, False))
	out.append(((s.id, '__droplist', -1), 'panel', x0, y0, x0 + width, y0 + height, None, '__droplist', u'', '', '', 0, 0, False, True, 0, None,
		-1, -1, -1, False, 0, 0))
	cy = top - _drop[1]
	for i, item in enumerate(w.items):
		if cy + _DROP_ROW > top and cy < box[3]:
			out.append(((s.id, w.name, i), 'option', x0, cy, x0 + width, cy + _DROP_ROW, box, '__option', item[0], '', '', 2, _FONT_LEFT, i == w.selected,
				True, 0, None, -1, -1, -1, False, 0, 0))
		cy += _DROP_ROW


_widths = {}    # (text, font) -> how wide it is


def _textWidth(text, font=-1):
	"""How wide a text is in a widget's font (FontTypes; -1 the theme's size 2), as the host draws it."""
	key = (text, font)
	width = _widths.get(key)
	if width is None:
		width = int(oc4host.ui_text_width(text, font))
		if not oc4host.ui_text_measured():
			return width
		if len(_widths) > 4000:
			_widths.clear()
		_widths[key] = width
	return width


def wheel(x, y, delta, shift=False):
	"""The mouse wheel over something that scrolls scrolls it, three rows a notch (sideways with
	Shift, or when it only scrolls so); whether it did."""
	for (x0, y0, x1, y1, fileId, name, step, down, across) in reversed(_areas):
		if x0 <= x < x1 and y0 <= y < y1:
			by = -int(delta * 3 * step / 120)
			if fileId < 0:
				# (the open pull down's list: kept within it when it is shown again)
				_drop[1] = max(0, _drop[1] + by)
				_changed()
				return True
			s = _screens.get(fileId)
			w = s.get(name) if s is not None else None
			if w is not None:
				if across and (shift or not down):
					_set(w, 'hoffset', max(0, min(w.hextent - (x1 - x0), w.hoffset + by * 2)))
				else:
					_set(w, 'offset', max(0, min(w.extent - (y1 - y0), w.offset + by)))
			return True
	return False


def scrollTo(fileId, name, part, x, y):
	"""A click on a scroll bar: what it scrolls is moved so that the bar's grip is under the mouse."""
	r = _bars.get((int(fileId), _str(name), int(part)))
	s = _screens.get(int(fileId))
	w = s.get(name) if s is not None else None
	if r is None or w is None:
		return
	if int(part) == -2:
		high = r[3] - r[1]
		at = float(y - r[1]) / max(1, high)
		_set(w, 'offset', int(max(0, min(w.extent - high, at * w.extent - high / 2))))
	else:
		wide = r[2] - r[0]
		at = float(x - r[0]) / max(1, wide)
		_set(w, 'hoffset', int(max(0, min(w.hextent - wide, at * w.hextent - wide / 2))))


def openDropdown(fileId, name):
	"""A click on a pull down opens its list, or closes it when it is open."""
	key = (int(fileId), _str(name))
	_drop[0], _drop[1] = (None if _drop[0] == key else key), 0
	_changed()


def closeDropdown():
	"""Closes the open pull down's list; whether one was open."""
	if _drop[0] is None:
		return False
	_drop[0] = None
	_changed()
	return True


def chooseOption(fileId, name, index):
	"""An item of the open pull down's list chosen: the pull down shows it; whether there is one."""
	_drop[0] = None
	_changed()
	s = _screens.get(int(fileId))
	w = s.get(name) if s is not None else None
	if w is None or not w.items or not 0 <= int(index) < len(w.items):
		return False
	w.selected = int(index)
	return True


_number = __import__('re').compile(r'^\s*([-+]?[0-9][0-9,]*(\.[0-9]+)?)')
_markup = __import__('re').compile(r'<[^>]*>')


def _sortKey(cell):
	"""How a table's cell sorts: numbers by their value, before texts in their letters' order;
	an empty cell last."""
	if not cell:
		return (2, 0, u'')
	text = _markup.sub(u'', _text(cell[0])).strip()
	m = _number.match(text)
	if m:
		try:
			return (0, float(m.group(1).replace(u',', u'')), text.lower())
		except ValueError:
			pass
	return (1, 0, text.lower())


def sortBy(fileId, name, part):
	"""A click on a sorting table's header: its rows by that column, the other way round when
	they already were."""
	s = _screens.get(int(fileId))
	w = s.get(name) if s is not None else None
	if w is None or w.sort is None or int(part) > -100:
		return
	column = -100 - int(part)
	w.sort = (column, not w.sort[1]) if w.sort[0] == column else (column, False)
	_changed()


def rowClicked(fileId, name, part, shift, ctrl):
	"""A click on a row of a table or a list box selects it (a table's: Ctrl adds or takes it,
	Shift a range); the (row, column) the script hears of, or None."""
	s = _screens.get(int(fileId))
	w = s.get(name) if s is not None else None
	if w is None:
		return None
	part = int(part)
	if w.kind == 'table':
		row, column = part >> 8, part & 0xFF
		if ctrl:
			picked = set(w.picked) ^ set([row])
		elif shift and w.selected >= 0:
			picked = set(range(min(row, w.selected), max(row, w.selected) + 1))
		else:
			picked = set([row])
		if not shift:
			w.selected = row
		_set(w, 'picked', frozenset(picked))
		return (row, column)
	if w.kind == 'listbox':
		_set(w, 'selected', part)
		return (part, 0)
	return None


def helpArea():
	"""(x, y, width, min width, expand right, expand down) of the main interface's help, or None."""
	for s in _screens.values():
		if s.main and s.helpArea is not None:
			return s.helpArea
	return None


def widgetText(fileId, name):
	s = _screens.get(fileId)
	w = s.get(name) if s is not None else None
	return w.help if w is not None else u''


def toggle(fileId, name):
	"""A click on a checkbox turns it."""
	s = _screens.get(fileId)
	w = s.get(name) if s is not None else None
	if w is not None and w.kind == 'checkbox':
		w.state = not w.state
		_changed()


def closeOnEscape(fileId):
	s = _screens.get(fileId)
	return s is not None and s.shown and not s.main and s.closeOnEscape


def reset(fileId):
	"""Forgets a screen's widgets (before it is laid out again for another resolution)."""
	s = _screens.get(int(fileId))
	if s is not None:
		s.widgets, s.order, s.hideLists = {}, [], {}
		s.minimap, s.helpArea, s.endTurn = None, None, None
		_changed()


def hide(fileId):
	s = _screens.get(fileId)
	if s is not None and s.shown:
		s.shown = False
		_forget(s)
		_changed()


def _forget(s):
	"""A screen closed (not the main interface): its widgets go with it, so that the next one
	shown under its id (another wonder's movie) starts empty rather than over the last."""
	if not s.main:
		s.widgets, s.order, s.hideLists = {}, [], {}
		s.minimap, s.helpArea, s.endTurn = None, None, None


def modalUp():
	"""The host: whether a screen of its own is up over the map (an advisor, a wonder's movie)."""
	for s in _screens.values():
		if s.shown and not s.main and s.id != _DIPLO:
			return True
	return False


# -------------------------------------------------------------------- CyInterface

class CyAudioGame(object):
	"""The game's sounds by script: what plays is the engine's (nothing in a scripted run)."""

	def __getattr__(self, name):
		if name.startswith('__'):
			raise AttributeError(name)
		return lambda *args, **kwargs: 0

	def Play2DSound(self, szSound, *args):
		oc4host.audio_play(str(szSound))
		return 0

	def Play2DSoundWithId(self, iScriptId, *args):
		oc4host.audio_play_id(_int(iScriptId), 1)
		return 0

	def Play3DSound(self, szSound, *args):
		oc4host.audio_play(str(szSound))
		return 0

	def Play3DSoundWithId(self, iScriptId, *args):
		oc4host.audio_play_id(_int(iScriptId), 2)
		return 0


class CyInterface(object):
	"""The interface's state the scripts read: the host's (selection, dirty bits, city screen)."""

	def getMousePos(self): return _Point(_mouse[0], _mouse[1])

	def __getattr__(self, name):
		if name.startswith('__'):
			raise AttributeError(name)
		_log('CyInterface.' + name)
		return lambda *args, **kwargs: 0

	def isDirty(self, eDirty): return oc4host.ui_is_dirty(int(eDirty))
	def setDirty(self, eDirty, bDirty): oc4host.ui_set_dirty(int(eDirty), bool(bDirty))
	def makeInterfaceDirty(self): oc4host.ui_make_dirty()
	def getShowInterface(self): return _cpe.InterfaceVisibility(oc4host.ui_show_interface())
	def setShowInterface(self, eVisibility): oc4host.ui_set_show_interface(int(eVisibility))
	def isCityScreenUp(self): return oc4host.ui_city_screen_up()
	def isCitySelection(self): return oc4host.ui_city_selection()
	def isOneCitySelected(self): return oc4host.ui_city_selection()
	def isScreenUp(self, iEnumVal):
		s = _screens.get(int(iEnumVal))
		return bool(s and s.shown)

	def getHeadSelectedCity(self):
		owner, cid = oc4host.ui_head_city()
		if owner < 0:
			return None
		return _cpe.CyGlobalContext().getPlayer(owner).getCity(cid)

	def selectCity(self, pCity, bTestProduction=False):
		if pCity is None or pCity.isNone():
			oc4host.ui_clear_cities()
		else:
			oc4host.ui_select_city(pCity.getOwner(), pCity.getID(), bool(bTestProduction))

	def addSelectedCity(self, pCity, bToggle=False):
		if pCity is not None and not pCity.isNone():
			oc4host.ui_add_city(pCity.getOwner(), pCity.getID(), bool(bToggle))

	def clearSelectedCities(self): oc4host.ui_clear_cities()

	def isCitySelected(self, pCity):
		return pCity is not None and not pCity.isNone() and oc4host.ui_city_selected(pCity.getOwner(), pCity.getID())

	def getHeadSelectedUnit(self):
		owner, uid = oc4host.ui_head_unit()
		if owner < 0:
			return None
		return _cpe.CyGlobalContext().getPlayer(owner).getUnit(uid)

	def getSelectionUnit(self, iIndex=0):
		owner, uid = oc4host.ui_selection_unit(int(iIndex))
		if owner < 0:
			return None
		return _cpe.CyGlobalContext().getPlayer(owner).getUnit(uid)

	def getLengthSelectionList(self): return oc4host.ui_selection_length()
	def mirrorsSelectionGroup(self): return oc4host.ui_mirrors_selection_group()

	def getSelectionPlot(self):
		x, y = oc4host.ui_selection_plot()
		if x < 0:
			return None
		return _cpe.CyMap().plot(x, y)

	def selectUnit(self, pUnit, bClear, bToggle, bSound=False):
		oc4host.ui_select_unit(pUnit.getOwner(), pUnit.getID(), bool(bClear), bool(bToggle))

	def selectGroup(self, pUnit, bShift, bCtrl, bAlt):
		oc4host.ui_select_group(pUnit.getOwner(), pUnit.getID(), bool(bShift), bool(bCtrl), bool(bAlt))

	def clearSelectionList(self): oc4host.ui_clear_selection()

	def insertIntoSelectionList(self, pUnit, bClear, bToggle, bGroup, bSound=False):
		oc4host.ui_insert_selection(pUnit.getOwner(), pUnit.getID(), bool(bClear), bool(bToggle), bool(bGroup))

	def getInterfaceMode(self): return _cpe.InterfaceModeTypes(oc4host.ui_interface_mode())
	def setInterfaceMode(self, eMode): oc4host.ui_set_interface_mode(int(eMode))
	def canHandleAction(self, iAction, bTestVisible=False): return oc4host.ui_can_handle_action(int(iAction), bool(bTestVisible))
	def getActionsToShow(self): return tuple(oc4host.ui_actions_to_show())
	def shouldShowAction(self, iAction): return oc4host.ui_can_handle_action(int(iAction), True)
	def canCreateGroup(self): return oc4host.ui_can_create_group()
	def canDeleteGroup(self): return oc4host.ui_can_delete_group()
	def canSelectHeadUnit(self): return oc4host.ui_head_unit()[0] >= 0

	# the head selected city's production queue
	def getNumOrdersQueued(self): return oc4host.ui_orders()
	def getOrderNodeType(self, iNode): return _cpe.OrderTypes(oc4host.ui_order(int(iNode))[0])
	def getOrderNodeData1(self, iNode): return oc4host.ui_order(int(iNode))[1]
	def getOrderNodeData2(self, iNode): return oc4host.ui_order(int(iNode))[2]
	def getOrderNodeSave(self, iNode): return oc4host.ui_order(int(iNode))[3]

	# the units on the selection's plot (the plot list along the bottom)
	def cacheInterfacePlotUnits(self, pPlot):
		self.__class__._plotUnits = []
		if pPlot is None or pPlot.isNone():
			return
		team = _cpe.CyGame().getActiveTeam()
		for i in range(pPlot.getNumUnits()):
			u = pPlot.getUnit(i)
			if not u.isNone() and not u.isInvisible(team, False):
				self.__class__._plotUnits.append(u)

	def getNumCachedInterfacePlotUnits(self): return len(getattr(self.__class__, '_plotUnits', []))

	def getCachedInterfacePlotUnit(self, iIndex):
		units = getattr(self.__class__, '_plotUnits', [])
		return units[iIndex] if 0 <= iIndex < len(units) else None

	def getNumVisibleUnits(self):
		plot = self.getSelectionPlot()
		if plot is None or plot.isNone():
			return 0
		team = _cpe.CyGame().getActiveTeam()
		return len([i for i in range(plot.getNumUnits()) if not plot.getUnit(i).isInvisible(team, False)])

	def getInterfacePlotUnit(self): return None
	def getPlotListColumn(self): return oc4host.ui_plot_list_column()

	# The plot list's rows fill from the bottom: the buttons before the first one used (the
	# host's, which the DLL's clicks on it go by).
	def getPlotListOffset(self): return oc4host.ui_plot_list_offset()

	def getCityTabSelectionRow(self): return getattr(self.__class__, '_cityTab', 0)
	def setCityTabSelectionRow(self, eIndex): self.__class__._cityTab = int(eIndex)

	def getHelpString(self): return oc4host.ui_help_string()
	def determineWidth(self, szBuffer): return _textWidth(_text(szBuffer))

	def getEndTurnState(self): return _cpe.EndTurnButtonStates(oc4host.ui_end_turn_state())
	def shouldDisplayEndTurn(self): return oc4host.ui_end_turn_message()
	def shouldDisplayEndTurnButton(self): return True
	def shouldDisplayWaitingOthers(self): return not oc4host.ui_turn_active()
	def shouldDisplayWaitingYou(self): return False
	def shouldDisplayReturn(self): return False
	def shouldDisplayFlag(self): return oc4host.ui_display_flag()
	def shouldDisplayUnitModel(self): return oc4host.ui_display_unit_model() and oc4host.ui_head_unit()[0] >= 0 and not oc4host.ui_city_screen_up()
	def shouldShowSelectionButtons(self): return True
	def shouldShowResearchButtons(self): return oc4host.ui_should_show_research()
	def shouldShowChangeResearchButton(self): return False
	def shouldShowYieldVisibleButton(self): return False
	def shouldFlash(self, iPlayer): return False
	def isFlashingPlayer(self, iPlayer): return False
	def isFlashing(self): return False
	def checkFlashUpdate(self): return False
	def checkFlashReset(self, ePlayer): pass

	def isScoresVisible(self): return True
	def isScoresMinimized(self): return False
	def toggleScoresMinimized(self): pass
	def toggleScoresVisible(self): pass
	def isOOSVisible(self): return False
	def isNetStatsVisible(self): return False
	def isYieldVisibleMode(self): return False
	def toggleYieldVisibleMode(self): pass
	def toggleBareMapMode(self): pass
	def isFocused(self): return False
	def isFocusedWidget(self): return False
	def isInAdvancedStart(self): return False
	def isInMainMenu(self): return False
	def isUnitFocus(self): return False
	def isLeftMouseDown(self): return bool(oc4host.ui_mouse_buttons()[0])
	def isRightMouseDown(self): return bool(oc4host.ui_mouse_buttons()[1])
	def getMouseOverPlot(self):
		x, y = oc4host.ui_mouse_plot()
		return _cpe.CyMap().plot(x, y)
	def countEntities(self, iI): return 0
	def noTechSplash(self): return True
	def shiftKey(self): return oc4host.ui_key(0)
	def ctrlKey(self): return oc4host.ui_key(1)
	def altKey(self): return oc4host.ui_key(2)
	def lookAtCityOffset(self, iCity): oc4host.ui_look_at_city(int(iCity))
	def lookAtCityBuilding(self, iCity, iBuilding): oc4host.ui_look_at_city(int(iCity))

	def addMessage(self, iPlayer, bForce, iLength, szString, szSound='', eType=0, szIcon='', eFlashColor=-1, iFlashX=-1, iFlashY=-1,
			bShowOffScreenArrows=False, bShowOnScreenArrows=False):
		oc4host.ui_add_message(_int(iPlayer), bool(bForce), _int(iLength), _text(szString), _str(szSound), _int(eType, 0), _str(szIcon),
			_int(eFlashColor), _int(iFlashX), _int(iFlashY), bool(bShowOffScreenArrows), bool(bShowOnScreenArrows))

	def addImmediateMessage(self, szString, szSound=''):
		oc4host.ui_add_message(-1, True, -1, _text(szString), _str(szSound), int(_cpe.InterfaceMessageTypes.MESSAGE_TYPE_DISPLAY_ONLY), '', -1, -1, -1,
			False, False)

	def addCombatMessage(self, iPlayer, szString):
		oc4host.ui_add_combat_message(_int(iPlayer), _text(szString))
	def addQuestMessage(self, *args): pass
	def playGeneralSound(self, szSound, *args): oc4host.audio_play(str(szSound))
	def playAdvisorSound(self, szSound, *args): oc4host.audio_play(str(szSound))
	def playGeneralSoundByID(self, iScriptId, *args): oc4host.audio_play_id(_int(iScriptId), 1)

	def playGeneralSoundAtPlot(self, szSound, pPlot, *args):
		if pPlot is None or pPlot.isNone():
			oc4host.audio_play(str(szSound))
		else:
			oc4host.audio_play_at(str(szSound), pPlot.getX(), pPlot.getY())
	def stopAdvisorSound(self): pass
	def stop2DSound(self): pass
	def DoSoundtrack(self, *args): return True
	def setSoundSelectionReady(self, *args): pass
	def setBusy(self, *args): pass
	def setPausedPopups(self, *args): pass
	def setWorldBuilder(self, bOn): oc4host.ui_set_world_builder(bool(bOn))
	def exitingToMainMenu(self, *args): pass
	def doPing(self, *args): pass
	def startTimer(self): pass
	def endTimer(self, *args): pass


def _globeUp():
	try:
		return bool(oc4host.globe_up())
	except Exception:
		return False


class _GlobeLayer(object):
	"""CyGlobeLayer: one of the game core's layers (CvGlobeLayerData)."""

	def __init__(self, index, data):
		self._i = index
		self._type, self._name, self._help, self._style, self._options, self._globe = data

	def getName(self): return self._name
	def getButtonHelpTag(self): return self._help
	def getButtonStyle(self): return self._style
	def getNumOptions(self): return self._options
	def isGlobeviewRequired(self): return self._globe
	def getType(self): return self._type
	def getID(self): return self._i
	def getOptionName(self, iOption):
		try:
			return oc4host.globe_option_name(self._type, _int(iOption))
		except Exception:
			return u''
	def getCurrentOption(self):
		try:
			layer, option = oc4host.globe_layer()
			return option if layer == self._i else 0
		except Exception:
			return 0


class CyGlobeLayerManager(object):
	"""The globe view's layers, as the game core lists them, and the one shown."""

	def __getattr__(self, name):
		if name.startswith('__'):
			raise AttributeError(name)
		_log('CyGlobeLayerManager.' + name)
		return lambda *args, **kwargs: 0

	def _layers(self):
		try:
			return oc4host.globe_layers()
		except Exception:
			return []
	def getNumLayers(self): return len(self._layers())
	def getLayer(self, i):
		layers = self._layers()
		i = _int(i)
		return _GlobeLayer(i, layers[i]) if 0 <= i < len(layers) else None
	def getCurrentLayerID(self):
		try:
			return oc4host.globe_layer()[0]
		except Exception:
			return -1
	def getCurrentLayer(self):
		return self.getLayer(self.getCurrentLayerID())
	def getCurrentLayerName(self):
		layer = self.getCurrentLayer()
		return layer.getName() if layer is not None else ''
	def getLayerID(self, name):
		for i, l in enumerate(self._layers()):
			if l[1] == _str(name):
				return i
		return -1
	def isGlobeviewUp(self): return _globeUp()


_engineFlags = {'billboards': True, 'flags': True, 'cursor': True}


class CyEngine(object):
	"""The map's engine as the scripts see it: no globe view, the display toggles they set
	(kept, not yet drawn), the plots they colour and the signs they place still to come."""

	def __getattr__(self, name):
		if name.startswith('__'):
			raise AttributeError(name)
		_log('CyEngine.' + name)
		return lambda *args, **kwargs: 0

	def isGlobeviewUp(self): return _globeUp()
	# an effect set off at a point (CyPlot.getPoint): the window shows it
	def triggerEffect(self, iEffect, point, fRotation=0.0):
		try:
			oc4host.engine_effect(_int(iEffect), float(point.x), float(point.y), float(point.z), float(fRotation))
		except AttributeError:
			pass
	def setDirty(self, *args): pass
	def getCityBillboardVisibility(self): return _engineFlags['billboards']
	def setCityBillboardVisibility(self, b): _engineFlags['billboards'] = bool(b)
	def getUnitFlagVisibility(self): return _engineFlags['flags']
	def setUnitFlagVisibility(self, b): _engineFlags['flags'] = bool(b)
	def getSelectionCursorVisibility(self): return _engineFlags['cursor']
	def setSelectionCursorVisibility(self, b): _engineFlags['cursor'] = bool(b)
	# the plots it marks (oc4host keeps them, the window draws them): coloured, outlined
	def addColoredPlot(self, iX, iY, color, ePlotStyle, eLayer):
		oc4host.engine_mark(_int(iX), _int(iY), float(color.r), float(color.g), float(color.b), float(color.a), _int(eLayer, 0), False)

	def addColoredPlotAlt(self, iX, iY, ePlotStyle, eLayer, szColor, fAlpha):
		c = _namedColor(szColor)
		if c is not None:
			oc4host.engine_mark(_int(iX), _int(iY), c[0], c[1], c[2], c[3] * float(fAlpha), _int(eLayer, 0), False)

	def clearColoredPlots(self, eLayer): oc4host.engine_clear(_int(eLayer, -1), False)

	def fillAreaBorderPlot(self, iX, iY, color, eLayer):
		oc4host.engine_mark(_int(iX), _int(iY), float(color.r), float(color.g), float(color.b), float(color.a), _int(eLayer, 0), True)

	def fillAreaBorderPlotAlt(self, iX, iY, eLayer, szColor, fAlpha):
		c = _namedColor(szColor)
		if c is not None:
			oc4host.engine_mark(_int(iX), _int(iY), c[0], c[1], c[2], c[3] * float(fAlpha), _int(eLayer, 0), True)

	def clearAreaBorderPlots(self, eLayer): oc4host.engine_clear(_int(eLayer, -1), True)

	# the signs (a player's caption on a plot) and landmarks (everyone's)
	def addSign(self, pPlot, iPlayer, szCaption): oc4host.engine_sign(pPlot.getX(), pPlot.getY(), _int(iPlayer), _text(szCaption))
	def removeSign(self, pPlot, iPlayer): oc4host.engine_unsign(pPlot.getX(), pPlot.getY(), _int(iPlayer), False)
	def addLandmark(self, pPlot, szCaption): oc4host.engine_sign(pPlot.getX(), pPlot.getY(), -1, _text(szCaption))
	def addLandmarkPopup(self, pPlot): pass
	def removeLandmark(self, pPlot): oc4host.engine_unsign(pPlot.getX(), pPlot.getY(), -1, True)
	def getNumSigns(self): return len(oc4host.engine_signs())

	def getSignByIndex(self, i):
		signs = oc4host.engine_signs()
		return _Sign(*signs[int(i)]) if 0 <= int(i) < len(signs) else None


def _namedColor(szColor):
	"""A colour info by its type ("COLOR_RED") as (r, g, b, a); None for none."""
	gc = _cpe.CyGlobalContext()
	i = gc.getInfoTypeForString(_str(szColor))
	if i < 0:
		return None
	c = gc.getColorInfo(i).getColor()
	return (float(c.r), float(c.g), float(c.b), float(c.a))


class _Sign(object):
	"""A sign as CyEngine.getSignByIndex gives it."""
	def __init__(self, x, y, player, caption):
		self._x, self._y, self._player, self._caption = x, y, player, caption

	def getPlot(self): return _cpe.CyMap().plot(self._x, self._y)
	def getPlayerType(self): return self._player
	def getCaption(self): return self._caption


def _lookAtPoint(point):
	try:
		oc4host.camera_look_at(float(point.x), float(point.y))
	except Exception:
		pass


class CyCamera(object):
	"""The map's camera: the scripts' looks at a point, a plot or a unit move it there; its
	angles as the game's default view has them."""

	def __getattr__(self, name):
		if name.startswith('__'):
			raise AttributeError(name)
		_log('CyCamera.' + name)
		return lambda *args, **kwargs: 0

	def LookAt(self, point, *args): _lookAtPoint(point)
	def JustLookAt(self, point): _lookAtPoint(point)
	def SimpleLookAt(self, point, *args): _lookAtPoint(point)
	def JustLookAtPlot(self, plot): _lookAtPoint(plot.getPoint())
	def LookAtUnit(self, unit): _lookAtPoint(unit.plot().getPoint())
	def GetZoom(self): return 1.0
	def SetZoom(self, f): pass
	def GetBaseTurn(self): return 0.0
	def SetBaseTurn(self, f): pass
	def GetBasePitch(self): return 0.0
	def SetBasePitch(self, f): pass


for _name in ('CyGInterfaceScreen', 'CyInterface', 'CyGlobeLayerManager', 'CyAudioGame', 'CyEngine', 'CyCamera'):
	setattr(_cpe, _name, globals()[_name])

# Profile options the interface asks every frame.
_cpe.CyUserProfile.isClockOn = lambda self: False
_cpe.CyUserProfile.wasClockJustTurnedOn = lambda self: False
_cpe.CyUserProfile.setClockJustTurnedOn = lambda self, b: None
_cpe.CyUserProfile.getGraphicOption = lambda self, i: False
# (the map's toggles by the minimap, as they start: no grid, no bare map, no yields, the scores shown)
_cpe.CyUserProfile.getGrid = lambda self: False
_cpe.CyUserProfile.getMap = lambda self: False
_cpe.CyUserProfile.getYields = lambda self: False
_cpe.CyUserProfile.getScores = lambda self: True


def getClockText():
	"""The time of day for the interface's clock (the engine's, in CvPythonExtensions)."""
	return unicode(time.strftime('%H:%M'))


_cpe.getClockText = getClockText

# The screen's size, as the scripts place their popups by it.
_cpe.CyUserProfile.getResolution = lambda self: 0
_cpe.CyUserProfile.getResolutionString = lambda self, i: '%dx%d' % (_resolution[0], _resolution[1])


# ---------------------------------------------------------------- popups

# The items as the host has them, and what a radio item is one of.
_P_TEXT, _P_BUTTON, _P_RADIO, _P_CHECK, _P_SEPARATOR, _P_IMAGE, _P_EDIT = range(7)
_C_RADIO, _C_PULLDOWN, _C_LISTBOX = range(3)
_SPIN_GROUPS = 10000  # a spin box is an edit box for its number, in these groups

_popups = {}  # number: a launched popup, until it is answered
_popupNumber = [0]


class PopupReturn(object):
	"""What the player chose in a script's popup, as the event manager's applyEvent gets it."""

	def __init__(self, button, chosen, checked, edits, spinners):
		self._button, self._chosen, self._checked, self._edits, self._spinners = button, chosen, checked, edits, spinners

	def getButtonClicked(self): return self._button
	def getSelectedRadioButton(self, iGroup=0): return self._chosen.get((_C_RADIO, int(iGroup)), -1)
	def getSelectedPullDownValue(self, iGroup=0): return self._chosen.get((_C_PULLDOWN, int(iGroup)), -1)
	def getSelectedListBoxValue(self, iGroup=0): return self._chosen.get((_C_LISTBOX, int(iGroup)), -1)
	def getCheckboxBitfield(self, iGroup=0): return self._checked.get(int(iGroup), 0)
	def getEditBoxString(self, iGroup=0): return self._edits.get(int(iGroup), u'')
	def getSpinnerWidgetValue(self, iGroup=0): return self._spinners.get(int(iGroup), 0)
	def getRadioButtonSize(self): return len([k for k in self._chosen if k[0] == _C_RADIO])
	def getPulldownSize(self): return len([k for k in self._chosen if k[0] == _C_PULLDOWN])
	def getListBoxSize(self): return len([k for k in self._chosen if k[0] == _C_LISTBOX])
	def getCheckboxSize(self): return len(self._checked)
	def getEditboxSize(self): return len(self._edits)
	def getSpinnerWidsize(self): return len(self._spinners)


class CyPopup(object):
	"""A script's own popup (the engine's CyPopup, PyPopup's): filled, then launched. Its
	answer goes to CvEventInterface.applyEvent with its id and user data, as the original's
	does for a popup with an event context; one without (PyPopup.PyPopup()) only tells.
	Pulldowns and list boxes are choices among their entries; tables are lines."""

	def __init__(self, iID=-1, eContextType=-1, bDynamic=True):
		self._id, self._context = _int(iID), _int(eContextType)
		self._header = u''
		self._items = []
		self._userData = None
		self._buttons = 0
		self._spinners = {}
		self._tables = {}
		self._onAnswer = None   # (the engine's own popups: what is done with the answer)

	def __getattr__(self, name):
		if name.startswith('__'):
			raise AttributeError(name)
		_log('CyPopup.' + name)
		return lambda *args, **kwargs: None

	def _add(self, kind, text=u'', help=u'', icon='', id=-1, group=0, checked=False, control=_C_RADIO):
		self._items.append([kind, _text(text), _text(help), _str(icon), int(id), int(group), bool(checked), control])

	def _find(self, kind, control, iID, iGroup):
		for item in self._items:
			if item[0] == kind and item[7] == control and item[4] == int(iID) and item[5] == int(iGroup):
				return item
		return None

	def _choose(self, control, iID, iGroup):
		for item in self._items:
			if item[0] == _P_RADIO and item[7] == control and item[5] == int(iGroup):
				item[6] = item[4] == int(iID)

	def isNone(self): return False
	def setUserData(self, userData): self._userData = userData
	def setPosition(self, iX, iY): pass
	def setSize(self, iXS, iYS): pass
	def setTimer(self, iTime): pass

	# texts
	def setHeaderString(self, szText, uiFlags=_FONT_CENTER): self._header = _text(szText)
	def setBodyString(self, szText, uiFlags=_FONT_LEFT): self._add(_P_TEXT, szText)
	def setPythonBodyString(self, szText, szName, szHelpText, uiFlags=_FONT_LEFT): self._add(_P_TEXT, szText, szHelpText)
	def setPythonBodyStringXY(self, szText, szName, szHelpText, iX=-1, iY=-1, uiFlags=_FONT_LEFT): self._add(_P_TEXT, szText, szHelpText)
	def addSeparator(self, *args): self._add(_P_SEPARATOR)

	# radio buttons and checkboxes
	def createRadioButtons(self, iNumButtons, iGroup=0):
		for i in range(int(iNumButtons)):
			self._add(_P_RADIO, id=i, group=iGroup, checked=i == 0)

	def createPythonRadioButtons(self, iNumButtons, iGroup=0): self.createRadioButtons(iNumButtons, iGroup)

	def setRadioButtonText(self, iRadioButtonID, szText, iGroup=0): self.setPythonRadioButtonText(iRadioButtonID, szText, u'', iGroup)

	def setPythonRadioButtonText(self, iRadioButtonID, szText, szHelpText, iGroup=0):
		item = self._find(_P_RADIO, _C_RADIO, iRadioButtonID, iGroup)
		if item is not None:
			item[1], item[2] = _text(szText), _text(szHelpText)

	def createCheckBoxes(self, iNumBoxes, iGroup=0):
		for i in range(int(iNumBoxes)):
			self._add(_P_CHECK, id=i, group=iGroup)

	def createPythonCheckBoxes(self, iNumBoxes, iGroup=0): self.createCheckBoxes(iNumBoxes, iGroup)

	def setCheckBoxText(self, iCheckBoxID, szText, iGroup=0): self.setPythonCheckBoxText(iCheckBoxID, szText, u'', iGroup)

	def setPythonCheckBoxText(self, iCheckBoxID, szText, szHelpText, iGroup=0):
		item = self._find(_P_CHECK, _C_RADIO, iCheckBoxID, iGroup)
		if item is not None:
			item[1], item[2] = _text(szText), _text(szHelpText)

	def setCheckBoxState(self, iCheckBoxID, bChecked, iGroup=0):
		item = self._find(_P_CHECK, _C_RADIO, iCheckBoxID, iGroup)
		if item is not None:
			item[6] = bool(bChecked)

	# edit boxes and spin boxes
	def createEditBox(self, szText, *args):
		# (szText, iGroup) or (szText, iX, iY, iGroup)
		self._add(_P_EDIT, szText, group=_int(args[-1], 0) if args else 0)

	def createEditBoxXY(self, szText, iX=-1, iY=-1, iGroup=0): self._add(_P_EDIT, szText, group=iGroup)
	def createPythonEditBox(self, szText, szHelpText, iGroup=0): self._add(_P_EDIT, szText, szHelpText, group=iGroup)
	def createPythonEditBoxXY(self, szText, szHelpText, iGroup=0, iX=-1, iY=-1): self._add(_P_EDIT, szText, szHelpText, group=iGroup)
	def setEditBoxMaxCharCount(self, *args): pass

	def createSpinBox(self, iIndex, szHelpText, iDefault, iIncrement, iMax, iMin):
		# (a box for its number, kept between the bounds)
		self._spinners[int(iIndex)] = (int(iDefault), int(iMin), int(iMax))
		self._add(_P_EDIT, u'%d' % int(iDefault), szHelpText, group=_SPIN_GROUPS + int(iIndex))

	# pulldowns and list boxes: their entries to choose from
	def createPullDown(self, iGroup=0): pass
	def createPullDownXY(self, iGroup=0, iX=-1, iY=-1): pass
	def createPythonPullDown(self, szHelpText, iGroup=0): pass
	def createPythonPullDownXY(self, szHelpText, iGroup=0, iX=-1, iY=-1): pass
	def createListBox(self, iGroup=0): pass
	def createListBoxXY(self, iGroup=0, iX=-1, iY=-1): pass
	def createPythonListBox(self, szHelpText, iGroup=0): pass
	def createPythonListBoxXY(self, szHelpText, iGroup=0, iX=-1, iY=-1): pass

	def _entry(self, control, szText, iID, iGroup):
		first = not [i for i in self._items if i[0] == _P_RADIO and i[7] == control and i[5] == int(iGroup)]
		self._add(_P_RADIO, szText, id=_int(iID, 0), group=iGroup, checked=first, control=control)

	def addPullDownString(self, szText, iID, iGroup=0): self._entry(_C_PULLDOWN, szText, iID, iGroup)
	def addListBoxString(self, szText, iID, iGroup=0): self._entry(_C_LISTBOX, szText, iID, iGroup)
	def setSelectedPulldownID(self, iID, iGroup=0): self._choose(_C_PULLDOWN, iID, iGroup)
	def setSelectedListBoxStringID(self, iID, iGroup=0): self._choose(_C_LISTBOX, iID, iGroup)

	# buttons and pictures
	def addButton(self, szText): self.addPythonButton('', szText, u'')
	def addButtonXY(self, szText, iX=-1, iY=-1): self.addPythonButton('', szText, u'')

	def addPythonButton(self, szFunctionName, szButtonText, szHelpText, szArt='', iData1=-1, iData2=-1, bOption=True):
		self._add(_P_BUTTON, szButtonText, szHelpText, szArt if _str(szArt).lower().endswith('.dds') else '', id=self._buttons)
		self._buttons += 1

	def addPythonButtonXY(self, szFunctionName, szButtonText, szHelpText, szArt='', iData1=-1, iData2=-1, bOption=True, iX=-1, iY=-1):
		self.addPythonButton(szFunctionName, szButtonText, szHelpText, szArt, iData1, iData2, bOption)

	def addDDS(self, szImageLocation, iX=0, iY=0, iWidth=0, iHeight=0): self._add(_P_IMAGE, icon=szImageLocation)
	def addPythonDDS(self, szImageLocation, szHelpText, iX=0, iY=0, iWidth=0, iHeight=0): self._add(_P_IMAGE, help=szHelpText, icon=szImageLocation)

	# tables: a line per row
	def createTable(self, iRows, iColumns, iGroup=0): self._tables[int(iGroup)] = {}
	def setTableCellSize(self, iCol, iPixels, iGroup=0): pass
	def setTableYSize(self, iRow, iSize, iGroup=0): pass
	def addTableCellText(self, iRow, iCol, szText, iGroup=0): self._tables.setdefault(int(iGroup), {})[(int(iRow), int(iCol))] = _text(szText)
	def addTableCellImage(self, iRow, iCol, szImage, iGroup=0): pass
	def addTableCellDDS(self, iRow, iCol, szImage, iX=0, iY=0, iWidth=0, iHeight=0, iGroup=0): pass

	def completeTableAndAttach(self, iGroup=0, iX=-1, iY=-1):
		cells = self._tables.pop(int(iGroup), {})
		for row in sorted(set([r for r, c in cells])):
			self._add(_P_TEXT, u'    '.join([cells[(r, c)] for r, c in sorted(cells) if r == row and cells[(r, c)]]))

	def launch(self, bCreateOkButton=True, eState=0):
		_popupNumber[0] += 1
		number = _popupNumber[0]
		_popups[number] = self
		# (POPUPSTATE_IMMEDIATE: before the ones waiting)
		oc4host.popup_launch(number, self._header, [tuple(i) for i in self._items], bool(bCreateOkButton), _int(eState, 0) == 0)


def popupAnswered(number, button, chosen, checked, edits):
	"""The host: the player answered the script's popup `number`."""
	popup = _popups.pop(number, None)
	if popup is None or (popup._onAnswer is None and (popup._id == -1 or popup._context == -1)):
		return
	spinners = {}
	for index, (default, low, high) in popup._spinners.items():
		text = dict(edits).get(_SPIN_GROUPS + index, u'')
		try:
			value = int(text)
		except ValueError:
			value = default
		spinners[index] = max(min(value, max(low, high)), min(low, high))
	edits = dict([(g, t) for g, t in edits if g < _SPIN_GROUPS])
	answer = PopupReturn(button, dict([((c, g), i) for c, g, i in chosen]), dict(checked), edits, spinners)
	if popup._onAnswer is not None:
		popup._onAnswer(answer)
		return
	import CvEventInterface
	CvEventInterface.applyEvent((popup._id, _cpe.CyGame().getActivePlayer(), popup._userData, answer))


class CyPopupInfo(object):
	"""A popup for the DLL to show (the engine's CyPopupInfo, over the DLL's CvPopupInfo):
	BUTTONPOPUP_PYTHON ones call the script back (on focus, when clicked) through the DLL."""

	def __init__(self):
		self._type, self._data, self._flags, self._options = 0, [-1, -1, -1], 0, [False, False]
		self._text, self._onFocus, self._onClicked, self._module = u'', '', '', ''
		self._buttons = []

	def __getattr__(self, name):
		if name.startswith('__'):
			raise AttributeError(name)
		_log('CyPopupInfo.' + name)
		return lambda *args, **kwargs: None

	def isNone(self): return False
	def setButtonPopupType(self, eValue): self._type = int(eValue)
	def getButtonPopupType(self): return _cpe.ButtonPopupTypes(self._type)
	def setData1(self, i): self._data[0] = int(i)
	def setData2(self, i): self._data[1] = int(i)
	def setData3(self, i): self._data[2] = int(i)
	def getData1(self): return self._data[0]
	def getData2(self): return self._data[1]
	def getData3(self): return self._data[2]
	def setFlags(self, i): self._flags = int(i)
	def getFlags(self): return self._flags
	def setOption1(self, b): self._options[0] = bool(b)
	def setOption2(self, b): self._options[1] = bool(b)
	def getOption1(self): return self._options[0]
	def getOption2(self): return self._options[1]
	def setText(self, szText): self._text = _text(szText)
	def getText(self): return self._text
	def setOnFocusPythonCallback(self, s): self._onFocus = _str(s)
	def getOnFocusPythonCallback(self): return self._onFocus
	def setOnClickedPythonCallback(self, s): self._onClicked = _str(s)
	def getOnClickedPythonCallback(self): return self._onClicked
	def setPythonModule(self, s): self._module = _str(s)
	def getPythonModule(self): return self._module
	def addPythonButton(self, szText, szArt=''): self._buttons.append((_text(szText), _str(szArt)))
	def getNumPythonButtons(self): return len(self._buttons)
	def getPythonButtonText(self, i): return self._buttons[int(i)][0]
	def getPythonButtonArt(self, i): return self._buttons[int(i)][1]

	def addPopup(self, iPlayer):
		oc4host.popup_info_add(int(iPlayer), self._type, self._data[0], self._data[1], self._data[2], self._flags, self._options[0],
			self._options[1], self._text, self._onFocus, self._onClicked, self._module, self._buttons)


for _name in ('CyPopup', 'CyPopupInfo', 'PopupReturn'):
	setattr(_cpe, _name, globals()[_name])


# -------------------------------------------------------------------- diplomacy

_DIPLO = 9000   # the engine's own diplomacy screen (Screens.cpp's kDiplomacyScreen)
_diplo = {}     # what the rows of its lists stand for: list name -> [entry, ...]


class CyDiplomacy(object):
	"""The engine's side of a talk with a leader, for the game's script (CvDiplomacy.py): the
	host keeps the talk (what the leader says, the player's answers) and its trade table."""

	def __getattr__(self, name):
		if name.startswith('__'):
			raise AttributeError(name)
		_log('CyDiplomacy.' + name)
		return lambda *args, **kwargs: 0

	def getWhoTradingWith(self): return oc4host.diplo_who()
	def getData(self): return oc4host.diplo_data()
	def setAIString(self, szString, args=()): oc4host.diplo_set_ai_string(_text(szString), tuple(args or ()))
	def setAIComment(self, eComment): oc4host.diplo_set_ai_comment(int(eComment))
	def clearUserComments(self): oc4host.diplo_clear_user()

	def addUserComment(self, eComment, iData1, iData2, szString, args=()):
		oc4host.diplo_add_user(int(eComment), _int(iData1), _int(iData2), _text(szString), tuple(args or ()))

	def startTrade(self, eComment=-1, bRenegotiate=False): oc4host.diplo_start_trade(bool(bRenegotiate))
	def endTrade(self): oc4host.diplo_end_trade()
	def showAllTrade(self, bShow): oc4host.diplo_show_all(bool(bShow))
	def ourOfferEmpty(self): return int(oc4host.diplo_our_empty())
	def theirOfferEmpty(self): return int(oc4host.diplo_their_empty())
	def atWar(self): return bool(oc4host.diplo_at_war())
	def isAIOffer(self): return bool(oc4host.diplo_is_ai_offer())
	def setAIOffer(self, iOffer): oc4host.diplo_set_ai_offer(bool(iOffer))
	def offerDeal(self): return int(oc4host.diplo_offer())
	def counterPropose(self): return int(oc4host.diplo_counter())
	def implementDeal(self): oc4host.diplo_implement()
	def declareWar(self): oc4host.diplo_declare_war()
	def diploEvent(self, eDiploEvent, iData1, iData2): oc4host.diplo_event(int(eDiploEvent), _int(iData1), _int(iData2))
	def hasAnnualDeal(self): return bool(oc4host.diplo_annual())
	def theirVassalTribute(self): return bool(oc4host.diplo_vassal_tribute())
	def closeScreen(self):
		if _diploAnswering:
			_diploAnswering[-1] = True
		else:
			oc4host.diplo_close()
	def performHeadAction(self, eAction):
		who = oc4host.diplo_who()
		if who >= 0:
			leader = _int(_cpe.CyGlobalContext().getPlayer(who).getLeaderType())
			_headAction(_screens.get(_DIPLO), 'DiploLeader', _int(eAction, -1), leader)


_cpe.CyDiplomacy = CyDiplomacy


def _art(name):
	try:
		return _str(_cpe.CyArtFileMgr().getInterfaceArtInfo(name).getPath())
	except Exception:
		return ''


_diploOpen = set()  # the item lists the player opened in this talk ('their', 'our')
# The original's frame (DIPLOMACY_NEW_BACKGROUND, 512 x 1024): what of it shows, its title bar,
# the leader's window between its posts, the box of what the leader says, the answers' box.
_FRAME_LEFT, _FRAME_TOP, _FRAME_RIGHT, _FRAME_BOTTOM, _FRAME_W, _FRAME_H = 12, 15, 500, 1010, 512, 1024
_FRAME_WINDOW = (76, 70, 436, 680)
_FRAME_SPEECH, _FRAME_ANSWERS = (695, 825), (830, 1005)


def _diploList(screen, name, header, items, theirs, offered, x, y, w, h, root=None, opened=True):
	"""A list of the trade table on the theme's diplomacy panels (Panel_Dip_Top/Bottom and their
	header strips): a player's items, a tree under "Tradeable Items" (open or closed), or an
	offer. An item its owner will not give is red (its help says why)."""
	WidgetTypes = _cpe.WidgetTypes
	screen.addPanel(name + 'Panel', u'', u'', True, False, x, y, w, h, _cpe.PanelStyles.PANEL_STYLE_MAIN)
	screen.setStyle(name + 'Panel', 'Panel_Dip_Bottom_Style' if offered else 'Panel_Dip_Top_Style')
	screen.addPanel(name + 'Header', u'', u'', True, False, x, y, w, 24, _cpe.PanelStyles.PANEL_STYLE_MAIN)
	screen.setStyle(name + 'Header', 'Panel_Dip_SidesHeaderBottom_Style' if offered else 'Panel_Dip_SidesHeaderTop_Style')
	screen.setLabel(name + 'Title', 'Background', u'<font=2b>' + _text(header) + u'</font>', _FONT_CENTER, x + w / 2, y + 4, -0.1,
		_cpe.FontTypes.GAME_FONT, WidgetTypes.WIDGET_GENERAL, -1, -1)
	screen.addTableControlGFC(name, 1, x + 4, y + 28, w - 8, h - 32, False, False, 24, 24, _cpe.TableStyles.TABLE_STYLE_EMPTY)
	screen.setTableColumnHeader(name, 0, u'', w - 8)
	table = screen._s.get(name)
	rows = []
	if root is not None:
		row = screen.appendTableRow(name)
		screen.setTableText(name, 0, row, _text(root), '', WidgetTypes.WIDGET_GENERAL, -1, -1, _FONT_LEFT)
		rows.append(('root', theirs))
	if opened:
		under = False
		for (itemType, data, text, icon, heading, denied) in items:
			row = screen.appendTableRow(name)
			# (in the tree: headings under its root, their items under them)
			indent = u'' if root is None else (u'   ' if heading or not under else u'      ')
			if heading:
				under = True
				cell = indent + u'<font=2b>' + text + u'</font>'
			elif denied:
				cell = indent + u'<color=255,96,96,255>' + text + u'</color>'
			else:
				cell = indent + text
			widget = WidgetTypes.WIDGET_GENERAL if heading else WidgetTypes.WIDGET_TRADE_ITEM
			screen.setTableText(name, 0, row, cell, icon, widget, itemType, data, _FONT_LEFT)
			if theirs and not heading:
				# (the leader's item: the help says what it is to the player, bOption)
				table.rows[row][0] = table.rows[row][0] + (True,)
			rows.append(('item', theirs, offered, itemType, data, heading, denied, text))
	_diplo[name] = rows


def diploShow():
	"""The host: the talk changed. The engine's diplomacy screen shows it as the original does,
	over the map and the interface: in the middle the original's frame, its title bar with the
	leader's name and attitude, the leader between its posts (its portrait: the original's head
	is 3D), what the leader says and the answers; on its sides what each one has (a tree of
	tradeable items, open when they trade) and what each offers. A click moves an item into its
	offer, or out of it."""
	s = _screenOf(_DIPLO, 'DiplomacyScreen')
	kept = dict([(n, (w.offset, w.hoffset)) for n, w in s.widgets.items()])
	reset(_DIPLO)
	_diplo.clear()
	who = oc4host.diplo_who()
	if who < 0:
		_diploOpen.clear()
		_heads.pop('DiploLeader', None)
		hide(_DIPLO)
		oc4host.audio_talk_music(-1, -1)
		return
	gc = _cpe.CyGlobalContext()
	WidgetTypes, PanelStyles, FontTypes = _cpe.WidgetTypes, _cpe.PanelStyles, _cpe.FontTypes
	translator = _cpe.CyTranslator()
	screen = CyGInterfaceScreen('DiplomacyScreen', _DIPLO)
	s.closeOnEscape = False
	W, H = _resolution
	me = oc4host.diplo_player()
	them, us = gc.getPlayer(who), gc.getPlayer(me)
	leader = gc.getLeaderHeadInfo(them.getLeaderType())
	trading, showAll, current = oc4host.diplo_trading()
	# (the leader's music for the era we are in: at war its war theme, each after its intro)
	era = us.getCurrentEra()
	if gc.getTeam(us.getTeam()).isAtWar(them.getTeam()):
		oc4host.audio_talk_music(leader.getDiploWarIntroMusicScriptIds(era), leader.getDiploWarMusicScriptIds(era))
	else:
		oc4host.audio_talk_music(leader.getDiploPeaceIntroMusicScriptIds(era), leader.getDiploPeaceMusicScriptIds(era))
	screen.setDimensions(0, 0, W, H)
	screen.showWindowBackground(False)
	# (the talk takes every click; the map and the interface show through)
	screen.addPanel('DiploCover', u'', u'', True, False, 0, 0, W, H, PanelStyles.PANEL_STYLE_EMPTY)

	# The middle column, the original's frame stretched over it.
	CW = 480
	cx = W / 2 - CW / 2
	sx = float(CW) / (_FRAME_RIGHT - _FRAME_LEFT)
	sy = float(H) / (_FRAME_BOTTOM - _FRAME_TOP)
	fx = lambda u: int(cx + (u - _FRAME_LEFT) * sx)
	fy = lambda v: int((v - _FRAME_TOP) * sy)

	# the leader, under the frame's posts
	wx0, wy0, wx1, wy1 = fx(_FRAME_WINDOW[0]), fy(_FRAME_WINDOW[1]), fx(_FRAME_WINDOW[2]), fy(_FRAME_WINDOW[3])
	try:
		mood = int(them.AI_getAttitude(me))
	except Exception:
		mood = -1
	screen.addLeaderheadGFC('DiploLeader', them.getLeaderType(), mood, wx0, wy0, wx1 - wx0, wy1 - wy0, WidgetTypes.WIDGET_LEADERHEAD, who, me)
	screen.addDDSGFC('DiploFrame', _art('DIPLOMACY_NEW_BACKGROUND'), fx(0), fy(0), int(_FRAME_W * sx), int(_FRAME_H * sy),
		WidgetTypes.WIDGET_GENERAL, -1, -1)

	# its title bar: the leader, the civilization, its attitude
	try:
		attitude = gc.getAttitudeInfo(them.AI_getAttitude(me)).getDescription()
	except Exception:
		attitude = u''
	title = u'%s - %s' % (them.getName(), them.getCivilizationDescription(0))
	if attitude:
		title += u' (%s)' % attitude
	screen.setText('DiploTitle', 'Background', u'<font=3b>' + title + u'</font>', _FONT_LEFT, cx + 8, fy(_FRAME_TOP) + (fy(_FRAME_WINDOW[1]) - 22) / 2,
		-0.1, FontTypes.TITLE_FONT, WidgetTypes.WIDGET_LEADERHEAD, who, me)

	# what the leader says
	sy0, sy1 = fy(_FRAME_SPEECH[0]), fy(_FRAME_SPEECH[1])
	screen.addMultilineText('DiploAIText', u'<font=3>' + oc4host.diplo_ai_text() + u'</font>', cx + 16, sy0 + 14, CW - 32, sy1 - sy0 - 20,
		WidgetTypes.WIDGET_GENERAL, -1, -1, _FONT_LEFT)

	# the answers, one under the other in the middle (gold under the mouse); they scroll when many
	ay0, ay1 = fy(_FRAME_ANSWERS[0]) + 8, fy(_FRAME_ANSWERS[1])
	screen.addScrollPanel('DiploAnswers', u'', cx + 8, ay0, CW - 16, ay1 - ay0, PanelStyles.PANEL_STYLE_EMPTY)
	answers = []
	step = 31
	for i, (comment, data1, data2, answer) in enumerate(oc4host.diplo_user()):
		text = u'<font=3>' + answer + u'</font>'
		width = _textWidth(text, 3)
		screen.setTextAt('DiploAnswer%d' % i, 'DiploAnswers', text, _FONT_LEFT, max(0, (CW - 16 - width) / 2), i * step, -0.1, FontTypes.GAME_FONT,
			WidgetTypes.WIDGET_DIPLOMACY_RESPONSE, comment, i)
		answers.append((comment, data1, data2))
	_diplo['answers'] = answers

	# The sides: what each one has (its tradeable items), what each offers.
	SW = 254
	lx, rx = cx - 9 - SW, cx + CW + 8
	split = fy(_FRAME_SPEECH[0])
	theirName, root = them.getName().upper(), translator.getText('TXT_KEY_TRADEABLE_ITEMS', ())
	listsOpen = trading and showAll and not current
	_diploList(screen, 'DiploTheirItems', translator.getText('TXT_KEY_TRADE_PLAYER_HAS', (theirName,)).upper(), oc4host.diplo_items(True, False),
		True, False, lx, 0, SW, split, root, listsOpen or 'their' in _diploOpen)
	_diploList(screen, 'DiploOurItems', translator.getText('TXT_KEY_TRADE_YOU_HAVE', ()).upper(), oc4host.diplo_items(False, False),
		False, False, rx, 0, SW, split, root, listsOpen or 'our' in _diploOpen)
	_diploList(screen, 'DiploTheirOffer', translator.getText('TXT_KEY_TRADE_PLAYER_OFFERS', (theirName,)).upper(), oc4host.diplo_items(True, True),
		True, True, lx, split + 8, SW, H - split - 8)
	_diploList(screen, 'DiploOurOffer', translator.getText('TXT_KEY_TRADE_YOU_OFFER', ()).upper(), oc4host.diplo_items(False, True),
		False, True, rx, split + 8, SW, H - split - 8)

	for n, (offset, hoffset) in kept.items():
		w = s.widgets.get(n)
		if w is not None:
			w.offset, w.hoffset = offset, hoffset
	screen.showScreen(0, False)

def _diploRefresh():
	"""The trade table changed: the script's answers for it again (the original's refresh)."""
	import CvDiplomacyInterface
	CvDiplomacyInterface.refresh((oc4host.diplo_comment(),))


def _diploGold(theirs, itemType, text):
	"""Gold into an offer: how much is asked first (most what its owner can give)."""
	perTurn = itemType == int(_cpe.TradeableItems.TRADE_GOLD_PER_TURN)
	most = oc4host.diplo_max_gold(bool(theirs), perTurn)
	if most <= 0:
		return
	popup = CyPopup(-1, -1)
	popup.setHeaderString(text)
	# (the game's question: "How much gold would you like to offer?")
	popup.setBodyString(_cpe.CyTranslator().getText('TXT_KEY_TRADE_GPT_TO_OFFER' if perTurn else 'TXT_KEY_TRADE_GOLD_TO_OFFER', ()))
	popup.createSpinBox(0, u'', most, 1, most, 0)

	def answered(answer):
		amount = answer.getSpinnerWidgetValue(0)
		if amount > 0 and oc4host.diplo_who() >= 0:
			oc4host.diplo_toggle(bool(theirs), False, itemType, 0, amount)
			_diploRefresh()

	popup._onAnswer = answered
	popup.launch(True, 0)



def _diploTrade():
	"""An item taken while the leader only talks, or makes its own offer: now they trade (the
	script's answers for AI_DIPLOCOMMENT_TRADING, both lists open)."""
	trading, showAll, current = oc4host.diplo_trading()
	if trading and showAll and not current:
		return
	import CvDiplomacy
	CvDiplomacy.CvDiplomacy().setAIComment(_cpe.CyGlobalContext().getInfoTypeForString('AI_DIPLOCOMMENT_TRADING'))
	oc4host.diplo_show_all(True)


# (an answer's handlers all see the talk; one that closes it closes it after them, as BUG's
# DiplomacyUtil runs after the mod's own handleUserResponse and still asks who it was with)
_diploAnswering = []


def _diploAnswer(entry):
	import CvDiplomacyInterface
	_diploAnswering.append(False)
	try:
		CvDiplomacyInterface.handleUserResponse(entry)
	finally:
		if _diploAnswering.pop():
			oc4host.diplo_close()


def diploClick(name, part, right=False, shift=False):
	"""The host: a click on the diplomacy screen. An answer is the script's (handleUserResponse);
	"Tradeable Items" opens its list or closes it; an item goes into its offer, or out of it, and
	the script answers the table again."""
	name = _str(name)
	if right:
		return
	if name.startswith('DiploAnswer') and name[11:].isdigit():
		answers = _diplo.get('answers', [])
		if int(name[11:]) < len(answers):
			_diploAnswer(answers[int(name[11:])])
		return
	rows = _diplo.get(name)
	row = int(part) >> 8
	if not rows or not 0 <= row < len(rows):
		return
	if rows[row][0] == 'root':
		side = 'their' if rows[row][1] else 'our'
		if side in _diploOpen:
			_diploOpen.discard(side)
		else:
			_diploOpen.add(side)
		diploShow()
		return
	kind, theirs, offered, itemType, data, heading, denied, text = rows[row]
	if heading or (denied and not offered):
		return
	# (the current deals shown: a click on one of their items cancels its deal, as it may)
	if offered and oc4host.diplo_trading()[2]:
		done, reason = oc4host.diplo_cancel(itemType, data)
		if done:
			import CvDiplomacyInterface
			CvDiplomacyInterface.dealCanceled()
		elif reason:
			CyInterface().addImmediateMessage(reason, '')
		_diploRefresh()
		return
	_diploTrade()
	if not offered and itemType in (int(_cpe.TradeableItems.TRADE_GOLD), int(_cpe.TradeableItems.TRADE_GOLD_PER_TURN)):
		_diploGold(theirs, itemType, text)
		return
	oc4host.diplo_toggle(bool(theirs), bool(offered), itemType, data, 0)
	_diploRefresh()


_DIPLO_ESCAPE = ('EXIT', 'NEVERMIND', 'REJECT', 'REJECT_DEMAND', 'REJECT_OFFER', 'REFUSE_HELP', 'NO_CONVERT', 'NO_JOIN_WAR',
	'NO_MAKE_PEACE_WITH', 'NO_RENEGOTIATE', 'NO_REVOLUTION', 'NO_STOP_TRADING')
_DIPLO_ENTER = ('ACCEPT', 'ACCEPT_DEMAND', 'ACCEPT_OFFER', 'GIVE_HELP', 'PEACE', 'COMPLETE_DEAL')


def _diploPick(kinds):
	gc = _cpe.CyGlobalContext()
	answers = _diplo.get('answers', [])
	for kind in kinds:
		comment = gc.getInfoTypeForString('USER_DIPLOCOMMENT_' + kind)
		for entry in answers:
			if entry[0] == comment:
				_diploAnswer(entry)
				return


def diploEscape():
	"""Esc in a talk: its way out, or a no (never war); the talk keeps the key either way."""
	_diploPick(_DIPLO_ESCAPE)
	return True


def diploKey(key):
	"""The host: a key (a Windows key code) while a talk is up, which keeps it. Enter: a yes; 1 to 9
	the answer so numbered."""
	if key == 0x0D:
		_diploPick(_DIPLO_ENTER)
	elif 0x31 <= key <= 0x39:
		answers = _diplo.get('answers', [])
		if key - 0x31 < len(answers):
			_diploAnswer(answers[key - 0x31])
	elif key == 0x1B:
		diploEscape()
	return True
