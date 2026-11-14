## openciv4: the options screen's controls and the player's profile.
##
## The game's options screen (CvOptionsScreen.py) is a CyGTabCtrl: a window of tabs whose
## forms are boxes of controls (vertical and horizontal boxes, panels, labels, check boxes,
## pull downs, sliders, buttons, edit boxes), each control calling back into the script's
## callback module (CvOptionsScreenCallbackInterface) when it is used. Here the boxes are laid
## out (each box's children one under / beside another, the space shared) onto a screen of
## oc4screens' widgets, a tab at a time.
##
## CyUserProfile is the player's profile those callbacks read and set: the volumes (the sounds
## play as loud), the graphic and player options, the clock; kept in the profile file the host
## names (Documents\My Games\openciv4\profile.ini).

import CvPythonExtensions as _cpe
import oc4host
import oc4screens

OPTIONS = 31002 # its python file id (none of the game's screens has it)
W, H = 800, 600

# ---------------------------------------------------------------- the profile

_VOLUME_STOPS = 20
_VOLUMES = ('Master', 'Music', 'SoundEffects', 'Speech', 'Ambience', 'Interface')
_UI_SCALES = (0, 100, 125, 150, 200) # the resolution's choices: the interface's scale, 0 the window's own


def _scales():
	"""The scales the resolution offers at this window's size: one per size (a small window
	cannot scale up as far, so several come out the same), the player's own kept among them."""
	out, seen = [0], {}
	for scale in _UI_SCALES[1:]:
		size = tuple(oc4host.ui_size_at(scale))
		if size not in seen:
			seen[size] = scale
			out.append(scale)
		elif scale == _profile['uiScale']:
			out[out.index(seen[size])] = seen[size] = scale
	return out

_profile = {
	'name': 'openciv4',
	'volume': dict((v, _VOLUME_STOPS) for v in _VOLUMES),
	'nosound': dict((v, False) for v in _VOLUMES),
	'graphic': {},
	'player': {},
	'clock': False, 'hours24': False,
	'graphicsLevel': 0, 'renderQuality': 0, 'globeView': 0, 'movieQuality': 0, 'mainMenu': 0, 'antiAliasing': 0, 'uiScale': 0,
	'speakers': '', 'musicPath': '', 'voice': False,
	'wasdPan': False, 'autoEndTurn': False, 'edgeScroll': True,
}


def _path():
	try:
		return oc4host.profile_path()
	except Exception:
		return ''


def _load():
	path = _path()
	if not path:
		return
	try:
		f = open(path, 'r')
	except IOError:
		return
	try:
		for line in f:
			if '=' not in line:
				continue
			key, value = line.strip().split('=', 1)
			parts = key.split('.')
			try:
				if parts[0] in ('volume',) and parts[1] in _VOLUMES:
					_profile['volume'][parts[1]] = max(0, min(_VOLUME_STOPS, int(value)))
				elif parts[0] == 'nosound' and parts[1] in _VOLUMES:
					_profile['nosound'][parts[1]] = value == '1'
				elif parts[0] in ('graphic', 'player'):
					_profile[parts[0]][int(parts[1])] = value == '1'
				elif parts[0] in ('clock', 'hours24', 'voice', 'wasdPan', 'autoEndTurn', 'edgeScroll'):
					_profile[parts[0]] = value == '1'
				elif parts[0] in ('graphicsLevel', 'renderQuality', 'globeView', 'movieQuality', 'mainMenu', 'antiAliasing', 'uiScale'):
					_profile[parts[0]] = int(value)
				elif parts[0] in ('name', 'speakers', 'musicPath'):
					_profile[parts[0]] = value
			except (ValueError, IndexError):
				pass
	finally:
		f.close()


def _save():
	path = _path()
	if not path:
		return
	lines = []
	for key in ('name', 'speakers', 'musicPath'):
		lines.append('%s=%s' % (key, _profile[key]))
	for key in ('clock', 'hours24', 'voice', 'wasdPan', 'autoEndTurn', 'edgeScroll'):
		lines.append('%s=%d' % (key, 1 if _profile[key] else 0))
	for key in ('graphicsLevel', 'renderQuality', 'globeView', 'movieQuality', 'mainMenu', 'antiAliasing', 'uiScale'):
		lines.append('%s=%d' % (key, _profile[key]))
	for v in _VOLUMES:
		lines.append('volume.%s=%d' % (v, _profile['volume'][v]))
		lines.append('nosound.%s=%d' % (v, 1 if _profile['nosound'][v] else 0))
	for kind in ('graphic', 'player'):
		for i, on in sorted(_profile[kind].items()):
			lines.append('%s.%d=%d' % (kind, i, 1 if on else 0))
	try:
		f = open(path, 'w')
		f.write('\n'.join(lines) + '\n')
		f.close()
	except IOError:
		pass


