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
## names (Documents\My Games\openciv4\profile.ini). It also keeps the map's toggles by the minimap
## that the original keeps in its profile (Profiles\<name>.pfl, written at each click): the plot
## grid, the bare map, the yields and the scores (grid=, map=, yields=, scores=); every game, new,
## loaded or after a restart, starts with them as the profile has them. The unit and resource
## icon layers are not kept (nor in the original).

import os
import time
import CvPythonExtensions as _cpe
import oc4host
import oc4screens

OPTIONS = 31002 # its python file id (none of the game's screens has it)
W, H = 800, 600

# ---------------------------------------------------------------- the profile

_VOLUME_STOPS = 20
# The speaker setups the original offers (Miles': its names, the first four by text key, as the
# original's getSpeakerConfigFromList gives them). On macOS the sound goes out as the system has
# it set; Mono makes both sides the same.
_SPEAKERS = ('TXT_KEY_OPTION_SOUND_MONO', 'TXT_KEY_OPTION_SOUND_STEREO', 'TXT_KEY_OPTION_SOUND_SYSTEM_CONFIG', 'TXT_KEY_OPTION_SOUND_HEADPHONES',
	'Dolby Surround', 'SRS Circle Surround', '4.0 Discrete', '5.1 Discrete', '6.1 Discrete', '7.1 Discrete', '8.1 Discrete', 'DirectSound 3D',
	'EAX 2.0', 'EAX Advanced HD 3.0', 'EAX Advanced HD 4.0')
_SPEAKERS_DEFAULT = 'TXT_KEY_OPTION_SOUND_SYSTEM_CONFIG'
# The voice chat's controls: the original's is GameSpy's multiplayer voice, which OpenCiv4 has not;
# they show as the original's, the system's own devices in them, but cannot be used.
_VOICE_CONTROLS = ('VoiceChatCheckbox', 'CaptureDeviceDropdownBox', 'PlaybackDeviceDropdownBox', 'CaptureVolumeSlider', 'PlaybackVolumeSlider')
_VOICE_TIP = u'OpenCiv4 does not implement voice chat.'
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

# CivilizationIV.ini's settings the original's options screen has not, on the OpenCiv4 tab with
# the ini's defaults: NoTechSplash, NoEventMessages, AutoSaveInterval, MaxAutoSaves (and
# openciv4's own autosaves), FIELD_OF_VIEW (0: GlobalDefines.xml's), SetMaxFrameRate (0: none).
_INI_FLAGS = ('noTechSplash', 'noEventMessages', 'openciv4AutoSaves')
_INI_NUMBERS = (('autoSaveInterval', 4), ('autoSavesKept', 5), ('fieldOfView', 0), ('maxFrameRate', 0))


def _fresh():
	"""A new profile's options."""
	fresh = {
		'name': 'openciv4',
		'volume': dict((v, _VOLUME_STOPS) for v in _VOLUMES),
		'nosound': dict((v, False) for v in _VOLUMES),
		'graphic': {},
		'player': {},
		'clock': False, 'hours24': False,
		'graphicsLevel': 0, 'renderQuality': 0, 'globeView': 0, 'movieQuality': 0, 'mainMenu': 0, 'antiAliasing': 0, 'uiScale': 0,
		'speakers': '', 'musicPath': '', 'voice': False, 'captureVolume': 67, 'playbackVolume': 67,
		'wasdPan': False, 'autoEndTurn': False, 'edgeScroll': True, 'cityOneClick': False, 'keepAttacker': False, 'quickEnemyMoves': False,
		'grid': False, 'map': False, 'yields': False, 'scores': True,
	}
	for k in _INI_FLAGS:
		fresh[k] = False
	for k, v in _INI_NUMBERS:
		fresh[k] = v
	return fresh

_profile = _fresh()
_TOGGLES = ('grid', 'map', 'yields', 'scores') # (CyUserProfile's getGrid, getMap, getYields, getScores)


def _path():
	try:
		return oc4host.profile_path()
	except Exception:
		return ''


try:
	_unicode = unicode
except NameError: # (Python 3, as the tests run it: str is text)
	_unicode = None


def _utf8(s):
	"""Text as UTF-8 bytes, as the profile keeps it (Python 2's str(u'...') fails on what isn't ASCII)."""
	return s.encode('utf-8') if _unicode is not None and isinstance(s, _unicode) else str(s)


def _fs(path):
	"""A path as the file system calls take it: unicode, so that Windows opens it by its wide name,
	not in the system's code page (bytes that aren't UTF-8 are left as they are)."""
	if _unicode is None or isinstance(path, _unicode):
		return path
	try:
		return path.decode('utf-8')
	except UnicodeDecodeError:
		return path


def _bytes(text):
	"""Text as the files keep it: UTF-8, whatever the system's code page."""
	return text if _unicode is not None and not isinstance(text, _unicode) else text.encode('utf-8')


def _textLine(line):
	"""A line read from a file (bytes) as the profile keeps text: Python 2's UTF-8 str as it is,
	Python 3's str decoded."""
	return line if _unicode is not None else line.decode('utf-8', 'replace')


def _write(path, text):
	"""Writes the file whole or not at all: to a file beside it first, then over it (a crash while
	writing leaves the old file). In UTF-8, not the system's code page."""
	path = _fs(path)
	tmp = path + '.tmp'
	f = open(tmp, 'wb')
	try:
		f.write(_bytes(text))
	finally:
		f.close()
	try:
		os.rename(tmp, path)
	except OSError:
		# (Windows: no rename over a file that is there)
		os.remove(path)
		os.rename(tmp, path)


# The profile's form: 2 since the player options are the player's choices (before, the options
# screen wrote every player option it had shown, those it had never read as off).
_VERSION = 2
# (the options CIV4PlayerOptionInfos.xml has on: advisor pop-ups, Sid's tips, Wait at End of Turn)
_DEFAULTS_ON = (0, 1, 2)


# ---------------------------------------------------------------- the profiles

# As the original keeps its profiles, one file each (Profiles\<name>.pfl) and the one loaded at
# start named by CivilizationIV.ini's UserProfile: Profiles/<name>.ini beside profile.ini, each
# with the options the original keeps in a profile (the Game, Graphics, Audio and Other tabs' and
# the minimap's toggles). profile.ini keeps the rest, the same whichever profile is loaded:
# UserProfile=<name> and OpenCiv4's own options (the keys in _GLOBAL; the ini's settings too). A
# profile.ini from before (every option in it) is the profile "openciv4"'s first file.
_GLOBAL = ('UserProfile', 'wasdPan', 'autoEndTurn', 'edgeScroll', 'cityOneClick', 'keepAttacker', 'quickEnemyMoves') + _INI_FLAGS + tuple(k for k, v in _INI_NUMBERS)
_DEFAULT_NAME = 'openciv4'
_BAD = '\\/:*?"<>|'


def _isGlobal(key):
	return key.split('.')[0] in _GLOBAL


