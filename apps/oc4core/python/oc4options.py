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


# The profile's form: 2 since the player options are the player's choices (before, the options
# screen wrote every player option it had shown, those it had never read as off).
_VERSION = 2
# (the options CIV4PlayerOptionInfos.xml has on: advisor pop-ups, Sid's tips, Wait at End of Turn)
_DEFAULTS_ON = (0, 1, 2)


def _load():
	path = _path()
	if not path:
		return
	try:
		f = open(path, 'r')
	except IOError:
		return
	version = 1
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
				elif parts[0] == 'version':
					version = int(value)
			except (ValueError, IndexError):
				pass
	finally:
		f.close()
	# A profile the old options screen wrote: an option that is on by default and off there was
	# most likely never chosen (the screen showed it off), so it is the default again, once. (With
	# Wait at End of Turn off the game core ends a turn by itself after a unit moved.)
	if version < _VERSION:
		for i in _DEFAULTS_ON:
			if _profile['player'].get(i) is False:
				del _profile['player'][i]


def _save():
	path = _path()
	if not path:
		return
	lines = ['version=%d' % _VERSION]
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
		_applyPlayerOptions()
		oc4host.set_wasd_pan(_profile['wasdPan'])
		oc4host.set_auto_end_turn(_profile['autoEndTurn'])
		oc4host.set_edge_scroll(_profile['edgeScroll'])
	except Exception:
		pass


# W A S D panning (openciv4's own option, no game's): its check box on the options' Game tab,
# above the language; while it is on, Ctrl+Shift+W in the window turns it off (the host calls setWasdPan).
WASD_PAN_CHECKBOX = 'Oc4WasdPanCheckBox'
OC4_FORM = 'Oc4Form' # (the options' OpenCiv4 tab: openciv4's own options are there now, not on the Game tab)


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
# the others on the OpenCiv4 tab.
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


def _applyPlayerOptions():
	"""The player options the profile has, to the host: the game core sends them to the active
	player at the start of every game, new or loaded, as the original's profile (a save's own are
	overwritten)."""
	try:
		oc4host.reset_player_options()
		for i, on in _profile['player'].items():
			oc4host.set_player_option(int(i), bool(on))
	except Exception:
		pass


def _syncPlayerOptions():
	"""The player options as the host has them now (a choice on the options screen goes to the
	game core, CyMessageControl().sendPlayerOption, and into the profile there), for the file."""
	try:
		for i in range(64):
			state = oc4host.player_option_state(i)
			if state:
				_profile['player'][i] = state == 2
	except Exception:
		pass


def _playerDefault(i):
	try:
		return bool(_cpe.CyGlobalContext().getPlayerOptionsInfoByIndex(int(i)).getDefault())
	except Exception:
		return False


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
			# (the XML's defaults, to the profile and to the player in a game)
			_profile['player'].clear()
			_applyPlayerOptions()
			try:
				gc = _cpe.CyGlobalContext()
				for i in range(gc.getNumPlayerOptionInfos()):
					_cpe.CyMessageControl().sendPlayerOption(i, _playerDefault(i))
			except Exception:
				pass
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

	# the player's options (the profile's, the same in every game: the game core sends them to the
	# active player when a game starts or is loaded, and a choice sent from the screen comes back here)
	def getPlayerOption(self, i):
		try:
			state = oc4host.player_option_state(int(i))
			if state:
				return state == 2
		except Exception:
			pass
		return _profile['player'].get(int(i), _playerDefault(i))
	def setPlayerOption(self, i, b):
		_profile['player'][int(i)] = bool(b)
		try:
			oc4host.set_player_option(int(i), bool(b))
		except Exception:
			pass

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
		# (the game's own options screen: openciv4's own options on a tab of their own, after Other)
		if str(szName) == 'OtherForm':
			self._attachOpenCiv4Tab()

	def _attachOpenCiv4Tab(self):
		"""OpenCiv4's options, none of the original's: W A S D panning, ending idle turns, edge
		scrolling (kept in the profile as wasdPan, autoEndTurn, edgeScroll)."""
		self.attachTabItem(OC4_FORM, u'OpenCiv4')
		self.attachVBox(OC4_FORM, 'Oc4VBox')
		parent = 'Oc4Panel'
		self.attachPanel('Oc4VBox', parent)
		self.setStyle(parent, 'Panel_Tan15_Style')
		self.setLayoutFlag(parent, 'LAYOUT_SIZE_HEXPANDING')
		self.setLayoutFlag(parent, 'LAYOUT_SIZE_VEXPANDING')
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

	def attachVBox(self, parent, name): self._node('vbox', parent, name)
	def attachHBox(self, parent, name):
		self._node('hbox', parent, name)
	def attachPanel(self, parent, name): self._node('panel', parent, name)
	def attachScrollPanel(self, parent, name): self._node('scroll', parent, name)
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

	def setActiveTab(self, i):
		if 0 <= int(i) < len(self._tabs):
			self._tab = int(i)
			_schedule()

	def getActiveTab(self): return self._tab
	def setEnabled(self, *args): pass