def _applyGraphics():
	"""The graphic options the profile has, to the host (the game core and the window ask it)."""
	try:
		oc4host.reset_graphic_options()
		for i, on in _profile['graphic'].items():
			oc4host.set_graphic_option(int(i), bool(on))
		oc4host.set_ui_scale(_profile['uiScale'])
		oc4host.set_wasd_pan(_profile['wasdPan'])
		oc4host.set_auto_end_turn(_profile['autoEndTurn'])
		oc4host.set_edge_scroll(_profile['edgeScroll'])
	except Exception:
		pass


# W A S D panning (openciv4's own option, no game's): its check box on the options' Game tab,
# above the language; while it is on, Ctrl+Shift+W in the window turns it off (the host calls setWasdPan).
WASD_PAN_CHECKBOX = 'Oc4WasdPanCheckBox'


def setWasdPan(on):
	"""Ctrl+Shift+W: the player's choice, kept."""
	_profile['wasdPan'] = bool(on)
	_save()
	ctrl = _state['ctrl']
	if ctrl is not None:
		ctrl.setValue(WASD_PAN_CHECKBOX, bool(on))


def handleWasdPanClicked(args):
	on = bool(args[0])
	_profile['wasdPan'] = on
	_save()
	try:
		oc4host.choose_wasd_pan(on)
	except Exception:
		pass
	return 1


def _wasdPanOn():
	try:
		return bool(oc4host.wasd_pan())
	except Exception:
		return _profile['wasdPan']


# Ending idle turns (openciv4's own option too): its check box under W A S D's; Ctrl+Shift+E in
# the window changes it as well (the host calls setAutoEndTurn).
AUTO_END_TURN_CHECKBOX = 'Oc4AutoEndTurnCheckBox'


def setAutoEndTurn(on):
	"""Ctrl+Shift+E: the player's choice, kept."""
	_profile['autoEndTurn'] = bool(on)
	_save()
	ctrl = _state['ctrl']
	if ctrl is not None:
		ctrl.setValue(AUTO_END_TURN_CHECKBOX, bool(on))


def handleAutoEndTurnClicked(args):
	on = bool(args[0])
	_profile['autoEndTurn'] = on
	_save()
	try:
		oc4host.choose_auto_end_turn(on)
	except Exception:
		pass
	return 1


def _autoEndTurnOn():
	try:
		return bool(oc4host.auto_end_turn())
	except Exception:
		return _profile['autoEndTurn']


# Edge scrolling (openciv4's own option, on by default as in the original): its check box under
# the others on the Game tab.
EDGE_SCROLL_CHECKBOX = 'Oc4EdgeScrollCheckBox'


def handleEdgeScrollClicked(args):
	on = bool(args[0])
	_profile['edgeScroll'] = on
	_save()
	try:
		oc4host.choose_edge_scroll(on)
	except Exception:
		pass
	return 1


def _edgeScrollOn():
	try:
		return bool(oc4host.edge_scroll())
	except Exception:
		return _profile['edgeScroll']


def _graphicDefault(i):
	try:
		return bool(_cpe.CyGlobalContext().getGraphicOptionInfo(int(i)).getDefault())
	except Exception:
		return False


def _applyVolumes():
	"""The sounds as loud as the profile says (a kind without sound, or the master: none)."""
	gains = []
	for v in _VOLUMES:
		g = float(_profile['volume'][v]) / _VOLUME_STOPS
		if _profile['nosound'][v]:
			g = 0.0
		gains.append(g)
	try:
		oc4host.set_volumes(gains)
	except Exception:
		pass