def _profilesDir():
	path = _fs(_path())
	return os.path.join(os.path.dirname(path), u'Profiles') if path else u''


def _fileName(name):
	"""The profile's file (a name the file system cannot have: None)."""
	name = _utf8(name).strip()
	if not name or name in ('.', '..') or [c for c in name if c in _BAD or ord(c) < 32]:
		return None
	folder = _profilesDir()
	return os.path.join(folder, _fs(name) + u'.ini') if folder else None


def _profileNames():
	"""The profiles there are (their files'), sorted as the original lists them."""
	folder = _profilesDir()
	try:
		names = [_utf8(f[:-4]) for f in os.listdir(folder) if f.lower().endswith('.ini') and os.path.isfile(os.path.join(folder, f))]
	except OSError:
		names = []
	return sorted(names, key=lambda n: n.lower())


def _resetProfile():
	"""The profile's options as a new profile has them (the global ones kept)."""
	keep = dict((k, _profile[k]) for k in _GLOBAL if k in _profile)
	_profile.clear()
	_profile.update(_fresh())
	_profile.update(keep)


def _load():
	"""At start: profile.ini, then the profile it names (an older profile.ini's own options over it)."""
	path = _path()
	if not path:
		return
	legacy = _read(path, _isGlobal, True)
	name = _profile.get('UserProfile') or legacy.get('name') or _DEFAULT_NAME
	_profile['name'] = name
	own = _fileName(name)
	if own and os.path.isfile(own):
		_read(own, lambda key: not _isGlobal(key))
		_profile['name'] = name
	if legacy:
		_read(path, lambda key: not _isGlobal(key))
		_profile['name'] = name
		_save()


def _read(path, which, others=False):
	"""Reads the options in the file for which which(key) holds; with others, returns the rest's
	(key: value) without taking them."""
	rest = {}
	try:
		f = open(_fs(path), 'rb')
	except IOError:
		return rest
	version = 1
	player = False
	try:
		for line in f:
			line = _textLine(line)
			if '=' not in line:
				continue
			key, value = line.strip().split('=', 1)
			parts = key.split('.')
			if key == 'version':
				try:
					version = int(value)
				except ValueError:
					pass
				continue
			if not which(key):
				if others:
					rest[key] = value
				continue
			player = player or parts[0] == 'player'
			try:
				if parts[0] == 'UserProfile':
					_profile['UserProfile'] = value
				elif parts[0] in ('volume',) and parts[1] in _VOLUMES:
					_profile['volume'][parts[1]] = max(0, min(_VOLUME_STOPS, int(value)))
				elif parts[0] == 'nosound' and parts[1] in _VOLUMES:
					_profile['nosound'][parts[1]] = value == '1'
				elif parts[0] in ('graphic', 'player'):
					_profile[parts[0]][int(parts[1])] = value == '1'
				elif parts[0] in ('clock', 'hours24', 'voice', 'wasdPan', 'autoEndTurn', 'edgeScroll', 'cityOneClick', 'keepAttacker', 'quickEnemyMoves') + _TOGGLES:
					_profile[parts[0]] = value == '1'
				elif parts[0] in ('graphicsLevel', 'renderQuality', 'globeView', 'movieQuality', 'mainMenu', 'antiAliasing', 'uiScale', 'captureVolume', 'playbackVolume'):
					_profile[parts[0]] = int(value)
				elif parts[0] in ('name', 'speakers', 'musicPath'):
					_profile[parts[0]] = value
				elif parts[0] in _INI_FLAGS:
					_profile[parts[0]] = value == '1'
				elif parts[0] in dict(_INI_NUMBERS):
					_profile[parts[0]] = max(0, int(value))
			except (ValueError, IndexError):
				pass
	finally:
		f.close()
	# A profile the old options screen wrote: an option that is on by default and off there was
	# most likely never chosen (the screen showed it off), so it is the default again, once. (With
	# Wait at End of Turn off the game core ends a turn by itself after a unit moved.)
	if player and version < _VERSION:
		for i in _DEFAULTS_ON:
			if _profile['player'].get(i) is False:
				del _profile['player'][i]
	return rest


def _save(name=None):
	"""The options kept: the profile's in its file (the loaded one's, or under name), the rest in
	profile.ini. A profile with no name (while one is deleted) is not written, as the original's."""
	path = _path()
	if not path:
		return False
	if name is None:
		name = _profile['name']
	lines = _lines()
	own = _fileName(name)
	ok = False
	if own:
		try:
			if not os.path.isdir(os.path.dirname(own)):
				os.makedirs(os.path.dirname(own))
			_write(own, '\n'.join(['version=%d' % _VERSION] + [l for l in lines if not _isGlobal(l.split('=', 1)[0])]) + '\n')
			ok = True
		except (IOError, OSError):
			pass
	if name == _profile['name']:
		_profile['UserProfile'] = name
	try:
		_write(path, '\n'.join(['version=%d' % _VERSION, 'UserProfile=%s' % _profile.get('UserProfile', '')] +
			[l for l in lines if _isGlobal(l.split('=', 1)[0])]) + '\n')
	except (IOError, OSError):
		pass
	return ok


def _lines():
	lines = []
	for key in ('name', 'speakers', 'musicPath'):
		lines.append('%s=%s' % (key, _profile[key]))
	for key in ('clock', 'hours24', 'voice', 'wasdPan', 'autoEndTurn', 'edgeScroll', 'cityOneClick', 'keepAttacker', 'quickEnemyMoves') + _TOGGLES:
		lines.append('%s=%d' % (key, 1 if _profile[key] else 0))
	for key in ('graphicsLevel', 'renderQuality', 'globeView', 'movieQuality', 'mainMenu', 'antiAliasing', 'uiScale', 'captureVolume', 'playbackVolume'):
		lines.append('%s=%d' % (key, _profile[key]))
	for v in _VOLUMES:
		lines.append('volume.%s=%d' % (v, _profile['volume'][v]))
		lines.append('nosound.%s=%d' % (v, 1 if _profile['nosound'][v] else 0))
	for kind in ('graphic', 'player'):
		for i, on in sorted(_profile[kind].items()):
			lines.append('%s.%d=%d' % (kind, i, 1 if on else 0))
	for key in _INI_FLAGS:
		lines.append('%s=%d' % (key, 1 if _profile[key] else 0))
	for key, default in _INI_NUMBERS:
		lines.append('%s=%d' % (key, _profile[key]))
	return lines