def _schedule():
	_state['dirty'] = True
	oc4screens._changed()


def _screen():
	return oc4screens.CyGInterfaceScreen('OpenCiv4Options', OPTIONS)


# ---------------------------------------------------------------- laying it out

# The boxes laid out as the original's GFC does it, measured on its options screen: a control's
# default height is 31 (the theme's GFC_Control_Size Default, GSize(64, 31)), so a check box's row is
# 28 and the gap 3; a box's children are as wide as they need, and what is left goes to those that
# expand (LAYOUT_SIZE_HEXPANDING) or, in a column, to its spacers (the Language row at the panel's
# foot); a row with LAYOUT_HCENTER is centred (Restore Defaults and Exit).
_ROW = {'label': 28, 'checkbox': 28, 'radio': 28, 'dropdown': 28, 'button': 31, 'edit': 26, 'hslider': 18, 'hsep': 10,
	'spacer': 8, 'image': 4, 'vsep': 0}
_GAP = 3
_PAD = 8
_SCROLL_PAD = 4 # (a scroll panel's inside, left and right)
_FONT = u'<font=3>' # (the controls' text: the theme's SF_CtrlTheme_Civ4_Control_Font_Size3_Normal, 16 px)
_BOX = 20 # (a check box's picture; its mark is 14 across inside it)
_BOX_TEXT = 24 # (from the box's left to its text's)


def _width(text):
	try:
		return oc4screens._textWidth(_FONT + text + u'</font>', _cpe.FontTypes.GAME_FONT)
	except Exception:
		return 8 * len(text)


def _pad(n):
	"""A box's inside: a panel's 8 all round, a scroll panel's 4 at the sides."""
	if n.kind == 'panel':
		return _PAD, _PAD
	if n.kind == 'scroll':
		return _SCROLL_PAD, 0
	return 0, 0


def _hgrow(n):
	return 'LAYOUT_SIZE_HEXPANDING' in n.flags or 'LAYOUT_SIZE_HPREFERREDEXPANDING' in n.flags


def _vgrow(n):
	return 'LAYOUT_SIZE_VEXPANDING' in n.flags or 'LAYOUT_SIZE_VPREFERREDEXPANDING' in n.flags


def _natural(n):
	"""How wide a control is of itself (None: as wide as it is given)."""
	k = n.kind
	if k == 'label':
		return _width(n.text) + 8
	if k in ('checkbox', 'radio'):
		return _BOX_TEXT + _width(n.text) + 8
	if k == 'button':
		return max(59, _width(n.text) + 20)
	if k == 'dropdown':
		return max([_width(i) for i in n.items] or [40]) + 36
	if k in ('spacer', 'vsep'):
		return 12
	if k in ('vbox', 'scroll', 'panel'):
		widths = [_natural(c) for c in n.children]
		if None in widths:
			return None
		return max(widths or [0]) + 2 * _pad(n)[0]
	if k == 'hbox':
		widths = [_natural(c) for c in n.children]
		if None in widths:
			return None
		return sum(widths) + _GAP * max(0, len(widths) - 1)
	return None


def _height(n, w):
	k = n.kind
	if k in _ROW:
		return _ROW[k]
	if k == 'vslider':
		return 4 * (n.maximum - n.minimum + 1) + 4
	if k == 'hbox':
		start, widths = _shares(n, w)
		return max([_height(c, cw) for c, cw in zip(n.children, widths)] or [0])
	if k == 'panel' and not n.children:
		return 0 # (an empty one: the resolution's screen picture, none here)
	px, py = _pad(n)
	inner = w - 2 * px
	h = sum(_height(c, inner) for c in n.children) + _GAP * max(0, len(n.children) - 1)
	return h + 2 * py