class _UserProfile(object):
	"""CyUserProfile: the player's options (one profile, the host's file)."""

	def __getattr__(self, name):
		if name.startswith('__'):
			raise AttributeError(name)
		return lambda *args, **kwargs: 0

	# the profile itself
	def getProfileName(self): return _profile['name']
	def setProfileName(self, s): _profile['name'] = str(s)
	def getNumProfileFiles(self): return 1
	def getProfileFileName(self, i): return 'PROFILES\\%s.prf' % _profile['name']
	def loadProfileFileNames(self): pass
	def readFromFile(self, s): _load(); _applyVolumes(); _applyGraphics(); return True
	def writeToFile(self, s): _save(); return True
	def deleteProfileFile(self, s): return False
	def resetOptions(self, eTab):
		if eTab == _cpe.TabGroupTypes.TABGROUP_GRAPHICS:
			_profile['graphic'].clear()
			_applyGraphics()
		elif eTab == _cpe.TabGroupTypes.TABGROUP_AUDIO:
			for v in _VOLUMES:
				_profile['volume'][v], _profile['nosound'][v] = _VOLUME_STOPS, False
			_applyVolumes()
		elif eTab == _cpe.TabGroupTypes.TABGROUP_GAME:
			_profile['player'].clear()
		elif eTab == _cpe.TabGroupTypes.TABGROUP_CLOCK:
			_profile['clock'] = _profile['hours24'] = False

	# graphics
	def getGraphicOption(self, i): return _profile['graphic'].get(int(i), _graphicDefault(i))
	def setGraphicOption(self, i, b): _profile['graphic'][int(i)] = bool(b); _save(); _applyGraphics()
	def getGraphicsLevel(self): return _profile['graphicsLevel']
	def setGraphicsLevel(self, i): _profile['graphicsLevel'] = int(i)
	def getRenderQualityLevel(self): return _profile['renderQuality']
	def setRenderQualityLevel(self, i): _profile['renderQuality'] = int(i)
	def getGlobeViewRenderLevel(self): return _profile['globeView']
	def setGlobeViewRenderLevel(self, i): _profile['globeView'] = int(i)
	def getMovieQualityLevel(self): return _profile['movieQuality']
	def setMovieQualityLevel(self, i): _profile['movieQuality'] = int(i)
	def getMainMenu(self): return _profile['mainMenu']
	def setMainMenu(self, i): _profile['mainMenu'] = int(i)
	def getAntiAliasing(self): return _profile['antiAliasing']
	def setAntiAliasing(self, i): _profile['antiAliasing'] = int(i)
	def getAntiAliasingMaxMultiSamples(self): return 0
	# (the map keeps the window's pixels: the resolution's choices are the interface's, which
	# takes effect at once)
	def getResolution(self):
		scale = _profile['uiScale']
		scales = _scales()
		return scales.index(scale) if scale in scales else 0
	def setResolution(self, i):
		scales = _scales()
		scale = scales[int(i)] if 0 <= int(i) < len(scales) else 0
		if scale != _profile['uiScale']:
			_profile['uiScale'] = scale
			_save()
			oc4host.set_ui_scale(scale)
	def getResolutionMaxModes(self): return len(_scales())
	def getResolutionString(self, i):
		scales = _scales()
		scale = scales[int(i)] if 0 <= int(i) < len(scales) else 0
		w, h = oc4host.ui_size_at(scale)
		return '%dx%d (%s)' % (w, h, '%d%%' % scale if scale else 'Auto')

	# the player's options (the game's own when one is on: the active player's)
	def getPlayerOption(self, i):
		try:
			game = _cpe.CyGame()
			player = _cpe.CyGlobalContext().getPlayer(game.getActivePlayer())
			if player is not None and not player.isNone():
				return player.isOption(int(i))
		except Exception:
			pass
		return _profile['player'].get(int(i), False)
	def setPlayerOption(self, i, b): _profile['player'][int(i)] = bool(b)

	# sounds
	def getVolumeStops(self): return _VOLUME_STOPS
	def recalculateAudioSettings(self): _applyVolumes()
	def getMusicPath(self): return _profile['musicPath']
	def setMusicPath(self, s): _profile['musicPath'] = str(s)
	def musicPathDialogBox(self): pass
	def getSpeakerConfig(self): return _profile['speakers']
	def setSpeakerConfig(self, s): _profile['speakers'] = str(s)
	def getSpeakerConfigFromList(self, i): return ''
	def useVoice(self): return _profile['voice']
	def setUseVoice(self, b): _profile['voice'] = bool(b)
	def getNumCaptureDevices(self): return 0
	def getNumPlaybackDevices(self): return 0
	def getCaptureDeviceIndex(self): return 0
	def getPlaybackDeviceIndex(self): return 0
	def getCaptureDeviceDesc(self, i): return ''
	def getPlaybackDeviceDesc(self, i): return ''
	def getCaptureVolume(self): return 0
	def getPlaybackVolume(self): return 0
	def getMaxCaptureVolume(self): return 0
	def getMaxPlaybackVolume(self): return 0

	# the clock
	def isClockOn(self): return _profile['clock']
	def setClockOn(self, b): _profile['clock'] = bool(b)
	def is24Hours(self): return _profile['hours24']
	def set24Hours(self, b): _profile['hours24'] = bool(b)
	def wasClockJustTurnedOn(self): return False
	def setClockJustTurnedOn(self, b): pass

	# the map's toggles by the minimap, as they start
	def getGrid(self): return False
	def getMap(self): return False
	def getYields(self): return False
	def getScores(self): return True