def _applyGraphics():
	"""The graphic options the profile has, to the host (the game core and the window ask it)."""
	_applyIniOptions()
	try:
		oc4host.reset_graphic_options()
		for i, on in _profile['graphic'].items():
			oc4host.set_graphic_option(int(i), bool(on))
	except Exception:
		pass
	_applyPlayerOptions()
	# (each on its own: a host without one of them, or one that fails, still gets the rest)
	for call, args in (('set_ui_scale', (_profile['uiScale'],)),
			('set_graphic_levels', (_profile['movieQuality'], _AA_SAMPLES[min(max(_profile['antiAliasing'], 0), len(_AA_SAMPLES) - 1)])),
			('set_wasd_pan', (_profile['wasdPan'],)), ('set_auto_end_turn', (_profile['autoEndTurn'],)),
			('set_edge_scroll', (_profile['edgeScroll'],)), ('set_city_one_click', (_profile['cityOneClick'],)),
			('set_keep_attacker', (_profile['keepAttacker'],))):
		try:
			getattr(oc4host, call)(*args)
		except Exception:
			pass
	try:
		oc4host.set_quick_enemy_moves(_profile['quickEnemyMoves'])
	except Exception:
		pass


def _applyToggles():
	"""The minimap's toggles as the profile has them, to the host (a new game, a loaded one and
	the next start of the program all take them so)."""
	try:
		oc4host.load_map_toggles(*[bool(_profile[k]) for k in _TOGGLES])
	except Exception:
		pass


def setMapToggles(grid, bareMap, yields, scores):
	"""A toggle by the minimap switched (its button or its key; the host tells): kept at once."""
	now = dict(zip(_TOGGLES, (bool(grid), bool(bareMap), bool(yields), bool(scores))))
	if [_profile[k] for k in _TOGGLES] != [now[k] for k in _TOGGLES]:
		_profile.update(now)
		_save()


def _toggle(i):
	"""A minimap toggle as it is now (the profile's, without the host)."""
	try:
		return bool(oc4host.map_toggles()[i])
	except Exception:
		return bool(_profile[_TOGGLES[i]])


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


# Opening a city with one click (openciv4's own option, off: the original opens a city with a double
# click on it or its name bar, and one click on the bar selects it).
CITY_ONE_CLICK_CHECKBOX = 'Oc4CityOneClickCheckBox'


def handleCityOneClickClicked(args):
	on = bool(args[0])
	_profile['cityOneClick'] = on
	_save()
	try:
		oc4host.set_city_one_click(on)
	except Exception:
		pass
	return 1


def _cityOneClickOn():
	try:
		return bool(oc4host.city_one_click())
	except Exception:
		return _profile['cityOneClick']


# After an attack, the camera and the selection stay on the unit that attacked (openciv4's own
# option, off by default: the original selects the next unit and looks at it as soon as the
# battle ends): its check box on the OpenCiv4 tab.
KEEP_ATTACKER_CHECKBOX = 'Oc4KeepAttackerCheckBox'


def handleKeepAttackerClicked(args):
	on = bool(args[0])
	_profile['keepAttacker'] = on
	_save()
	try:
		oc4host.set_keep_attacker(on)
	except Exception:
		pass
	return 1


def _keepAttackerOn():
	try:
		return bool(oc4host.keep_attacker())
	except Exception:
		return _profile['keepAttacker']


# Quick enemy moves (openciv4's own option, off as the original has none): the other civilizations'
# moves the Show Enemy / Friendly Moves options show come at about half the time, the camera still
# on each.
QUICK_ENEMY_MOVES_CHECKBOX = 'Oc4QuickEnemyMovesCheckBox'


def handleQuickEnemyMovesClicked(args):
	on = bool(args[0])
	_profile['quickEnemyMoves'] = on
	_save()
	try:
		oc4host.set_quick_enemy_moves(on)
	except Exception:
		pass
	return 1


def _quickEnemyMovesOn():
	try:
		return bool(oc4host.quick_enemy_moves())
	except Exception:
		return _profile['quickEnemyMoves']


# CivilizationIV.ini's settings (see _INI_FLAGS): check boxes and number boxes on the OpenCiv4 tab.
NO_TECH_SPLASH_CHECKBOX = 'Oc4NoTechSplashCheckBox'
NO_EVENT_MESSAGES_CHECKBOX = 'Oc4NoEventMessagesCheckBox'
OPENCIV4_AUTOSAVES_CHECKBOX = 'Oc4AutoSavesCheckBox'
_xmlFieldOfView = [None] # (GlobalDefines.xml's, before the profile's replaced it)


def _applyIniOptions():
	try:
		oc4host.set_ini_options(_profile['noTechSplash'], _profile['noEventMessages'], _profile['autoSaveInterval'],
			_profile['autoSavesKept'], _profile['openciv4AutoSaves'], _profile['maxFrameRate'])
	except Exception:
		pass
	if _gameStarted[0]:
		_applyFieldOfView()


_gameStarted = [False] # (before, the game core has no defines to read)


def gameStarted():
	"""The host: a game has started (GlobalDefines.xml read); the profile's field of view now."""
	_gameStarted[0] = True
	_applyFieldOfView()


def _applyFieldOfView():
	"""FIELD_OF_VIEW: a non-zero one replaces GlobalDefines.xml's, as the ini's does (BUG's slider
	on the map may change it again later)."""
	try:
		gc = _cpe.CyGlobalContext()
		fov = _profile['fieldOfView']
		if fov > 0:
			if _xmlFieldOfView[0] is None:
				_xmlFieldOfView[0] = gc.getDefineFLOAT('FIELD_OF_VIEW')
			gc.setDefineFLOAT('FIELD_OF_VIEW', float(fov))
		elif _xmlFieldOfView[0] is not None:
			gc.setDefineFLOAT('FIELD_OF_VIEW', float(_xmlFieldOfView[0]))
			_xmlFieldOfView[0] = None
	except Exception:
		pass


# The OpenCiv4 tab's options: its check boxes with their profile keys, then its number boxes'
# (Oc4Edit<key>), which Restore Defaults on that tab sets back to a new profile's.
_OC4_TAB_CHECKBOXES = ((WASD_PAN_CHECKBOX, 'wasdPan'), (AUTO_END_TURN_CHECKBOX, 'autoEndTurn'), (EDGE_SCROLL_CHECKBOX, 'edgeScroll'),
	(CITY_ONE_CLICK_CHECKBOX, 'cityOneClick'), (KEEP_ATTACKER_CHECKBOX, 'keepAttacker'), (QUICK_ENEMY_MOVES_CHECKBOX, 'quickEnemyMoves'),
	(NO_TECH_SPLASH_CHECKBOX, 'noTechSplash'), (NO_EVENT_MESSAGES_CHECKBOX, 'noEventMessages'), (OPENCIV4_AUTOSAVES_CHECKBOX, 'openciv4AutoSaves'))


def handleOc4Reset(args):
	"""Restore Defaults on the OpenCiv4 tab: OpenCiv4's options as a new profile has them, as the
	game's button does for its own tab's (CvOptionsScreenCallbackInterface: resetOptions, refresh,
	saveProfile); the other tabs' are kept."""
	fresh = _fresh()
	for name, key in _OC4_TAB_CHECKBOXES:
		_profile[key] = fresh[key]
	for key, default in _INI_NUMBERS:
		_profile[key] = fresh[key]
	_save()
	_applyGraphics()
	ctrl = _state['ctrl']
	if ctrl is not None:
		for name, key in _OC4_TAB_CHECKBOXES:
			ctrl.setValue(name, bool(_profile[key]))
		for key, default in _INI_NUMBERS:
			ctrl.setText('Oc4Edit' + key, str(_profile[key]))
	return 1