def _shares(n, w):
	"""Where an horizontal box's children start (from its left) and their widths: each as wide
	as it needs, the rest shared by those that expand, else the row centred (LAYOUT_HCENTER) or
	left as it is; when one has no width of its own, separators and spacers narrow, labels of their
	default size (CF_LABEL_DEFAULTSIZE) as wide as their text (up to half the box), the rest alike."""
	gaps = _GAP * max(0, len(n.children) - 1)
	natural = [_natural(c) for c in n.children]
	if n.children and None not in natural:
		extra = max(0, w - sum(natural) - gaps)
		grow = [i for i, c in enumerate(n.children) if _hgrow(c)]
		if grow:
			for i in grow:
				natural[i] += extra / len(grow)
			return 0, natural
		return (extra / 2 if 'LAYOUT_HCENTER' in n.flags else 0), natural
	fixed = dict((c.id, 12) for c in n.children if c.kind in ('vsep', 'spacer'))
	if len(n.children) > 1:
		for c in n.children:
			if c.kind == 'label' and 'CF_LABEL_DEFAULTSIZE' in c.flags:
				fixed[c.id] = min(_width(c.text) + 8, w / 2)
	others = [c for c in n.children if c.id not in fixed]
	rest = max(0, w - sum(fixed.values()) - gaps)
	each = rest / max(1, len(others))
	return 0, [fixed.get(c.id, each) for c in n.children]