def _volumeAccessors():
	for v in _VOLUMES:
		def get(self, v=v): return _profile['volume'][v]
		def set(self, i, v=v):
			_profile['volume'][v] = max(0, min(_VOLUME_STOPS, int(i)))
			_applyVolumes()
		def isNo(self, v=v): return _profile['nosound'][v]
		def setNo(self, b, v=v):
			_profile['nosound'][v] = bool(b)
			_applyVolumes()
		setattr(_UserProfile, 'get%sVolume' % v, get)
		setattr(_UserProfile, 'set%sVolume' % v, set)
		setattr(_UserProfile, 'is%sNoSound' % v, isNo)
		setattr(_UserProfile, 'set%sNoSound' % v, setNo)


_volumeAccessors()


# ---------------------------------------------------------------- the tab control

class _Node(object):
	__slots__ = ('id', 'kind', 'name', 'children', 'text', 'value', 'items', 'callback', 'flags', 'style', 'minimum', 'maximum', 'tip')

	def __init__(self, id, kind, name):
		self.id, self.kind, self.name = id, kind, name
		self.children, self.text, self.value, self.items, self.callback = [], u'', 0, (), None
		self.flags, self.style, self.minimum, self.maximum, self.tip = set(), '', 0, 0, u''


_state = {'ctrl': None, 'dirty': False}


def _text(s):
	try:
		return oc4screens._text(s)
	except Exception:
		return u'%s' % (s,)