def handleIniFlagClicked(args):
	"""One of the ini's check boxes (its argument the profile's key)."""
	_profile[str(args[1])] = bool(args[0])
	_applyIniOptions()
	return 1


def handleIniNumberTyped(args):
	"""One of the ini's number boxes, typed into (its argument the profile's key): digits only, an
	empty box 0."""
	digits = ''.join([c for c in u'%s' % (args[0],) if c in u'0123456789'])
	_profile[str(args[1])] = int(digits[:6]) if digits else 0
	_applyIniOptions()
	return 1


def _editTyped(w):
	"""oc4screens: an edit box typed into; one of the tab's, its callback with the text."""
	ctrl = _state['ctrl']
	if ctrl is None or not w.name.startswith('OptW'):
		return
	try:
		n = ctrl._nodes.get(int(w.name[4:]))
	except ValueError:
		return
	if n is None or n.kind != 'edit':
		return
	n.text = w.text
	_call(n, (w.text, n.callback[2] if n.callback else n.name))


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


# What the original's Graphics Level sets (CyUserProfile.setGraphicsLevel, measured in Wine): the
# graphic options it switches on and off, the render, globe and movie quality (0 high, 1 medium,
# 2 low) and the anti-aliasing (an index into 0, 2, 4, 8 samples; None: as it is).
_LEVEL_PRESETS = {
	0: ((), ('SINGLE_UNIT_GRAPHICS', 'FROZEN_ANIMATIONS', 'EFFECTS_DISABLED', 'GLOBE_VIEW_BUILDINGS_DISABLED', 'LOWRES_TEXTURES'), 0, None),
	1: (('SINGLE_UNIT_GRAPHICS', 'GLOBE_VIEW_BUILDINGS_DISABLED'), ('FROZEN_ANIMATIONS', 'EFFECTS_DISABLED', 'LOWRES_TEXTURES'), 1, 1),
	2: (('SINGLE_UNIT_GRAPHICS', 'FROZEN_ANIMATIONS', 'EFFECTS_DISABLED', 'GLOBE_VIEW_BUILDINGS_DISABLED', 'LOWRES_TEXTURES'), ('HIRES_TERRAIN',), 2, 0),
}


# The original's options OpenCiv4 does not do, by their controls' names on the options screen
# (CvOptionsScreen.py): Render Quality and Globe Quality, which made no difference in the original
# either, and High Detail Terrain (the terrain's mesh detail; OpenCiv4 has one).
_AA_SAMPLES = (0, 2, 4, 8) # the anti-aliasing drop-down's choices (CvOptionsScreen: 0, 2, 4, 8, 16), as the renderer does them
_NOT_DONE_TIP = u'OpenCiv4 does not implement this option.'


def _notDone():
	names = set(('GraphicsQualityLabel', 'RenderQualityDropdownBox', 'GlobeViewLabel', 'GlobeViewDropdownBox'))
	try:
		names.add('GraphicOptionCheckbox_%d' % _graphicOption('HIRES_TERRAIN'))
	except Exception:
		pass
	return names


def _graphicOption(name):
	return int(getattr(_cpe.GraphicOptionTypes, 'GRAPHICOPTION_' + name))


def _graphicDefault(i):
	"""The XML's default (CIV4GraphicOptionInfos), but full screen: OpenCiv4 starts in a window
	until it is chosen (the host's graphicOptionChosen)."""
	try:
		if int(i) == int(_cpe.GraphicOptionTypes.GRAPHICOPTION_FULLSCREEN):
			return False
		return bool(_cpe.CyGlobalContext().getGraphicOptionsInfoByIndex(int(i)).getDefault())
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
		oc4host.set_mono(_profile['speakers'] == _SPEAKERS[0])
		oc4host.set_music_folder(_profile['musicPath'])
	except Exception:
		pass


_deviceCache = {}


def _devices(recording):
	"""The system's microphones or outputs, by name (asked once a showing of the screen)."""
	if recording not in _deviceCache:
		try:
			_deviceCache[recording] = [n.decode('utf-8', 'replace') if isinstance(n, str) else n for n in oc4host.audio_devices(recording)]
		except Exception:
			_deviceCache[recording] = []
	return _deviceCache[recording]


def _deviceName(recording, i):
	names = _devices(recording)
	return names[int(i)] if 0 <= int(i) < len(names) else u''


def folderChosen(path):
	"""The folder the player chose in the system's dialog (Browse...): in the path box with Custom
	Music Folder ticked (the game's own updateMusicPath), and kept: their music plays from there."""
	if not path:
		return
	try:
		import CvScreensInterface
		CvScreensInterface.updateMusicPath((path.decode('utf-8', 'replace') if isinstance(path, str) else path,))
	except Exception:
		import traceback
		traceback.print_exc()
	_profile['musicPath'] = _utf8(path)
	_save()
	_applyVolumes()


_names = [] # (the profiles listed, loadProfileFileNames)
_clock = {'justOn': False}


