## openciv4: a temporary main menu.
##
## The original's main menu is the engine's own (Civ4BeyondSword.exe draws it; the scripts have
## no part in it), so this one stands in for it until it is made: a new game (the leader, the
## map script, the world, the difficulty and the speed from the XML), a saved game, or the way
## out. It is a screen like the game's (oc4screens' widgets), drawn before any game exists;
## the host reads what was chosen with takeChoice().

import os
import time
import CvPythonExtensions as _cpe
import oc4screens

MENU = 31000 # its python file id (none of the game's screens has it)

_state = {
	'page': 'main',
	'title': u'',
	'maps': [],    # map script module names
	'folders': [], # (folder, label): where saves are looked for
	'saves': [],   # (path, name, label, modified) of the load page, newest first
	'mods': [],    # the other mods played here (as saves name them, 'Mods\Name\'; '': the game without one)
	'choice': None,
	'message': u'', # why the last choice could not be played (shown on the page it came from)
	'picked': {},  # the new game page's choices, kept across pages: dropdown name -> index
	'custom': None, # the custom game page's choices (see _customDefaults)
	'lists': {},   # the civilizations' and leaders' lists, made once a menu (key -> list)
}

_DROPDOWNS = ('MenuLeader', 'MenuMap', 'MenuSize', 'MenuClimate', 'MenuSea', 'MenuEra', 'MenuSpeed', 'MenuHandicap')


def _gc():
	return _cpe.CyGlobalContext()


def _t(key, fallback):
	text = _cpe.CyTranslator().getText(key, ())
	return text if text and text != key else fallback


def _u(s):
	if isinstance(s, unicode):
		return s
	try:
		return s.decode('utf-8')
	except UnicodeDecodeError:
		return s.decode('mbcs')


def _screen():
	return oc4screens.CyGInterfaceScreen('OpenCiv4MainMenu', MENU)


def show(title, maps, folders, message='', page='', mods=()):
	"""The menu up, on its first page: maps are the map scripts' module names, folders
	(folder, label) pairs where saved games are; with a message (a choice that could not be
	played) on the page given ('new', 'custom' or 'load'; else the load page), saying it."""
	_state['title'] = _u(title)
	_state['maps'] = sorted([_u(m) for m in maps], key=lambda m: m.lower())
	_state['folders'] = [(_u(f), _u(l)) for f, l in folders]
	_state['mods'] = list(mods)
	_state['choice'] = None
	_state['lists'] = {}
	_state['message'] = _u(message)
	_state['page'] = page if page in ('new', 'custom', 'load') else 'load' if message else 'main'
	_draw()


def relayout():
	_keepCustom()
	_draw()


def close():
	oc4screens.reset(MENU)
	oc4screens._screens.pop(MENU, None)
	oc4screens._changed()


def takeChoice():
	"""None while the player chooses; then ('new', {settings}), ('load', utf-8 path, humans to host for or 0),
	('join', address), ('mod', its folder) or ('exit',).
	A new game's settings may also hold 'players' (a count), 'options' ({'GAMEOPTION_...': on}),
	'victories' (['VICTORY_...'], those on), 'mapOptions' ({option: value}), 'slots'
	({slot: {'status': 'human'|'ai'|'closed', 'civ', 'leader', 'team'}}) and 'advancedStartPoints';
	left out, the usual."""
	c = _state['choice']
	_state['choice'] = None
	return c


def escape():
	if _state['page'] != 'main':
		_keepPicks()
		_keepCustom()
		_state['message'] = u''
		_state['page'] = 'main'
		_draw()
	return True


# ---------------------------------------------------------------- pages