class CyGTabCtrl(object):
	"""A window of tabs, its forms boxes of controls; at most one up (the options screen's)."""

	def __init__(self, szName='', bStatic=False, bCloseButton=False):
		self._title = _text(szName)
		self._nodes = {}   # id: node
		self._byName = {}  # name: the last node so named (some names are used again in other forms)
		self._tabs = []    # (form node, label)
		self._tab = 0
		self._next = 1
		self._shown = True
		self._size = (W, H)
		_state['ctrl'] = self
		_schedule()

	def __getattr__(self, name):
		if name.startswith('__'):
			raise AttributeError(name)
		return lambda *args, **kwargs: None

	# the window
	def setModal(self, i): pass
	def setSize(self, w, h):
		# (the window's size: the game's options 800 x 600, BUG's 950 x 715 for its 14 tabs)
		self._size = (max(400, int(w)), max(300, int(h)))
		_schedule()
	def setControlsExpanding(self, b): pass
	def setColumnLength(self, i): pass
	def destroy(self):
		self._shown = False
		if _state['ctrl'] is self:
			_state['ctrl'] = None
		oc4screens.reset(OPTIONS)
		oc4screens.hide(OPTIONS)

	def _node(self, kind, parent, name):
		n = _Node(self._next, kind, str(name))
		self._next += 1
		self._nodes[n.id] = n
		self._byName[n.name] = n
		p = self._byName.get(str(parent))
		if p is not None:
			p.children.append(n)
		_schedule()
		return n

	def _callback(self, n, module, function, arg):
		n.callback = (str(module), str(function), arg)

	def attachTabItem(self, szName, szLabel):
		n = _Node(self._next, 'form', str(szName))
		self._next += 1
		self._nodes[n.id] = n
		self._byName[n.name] = n
		self._tabs.append((n, _text(szLabel)))
		_schedule()

	def attachVBox(self, parent, name): self._node('vbox', parent, name)
	def attachHBox(self, parent, name):
		# (openciv4's own option, on the Game tab above the language)
		if str(parent) == 'GamePanelCenter' and str(name) == 'LangHBox':
			self.attachCheckBox(parent, WASD_PAN_CHECKBOX, u'Pan the map with W A S D (Ctrl+Shift+W turns it off)', 'oc4options', 'handleWasdPanClicked',
				WASD_PAN_CHECKBOX, _wasdPanOn())
			self.setToolTip(WASD_PAN_CHECKBOX, u'Holding W, A, S or D on its own moves the map, as the arrows do. Their own hotkeys '
				u'(S Sentry, A Automate, W next unit, D destroy) then need the unit\'s buttons; with Shift, Ctrl or Alt they are the '
				u'game\'s as ever, and in menus and screens they are letters.')
			self.attachCheckBox(parent, AUTO_END_TURN_CHECKBOX, u'End idle turns automatically (Ctrl+Shift+E)', 'oc4options', 'handleAutoEndTurnClicked',
				AUTO_END_TURN_CHECKBOX, _autoEndTurnOn())
			self.setToolTip(AUTO_END_TURN_CHECKBOX, u'When no unit needs orders and nothing is open, the turn ends after 2 seconds '
				u'even if you did nothing in it. Press a key or click while it waits to stay on that turn. Popups, the city '
				u'screen and other screens pause it. Pair with Automate Production and automated workers to let the game run.')
			self.attachCheckBox(parent, EDGE_SCROLL_CHECKBOX, u'Scroll the map at the screen edge', 'oc4options', 'handleEdgeScrollClicked',
				EDGE_SCROLL_CHECKBOX, _edgeScrollOn())
			self.setToolTip(EDGE_SCROLL_CHECKBOX, u'Moving the mouse to the edge of the screen or window moves the map that way. '
				u'Turn it off to pan only with the arrow keys (or W A S D) and the mouse.')
		self._node('hbox', parent, name)
	def attachPanel(self, parent, name): self._node('panel', parent, name)
	def attachScrollPanel(self, parent, name): self._node('vbox', parent, name)
	def attachSpacer(self, parent): self._node('spacer', parent, '')
	def attachHSeparator(self, parent, name): self._node('hsep', parent, name)
	def attachVSeparator(self, parent, name): self._node('vsep', parent, name)

	def attachLabel(self, parent, name, text):
		self._node('label', parent, name).text = _text(text)

	def attachImage(self, parent, name, path):
		self._node('image', parent, name).text = str(path)

	def attachCheckBox(self, parent, name, text, module, function, arg, bValue):
		n = self._node('checkbox', parent, name)
		n.text, n.value = _text(text), bool(bValue)
		self._callback(n, module, function, arg)

	def attachRadioButton(self, parent, name, text, module, function, arg, bValue):
		n = self._node('radio', parent, name)
		n.text, n.value = _text(text), bool(bValue)
		self._callback(n, module, function, arg)

	def attachButton(self, parent, name, text, module, function, arg):
		n = self._node('button', parent, name)
		n.text = _text(text)
		self._callback(n, module, function, arg)

	def attachEdit(self, parent, name, text, module, function, arg):
		n = self._node('edit', parent, name)
		n.text = _text(text)
		self._callback(n, module, function, arg)

	def attachDropDown(self, parent, name, desc, items, module, function, arg, iValue):
		n = self._node('dropdown', parent, name)
		n.items, n.value = tuple(_text(i) for i in items), int(iValue)
		self._callback(n, module, function, arg)

	def _slider(self, kind, parent, name, module, function, arg, iMin, iMax, iValue):
		n = self._node(kind, parent, name)
		n.minimum, n.maximum, n.value = int(iMin), int(iMax), int(iValue)
		self._callback(n, module, function, arg)

	def attachHSlider(self, parent, name, module, function, arg, iMin, iMax, iValue):
		self._slider('hslider', parent, name, module, function, arg, iMin, iMax, iValue)

	def attachVSlider(self, parent, name, module, function, arg, iMin, iMax, iValue):
		self._slider('vslider', parent, name, module, function, arg, iMin, iMax, iValue)

	# what the script sets and reads
	def setLayoutFlag(self, name, flag):
		n = self._byName.get(str(name))
		if n is not None:
			n.flags.add(str(flag))

	def setControlFlag(self, name, flag): self.setLayoutFlag(name, flag)

	def setStyle(self, name, style):
		n = self._byName.get(str(name))
		if n is not None:
			n.style = str(style)

	def setToolTip(self, name, text):
		n = self._byName.get(str(name))
		if n is not None:
			n.tip = _text(text)

	def setValue(self, name, value):
		n = self._byName.get(str(name))
		if n is not None:
			n.value = value
			_schedule()

	def getValue(self, name):
		n = self._byName.get(str(name))
		return n.value if n is not None else 0

	def setText(self, name, text):
		n = self._byName.get(str(name))
		if n is not None:
			n.text = _text(text)
			_schedule()

	def getText(self, name):
		n = self._byName.get(str(name))
		if n is None:
			return u''
		if n.kind == 'edit':
			# (as typed in)
			w = _screen()._s.get('OptW%d' % n.id)
			if w is not None:
				n.text = w.text
		return n.text

	def changeDropdownContents(self, name, items):
		n = self._byName.get(str(name))
		if n is not None:
			n.items = tuple(_text(i) for i in items)
			_schedule()

	def setFocus(self, *args): pass
	def setEnabled(self, *args): pass