class _UserProfile(object):
	"""CyUserProfile: the player's options (one profile, the host's file)."""

	def __getattr__(self, name):
		if name.startswith('__'):
			raise AttributeError(name)
		return lambda *args, **kwargs: 0

	# the profiles (Profiles/<name>.ini): the Other tab's list, Create New Profile, Delete Profile
	def getProfileName(self): return _profile['name']
	def setProfileName(self, s): _profile['name'] = _utf8(s)
	def loadProfileFileNames(self): _names[:] = _profileNames() or [_profile['name']]
	def getNumProfileFiles(self): return len(_names)
	def getProfileFileName(self, i):
		# (as the original's, a path the screen cuts the name from: after PROFILES\ and before the extension)
		i = int(i)
		return 'PROFILES\\%s.pfl' % (_names[i] if 0 <= i < len(_names) else _profile['name'])
	def isProfileFileExist(self, s):
		own = _fileName(s)
		return bool(own and os.path.isfile(own))
	def readFromFile(self, s):
		"""Loads the profile s (its file): its options, to the sounds, the map and the game core."""
		own = _fileName(s)
		if not own or not os.path.isfile(own):
			return False
		_resetProfile()
		_read(own, lambda key: not _isGlobal(key))
		_profile['name'] = _utf8(s)
		_save()
		_clock['justOn'] = True
		_applyVolumes()
		_applyGraphics()
		_applyPlayerOptions()
		toggles = globals().get('_applyToggles') # (the minimap's toggles, where they are kept)
		if toggles:
			toggles()
		return True
	def writeToFile(self, s): return _save(_utf8(s))
	def deleteProfileFile(self, s):
		"""Deletes the profile s's file; not the last one (a profile is always loaded)."""
		own = _fileName(s)
		if not own or not os.path.isfile(own) or len(_profileNames()) < 2:
			return False
		try:
			os.remove(own)
		except OSError:
			return False
		return True
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
	def setGraphicsLevel(self, i):
		# (the original's level is a preset of the others, as measured in it: see _LEVEL_PRESETS)
		_profile['graphicsLevel'] = int(i)
		preset = _LEVEL_PRESETS.get(int(i))
		if preset:
			on, off, level, aa = preset
			for name in on:
				_profile['graphic'][_graphicOption(name)] = True
			for name in off:
				_profile['graphic'][_graphicOption(name)] = False
			_profile['renderQuality'] = _profile['globeView'] = _profile['movieQuality'] = level
			if aa is not None:
				_profile['antiAliasing'] = aa
		_save()
		_applyGraphics()
	def getRenderQualityLevel(self): return _profile['renderQuality']
	def setRenderQualityLevel(self, i): _profile['renderQuality'] = int(i)
	# (the movies' and the anti-aliasing's take effect at once)
	def getGlobeViewRenderLevel(self): return _profile['globeView']
	def setGlobeViewRenderLevel(self, i): _profile['globeView'] = int(i)
	def getMovieQualityLevel(self): return _profile['movieQuality']
	def setMovieQualityLevel(self, i): _profile['movieQuality'] = int(i); _save(); _applyGraphics()
	def getMainMenu(self): return _profile['mainMenu']
	def setMainMenu(self, i): _profile['mainMenu'] = int(i)
	def getAntiAliasing(self): return _profile['antiAliasing']
	def setAntiAliasing(self, i): _profile['antiAliasing'] = int(i); _save(); _applyGraphics()
	def getAntiAliasingMaxMultiSamples(self): return len(_AA_SAMPLES) - 1
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
	def setMusicPath(self, s): _profile['musicPath'] = _utf8(s); _save(); _applyVolumes()
	def musicPathDialogBox(self):
		# (the system's folder dialog; the folder chosen comes back to folderChosen)
		try:
			import os
			oc4host.choose_folder(_profile['musicPath'] or os.path.expanduser('~/Music'))
		except Exception:
			pass
	def getSpeakerConfig(self): return _profile['speakers'] or _SPEAKERS_DEFAULT
	def setSpeakerConfig(self, s):
		if str(s) in _SPEAKERS:
			_profile['speakers'] = str(s)
			_save()
			_applyVolumes()
	def getSpeakerConfigFromList(self, i): return _SPEAKERS[int(i)] if 0 <= int(i) < len(_SPEAKERS) else ''
	def useVoice(self): return False
	def setUseVoice(self, b): pass
	def getNumCaptureDevices(self): return len(_devices(True))
	def getNumPlaybackDevices(self): return len(_devices(False))
	def getCaptureDeviceIndex(self): return 0
	def getPlaybackDeviceIndex(self): return 0
	def getCaptureDeviceDesc(self, i): return _deviceName(True, i)
	def getPlaybackDeviceDesc(self, i): return _deviceName(False, i)
	def setCaptureDevice(self, i): pass
	def setPlaybackDevice(self, i): pass
	def getCaptureVolume(self): return _profile['captureVolume']
	def getPlaybackVolume(self): return _profile['playbackVolume']
	def setCaptureVolume(self, i): _profile['captureVolume'] = max(0, min(100, int(i)))
	def setPlaybackVolume(self, i): _profile['playbackVolume'] = max(0, min(100, int(i)))
	def getMaxCaptureVolume(self): return 100
	def getMaxPlaybackVolume(self): return 100

	# the clock
	# (the original marks the clock just turned on at every setClockOn, on or off, and at a profile's
	# load; BUG's clock starts its time-and-turn alternation over then and clears the mark)
	def isClockOn(self): return _profile['clock']
	def setClockOn(self, b): _profile['clock'] = bool(b); _clock['justOn'] = True
	def is24Hours(self): return _profile['hours24']
	def set24Hours(self, b): _profile['hours24'] = bool(b)
	def wasClockJustTurnedOn(self): return _clock['justOn']
	def setClockJustTurnedOn(self, b): _clock['justOn'] = bool(b)

	# the map's toggles by the minimap (the profile keeps them as they are switched)
	def getGrid(self): return _toggle(0)
	def getMap(self): return _toggle(1)
	def getYields(self): return _toggle(2)
	def getScores(self): return _toggle(3)


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
	__slots__ = ('id', 'kind', 'name', 'children', 'text', 'value', 'items', 'callback', 'flags', 'style', 'minimum', 'maximum', 'tip', 'disabled', 'offset', 'inside')

	def __init__(self, id, kind, name):
		self.id, self.kind, self.name = id, kind, name
		self.children, self.text, self.value, self.items, self.callback = [], u'', 0, (), None
		self.flags, self.style, self.minimum, self.maximum, self.tip = set(), '', 0, 0, u''
		self.disabled = False
		self.offset, self.inside = 0, False # (a scroll panel: how far it is scrolled; being laid out inside)


_state = {'ctrl': None, 'dirty': False, 'scrolling': 0}


def _translate(key, fallback):
	"""The game's text for a key (CyTranslator), or the fallback without one."""
	try:
		text = _cpe.CyTranslator().getText(key, ())
		if text and text != key:
			return text
	except Exception:
		pass
	return fallback


def _text(s):
	try:
		return oc4screens._text(s)
	except Exception:
		return u'%s' % (s,)


