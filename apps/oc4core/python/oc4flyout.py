## The plot's menu (the engine's "flyout"): Shift and a left click on the map, as the original's.
## The game core lists its items (CvGame::startFlyoutMenu: the city's production, Move To, Select
## All, Wake / Fortify / Sleep All, the units there) and does the one chosen
## (CvGame::applyFlyoutMenu, through oc4host.flyout_choose); this draws them over the main interface.
##
## Seen in Wine (backlog #113, testgames/shots/wine-stacksel/ss-m10.png and wine-stacksel-2/,
## 1600x900): the list's top left corner at the mouse, 222 x 288, rows 28 apart starting 13 down,
## the text 35 in, ten rows shown and a scroll bar for more, on the dark blue panel of the main
## interface, no stripes. A city's production is one row, "Change Production", first: the game
## core's items between that title and the next empty one are its submenu (sg-10.png).

import CvPythonExtensions as _cpe
import oc4host

_W, _H = 222, 288
_shown = []     # the widgets up
_rows = {}      # list name -> what each row does: an item's index, or ('sub', [indices], title)
_at = [0, 0, 0] # where the menu is: x, y, height


def _screen():
	import CvScreenEnums
	return _cpe.CyGInterfaceScreen('MainInterface', CvScreenEnums.MAIN_INTERFACE)


def _list(screen, panel, name, x, y, rows):
	"""A panel with its rows at x, y (kept on the screen); returns its height."""
	xr, yr = screen.getXResolution(), screen.getYResolution()
	h = min(_H, 16 + 28 * len(rows))
	# (OpenCiv4's: kept on the screen, left of or above the mouse at the right or bottom edge;
	# the original's at an edge is not seen yet)
	if x + _W > xr:
		x = max(0, x - _W)
	if y + h > yr:
		y = max(0, y - h)
	screen.addPanel(panel, u'', u'', True, False, x, y, _W, h, _cpe.PanelStyles.PANEL_STYLE_MAIN)
	screen.addListBoxGFC(name, u'', x + 22, y + 8, _W - 30, h - 16, _cpe.TableStyles.TABLE_STYLE_EMPTY)  # (no stripes, as Wine's)
	box = screen._s.get(name)
	if box is not None:
		box.rowHeight = 28
	for k, (text, what) in enumerate(rows):
		screen.appendListBoxString(name, text, _cpe.WidgetTypes.WIDGET_GENERAL, k, -1)
	_rows[name] = [what for (text, what) in rows]
	_shown.extend([panel, name])
	return x, y, h


def show(items, x, y):
	"""items: (text, kind) in the game core's order, kind 0 an item to do, 1 a submenu's title,
	2 its end; x, y: the mouse."""
	hide()
	rows, sub = [], None
	for i, (text, kind) in enumerate(items):
		if kind == 1:
			sub = (text, [])
			rows.append((text, ('sub', sub[1], text)))
		elif kind == 2:
			sub = None
		elif sub is not None:
			sub[1].append(i)
		else:
			rows.append((text, i))
	if not rows:
		return
	_items[:] = [text for (text, kind) in items]
	_at[:] = list(_list(_screen(), 'OC4FlyoutPanel', 'OC4FlyoutList', x, y, rows))


_items = []


def _hideSub():
	screen = _screen()
	for name in ('OC4FlyoutSubPanel', 'OC4FlyoutSubList'):
		if name in _shown:
			screen.deleteWidget(name)
			_shown.remove(name)
	_rows.pop('OC4FlyoutSubList', None)


def hide():
	"""True if the menu was up."""
	if not _shown:
		return False
	screen = _screen()
	for name in _shown:
		screen.deleteWidget(name)
	del _shown[:]
	_rows.clear()
	return True


def isShown():
	return bool(_shown)


def handleInput(argsList):
	"""A row chosen (oc4screens' input tuple: notify at 0, the row at 1, the function name at 5)."""
	notify, data, function = argsList[0], argsList[1], argsList[5]
	if notify != 11:  # NOTIFY_LISTBOX_ITEM_SELECTED
		return 1
	name = 'OC4FlyoutSubList' if str(function).startswith('OC4FlyoutSubList') else 'OC4FlyoutList'
	rows = _rows.get(name, [])
	if not 0 <= int(data) < len(rows):
		return 1
	what = rows[int(data)]
	if isinstance(what, tuple):
		# A submenu's title: its items in a list beside the menu, level with the row (opened by a
		# click here; whether the original opens it on the mouse resting on it is not seen yet).
		_hideSub()
		x, y, h = _at
		_list(_screen(), 'OC4FlyoutSubPanel', 'OC4FlyoutSubList', x + _W, y + 28 * int(data),
			[(_items[i], i) for i in what[1]])
		return 1
	hide()
	oc4host.flyout_choose(int(what))
	return 1