def _schedule():
	_state['dirty'] = True
	oc4screens._changed()


def _screen():
	return oc4screens.CyGInterfaceScreen('OpenCiv4Options', OPTIONS)


# ---------------------------------------------------------------- laying it out

_ROW = {'label': 20, 'checkbox': 22, 'radio': 22, 'dropdown': 26, 'button': 28, 'edit': 26, 'hslider': 18, 'hsep': 10,
	'spacer': 8, 'image': 4, 'vsep': 0}
_GAP = 3
_PAD = 8


def _height(n, w):
	k = n.kind
	if k in _ROW:
		return _ROW[k]
	if k == 'vslider':
		return 4 * (n.maximum - n.minimum + 1) + 4
	if k == 'hbox':
		widths = _shares(n, w)
		return max([_height(c, cw) for c, cw in zip(n.children, widths)] or [0])
	pad = _PAD if k == 'panel' else 0
	inner = w - 2 * pad
	h = sum(_height(c, inner) for c in n.children) + _GAP * max(0, len(n.children) - 1)
	return h + 2 * pad


def _natural(n):
	"""A label's own width (CF_LABEL_DEFAULTSIZE, as BUG asks for its pull downs' titles)."""
	try:
		return oc4screens._textWidth(u'<font=2>' + n.text + u'</font>', _cpe.FontTypes.GAME_FONT) + 8
	except Exception:
		return 8 * len(n.text) + 8


def _shares(n, w):
	"""The widths of an horizontal box's children: separators and spacers narrow, labels of their
	default size (CF_LABEL_DEFAULTSIZE) as wide as their text (up to half the box), the rest alike."""
	fixed = dict((c.id, 12) for c in n.children if c.kind in ('vsep', 'spacer'))
	if len(n.children) > 1:
		for c in n.children:
			if c.kind == 'label' and 'CF_LABEL_DEFAULTSIZE' in c.flags:
				fixed[c.id] = min(_natural(c), w / 2)
	others = [c for c in n.children if c.id not in fixed]
	rest = max(0, w - sum(fixed.values()) - _GAP * max(0, len(n.children) - 1))
	each = rest / max(1, len(others))
	return [fixed.get(c.id, each) for c in n.children]


