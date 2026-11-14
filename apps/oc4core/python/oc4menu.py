## openciv4: a temporary main menu.
##
## The original's main menu is the engine's own (Civ4BeyondSword.exe draws it; the scripts have
## no part in it), so this one stands in for it until it is made: a new game (the leader, the
## map script, the world, the difficulty and the speed from the XML), a saved game, or the way
## out. It is a screen like the game's (oc4screens' widgets), drawn before any game exists;
## the host reads what was chosen with takeChoice().

import os
import sys
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
	'message': u'', # why the last choice could not be played (shown on the load page)
	'picked': {},  # the new game page's choices, kept across pages: dropdown name -> index
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
	(folder, label) pairs where saved games are; with a message (a save that could not be
	loaded) on the load page, saying it."""
	_state['title'] = _u(title)
	_state['maps'] = sorted([_u(m) for m in maps], key=lambda m: m.lower())
	_state['folders'] = [(_u(f), _u(l)) for f, l in folders]
	_state['mods'] = list(mods)
	_state['choice'] = None
	_state['message'] = _u(message)
	_state['page'] = page if page in ('load', 'pitboss') else ('load' if message else 'main')
	_draw()


def relayout():
	_draw()


def close():
	oc4screens.reset(MENU)
	oc4screens._screens.pop(MENU, None)
	oc4screens._changed()


def takeChoice():
	"""None while the player chooses; then ('new', {settings}), ('load', utf-8 path, humans to host for or 0),
	('join', address), ('mod', its folder) or ('exit',)."""
	c = _state['choice']
	_state['choice'] = None
	return c


def escape():
	if _state['page'] != 'main':
		_keepPicks()
		_state['page'] = 'main'
		_draw()
	return True


# ---------------------------------------------------------------- pages

def _draw():
	screen = _screen()
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
	screen.addPanel('MenuTitleBar', u'', u'', True, False, 0, 0, W, 56, _cpe.PanelStyles.PANEL_STYLE_TOPBAR)
	screen.setLabel('MenuTitle', 'Background', u'<font=4b>' + _state['title'] + u'</font>', 1 << 2, W / 2, 14, 0,
		_cpe.FontTypes.TITLE_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
	screen.setLabel('MenuVersion', 'Background', u'<font=1>openciv4 - temporary menu</font>', 1 << 1, W - 12, H - 24, 0,
		_cpe.FontTypes.SMALL_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
	{'main': _mainPage, 'new': _newPage, 'load': _loadPage, 'multi': _multiPage, 'pitboss': _pitbossPage, 'mods': _modsPage}[_state['page']](screen, W, H)


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
	w, h = 360, 354
	x, y = (W - w) / 2, (H - h) / 2
	_state['host'] = 0
	screen.addPanel('MenuMainPanel', u'', u'', True, False, x, y, w, h, _cpe.PanelStyles.PANEL_STYLE_MAIN)
	screen.setLabel('MenuMainHeader', 'Background', u'<font=3b>' + _t('TXT_KEY_MAIN_MENU_SINGLE_PLAYER', u'Single Player') + u'</font>',
		1 << 2, W / 2, y + 18, 0, _cpe.FontTypes.TITLE_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
	_button(screen, 'MenuNew', _t('TXT_KEY_MAIN_MENU_NEW_GAME', u'New Game'), x + 30, y + 60, w - 60)
	_button(screen, 'MenuLoad', _t('TXT_KEY_MAIN_MENU_LOAD_GAME', u'Load Game'), x + 30, y + 112, w - 60)
	_button(screen, 'MenuMulti', _t('TXT_KEY_MAIN_MENU_MULTIPLAYER', u'Multiplayer'), x + 30, y + 164, w - 60)
	_button(screen, 'MenuMods', _t('TXT_KEY_MAIN_MENU_LOAD_MOD', u'Load a Mod'), x + 30, y + 216, w - 60)
	_button(screen, 'MenuExit', _t('TXT_KEY_MAIN_MENU_EXIT_GAME', u'Exit Game'), x + 30, y + 284, w - 60)


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
	w, h = 520, 430
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
	if sys.platform.startswith('linux'):
		_button(screen, 'MenuPitboss', u'Join a PitBoss server', x + 30, y + 270, w - 60)
	_button(screen, 'MenuJoin', u'Join', x + w - 230, y + h - 64, 200)
	_button(screen, 'MenuBack', _t('TXT_KEY_MAIN_MENU_GO_BACK', u'Go Back'), x + 30, y + h - 64, 200)


def _pitbossPage(screen, W, H):
	w, h = 560, 400
	x, y = (W - w) / 2, (H - h) / 2
	screen.addPanel('MenuPitbossPanel', u'', u'', True, False, x, y, w, h, _cpe.PanelStyles.PANEL_STYLE_MAIN)
	screen.setLabel('MenuPitbossHeader', 'Background', u'<font=3b>Join PitBoss</font>', 1 << 2, W / 2, y + 18, 0,
		_cpe.FontTypes.TITLE_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
	for n, label, yy, text in (('MenuPitbossAddress', u'Server', 76, _state.get('address', u'127.0.0.1:2056')),
		('MenuPitbossPassword', u'Seat password', 184, u'')):
		screen.setLabel(n + 'Label', 'Background', u'<font=2>' + label + u'</font>', 1 << 0, x + 30, y + yy + 4, 0,
			_cpe.FontTypes.GAME_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
		screen.addEditBoxGFC(n, x + 210, y + yy, w - 240, 32, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1, _cpe.FontTypes.GAME_FONT)
		screen.setEditBoxString(n, text)
	screen.setEditBoxPassword('MenuPitbossPassword')
	screen.setLabel('MenuPitbossSeatLabel', 'Background', u'<font=2>Assigned seat (0-17)</font>', 1 << 0, x + 30, y + 134, 0,
		_cpe.FontTypes.GAME_FONT, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1)
	screen.addDropDownBoxGFC('MenuPitbossSeat', x + 210, y + 130, w - 240, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1, _cpe.FontTypes.GAME_FONT)
	for seat in range(18):
		screen.addPullDownString('MenuPitbossSeat', unicode(seat), seat, seat, seat == _state.get('seat', 0))
	screen.addMultilineText('MenuPitbossMessage', u'<font=2>' + (_state['message'] or u'Use the seat and password supplied by the operator. Your civilization stays human while you are away.') + u'</font>',
		x + 30, y + 236, w - 60, 76, _cpe.WidgetTypes.WIDGET_GENERAL, -1, -1, 1 << 0)
	_button(screen, 'MenuPitbossGo', u'Connect', x + w - 230, y + h - 64, 200)
	_button(screen, 'MenuBack', u'Go Back', x + 30, y + h - 64, 200)


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
	screen.setLabel('MenuNewHeader', 'Background', u'<font=3b>' + _t('TXT_KEY_MAIN_MENU_NEW_GAME', u'New Game') + u'</font>',
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
	if notify != 0: # (NOTIFY_CLICKED; a pull down's or a table's choice needs nothing now)
		return 0
	screen = _screen()
	if function == 'MenuNew':
		_state['page'] = 'new'
	elif function == 'MenuLoad':
		_state['page'] = 'load'
	elif function == 'MenuExit':
		_state['choice'] = ('exit',)
		return 1
	elif function == 'MenuMulti':
		_state['page'] = 'multi'
	elif function == 'MenuMods':
		_state['page'] = 'mods'
	elif function == 'MenuPitboss':
		_state['page'] = 'pitboss'
	elif function == 'MenuPitbossGo':
		_state['address'] = _u(screen.getEditBoxString('MenuPitbossAddress')).strip()
		_state['seat'] = screen.getPullDownData('MenuPitbossSeat', screen.getSelectedPullDownID('MenuPitbossSeat'))
		password = _u(screen.getEditBoxString('MenuPitbossPassword'))
		if _state['address'] and password:
			_state['choice'] = ('pitboss', _state['address'].encode('utf-8'), _state['seat'], password.encode('utf-8'))
			screen.setEditBoxString('MenuPitbossPassword', u'')
			return 1
		_state['message'] = u'Enter the server address and your seat password.'
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