class CyGTabCtrl(object):
	"""A window of tabs, its forms boxes of controls; at most one up (the options screen's)."""

	def __init__(self, szName='', bStatic=False, bCloseButton=False):
		self._title = _text(szName)
		_deviceCache.clear() # (the system's sound devices asked again, as the screen is made)
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
		# (the original's options OpenCiv4 does not do: greyed, with a tool tip saying so)
		if n.name in _notDone():
			n.disabled, n.tip = True, _NOT_DONE_TIP
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
		scrolling, opening a city with one click, keeping the camera on an attacker (kept in the profile as
		wasdPan, autoEndTurn, edgeScroll, cityOneClick, keepAttacker)."""
		self.attachTabItem(OC4_FORM, u'OpenCiv4')
		self.attachVBox(OC4_FORM, 'Oc4VBox')
		# (as the game's tabs: the options in a scroll panel, then a line and Restore Defaults and Exit)
		self.attachScrollPanel('Oc4VBox', 'Oc4Scroll')
		self.setLayoutFlag('Oc4Scroll', 'LAYOUT_SIZE_HEXPANDING')
		self.setLayoutFlag('Oc4Scroll', 'LAYOUT_SIZE_VEXPANDING')
		self._attachLowerButtons('Oc4VBox', 'Oc4', 'handleOc4Reset')
		parent = 'Oc4Panel'
		self.attachPanel('Oc4Scroll', parent)
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
		self.attachCheckBox(parent, CITY_ONE_CLICK_CHECKBOX, u'Open a city with a single click', 'oc4options', 'handleCityOneClickClicked',
			CITY_ONE_CLICK_CHECKBOX, _cityOneClickOn())
		self.setToolTip(CITY_ONE_CLICK_CHECKBOX, u'One click on your city or its name bar opens the city screen. Off, as in the '
			u'original: a click on the name bar selects the city, a double click on the city or its bar opens it.')
		self.attachCheckBox(parent, KEEP_ATTACKER_CHECKBOX, u'Keep the camera on a unit after it attacks', 'oc4options', 'handleKeepAttackerClicked',
			KEEP_ATTACKER_CHECKBOX, _keepAttackerOn())
		self.setToolTip(KEEP_ATTACKER_CHECKBOX, u'After your unit attacks, the camera stays on it and it stays selected until you '
			u'click or press a key, so you see how the battle ended. Off, as in the original game, the next unit that needs '
			u'orders is selected and the camera goes to it as soon as the battle is over.')
		self.attachCheckBox(parent, QUICK_ENEMY_MOVES_CHECKBOX, u'Quick enemy moves', 'oc4options', 'handleQuickEnemyMovesClicked',
			QUICK_ENEMY_MOVES_CHECKBOX, _quickEnemyMovesOn())
		self.setToolTip(QUICK_ENEMY_MOVES_CHECKBOX, u'With Show Enemy Moves or Show Friendly Moves on, the other civilizations\' '
			u'units move about twice as fast. The camera still goes to each unit and stays a moment after it arrives. '
			u'The original has no such option: its Quick Moves speeds up only your own units.')
		self._attachIniOptions(parent)

	def _attachLowerButtons(self, vbox, prefix, reset):
		"""A tab's foot as the game's tabs have it (CvOptionsScreen.py): a line, then Restore Defaults
		(oc4options' reset) and Exit (the game's own handleExitButtonInput) centred under it."""
		self.attachHSeparator(vbox, prefix + 'ExitSeparator')
		row = prefix + 'LowerHBox'
		self.attachHBox(vbox, row)
		self.setLayoutFlag(row, 'LAYOUT_HCENTER')
		self.attachButton(row, prefix + 'OptionsResetButton', _translate('TXT_KEY_OPTIONS_RESET', u'Restore Defaults'),
			'oc4options', reset, prefix + 'OptionsResetButton')
		name = prefix + 'OptionsExitButton'
		self.attachButton(row, name, _translate('TXT_KEY_PEDIA_SCREEN_EXIT', u'Exit'),
			'CvOptionsScreenCallbackInterface', 'handleExitButtonInput', name)
		self.setLayoutFlag(name, 'LAYOUT_HCENTER')

	def _attachIniOptions(self, parent):
		"""CivilizationIV.ini's settings the original sets only in its ini (see _INI_FLAGS)."""
		self.attachHSeparator(parent, 'Oc4IniSeparator')
		self.attachLabel(parent, 'Oc4IniLabel', u'From CivilizationIV.ini')
		for name, key, text, tip in (
				(NO_TECH_SPLASH_CHECKBOX, 'noTechSplash', u'No tech splash screens',
					u'A new technology is not shown on its own screen with its quote (the ini\'s NoTechSplash).'),
				(NO_EVENT_MESSAGES_CHECKBOX, 'noEventMessages', u'No event messages on the screen',
					u'The messages are not shown at the top of the map; they are still in the turn log (Ctrl+Tab) and '
					u'their sounds play (the ini\'s NoEventMessages).')):
			self.attachCheckBox(parent, name, text, 'oc4options', 'handleIniFlagClicked', key, _profile[key])
			self.setToolTip(name, tip)
		for key, before, after, tip in (
				('autoSaveInterval', u'Autosave every', u'turns (0: never)',
					u'The game is saved at the start of every this many turns, and at its first, into Saves\\single\\auto '
					u'(the ini\'s AutoSaveInterval, 4).'),
				('autoSavesKept', u'Keep', u'autosaves',
					u'The oldest autosaves beyond this many are deleted (the ini\'s MaxAutoSaves, 5).')):
			self._attachNumber(parent, key, before, after, tip)
		self.attachCheckBox(parent, OPENCIV4_AUTOSAVES_CHECKBOX, u'OpenCiv4\'s autosaves: each game its own, one in 25 turns kept',
			'oc4options', 'handleIniFlagClicked', 'openciv4AutoSaves', _profile['openciv4AutoSaves'])
		self.setToolTip(OPENCIV4_AUTOSAVES_CHECKBOX, u'Autosaves go to My Games\\openciv4\\Saves\\auto, named by the civilization '
			u'and the map, the kept ones in turn, and one save in each 25 turns is kept for good. Autosave every 1 turn and keep 10 '
			u'is how OpenCiv4 saved before.')
		for key, before, after, tip in (
				('fieldOfView', u'Field of view', u'degrees (0: the game\'s, 42)',
					u'How wide the camera sees; more shows more of the map at once (the ini\'s FIELD_OF_VIEW). BUG\'s slider '
					u'on the map changes it too.'),
				('maxFrameRate', u'At most', u'frames a second (0: no limit)',
					u'Fewer frames save battery (the ini\'s SetMaxFrameRate).')):
			self._attachNumber(parent, key, before, after, tip)

	def _attachNumber(self, parent, key, before, after, tip):
		row = 'Oc4Row' + key
		self.attachHBox(parent, row)
		self.attachLabel(row, row + 'Before', before)
		name = 'Oc4Edit' + key
		self.attachEdit(row, name, str(_profile[key]), 'oc4options', 'handleIniNumberTyped', key)
		self._byName[name].flags.add('OC4_DIGITS')
		self.setToolTip(name, tip)
		self.attachLabel(row, row + 'After', after)

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
		if str(name) in _NETWORK:
			# (the connection speed: what the original's multiplayer sends, CivilizationIV.ini's
			# Bandwidth; OpenCiv4 has no multiplayer)
			n.disabled, n.tip = True, _NETWORK_TIP

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
		if n is not None and n.name not in _notDone():
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
			if n.kind == 'edit':
				# (the box as shown too: a redraw keeps what was typed into it)
				try:
					w = _screen()._s.get('OptW%d' % n.id)
					if w is not None:
						w.text = n.text
				except Exception:
					pass
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
	def setEnabled(self, name, bEnabled=True):
		n = self._byName.get(str(name))
		if n is not None:
			n.disabled = not bEnabled
			_schedule()


# The Other tab's connection speed: no sense in OpenCiv4 (shown as the original's, greyed, with why).
_NETWORK = ('BroadbandSelection', 'ModemSelection')
_NETWORK_TIP = (u'OpenCiv4 has no multiplayer games, so there is no connection to set the speed of: '
	u'this choice (the original\'s CivilizationIV.ini Bandwidth) does nothing here.')