def _place(screen, n, x, y, w, h):
	k = n.kind
	name = 'OptW%d' % n.id
	general = _cpe.WidgetTypes.WIDGET_GENERAL
	font = _cpe.FontTypes.GAME_FONT
	if k == 'hbox':
		cx = x
		for c, cw in zip(n.children, _shares(n, w)):
			_place(screen, c, cx, y, cw, _height(c, cw))
			cx += cw + _GAP
		return
	if k in ('vbox', 'form', 'panel'):
		pad = _PAD if k == 'panel' else 0
		if k == 'panel':
			screen.addPanel(name, u'', u'', True, False, x, y, w, h, _cpe.PanelStyles.PANEL_STYLE_IN)
		cy = y + pad
		for c in n.children:
			ch = _height(c, w - 2 * pad)
			_place(screen, c, x + pad, cy, w - 2 * pad, ch)
			cy += ch + _GAP
		return
	if k == 'label':
		screen.setLabel(name, 'Background', u'<font=2>' + n.text + u'</font>', 1 << 0, x, y + 2, 0, font, general, -1, -1)
	elif k in ('checkbox', 'radio'):
		screen.addCheckBoxGFC(name, '', '', x, y, 20, 20, general, -1, -1, '')
		w_ = screen._s.get(name)
		if w_ is not None:
			w_.state = bool(n.value)
		screen.setText(name + 'L', 'Background', u'<font=2>' + n.text + u'</font>', 1 << 0, x + 26, y + 2, 0, font, general, -1, -1)
		if n.tip:
			w_ = screen._s.get(name)
			if w_ is not None:
				w_.help = n.tip
	elif k == 'button':
		screen.setButtonGFC(name, n.text, '', x, y, min(w, 220), 26, general, -1, -1, _cpe.ButtonStyles.BUTTON_STYLE_STANDARD)
	elif k == 'edit':
		screen.addEditBoxGFC(name, x, y, min(w, 260), 24, general, -1, -1, font)
		screen.setEditBoxString(name, n.text)
	elif k == 'dropdown':
		dw = min(w, 260)
		if 'LAYOUT_RIGHT' in n.flags:
			x += w - dw
		screen.addDropDownBoxGFC(name, x, y, dw, general, -1, -1, font)
		for i, item in enumerate(n.items):
			screen.addPullDownString(name, item, i, i, i == int(n.value))
	elif k in ('hslider', 'vslider'):
		# (its stops, each a little button: the slider set to the one clicked)
		stops = n.maximum - n.minimum + 1
		for s in range(stops):
			value = n.minimum + s
			if k == 'hslider':
				size = max(3, min(10, (w - 4) / max(1, stops) - 1))
				bx, by, bw, bh = x + s * (size + 1), y + 2, size, 14
				on = value <= int(n.value)
			else:
				bx, by, bw, bh = x + 8, y + 2 + s * 4, 40, 3
				on = value >= int(n.value)
			bname = 'OptSlider%d' % (n.id * 1000 + s)
			screen.addCheckBoxGFC(bname, '', '', bx, by, bw, bh, general, -1, -1, '')
			b = screen._s.get(bname)
			if b is not None:
				b.state = on
	elif k == 'hsep':
		screen.addPanel(name, u'', u'', True, False, x, y + 4, w, 2, _cpe.PanelStyles.PANEL_STYLE_MAIN)


def resized():
	"""Another size: the window laid out again for it, as it would not be otherwise."""
	if _state['ctrl'] is not None:
		_schedule()