def _place(screen, n, x, y, w, h):
	k = n.kind
	name = 'OptW%d' % n.id
	general = _cpe.WidgetTypes.WIDGET_GENERAL
	font = _cpe.FontTypes.GAME_FONT
	if k == 'hbox':
		start, widths = _shares(n, w)
		cx = x + start
		for c, cw in zip(n.children, widths):
			_place(screen, c, cx, y, cw, h if _vgrow(c) else _height(c, cw))
			cx += cw + _GAP
		return
	if k in ('vbox', 'form', 'panel', 'scroll'):
		px, py = _pad(n)
		if k == 'panel' and h > 0:
			screen.addPanel(name, u'', u'', True, False, x, y, w, h, _cpe.PanelStyles.PANEL_STYLE_IN)
			if n.style:
				screen.setStyle(name, n.style)
		inner = w - 2 * px
		heights = [_height(c, inner) for c in n.children]
		# (the height left over: to the children that expand downwards, else to the spacers; a
		# tab's form fills the window)
		left = h - 2 * py - sum(heights) - _GAP * max(0, len(heights) - 1)
		if left > 0 and n.children:
			grow = [i for i, c in enumerate(n.children) if _vgrow(c)] or \
				[i for i, c in enumerate(n.children) if c.kind == 'spacer'] or ([0] if k == 'form' else [])
			for i in grow:
				heights[i] += left / len(grow)
		cy = y + py
		for c, ch in zip(n.children, heights):
			_place(screen, c, x + px, cy, inner, ch)
			cy += ch + _GAP
		return
	if k == 'label':
		screen.setLabel(name, 'Background', _FONT + n.text + u'</font>', 1 << 0, x, y + 4, 0, font, general, -1, -1)
	elif k in ('checkbox', 'radio'):
		# (the theme's check box, as the custom game screen's: GFC_Control_StandardCheckBox_Style)
		screen.addCheckBoxGFC(name, '', '', x, y + 4, _BOX, _BOX, general, -1, -1, _cpe.ButtonStyles.BUTTON_STYLE_STANDARD)
		screen.setStyle(name, 'GFC_Control_StandardCheckBox_Style')
		w_ = screen._s.get(name)
		if w_ is not None:
			w_.state = bool(n.value)
		screen.setText(name + 'L', 'Background', _FONT + n.text + u'</font>', 1 << 0, x + _BOX_TEXT, y + 4, 0, font, general, -1, -1)
		if n.tip:
			w_ = screen._s.get(name)
			if w_ is not None:
				w_.help = n.tip
	elif k == 'button':
		screen.setButtonGFC(name, n.text, '', x, y + 4, w, 25, general, -1, -1, _cpe.ButtonStyles.BUTTON_STYLE_STANDARD)
	elif k == 'edit':
		screen.addEditBoxGFC(name, x, y, min(w, 260), 24, general, -1, -1, font)
		screen.setEditBoxString(name, n.text)
	elif k == 'dropdown':
		dw = min(w, 260)
		if 'LAYOUT_RIGHT' in n.flags:
			x += w - dw
		screen.addDropDownBoxGFC(name, x, y + 1, dw, general, -1, -1, font)
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
	# (the window as the original's: the theme's standard window under its title bar, the title on
	# the bar at the left, the tabs under it as wide as their words, then the tab's form)
	for name, top, height, style in (('OptBack', _TITLE_H, h - _TITLE_H, _WINDOW_INNER), ('OptFrame', _TITLE_H, h - _TITLE_H, _WINDOW_FRAME),
			('OptTitleBar', 0, _TITLE_H, _WINDOW_TITLE)):
		screen.addPanel(name, u'', u'', True, False, x0, y0 + top, w, height, _cpe.PanelStyles.PANEL_STYLE_MAIN)
		screen.setStyle(name, style)
	screen.setLabel('OptTitle', 'Background', u'<font=3b>' + _DARK + ctrl._title + u'</color></font>', 1 << 0, x0 + 12, y0 + 3, 0,
		_cpe.FontTypes.TITLE_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
	y = _tabRows(screen, ctrl, x0 + 14, y0 + _TAB_Y, w - 28)
	form = ctrl._tabs[min(ctrl._tab, len(ctrl._tabs) - 1)][0]
	_place(screen, form, x0 + 30, y + 17, w - 60, h - (y + 17 - y0) - 30)


# The theme's window, title bar and tabs (as oc4wb draws the WorldBuilder's tool window).
_WINDOW_INNER = '@GFC_Control_DefaultWindow_Style|GFC_CtrlTheme_Bitmap_Control_Window_Skin_Inner_Default'
_WINDOW_TITLE = '@|SF_CtrlTheme_Civ4_Control_Window_Skin_Standard_TitleBar_Full_Default'
_WINDOW_FRAME = '@SF_CtrlTheme_Civ4_Control_Window_NoTitleBar_InFrameBorder_Normal_Style|GFC_CtrlTheme_Bitmap_Control_Border_Skin_Full_Default'
_TAB_SKIN = '@GFC_CtrlTheme_Bitmap_Control_Tab_SkinStyle_Top_Begin_BaseLevel_Single|GFC_CtrlTheme_Bitmap_Control_Tab_Skin_Full_'
_TAB_BASELINE = '@GFC_CtrlTheme_Bitmap_Control_Tab_SkinStyle_Top_Begin_BaseLevel_Single|GFC_CtrlTheme_Bitmap_Control_Tab_Skin_Baseline_Default'
_DARK = u'<color=40,28,16>' # (the title's and the tabs' words: the theme's dark brown)
_TITLE_H = 26
_TAB_Y = 39
_TAB_HEIGHT = 28


def _tabWidth(label):
	return _width(label) + 20


def _tabRows(screen, ctrl, x, y, width):
	"""Lays the tabs out in rows, each as wide as its word, from the left (BUG's 14 need more than
	one row); returns where the row under the last one would start."""
	rows, row, used = [], [], 0
	for i, (form, label) in enumerate(ctrl._tabs):
		tw = _tabWidth(label)
		if row and used + tw > width:
			rows.append(row)
			row, used = [], 0
		row.append((i, label, tw))
		used += tw + 2
	if row:
		rows.append(row)
	for r in rows:
		cx = x
		for i, label, tw in r:
			back = 'OptTabBack%d' % i
			screen.addPanel(back, u'', u'', True, False, cx, y, tw, _TAB_HEIGHT, _cpe.PanelStyles.PANEL_STYLE_MAIN)
			screen.setStyle(back, _TAB_SKIN + ('Select' if i == ctrl._tab else 'Default'))
			screen.setText('OptTab%d' % i, 'Background', _FONT + _DARK + label + u'</color></font>', 1 << 2, cx + tw / 2, y + 5, 0,
				_cpe.FontTypes.GAME_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
			cx += tw + 2
		y += _TAB_HEIGHT
	screen.addPanel('OptTabBaseline', u'', u'', True, False, x, y - 2, width, 7, _cpe.PanelStyles.PANEL_STYLE_MAIN)
	screen.setStyle('OptTabBaseline', _TAB_BASELINE)
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
	# (every choice kept at once, as the original writes its profile: a player option too)
	_syncPlayerOptions()
	_save()


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