_GREY = u'<color=150,150,150>'


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
_ROW = {'label': 28, 'checkbox': 28, 'radio': 28, 'dropdown': 28, 'button': 31, 'edit': 26, 'hslider': 28, 'hsep': 10,
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
	return 'LAYOUT_SIZE_HEXPANDING' in n.flags or 'LAYOUT_SIZE_HPREFERREDEXPANDING' in n.flags or 'LAYOUT_SIZE_HFIXEDEXPANDING' in n.flags


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
	if k == 'edit' and 'OC4_DIGITS' in n.flags:
		return 64
	if k == 'hslider':
		return 210 # (the original's voice volume sliders)
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
		return 92 # (the original's volume sliders' height)
	if k == 'hbox':
		start, widths = _shares(n, w)
		return max([_height(c, cw) for c, cw in zip(n.children, widths)] or [0])
	if k == 'panel' and not n.children:
		return 0 # (an empty one: the resolution's screen picture, none here)
	if k == 'scroll' and not n.inside and not _state['scrolling']:
		# (a scroll panel: as high as its box is given, what it holds scrolls in it; one inside
		# another, the Audio tab's volume sliders in its panel, as high as what it holds)
		return 0
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
	if 'LAYOUT_SIZE_HPREFERREDEXPANDING' in n.flags:
		# (a row at its preferred size: who knows their width keep it, the rest share what is left;
		# the Audio tab's voice column, the speakers and music the rest)
		for c, nat in zip(n.children, natural):
			if nat is not None and c.kind not in ('vsep', 'spacer'):
				fixed[c.id] = nat
	if any(_hgrow(c) for c in n.children) or 'LAYOUT_SIZE_HFIXEDEXPANDING' in n.flags:
		# (beside one that expands, a check box or a button is as wide as it needs: the Custom Music
		# Folder row, its path box the rest)
		for c, nat in zip(n.children, natural):
			if c.kind in ('checkbox', 'button') and nat is not None:
				fixed[c.id] = nat
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
	if k == 'scroll' and not n.inside:
		_scrollPanel(screen, n, name, x, y, w, h)
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
	# (a control OpenCiv4 does not do: greyed, takes no clicks, its tool tip says why)
	grey, ungrey = (_GREY, u'</color>') if n.disabled else (u'', u'')
	if k == 'label':
		screen.setLabel(name, 'Background', _FONT + grey + n.text + ungrey + u'</font>', 1 << 0, x, y + 4, 0, font, general, -1, -1)
		if n.tip:
			_tip(screen, name, n.tip)
	elif k in ('checkbox', 'radio'):
		# (the theme's check box, as the custom game screen's: GFC_Control_StandardCheckBox_Style)
		screen.addCheckBoxGFC(name, '', '', x, y + 4, _BOX, _BOX, general, -1, -1, _cpe.ButtonStyles.BUTTON_STYLE_STANDARD)
		screen.setStyle(name, 'GFC_Control_StandardCheckBox_Style')
		w_ = screen._s.get(name)
		if w_ is not None:
			w_.state = bool(n.value)
		text = n.text if not n.disabled else _GREY + n.text + u'</color>'
		screen.setText(name + 'L', 'Background', _FONT + text + u'</font>', 1 << 0, x + _BOX_TEXT, y + 4, 0, font, general, -1, -1)
		for part in (name, name + 'L'):
			w_ = screen._s.get(part)
			if w_ is not None:
				if n.tip:
					w_.help = n.tip
				if n.disabled and part == name:
					w_.enabled = False
	elif k == 'button':
		screen.setButtonGFC(name, n.text, '', x, y + 4, w, 25, general, -1, -1, _cpe.ButtonStyles.BUTTON_STYLE_STANDARD)
	elif k == 'edit':
		screen.addEditBoxGFC(name, x, y, min(w, 260), 24, general, -1, -1, font)
		screen.setEditBoxString(name, n.text)
		if 'OC4_DIGITS' in n.flags: # (a number box: digits only)
			screen.setEditBoxCharacters(name, u'0123456789')
			screen.setEditBoxMaxCharCount(name, 4)
	elif k == 'dropdown':
		dw = min(w, 260)
		if 'LAYOUT_RIGHT' in n.flags:
			x += w - dw
		screen.addDropDownBoxGFC(name, x, y + 1, dw, general, -1, -1, font)
		for i, item in enumerate(n.items):
			screen.addPullDownString(name, item, i, i, i == int(n.value))
		if n.tip:
			_tip(screen, name, n.tip)
		if n.disabled:
			screen.enable(name, False)
	elif k in ('hslider', 'vslider'):
		# (the theme's slider: its fill on the side its control flag names, CF_SLIDER_FILL_UP / _DOWN)
		screen.addSlider(name, x, y, w, h, int(n.value), n.minimum, n.maximum, general, -1, -1, k == 'vslider')
		screen.setSliderFill(name, 'down' if 'CF_SLIDER_FILL_DOWN' in n.flags else 'up' if 'CF_SLIDER_FILL_UP' in n.flags else '')
	elif k == 'hsep':
		screen.addPanel(name, u'', u'', True, False, x, y + 4, w, 2, _cpe.PanelStyles.PANEL_STYLE_MAIN)
	if n.name in _VOICE_CONTROLS:
		# (voice chat: shown as the original's, not to be used)
		for part in (name, name + 'L'):
			w_ = screen._s.get(part)
			if w_ is not None:
				w_.enabled, w_.help = False, _VOICE_TIP


def _scrollPanel(screen, n, name, x, y, w, h):
	"""A scroll panel (attachScrollPanel): what it holds clipped to its box and, when longer,
	scrolled there by the wheel and the scroll bar on its right, as the original's (BUG's City
	Screen and Advisors tabs; the bar over the right edge of the tan panel, which keeps its width).
	Its controls are placed as usual, then moved into an engine scroll panel at the same place."""
	screen.addScrollPanel(name, u'', x, y, w, h, _cpe.PanelStyles.PANEL_STYLE_EMPTY)
	panel = screen._s.get(name)
	if panel is None:
		return
	panel.inset = 0
	# (where it was scrolled to: the screen is laid out again after every click)
	panel.offset = n.offset
	first = len(screen._s.order)
	n.inside = True
	_state['scrolling'] += 1
	try:
		_place(screen, n, x, y, w, h)
	finally:
		n.inside = False
		_state['scrolling'] -= 1
	for c in screen._s.order[first:]:
		cw = screen._s.widgets.get(c)
		if cw is None:
			continue
		cw.parent, cw.mode = name, 'rel'
		cw.x, cw.y = cw.x - x, cw.y - y
		panel.children.append(c)
	del screen._s.order[first:]


def _tip(screen, name, text):
	w = screen._s.get(name)
	if w is not None:
		w.help = text


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
	if ctrl is not None:
		# (what was typed into the edit boxes stays: they are made again)
		for n in ctrl._nodes.values():
			if n.kind == 'scroll':
				# (and how far each scroll panel was scrolled)
				w_ = _screen()._s.get('OptW%d' % n.id)
				if w_ is not None:
					n.offset = w_.offset
			if n.kind == 'edit':
				w_ = _screen()._s.get('OptW%d' % n.id)
				if w_ is not None:
					n.text = w_.text
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
	if function != 'OptW':
		return 0
	n = ctrl._nodes.get(ident)
	if n is None:
		return 0
	if n.disabled:
		return 1
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
	elif n.kind in ('hslider', 'vslider') and notify == 18:
		# (NOTIFY_SLIDER_NEWSTOP: the slider pressed or dragged to another stop; it shows it already)
		n.value = int(data)
		_call(n, (int(data), n.callback[2] if n.callback else n.name))
	elif n.kind == 'dropdown' and notify == 11:
		n.value = int(data)
		_call(n, (int(data), n.callback[2] if n.callback else n.name))
		_schedule()
	return 1


def _noRestart(bForceShowing=False):
	"""CvOptionsScreenCallbackInterface.restartPopup: the original's resolution, graphics level,
	single unit graphics, full screen, terrain detail and speakers took a restart; here none does."""
	pass


def _call(n, args):
	if not n.callback:
		return
	module, function = n.callback[0], n.callback[1]
	try:
		m = __import__(module)
		# (no "Program must be restarted" popup: every option here takes effect at once)
		if hasattr(m, 'restartPopup'):
			m.restartPopup = _noRestart
		getattr(m, function)(args)
	except Exception:
		import traceback
		traceback.print_exc()
	# (every choice kept at once, as the original writes its profile: a player option too)
	_syncPlayerOptions()
	_save()


# ---------------------------------------------------------------- in place

# ---------------------------------------------------------------- the clock and the alarm

# The engine's alarm clock (CvPythonExtensions' toggleAlarm and its getters), as in the original:
# set to go off in so many hours and minutes from this minute (Alarm On on the Other tab, with the
# boxes' hours and minutes), with a popup saying when; the interface clock has a * before it while
# it is set; at that minute it rings (its sound and a popup) and is off again. Not in the profile:
# kept until the program ends.
_alarm = {'at': None} # (the minute it goes off, in seconds since the epoch)


def _minuteNow():
	return int(time.time()) // 60 * 60


def _hhmm(t, hours24):
	"""A time as the alarm's popups write it: 6:53, or 6:53 AM with the 12-hour clock."""
	lt = time.localtime(t)
	if hours24:
		return u'%d:%02d' % (lt.tm_hour, lt.tm_min)
	ampm = _cpe.CyTranslator().getText('TXT_KEY_MISC_PM_AFTERNOON' if lt.tm_hour >= 12 else 'TXT_KEY_MISC_AM_MORNING', ())
	return u'%d:%02d%s' % ((lt.tm_hour + 11) % 12 + 1, lt.tm_min, ampm)


def _alarmPopup(key, t):
	try:
		popup = oc4screens.CyPopup(-1, _cpe.EventContextTypes.NO_EVENTCONTEXT, True)
		tr = _cpe.CyTranslator()
		popup.setHeaderString(tr.getText('TXT_KEY_POPUP_ALARM_TITLE', ()), 1 << 2)
		popup.setBodyString(tr.getText(key, (_hhmm(t, _profile['hours24']),)), 1 << 0)
		popup.launch(True, _cpe.PopupStates.POPUPSTATE_IMMEDIATE)
	except Exception:
		import traceback
		traceback.print_exc()


def _toggleAlarm(bOn, iHour=0, iMin=0):
	if bOn:
		_alarm['at'] = _minuteNow() + 60 * (60 * int(iHour) + int(iMin))
		_alarmPopup('TXT_KEY_MISC_ALARM_WILL_GO_OFF', _alarm['at'])
	else:
		_alarm['at'] = None


def _alarmLeft():
	"""Minutes till it goes off (0 when off)."""
	return max(0, (_alarm['at'] - _minuteNow()) // 60) if _alarm['at'] is not None else 0


def _alarmTime(i):
	return time.localtime(_alarm['at'])[i] if _alarm['at'] is not None else -1


def _checkAlarm():
	"""(every frame) The alarm's minute come: it rings."""
	if _alarm['at'] is not None and time.time() >= _alarm['at']:
		_alarm['at'] = None
		try:
			oc4host.audio_play('AS2D_ALARM')
		except Exception:
			pass
		_alarmPopup('TXT_KEY_MISC_ALARM_GOES_OFF', time.time())


_PATTERN = []


def clockText():
	"""getClockText: the time of day as the user's system writes it (the original takes Windows'),
	in 24 hours with the options' 24 Hour Clock; a * before it while the alarm is set."""
	if not _PATTERN:
		try:
			_PATTERN.append(str(oc4host.clock_pattern()))
		except Exception:
			_PATTERN.append('HH:mm')
	pattern = _PATTERN[0]
	if _profile['hours24'] and ('h' in pattern or 'a' in pattern or 't' in pattern):
		sep = ([c for c in pattern if c not in 'Hhm \'at'] or [':'])[0]
		pattern = 'HH%smm' % sep
	lt = time.localtime()
	out, i = [], 0
	while i < len(pattern):
		c = pattern[i]
		n = 1
		while i + n < len(pattern) and pattern[i + n] == c:
			n += 1
		if c == 'H':
			out.append('%02d' % lt.tm_hour if n > 1 else '%d' % lt.tm_hour)
		elif c in 'hK':
			h = (lt.tm_hour + 11) % 12 + 1 if c == 'h' else lt.tm_hour % 12
			out.append('%02d' % h if n > 1 else '%d' % h)
		elif c == 'm':
			out.append('%02d' % lt.tm_min if n > 1 else '%d' % lt.tm_min)
		elif c == 's':
			out.append('%02d' % lt.tm_sec if n > 1 else '%d' % lt.tm_sec)
		elif c in 'at':
			out.append('PM' if lt.tm_hour >= 12 else 'AM')
		elif c == "'":
			n = 1
		else:
			out.append(c * n)
		i += n
	text = u''.join(out).strip()
	return (u'*' if _alarm['at'] is not None else u'') + text


_cpe.isAlarmOn = lambda: _alarm['at'] is not None
_cpe.getAlarmHour = lambda: _alarmTime(3)
_cpe.getAlarmMin = lambda: _alarmTime(4)
_cpe.getAlarmHourLeft = lambda: _alarmLeft() // 60
_cpe.getAlarmMinLeft = lambda: _alarmLeft() % 60
_cpe.toggleAlarm = _toggleAlarm
_cpe.getClockText = clockText
if isinstance(getattr(oc4screens, '_clockText', None), list):
	oc4screens._clockText[0] = clockText
oc4screens.beforeDisplay.append(_checkAlarm)
_cpe.CyGTabCtrl = CyGTabCtrl
_cpe.CyUserProfile = _UserProfile
oc4screens.beforeDisplay.append(relayout)
if isinstance(getattr(oc4screens, 'editTyped', None), list): # (a test's stand-in may not have it)
	oc4screens.editTyped.append(_editTyped)
_load()
_applyVolumes()
_applyGraphics()
_applyToggles()