def relayout():
	"""(before the screens are drawn: the tab control laid out again if it changed)"""
	if not _state['dirty']:
		return
	_state['dirty'] = False
	ctrl = _state['ctrl']
	oc4screens.reset(OPTIONS)
	if ctrl is None or not ctrl._shown or not ctrl._tabs:
		oc4screens.hide(OPTIONS)
		return
	screen = _screen()
	RW, RH = screen.getXResolution(), screen.getYResolution()
	screen.setDimensions(0, 0, RW, RH)
	screen.showScreen(_cpe.PopupStates.POPUPSTATE_IMMEDIATE, False)
	screen.setCloseOnEscape(True)
	# (the size the script asked for, setSize, kept on the display)
	w, h = min(ctrl._size[0], RW), min(ctrl._size[1], RH)
	x0, y0 = (RW - w) / 2, max(0, (RH - h) / 2)
	screen.addPanel('OptBack', u'', u'', True, False, x0, y0, w, h, _cpe.PanelStyles.PANEL_STYLE_MAIN)
	screen.setLabel('OptTitle', 'Background', u'<font=3b>' + ctrl._title + u'</font>', 1 << 2, x0 + w / 2, y0 + 10, 0,
		_cpe.FontTypes.TITLE_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
	# the tabs: each as wide as its label, in as many rows as they need (BUG's 14 do not fit one),
	# each row stretched across the window
	y = _tabRows(screen, ctrl, x0 + 10, y0 + 38, w - 20)
	form = ctrl._tabs[min(ctrl._tab, len(ctrl._tabs) - 1)][0]
	_place(screen, form, x0 + 14, y + 6, w - 28, h - (y + 6 - y0) - 14)


_TAB_HEIGHT = 28


def _tabWidth(label):
	try:
		return oc4screens._textWidth(label, _cpe.FontTypes.GAME_FONT) + 24
	except Exception:
		return 9 * len(label) + 24


def _tabRows(screen, ctrl, x, y, width):
	"""Lays the tab buttons out in rows; returns where the row under the last one would start."""
	n = len(ctrl._tabs)
	tw = width / max(1, n)
	if all(_tabWidth(label) <= tw - 4 for form, label in ctrl._tabs):
		# (all fit alike in one row: one row of equal tabs, the game's own options screen)
		for i, (form, label) in enumerate(ctrl._tabs):
			screen.setButtonGFC('OptTab%d' % i, label, '', x + i * tw, y, tw - 4, _TAB_HEIGHT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1,
				_cpe.ButtonStyles.BUTTON_STYLE_STANDARD)
		return y + _TAB_HEIGHT + 4
	rows, row, used = [], [], 0
	for i, (form, label) in enumerate(ctrl._tabs):
		tw = _tabWidth(label)
		if row and used + tw > width:
			rows.append(row)
			row, used = [], 0
		row.append((i, label, tw))
		used += tw + 4
	if row:
		rows.append(row)
	for r in rows:
		natural = sum(tw for i, label, tw in r) + 4 * (len(r) - 1)
		extra = (width - natural) / max(1, len(r)) if len(rows) == 1 or r is not rows[-1] else 0
		cx = x
		for i, label, tw in r:
			bw = tw + extra
			screen.setButtonGFC('OptTab%d' % i, label, '', cx, y, bw, _TAB_HEIGHT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1,
				_cpe.ButtonStyles.BUTTON_STYLE_STANDARD)
			cx += bw + 4
		y += _TAB_HEIGHT + 4
	return y


def handleInput(args):
	notify, data, ident, function = args[0], args[1], args[3], args[5]
	ctrl = _state['ctrl']
	if ctrl is None:
		return 0
	if function == 'OptTab' and notify == 0:
		ctrl._tab = ident
		_schedule()
		return 1
	if function == 'OptSlider' and notify == 0:
		n = ctrl._nodes.get(ident / 1000)
		if n is not None:
			n.value = n.minimum + ident % 1000
			_call(n, (int(n.value), n.callback[2] if n.callback else n.name))
			_schedule()
		return 1
	if function != 'OptW':
		return 0
	n = ctrl._nodes.get(ident)
	if n is None:
		return 0
	if n.kind in ('checkbox', 'radio') and notify == 0:
		w = _screen()._s.get('OptW%d' % n.id)
		n.value = bool(w.state) if w is not None else not n.value
		if n.kind == 'radio':
			n.value = True
		_call(n, (n.value, n.callback[2] if n.callback else n.name))
		_schedule()
	elif n.kind == 'button' and notify == 0:
		_call(n, n.callback[2] if n.callback else n.name)
		_schedule()
	elif n.kind == 'dropdown' and notify == 11:
		n.value = int(data)
		_call(n, (int(data), n.callback[2] if n.callback else n.name))
		_schedule()
	return 1


def _call(n, args):
	if not n.callback:
		return
	module, function = n.callback[0], n.callback[1]
	try:
		m = __import__(module)
		getattr(m, function)(args)
	except Exception:
		import traceback
		traceback.print_exc()


# ---------------------------------------------------------------- in place

# (the engine's alarm clock, as the options screen sets it: off)
_alarm = {'on': False, 'hour': 0, 'min': 0}
_cpe.isAlarmOn = lambda: _alarm['on']
_cpe.getAlarmHour = lambda: _alarm['hour']
_cpe.getAlarmMin = lambda: _alarm['min']
_cpe.getAlarmHourLeft = lambda: _alarm['hour']
_cpe.getAlarmMinLeft = lambda: _alarm['min']
def _toggleAlarm(bOn, iHour=0, iMin=0):
	_alarm['on'], _alarm['hour'], _alarm['min'] = bool(bOn), int(iHour), int(iMin)
_cpe.toggleAlarm = _toggleAlarm
_cpe.CyGTabCtrl = CyGTabCtrl
_cpe.CyUserProfile = _UserProfile
oc4screens.beforeDisplay.append(relayout)
_load()
_applyVolumes()
_applyGraphics()