def _draw():
	screen = _screen()
	if _state['page'] == 'custom':
		offsets = _state.setdefault('customOffsets', {})
		for name in ('CGRoster', 'CGSettingsPanel', 'CGOptionsPanel', 'CGVictoriesPanel'):
			widget = screen._s.get(name)
			if widget is not None:
				offsets[name] = widget.offset
	oc4screens.reset(MENU)
	W, H = screen.getXResolution(), screen.getYResolution()
	screen.setDimensions(0, 0, W, H)
	screen.showScreen(_cpe.PopupStates.POPUPSTATE_IMMEDIATE, False)
	screen.setCloseOnEscape(False)
	art = _background()
	if art:
		# (the loading screen's picture, 4:3, over the whole window)
		scale = max(W / 1024.0, H / 768.0)
		w, h = int(1024 * scale), int(768 * scale)
		screen.addDDSGFC('MenuBackground', art, (W - w) / 2, (H - h) / 2, w, h, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
	if _state['page'] == 'custom':
		_setupPanel(screen, 'CGBackdrop', '__setupBackdrop', 0, 0, W, H)
		_customPage(screen, W, H)
		return
	screen.addPanel('MenuTitleBar', u'', u'', True, False, 0, 0, W, 56, _cpe.PanelStyles.PANEL_STYLE_TOPBAR)
	screen.setLabel('MenuTitle', 'Background', u'<font=4b>' + _state['title'] + u'</font>', 1 << 2, W / 2, 14, 0,
		_cpe.FontTypes.TITLE_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
	screen.setLabel('MenuVersion', 'Background', u'<font=1>openciv4 - temporary menu</font>', 1 << 1, W - 12, H - 24, 0,
		_cpe.FontTypes.SMALL_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
	{'main': _mainPage, 'new': _newPage, 'load': _loadPage, 'multi': _multiPage, 'mods': _modsPage}[_state['page']](screen, W, H)


def _background():
	afm = _cpe.CyArtFileMgr()
	for tag in ('MAINMENU_LOAD_BTS', 'MAINMENU_LOAD'):
		try:
			info = afm.getInterfaceArtInfo(tag)
			if info and info.getPath():
				return info.getPath()
		except Exception:
			pass
	return ''


def _button(screen, name, text, x, y, w, h=40):
	screen.setButtonGFC(name, u'<font=3>' + text + u'</font>', '', x, y, w, h, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1,
		_cpe.ButtonStyles.BUTTON_STYLE_STANDARD)


def _mainPage(screen, W, H):
	w, h = 360, 406
	x, y = (W - w) / 2, (H - h) / 2
	_state['host'] = 0
	screen.addPanel('MenuMainPanel', u'', u'', True, False, x, y, w, h, _cpe.PanelStyles.PANEL_STYLE_MAIN)
	screen.setLabel('MenuMainHeader', 'Background', u'<font=3b>' + _t('TXT_KEY_MAIN_MENU_SINGLE_PLAYER', u'Single Player') + u'</font>',
		1 << 2, W / 2, y + 18, 0, _cpe.FontTypes.TITLE_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
	_button(screen, 'MenuNew', _t('TXT_KEY_MAIN_MENU_PLAY_NOW', u'Play Now!'), x + 30, y + 60, w - 60)
	_button(screen, 'MenuCustom', _t('TXT_KEY_MAIN_MENU_CUSTOM_GAME', u'Custom Game'), x + 30, y + 112, w - 60)
	_button(screen, 'MenuLoad', _t('TXT_KEY_MAIN_MENU_LOAD_GAME', u'Load Game'), x + 30, y + 164, w - 60)
	_button(screen, 'MenuMulti', _t('TXT_KEY_MAIN_MENU_MULTIPLAYER', u'Multiplayer'), x + 30, y + 216, w - 60)
	_button(screen, 'MenuMods', _t('TXT_KEY_MAIN_MENU_LOAD_MOD', u'Load a Mod'), x + 30, y + 268, w - 60)
	_button(screen, 'MenuExit', _t('TXT_KEY_MAIN_MENU_EXIT_GAME', u'Exit Game'), x + 30, y + 336, w - 60)


def _modName(folder):
	name = _u(folder).strip('\\/')
	return name.split('\\')[-1] if name else u'Beyond the Sword (no mod)'


def _modsPage(screen, W, H):
	"""The other mods played here (from the menu): the program starts again in the one chosen."""
	w, h = 520, 420
	x, y = (W - w) / 2, (H - h) / 2
	screen.addPanel('MenuModsPanel', u'', u'', True, False, x, y, w, h, _cpe.PanelStyles.PANEL_STYLE_MAIN)
	screen.setLabel('MenuModsHeader', 'Background', u'<font=3b>' + _t('TXT_KEY_MAIN_MENU_LOAD_MOD', u'Load a Mod') + u'</font>',
		1 << 2, W / 2, y + 18, 0, _cpe.FontTypes.TITLE_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
	screen.addTableControlGFC('MenuModList', 1, x + 24, y + 56, w - 48, h - 140, False, True, 24, 24, _cpe.TableStyles.TABLE_STYLE_STANDARD)
	screen.setTableColumnHeader('MenuModList', 0, u'', w - 48)
	for folder in _state['mods']:
		row = screen.appendTableRow('MenuModList')
		screen.setTableText('MenuModList', 0, row, _modName(folder), '', _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1, 1 << 0)
	if _state['mods']:
		screen.selectRow('MenuModList', 0, True)
	else:
		screen.setLabel('MenuModsNone', 'Background', u'<font=2>No other mod has been played here yet: start one once with its --mod.</font>',
			1 << 2, W / 2, y + 120, 0, _cpe.FontTypes.GAME_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
	_button(screen, 'MenuModGo', _t('TXT_KEY_MAIN_MENU_LOAD_MOD', u'Load a Mod'), x + w - 230, y + h - 64, 200)
	_button(screen, 'MenuBack', _t('TXT_KEY_MAIN_MENU_GO_BACK', u'Go Back'), x + 30, y + h - 64, 200)


def _multiPage(screen, W, H):
	"""Multiplayer: host a new game or a saved one for a number of humans (the others join it,
	port 2056), or join one at an address."""
	w, h = 520, 380
	x, y = (W - w) / 2, (H - h) / 2
	screen.addPanel('MenuMultiPanel', u'', u'', True, False, x, y, w, h, _cpe.PanelStyles.PANEL_STYLE_MAIN)
	screen.setLabel('MenuMultiHeader', 'Background', u'<font=3b>' + _t('TXT_KEY_MAIN_MENU_MULTIPLAYER', u'Multiplayer') + u'</font>',
		1 << 2, W / 2, y + 18, 0, _cpe.FontTypes.TITLE_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
	screen.setLabel('MenuHumansLabel', 'Background', u'<font=2>Human players</font>', 1 << 0, x + 30, y + 68, 0,
		_cpe.FontTypes.GAME_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
	screen.addDropDownBoxGFC('MenuHumans', x + w - 190, y + 64, 160, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1, _cpe.FontTypes.GAME_FONT)
	for n in range(2, 9):
		screen.addPullDownString('MenuHumans', u'%d' % n, n - 2, n, n == _state.get('humans', 2))
	_button(screen, 'MenuHostNew', u'Host a new game', x + 30, y + 110, w - 60)
	_button(screen, 'MenuHostLoad', u'Host a saved game', x + 30, y + 162, w - 60)
	screen.setLabel('MenuAddressLabel', 'Background', u'<font=2>Join the game at</font>', 1 << 0, x + 30, y + 232, 0,
		_cpe.FontTypes.GAME_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
	screen.addEditBoxGFC('MenuAddress', x + w - 290, y + 226, 260, 30, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1, _cpe.FontTypes.GAME_FONT)
	screen.setEditBoxString('MenuAddress', _state.get('address', u'127.0.0.1:2056'))
	_button(screen, 'MenuJoin', u'Join', x + w - 230, y + h - 64, 200)
	_button(screen, 'MenuBack', _t('TXT_KEY_MAIN_MENU_GO_BACK', u'Go Back'), x + 30, y + h - 64, 200)


def _leaders():
	"""(leader, civilization) of the playable civilizations, by the leader's name."""
	gc = _gc()
	out = []
	for c in range(gc.getNumCivilizationInfos()):
		civ = gc.getCivilizationInfo(c)
		if not civ.isPlayable():
			continue
		for l in range(gc.getNumLeaderHeadInfos()):
			if civ.isLeaders(l):
				out.append((gc.getLeaderHeadInfo(l).getDescription(), civ.getShortDescription(0), l, c))
	out.sort(key=lambda e: (e[0], e[1]))
	return out


def _infos(count, get, default):
	"""(description, index, is the default) of an info class."""
	return [(get(i).getDescription(), i, i == default) for i in range(count)]


def _define(name, infoType, fallback):
	"""The default of an info class: its STANDARD_ define, else the type so named."""
	gc = _gc()
	i = gc.getDefineINT(name) if name else -1
	if i <= 0 and infoType:
		j = gc.getInfoTypeForString(infoType)
		i = j if j >= 0 else i
	return i if i >= 0 else fallback


def _newPage(screen, W, H):
	gc = _gc()
	w, h = 700, 560
	x, y = (W - w) / 2, (H - h) / 2
	screen.addPanel('MenuNewPanel', u'', u'', True, False, x, y, w, h, _cpe.PanelStyles.PANEL_STYLE_MAIN)
	screen.setLabel('MenuNewHeader', 'Background', u'<font=3b>' + _t('TXT_KEY_MAIN_MENU_PLAY_NOW', u'Play Now!') + u'</font>',
		1 << 2, W / 2, y + 18, 0, _cpe.FontTypes.TITLE_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
	maps = _state['maps']
	defaultMap = [m.lower() for m in maps].index('continents') if 'continents' in [m.lower() for m in maps] else 0
	rows = [
		('MenuLeader', _t('TXT_KEY_MAIN_MENU_CHOOSE_CIVILIZATION', u'Choose your Civilization'),
			[(_t('TXT_KEY_MAIN_MENU_RANDOM', u'Random'), -1, True)] +
			[(u'%s (%s)' % (n, c), i, False) for i, (n, c, l, civ) in enumerate(_leaders())]),
		('MenuMap', _t('TXT_KEY_MAIN_MENU_SELECT_MAP', u'Map'), [(m, i, i == defaultMap) for i, m in enumerate(maps)]),
		('MenuSize', _t('TXT_KEY_MAIN_MENU_SELECT_WORLD_SIZE', u'World size'),
			_infos(gc.getNumWorldInfos(), gc.getWorldInfo, _define('', 'WORLDSIZE_STANDARD', 0))),
		('MenuClimate', _t('TXT_KEY_MAIN_MENU_SELECT_CLIMATE', u'Climate'),
			_infos(gc.getNumClimateInfos(), gc.getClimateInfo, _define('STANDARD_CLIMATE', 'CLIMATE_TEMPERATE', 0))),
		('MenuSea', _t('TXT_KEY_MAIN_MENU_SELECT_SEALEVEL', u'Sea level'),
			_infos(gc.getNumSeaLevelInfos(), gc.getSeaLevelInfo, _define('STANDARD_SEALEVEL', 'SEALEVEL_MEDIUM', 0))),
		('MenuEra', _t('TXT_KEY_PEDIA_CATEGORY_ERA', u'Era'),
			_infos(gc.getNumEraInfos(), gc.getEraInfo, _define('STANDARD_ERA', 'ERA_ANCIENT', 0))),
		('MenuSpeed', _t('TXT_KEY_MAIN_MENU_SELECT_GAME_SPEED', u'Game speed'),
			_infos(gc.getNumGameSpeedInfos(), gc.getGameSpeedInfo, _define('STANDARD_GAMESPEED', 'GAMESPEED_NORMAL', 0))),
		('MenuHandicap', _t('TXT_KEY_MAIN_MENU_SELECT_DIFFICULTY', u'Difficulty'),
			_infos(gc.getNumHandicapInfos(), gc.getHandicapInfo, _define('STANDARD_HANDICAP', 'HANDICAP_NOBLE', 0))),
	]
	picked = _state['picked']
	ry = y + 60
	for name, label, items in rows:
		screen.setLabel(name + 'Label', 'Background', u'<font=2>' + label + u'</font>', 1 << 0, x + 30, ry + 4, 0,
			_cpe.FontTypes.GAME_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
		screen.addDropDownBoxGFC(name, x + w - 340, ry, 310, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1, _cpe.FontTypes.GAME_FONT)
		keep = picked.get(name, -1)
		for k, (text, data, default) in enumerate(items):
			screen.addPullDownString(name, text, k, data, (k == keep) if keep >= 0 else default)
		ry += 50
	if _state['message']:
		_label(screen, 'MenuNewMessage', u'<color=255,96,96>' + _state['message'] + u'</color>', W / 2, y + h - 100, 2, 1 << 2)
	_button(screen, 'MenuLaunch', _t('TXT_KEY_MAIN_MENU_LAUNCH', u'Launch!'), x + w - 230, y + h - 64, 200)
	_button(screen, 'MenuBack', _t('TXT_KEY_MAIN_MENU_GO_BACK', u'Go Back'), x + 30, y + h - 64, 200)


def _saves():
	out = []
	for folder, label in _state['folders']:
		try:
			names = os.listdir(folder)
		except OSError:
			continue
		for n in names:
			if n.lower().endswith('.civbeyondswordsave'):
				p = os.path.join(folder, n)
				try:
					out.append((p, n[:-len('.CivBeyondSwordSave')], label, os.path.getmtime(p)))
				except OSError:
					pass
	out.sort(key=lambda s: -s[3])
	return out


def _loadPage(screen, W, H):
	w, h = 760, min(640, H - 120)
	x, y = (W - w) / 2, (H - h) / 2
	screen.addPanel('MenuLoadPanel', u'', u'', True, False, x, y, w, h, _cpe.PanelStyles.PANEL_STYLE_MAIN)
	screen.setLabel('MenuLoadHeader', 'Background', u'<font=3b>' + _t('TXT_KEY_MAIN_MENU_LOAD_GAME_TITLE', u'Load Game') + u'</font>',
		1 << 2, W / 2, y + 18, 0, _cpe.FontTypes.TITLE_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
	_state['saves'] = _saves()
	screen.addTableControlGFC('MenuSaves', 3, x + 24, y + 56, w - 48, h - 140, True, True, 24, 24, _cpe.TableStyles.TABLE_STYLE_STANDARD)
	screen.setTableColumnHeader('MenuSaves', 0, _t('TXT_KEY_MAIN_MENU_LOADSAVE_FILE_NAME', u'Filename'), w - 48 - 160 - 190)
	screen.setTableColumnHeader('MenuSaves', 1, u'', 160)
	screen.setTableColumnHeader('MenuSaves', 2, _t('TXT_KEY_MAIN_MENU_LOADSAVE_MODIFIED', u'Modified'), 190)
	for path, name, label, modified in _state['saves']:
		row = screen.appendTableRow('MenuSaves')
		screen.setTableText('MenuSaves', 0, row, name, '', _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1, 1 << 0)
		screen.setTableText('MenuSaves', 1, row, label, '', _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1, 1 << 0)
		screen.setTableText('MenuSaves', 2, row, time.strftime('%Y-%m-%d %H:%M', time.localtime(modified)), '',
			_cpe.WidgetTypes.WIDGET_GENERAL, -1, -1, 1 << 0)
	if _state['saves']:
		screen.selectRow('MenuSaves', 0, True)
	if _state['message']:
		screen.setLabel('MenuMessage', 'Background', u'<color=255,96,96>' + _state['message'] + u'</color>', 1 << 2, W / 2, y + h - 100, 0,
			_cpe.FontTypes.GAME_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
	_button(screen, 'MenuLoadGo', _t('TXT_KEY_MAIN_MENU_LOAD_GAME', u'Load Game'), x + w - 230, y + h - 64, 200)
	_button(screen, 'MenuBack', _t('TXT_KEY_MAIN_MENU_GO_BACK', u'Go Back'), x + 30, y + h - 64, 200)


# ---------------------------------------------------------------- the custom game page
#
# The original's Single Player -> Custom Game: the player slots (Human, AI or Closed, each
# with its civilization, leader and team), the world, the map script's own options, the
# victory conditions and the game options. Its choices stay in _state['custom'] (by type
# name, so another mod's are kept as far as they apply) and in customgame.json beside
# openciv4's profile, so the next game starts from them.

def _customFile():
	try:
		import oc4host
		path = oc4host.profile_path()
	except Exception:
		return ''
	return os.path.join(os.path.dirname(path), 'customgame.json') if path else ''


def _readCustomFile():
	path = _customFile()
	if not path:
		return None
	try:
		import json
		f = open(path, 'rb')
		try:
			data = json.loads(f.read())
		finally:
			f.close()
		return data if isinstance(data, dict) else None
	except Exception:
		return None


def _writeCustomFile(c):
	path = _customFile()
	if not path:
		return
	try:
		import json
		data = dict(c)
		# (not how many play: each game starts from the world size's own count)
		data.pop('touched', None)
		data['slots'] = [dict([(k, v) for k, v in slot.items() if k != 'status']) for slot in c['slots']]
		data['mapOptions'] = dict([(m, dict([(str(k), v) for k, v in o.items()])) for m, o in c['mapOptions'].items()])
		f = open(path, 'wb')
		try:
			f.write(json.dumps(data, sort_keys=True, indent=1))
		finally:
			f.close()
	except Exception:
		pass


def _type(count, get, name, fallback):
	"""The index of the info of that type name, else fallback."""
	for i in range(count):
		if get(i).getType() == name:
			return i
	return fallback


def _mapModule(name):
	try:
		return __import__(str(name))
	except Exception:
		return None


def _mapCall(module, fn, args, default):
	f = getattr(module, fn, None) if module is not None else None
	if f is None:
		return default
	try:
		return f(args) if args is not None else f()
	except Exception:
		return default


def _mapOptions(name):
	"""The map script's options, its hidden ones too (only Play Now hides them): (option, its
	name, [(value, text)], default, random)."""
	module = _mapModule(name)
	count = _mapCall(module, 'getNumCustomMapOptions', None, 0) or 0
	out = []
	for i in range(count):
		values = _mapCall(module, 'getNumCustomMapOptionValues', (i,), 0) or 0
		out.append((i, _u(_mapCall(module, 'getCustomMapOptionName', (i,), u'%d' % (i + 1))),
			[(v, _u(_mapCall(module, 'getCustomMapOptionDescAt', (i, v), u'%d' % (v + 1)))) for v in range(values)],
			_mapCall(module, 'getCustomMapOptionDefault', (i,), 0) or 0,
			bool(_mapCall(module, 'isRandomCustomMapOption', (i,), False))))
	return out, bool(_mapCall(module, 'isClimateMap', None, True)), bool(_mapCall(module, 'isSeaLevelMap', None, True))


def _gameOptions():
	gc = _gc()
	out = []
	for i in range(gc.getNumGameOptionInfos()):
		info = gc.getGameOptionInfo(i)
		visible = getattr(info, 'getVisible', None)
		if visible is None or visible():
			out.append(i)
	return out


def _worldRows():
	"""The world's pull downs: (key, label, count, get, default type)."""
	gc = _gc()
	return [
		('size', _t('TXT_KEY_MAIN_MENU_SELECT_WORLD_SIZE', u'World size'), gc.getNumWorldInfos(), gc.getWorldInfo, 'WORLDSIZE_STANDARD'),
		('climate', _t('TXT_KEY_MAIN_MENU_SELECT_CLIMATE', u'Climate'), gc.getNumClimateInfos(), gc.getClimateInfo, 'CLIMATE_TEMPERATE'),
		('sea', _t('TXT_KEY_MAIN_MENU_SELECT_SEALEVEL', u'Sea level'), gc.getNumSeaLevelInfos(), gc.getSeaLevelInfo, 'SEALEVEL_MEDIUM'),
		('era', _t('TXT_KEY_PEDIA_CATEGORY_ERA', u'Era'), gc.getNumEraInfos(), gc.getEraInfo, 'ERA_ANCIENT'),
		('speed', _t('TXT_KEY_MAIN_MENU_SELECT_GAME_SPEED', u'Game speed'), gc.getNumGameSpeedInfos(), gc.getGameSpeedInfo, 'GAMESPEED_NORMAL'),
		('handicap', _t('TXT_KEY_MAIN_MENU_SELECT_DIFFICULTY', u'Difficulty'), gc.getNumHandicapInfos(), gc.getHandicapInfo, 'HANDICAP_NOBLE'),
	]


def _defaultPlayers(sizeType):
	gc = _gc()
	i = _type(gc.getNumWorldInfos(), gc.getWorldInfo, sizeType, -1)
	return gc.getWorldInfo(i).getDefaultPlayers() if i >= 0 else 7


def _openSlots(c, n):
	"""The first n slots open (the human's and n - 1 AIs), the others closed."""
	for p, slot in enumerate(c['slots']):
		if p > 0:
			slot['status'] = 'ai' if p < n else 'closed'


def _customDefaults():
	gc = _gc()
	maps = _state['maps']
	names = [m.lower() for m in maps]
	c = {
		'map': maps[names.index('continents')] if 'continents' in names else (maps[0] if maps else u''),
		'mapOptions': {},
		'victories': [gc.getVictoryInfo(i).getType() for i in range(gc.getNumVictoryInfos())],
		'options': [gc.getGameOptionInfo(i).getType() for i in range(gc.getNumGameOptionInfos()) if gc.getGameOptionInfo(i).getDefault()],
		'slots': [{'status': p == 0 and 'human' or 'ai', 'civ': '', 'leader': '', 'team': p} for p in range(gc.getMAX_CIV_PLAYERS())],
		'touched': False,
		'startPoints': 600,
	}
	for key, label, count, get, default in _worldRows():
		c[key] = default
	_openSlots(c, _defaultPlayers(c['size']))
	saved = _readCustomFile()
	if saved:
		# (what the last game was set up with, as far as this game's XML has it)
		if saved.get('map', u'').lower() in names:
			c['map'] = maps[names.index(saved['map'].lower())]
		for key, label, count, get, default in _worldRows():
			if _type(count, get, saved.get(key), -1) >= 0:
				c[key] = saved[key]
		for key, count, get in (('victories', gc.getNumVictoryInfos(), gc.getVictoryInfo),
				('options', gc.getNumGameOptionInfos(), gc.getGameOptionInfo)):
			if isinstance(saved.get(key), list):
				c[key] = [v for v in saved[key] if _type(count, get, v, -1) >= 0]
		if isinstance(saved.get('mapOptions'), dict):
			for m, o in saved['mapOptions'].items():
				try:
					c['mapOptions'][m] = dict([(int(k), int(v)) for k, v in o.items()])
				except (ValueError, AttributeError, TypeError):
					pass
		slots = saved.get('slots')
		if isinstance(slots, list) and len(slots) == len(c['slots']):
			for p, s in enumerate(slots):
				if not isinstance(s, dict):
					continue
				slot = c['slots'][p]
				if _type(gc.getNumCivilizationInfos(), gc.getCivilizationInfo, s.get('civ'), -1) >= 0:
					slot['civ'] = s['civ']
				if _type(gc.getNumLeaderHeadInfos(), gc.getLeaderHeadInfo, s.get('leader'), -1) >= 0:
					slot['leader'] = s['leader']
				if isinstance(s.get('team'), int) and 0 <= s['team'] < len(c['slots']):
					slot['team'] = s['team']
		if isinstance(saved.get('startPoints'), int) and saved['startPoints'] >= 0:
			c['startPoints'] = saved['startPoints']
	return c


def _custom():
	if _state.get('custom') is None:
		_state['custom'] = _customDefaults()
	return _state['custom']


def _listed(key, make):
	"""The list made once (the game's XML does not change while the menu is up): a mod with
	100+ civilizations would otherwise go through them all for every row at every redraw."""
	lists = _state['lists']
	if key not in lists:
		lists[key] = make()
	return lists[key]


def _civs(human):
	"""(name, type) of the civilizations a slot can play, by name."""
	return _listed(('civs', human), lambda: _makeCivs(human))


def _makeCivs(human):
	gc = _gc()
	out = []
	for i in range(gc.getNumCivilizationInfos()):
		civ = gc.getCivilizationInfo(i)
		if (civ.isPlayable() if human else civ.isAIPlayable()) and i != gc.getDefineINT('BARBARIAN_CIVILIZATION'):
			out.append((civ.getShortDescription(0), civ.getType()))
	out.sort()
	return out


def _civLeaders(civType, anyCiv):
	"""(name, type) of the leaders a civilization can have (all with Lead from any civ), by name."""
	return _listed(('civLeaders', civType, anyCiv), lambda: _makeCivLeaders(civType, anyCiv))


def _makeCivLeaders(civType, anyCiv):
	gc = _gc()
	civ = _type(gc.getNumCivilizationInfos(), gc.getCivilizationInfo, civType, -1)
	barbarian = gc.getDefineINT('BARBARIAN_LEADER')
	out = []
	for l in range(gc.getNumLeaderHeadInfos()):
		if l == barbarian:
			continue
		if anyCiv or (civ >= 0 and gc.getCivilizationInfo(civ).isLeaders(l)):
			out.append((gc.getLeaderHeadInfo(l).getDescription(), gc.getLeaderHeadInfo(l).getType()))
	out.sort()
	return out


def _leadersOf(civTypes):
	"""(name, type) of the leaders of those civilizations, by name."""
	return _listed(('leadersOf', tuple(civTypes)), lambda: _makeLeadersOf(civTypes))


def _makeLeadersOf(civTypes):
	gc = _gc()
	civs = [_type(gc.getNumCivilizationInfos(), gc.getCivilizationInfo, t, -1) for t in civTypes]
	out = []
	for l in range(gc.getNumLeaderHeadInfos()):
		if [c for c in civs if c >= 0 and gc.getCivilizationInfo(c).isLeaders(l)]:
			out.append((gc.getLeaderHeadInfo(l).getDescription(), gc.getLeaderHeadInfo(l).getType()))
	out.sort()
	return out


def _civOfLeader(leaderType, civTypes):
	"""The first of those civilizations the leader leads, or ''."""
	gc = _gc()
	l = _type(gc.getNumLeaderHeadInfos(), gc.getLeaderHeadInfo, leaderType, -1)
	for t in civTypes:
		c = _type(gc.getNumCivilizationInfos(), gc.getCivilizationInfo, t, -1)
		if l >= 0 and c >= 0 and gc.getCivilizationInfo(c).isLeaders(l):
			return t
	return ''


def _leadAnyCiv(c):
	return 'GAMEOPTION_LEAD_ANY_CIV' in c['options']


def _pullDown(screen, name, x, y, w, items, selected):
	"""items: (text, data); the one whose data is selected shown (else the first)."""
	screen.addDropDownBoxGFC(name, x, y, w, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1, _cpe.FontTypes.GAME_FONT)
	for k, (text, data) in enumerate(items):
		screen.addPullDownString(name, text, k, k, data == selected)
	return items


def _label(screen, name, text, x, y, font=2, flags=1 << 0, help=None):
	text = u'<font=%s>%s</font>' % (font, text)
	if help:
		screen.setHelpLabel(name, 'Background', text, flags, x, y, 0, _cpe.FontTypes.GAME_FONT, help)
	else:
		screen.setLabel(name, 'Background', text, flags, x, y, 0, _cpe.FontTypes.GAME_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)


def _setupPanel(screen, name, style, x, y, w, h, scroll=False):
	screen.addPanel(name, u'', u'', True, scroll, x, y, w, h, _cpe.PanelStyles.PANEL_STYLE_EMPTY)
	screen.setStyle(name, style)
	screen.setHitTest(name, _cpe.HitTestTypes.HITTEST_NOHIT)
	if scroll:
		screen._s.get(name).offset = _state.get('customOffsets', {}).get(name, 0)


def _setupParent(screen, name, panel):
	# The stock dropdown and button constructors place widgets at the screen's origin.
	# Give them local coordinates in the scroll panel so the children clip and move as one.
	w = screen._s.get(name)
	screen._s.order.remove(name)
	screen._s.get(panel).children.append(name)
	w.parent, w.mode = panel, 'rel'


def _setupField(screen, panel, name, x, y, width, choices, selected, items):
	_pullDown(screen, name, x, y, width, choices, selected)
	_setupParent(screen, name, panel)
	screen.setStyle(name, '__setupField')
	items[name] = [data for text, data in choices]


def _setupLabel(screen, panel, name, text, x, y, font=2):
	screen.setLabelAt(name, panel, u'<font=%s>%s</font>' % (font, text), 1 << 0, x, y, 0,
		_cpe.FontTypes.GAME_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)


def _setupSection(screen, name, title, x, y, w, h):
	_setupPanel(screen, name + 'Frame', '__setupPanel', x, y, w, h)
	_label(screen, name + 'Title', u'<color=229,207,161>%s</color>' % title, x + 13, y + 10, '2b')
	_setupPanel(screen, name, 'PANEL_STYLE_EMPTY', x + 10, y + 34, w - 20, h - 42, True)


def _setupRoster(screen, x, y, w, h, c, shown, items, random):
	gc = _gc()
	_setupPanel(screen, 'CGRosterFrame', '__setupPanel', x, y, w, h)
	parts = (16, 12, 26, 29, 17)
	gap = 8
	inside = w - 36
	space = inside - 12 - gap * (len(parts) - 1)
	columns, widths = [], []
	for fraction in parts:
		columns.append(6 if not columns else columns[-1] + widths[-1] + gap)
		widths.append(space * fraction / 100)

	_setupPanel(screen, 'CGRosterHeader', '__setupBand', x + 1, y + 1, w - 2, 33)
	titles = (_t('TXT_KEY_MAIN_MENU_PLAYER', u'Player'), _t('TXT_KEY_PITBOSS_TEAM', u'Team'),
		u'Civilization', _t('TXT_KEY_PITBOSS_LEADER', u'Leader'), _t('TXT_KEY_PITBOSS_DIFFICULTY', u'Difficulty'))
	for i, (title, col) in enumerate(zip(titles, columns)):
		_label(screen, 'CGColumn%d' % i, u'<color=229,207,161>%s</color>' % title, x + 20 + col, y + 10, '2b')
	panel = 'CGRoster'
	_setupPanel(screen, panel, 'PANEL_STYLE_EMPTY', x + 10, y + 35, w - 20, h - 42, True)

	human, ai = _civs(True), _civs(False)
	anyCiv = _leadAnyCiv(c)
	teams = [(u'Team %d' % (t + 1), t) for t in range(len(c['slots']))]
	handicap = gc.getHandicapInfo(gc.getDefineINT('AI_HANDICAP')).getDescription()
	for p, slot in enumerate(c['slots'][:shown]):
		ry = p * 32
		row = 'CGRow%d' % p
		style = '__setupHuman' if p == 0 else '__setupBand' if p % 2 else '__setupStripe'
		_setupPanel(screen, row, style, 0, ry, inside, 31)
		_setupParent(screen, row, panel)
		if p == 0:
			_setupLabel(screen, panel, 'CGHuman', _t('TXT_KEY_MAIN_MENU_HUMAN', u'Human'), columns[0] + 7, ry + 8, '2b')
		else:
			_setupField(screen, panel, 'CGStatus%d' % p, columns[0], ry + 4, widths[0],
				[(_t('TXT_KEY_MAIN_MENU_AI', u'AI'), 'ai'), (_t('TXT_KEY_MAIN_MENU_CLOSED', u'Closed'), 'closed')], slot['status'], items)
		if slot['status'] == 'closed':
			for k in range(1, len(columns)):
				_setupLabel(screen, panel, 'CGClosed%d_%d' % (k, p), u'<color=138,151,166>\u2014</color>', columns[k] + 8, ry + 8)
			continue

		civs = human if p == 0 else ai
		leaders = _civLeaders('', True) if anyCiv else _leadersOf([t for n, t in civs])
		for k, key, choices in ((1, 'Team', teams), (2, 'Civ', [(random, '')] + civs), (3, 'Leader', [(random, '')] + leaders)):
			_setupField(screen, panel, 'CG%s%d' % (key, p), columns[k], ry + 4, widths[k], choices, slot[key.lower()], items)
		if p == 0:
			handicaps = [(gc.getHandicapInfo(i).getDescription(), gc.getHandicapInfo(i).getType()) for i in range(gc.getNumHandicapInfos())]
			_setupField(screen, panel, 'CGhandicap', columns[4], ry + 4, widths[4], handicaps, c['handicap'], items)
		else:
			_setupLabel(screen, panel, 'CGDifficulty%d' % p, u'<color=175,187,201>%s</color>' % handicap, columns[4] + 8, ry + 8)


def _setupSettings(screen, width, c, items, random):
	panel = 'CGSettingsPanel'
	mapOptions, climate, sea = _mapOptions(c['map'])
	rows = [('map', _t('TXT_KEY_PITBOSS_MAP', u'Map').rstrip(u': '), [(m, m) for m in _state['maps']], c['map'])]
	short = {'size': ('SIZE', u'Size'), 'climate': ('CLIMATE', u'Climate'), 'sea': ('SEALEVEL', u'Sea Level'),
		'era': ('ERA', u'Era'), 'speed': ('SPEED', u'Speed')}
	for key, label, count, get, default in _worldRows():
		if key == 'handicap' or (key == 'climate' and not climate) or (key == 'sea' and not sea):
			continue
		tag, fallback = short[key]
		label = _t('TXT_KEY_PITBOSS_' + tag, fallback).rstrip(u': ')
		rows.append((key, label, [(get(i).getDescription(), get(i).getType()) for i in range(count)], c[key]))
	chosen = c['mapOptions'].setdefault(c['map'], {})
	for i, name, values, default, canRandom in mapOptions:
		choices = [(t, v) for v, t in values] + ([(random, -1)] if canRandom else [])
		rows.append(('MapOpt%d' % i, name, choices, chosen.get(i, default)))
	labelWidth = min(width / 2, max(oc4screens._textWidth(label) for key, label, choices, selected in rows) + 14)
	ry = 0
	for key, label, choices, selected in rows:
		name = 'CGMap' if key == 'map' else 'CG' + key
		stacked = oc4screens._textWidth(label) > labelWidth - 10
		_setupLabel(screen, panel, name + 'Label', label, 4, ry + 4)
		ddX, ddY = (4, ry + 22) if stacked else (labelWidth, ry)
		_setupField(screen, panel, name, ddX, ddY, width - ddX, choices, selected, items)
		ry += 32 + (22 if stacked else 0)


def _setupChecks(screen, panel, prefix, entries, selected, width, columns):
	gap = 8
	if width < 425:
		columns = 1
	colW = (width - gap * (columns - 1)) / columns
	perColumn = (len(entries) + columns - 1) / columns
	for k, (i, info) in enumerate(entries):
		cx, cy = 2 + (k / perColumn) * (colW + gap), (k % perColumn) * 26
		name = '%s%d' % (prefix, i)
		screen.addCheckBoxGFCAt(panel, name, '', '', cx, cy + 4, 17, 17, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
		screen.setStyle(name, '__setupCheck')
		screen.setState(name, info.getType() in selected)
		label = '%sLabel%d' % (prefix, i)
		title = info.getDescription()
		shown = title
		if oc4screens._textWidth(title) > colW - 29:
			end = len(title) - 1
			while end > 0 and oc4screens._textWidth(title[:end].rstrip() + u'\u2026') > colW - 29:
				end -= 1
			shown = title[:end].rstrip() + u'\u2026'
		screen.setTextAt(label, panel, u'<font=2>%s</font>' % shown, 1 << 0, cx + 25, cy + 5, 0)
		help = info.getHelp() if prefix == 'CGOption' else u''
		screen._s.get(label).help = u'\n'.join(t for t in (title if shown != title else u'', help) if t)


def _customPage(screen, W, H):
	gc = _gc()
	c = _custom()
	_state['customItems'] = items = {}
	w = min(W - 52, 1344)
	x, y = (W - w) / 2, 20
	_setupPanel(screen, 'CGHeader', '__setupPanel', x, y, w, 48)
	_label(screen, 'CGTitle', _t('TXT_KEY_MAIN_MENU_CUSTOM_GAME', u'Custom Game'), x + 15, y + 10, '4b')
	_label(screen, 'CGGame', u'<color=193,201,210>%s</color>' % _state['title'], x + w - 15, y + 15, 2, 1 << 1)

	lastOpen = max([p for p, s in enumerate(c['slots']) if s['status'] != 'closed'] + [0])
	shown = min(len(c['slots']), lastOpen + 2)
	random = _t('TXT_KEY_MAIN_MENU_RANDOM', u'Random')
	rosterY = y + 60
	rosterH = 42 + 32 * min(shown, max(4, H / 96))
	_setupRoster(screen, x, rosterY, w, rosterH, c, shown, items, random)

	gap = 12
	btop, bottom = rosterY + rosterH + gap, H - 78
	bh = bottom - btop
	bw = w - 2 * gap
	settingsW, optionsW = bw * 32 / 100, bw * 45 / 100
	victoriesW = bw - settingsW - optionsW
	optionsX, victoriesX = x + settingsW + gap, x + settingsW + optionsW + 2 * gap

	_setupSection(screen, 'CGSettingsPanel', _t('TXT_KEY_MAIN_MENU_SETTINGS', u'Settings'), x, btop, settingsW, bh)
	_setupSection(screen, 'CGOptionsPanel', _t('TXT_KEY_MAIN_MENU_OPTIONS', u'Options'), optionsX, btop, optionsW, bh)
	_setupSection(screen, 'CGVictoriesPanel', _t('TXT_KEY_MAIN_MENU_VICTORIES', u'Victories'), victoriesX, btop, victoriesW, bh)
	_setupSettings(screen, settingsW - 37, c, items, random)
	_setupChecks(screen, 'CGOptionsPanel', 'CGOption', [(o, gc.getGameOptionInfo(o)) for o in _gameOptions()],
		c['options'], optionsW - 39, 2)
	_setupChecks(screen, 'CGVictoriesPanel', 'CGVictory', [(v, gc.getVictoryInfo(v)) for v in _victories()],
		c['victories'], victoriesW - 39, 1)

	fy = bottom + 12
	_setupPanel(screen, 'CGActions', '__setupPanel', x, fy, w, 46)
	_button(screen, 'MenuBack', _t('TXT_KEY_MAIN_MENU_GO_BACK', u'Go Back'), x + 7, fy + 6, 116, 34)
	screen.setStyle('MenuBack', '__setupField')
	_button(screen, 'MenuCustomLaunch', _t('TXT_KEY_MAIN_MENU_LAUNCH', u'Launch!'), x + w - 149, fy + 6, 142, 34)
	screen.setStyle('MenuCustomLaunch', '__setupPrimary')
	if _state['message']:
		_label(screen, 'CGMessage', u'<color=255,172,139>%s</color>' % _state['message'], W / 2, fy + 14, 2, 1 << 2)
	elif 'GAMEOPTION_ADVANCED_START' in c['options']:
		_label(screen, 'CGStartPointsLabel', u'Advanced Start Base Points', W / 2 - 16, fy + 14, 2, 1 << 1)
		screen.addEditBoxGFC('CGStartPoints', W / 2, fy + 10, 76, 26, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1, _cpe.FontTypes.GAME_FONT)
		screen.setStyle('CGStartPoints', '__setupField')
		screen.setEditBoxString('CGStartPoints', u'%d' % c.get('startPoints', 600))
		screen.setEditBoxMaxCharCount('CGStartPoints', 6, 6)
		screen.setEditBoxCharacters('CGStartPoints', u'0123456789')


def _victories():
	gc = _gc()
	return [v for v in range(gc.getNumVictoryInfos()) if not gc.getVictoryInfo(v).isTargetScore()]


def _keepCustom():
	"""The custom page's widgets into _state['custom']."""
	if _state['page'] != 'custom' or _state.get('custom') is None:
		return
	screen = _screen()
	c = _state['custom']
	items = _state.get('customItems', {})

	def picked(name):
		values = items.get(name)
		k = screen.getSelectedPullDownID(name)
		return values[k] if values is not None and 0 <= k < len(values) else None

	for p, slot in enumerate(c['slots']):
		for key, name in (('status', 'CGStatus%d'), ('civ', 'CGCiv%d'), ('leader', 'CGLeader%d'), ('team', 'CGTeam%d')):
			v = picked(name % p)
			if v is not None:
				slot[key] = v
	v = picked('CGMap')
	if v is not None:
		c['map'] = v
	for key, label, count, get, default in _worldRows():
		v = picked('CG' + key)
		if v is not None:
			c[key] = v
	chosen = c['mapOptions'].setdefault(c['map'], {})
	for name in items:
		if name.startswith('CGMapOpt'):
			v = picked(name)
			if v is not None:
				chosen[int(name[len('CGMapOpt'):])] = v
	gc = _gc()
	shownVictories = set(_victories())
	c['victories'] = [gc.getVictoryInfo(v).getType() for v in range(gc.getNumVictoryInfos())
		if (screen.getCheckBoxState('CGVictory%d' % v) if v in shownVictories else gc.getVictoryInfo(v).getType() in c['victories'])]
	if screen._s.get('CGStartPoints') is not None:
		try:
			points = _u(screen.getEditBoxString('CGStartPoints')).strip()
			c['startPoints'] = min(999999, max(0, int(points or u'0')))
		except (ValueError, TypeError, AttributeError):
			pass
	shown = set(_gameOptions())
	c['options'] = [gc.getGameOptionInfo(o).getType() for o in range(gc.getNumGameOptionInfos())
		if (screen.getCheckBoxState('CGOption%d' % o) if o in shown else gc.getGameOptionInfo(o).getType() in c['options'])]


def _customChanged(function, id):
	"""A pull down or check box of the custom page changed: what follows from it, then the page again."""
	c = _custom()
	size = c['size']
	anyBefore = _leadAnyCiv(c)
	if function in ('CGVictoryLabel', 'CGOptionLabel'):
		# (a click on the text turns its box)
		name = (function == 'CGVictoryLabel' and 'CGVictory%d' or 'CGOption%d') % id
		screen = _screen()
		screen.setState(name, not screen.getCheckBoxState(name))
	_keepCustom()
	if function == 'CGStatus':
		c['touched'] = True
	if c['size'] != size and not c['touched']:
		_openSlots(c, _defaultPlayers(c['size']))
	anyCiv = _leadAnyCiv(c)
	if function == 'CGLeader' and not anyCiv:
		# (a leader chosen: the leader's civilization)
		slot = c['slots'][id]
		if slot['leader']:
			slot['civ'] = _civOfLeader(slot['leader'], [t for n, t in _civs(id == 0)])
	elif function == 'CGCiv':
		# (a civilization chosen: its leader at random)
		c['slots'][id]['leader'] = ''
	if anyCiv and not anyBefore:
		# (Unrestricted Leaders turned on: every leader at random, the civilizations kept)
		for slot in c['slots']:
			slot['leader'] = ''
	# (a leader the civilization cannot have, now: at random)
	anyCiv = _leadAnyCiv(c)
	for slot in c['slots']:
		if slot['leader'] and slot['leader'] not in [t for n, t in _civLeaders(slot['civ'], anyCiv)]:
			slot['leader'] = ''
	_state['message'] = u''
	_draw()


def _customProblem(c, open):
	"""Why the game cannot start so, or u''."""
	gc = _gc()
	if len(open) < 2:
		return u'A game needs at least one AI player.'
	if len(set([s['team'] for p, s in open])) < 2:
		return u'Not every player can be on the same team.'
	if 'GAMEOPTION_ALWAYS_WAR' in c['options'] and 'GAMEOPTION_ALWAYS_PEACE' in c['options']:
		return u'Always War and Always Peace cannot both be on.'
	# (as the engine counts: every random civilization, the human's too, from the AI-playable
	# ones no slot has chosen)
	fixed = set([s['civ'] for p, s in open if s['civ']])
	random = len([1 for p, s in open if not s['civ']])
	if random > len([1 for n, t in _civs(False) if t not in fixed]):
		return u'There are not enough civilizations for that many random ones.'
	return u''


def _customLaunch():
	_keepCustom()
	c = _custom()
	open = [(p, s) for p, s in enumerate(c['slots']) if s['status'] != 'closed']
	problem = _customProblem(c, open)
	if problem:
		_state['message'] = problem
		_draw()
		return
	settings = {'map': c['map'].encode('utf-8')}
	mapOptions, climate, sea = _mapOptions(c['map'])
	for key, label, count, get, default in _worldRows():
		if (key == 'climate' and not climate) or (key == 'sea' and not sea):
			continue
		settings[key] = str(c[key])
	import random
	chosen = c['mapOptions'].get(c['map'], {})
	settings['mapOptions'] = {}
	for i, name, values, default, canRandom in mapOptions:
		v = chosen.get(i, default)
		if v < 0 or v >= len(values):
			v = random.randrange(len(values)) if values else default
		settings['mapOptions'][i] = v
	gc = _gc()
	settings['options'] = dict([(str(gc.getGameOptionInfo(o).getType()), gc.getGameOptionInfo(o).getType() in c['options'])
		for o in range(gc.getNumGameOptionInfos())])
	settings['victories'] = [str(v) for v in c['victories']]
	if 'GAMEOPTION_ADVANCED_START' in c['options']:
		settings['advancedStartPoints'] = c.get('startPoints', 600)
	# (the open slots one after another, as the map scripts count their players; the teams
	# numbered in the order they first come)
	teams = {}
	slots = {}
	for k, (p, s) in enumerate(open):
		slot = {'status': p == 0 and 'human' or 'ai', 'team': teams.setdefault(s['team'], len(teams))}
		if s['civ']:
			slot['civ'] = str(s['civ'])
		if s['leader']:
			slot['leader'] = str(s['leader'])
		slots[k] = slot
	settings['slots'] = slots
	settings['players'] = len(open)
	settings['seed'] = int(time.time() * 1000) & 0x7fffffff
	settings['host'] = _state.get('host', 0)
	_writeCustomFile(c)
	_state['choice'] = ('new', settings)


# ---------------------------------------------------------------- input

def _keepPicks():
	if _state['page'] != 'new':
		return
	screen = _screen()
	for name in _DROPDOWNS:
		_state['picked'][name] = screen.getSelectedPullDownID(name)


def _data(name):
	screen = _screen()
	return screen.getPullDownData(name, screen.getSelectedPullDownID(name))


def _launch():
	gc = _gc()
	_keepPicks()
	settings = {}
	leader = _data('MenuLeader')
	if leader >= 0:
		n, c, l, civ = _leaders()[leader]
		settings['civ'] = gc.getCivilizationInfo(civ).getType()
		settings['leader'] = gc.getLeaderHeadInfo(l).getType()
	m = _data('MenuMap')
	if 0 <= m < len(_state['maps']):
		settings['map'] = _state['maps'][m].encode('utf-8')
	for name, key, get in (('MenuSize', 'size', gc.getWorldInfo), ('MenuClimate', 'climate', gc.getClimateInfo),
			('MenuSea', 'sea', gc.getSeaLevelInfo), ('MenuEra', 'era', gc.getEraInfo), ('MenuSpeed', 'speed', gc.getGameSpeedInfo),
			('MenuHandicap', 'handicap', gc.getHandicapInfo)):
		i = _data(name)
		if i >= 0:
			settings[key] = get(i).getType()
	settings['seed'] = int(time.time() * 1000) & 0x7fffffff
	settings['host'] = _state.get('host', 0)
	_state['choice'] = ('new', settings)


def handleInput(args):
	"""A click (the host's arguments of CvScreensInterface.handleInput)."""
	notify, data, function = args[0], args[1], args[5]
	if _state['page'] == 'custom' and function.startswith('CG'):
		# (a pull down chosen, NOTIFY_LISTBOX_ITEM_SELECTED; a check box or its text clicked)
		if notify == 11 or (notify == 0 and function in ('CGVictory', 'CGOption', 'CGVictoryLabel', 'CGOptionLabel')):
			_customChanged(function, args[3])
			return 1
		return 0
	if notify != 0: # (NOTIFY_CLICKED; a pull down's or a table's choice needs nothing now)
		return 0
	screen = _screen()
	if function == 'MenuNew':
		_state['page'] = 'new'
	elif function == 'MenuCustom':
		_state['message'] = u''
		_state['page'] = 'custom'
	elif function == 'MenuCustomLaunch':
		_customLaunch()
		return 1
	elif function == 'MenuLoad':
		_state['page'] = 'load'
	elif function == 'MenuExit':
		_state['choice'] = ('exit',)
		return 1
	elif function == 'MenuMulti':
		_state['page'] = 'multi'
	elif function == 'MenuMods':
		_state['page'] = 'mods'
	elif function == 'MenuModGo':
		for i, folder in enumerate(_state['mods']):
			if screen.isRowSelected('MenuModList', i):
				_state['choice'] = ('mod', folder)
		return 1
	elif function in ('MenuHostNew', 'MenuHostLoad'):
		_state['humans'] = screen.getPullDownData('MenuHumans', screen.getSelectedPullDownID('MenuHumans'))
		_state['host'] = max(2, _state['humans'])
		_state['page'] = 'new' if function == 'MenuHostNew' else 'load'
	elif function == 'MenuJoin':
		_state['address'] = _u(screen.getEditBoxString('MenuAddress')).strip()
		if _state['address']:
			_state['choice'] = ('join', _state['address'].encode('utf-8'))
		return 1
	elif function == 'MenuBack':
		_state['message'] = u''
		_keepPicks()
		_keepCustom()
		_state['page'] = 'main'
	elif function == 'MenuLaunch':
		_launch()
		return 1
	elif function == 'MenuLoadGo':
		for i, s in enumerate(_state['saves']):
			if screen.isRowSelected('MenuSaves', i):
				_state['choice'] = ('load', _u(s[0]).encode('utf-8'), _state.get('host', 0))
				return 1
		return 1
	else:
		return 0
	_draw()
	return 1
