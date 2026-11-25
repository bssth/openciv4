## openciv4: the engine-side classes of CvPythonExtensions.
##
## In the original game Civ4BeyondSword.exe adds these to the module the DLL publishes
## into. CyFractal wraps a DLL object and is registered from C++ (EngineClasses.cpp);
## everything here is plain Python. Classes the headless host has no use for yet are
## stubs: each method returns 0, and outside graphics/UI also logs its first call (with
## OC4_STUB_LOG set, the graphics and UI ones too: what the scripts still miss).

import re
import sys
import oc4host
import CvPythonExtensions as _cpe


# Like the original engine: print goes to PythonDbg.log, errors also to PythonErr.log.
class _LogWriter(object):
	def __init__(self, logName, echo=None):
		self.logName = logName
		self.echo = echo

	def write(self, text):
		oc4host.log(self.logName, text)
		if self.echo:
			self.echo.write(text)

	def flush(self):
		pass

sys.stdout = _LogWriter('PythonDbg.log')
sys.stderr = _LogWriter('PythonErr.log', sys.__stderr__)


# The original event scripts sort (value, city) tuples. On a tie Python 2 compares the objects
# by memory address, so a game's outcome would depend on where they happen to live (Influenza
# picks the cities it hits this way). Ties go by the game's own ids instead.
def _orderBy(cls, key):
	# (scripts also test `None == city`: anything else keeps Python's own comparison)
	def compare(a, b):
		if isinstance(a, cls) and isinstance(b, cls):
			return cmp(key(a), key(b))
		return cmp(id(a), id(b))
	cls.__cmp__ = compare

_orderBy(_cpe.CyCity, lambda c: (c.getOwner(), c.getID()))
_orderBy(_cpe.CyPlot, lambda p: (p.getX(), p.getY()))
_orderBy(_cpe.CyPlayer, lambda p: p.getID())


class _Stub(object):
	_silent = False
	_logAll = __import__('os').environ.get('OC4_STUB_LOG') is not None

	def __init__(self, *args, **kwargs):
		pass

	def __getattr__(self, name):
		if name.startswith('__'):
			raise AttributeError(name)
		what = type(self).__name__ + '.' + name
		silent = self._silent and not self._logAll
		def method(*args, **kwargs):
			if not silent:
				oc4host.unimplemented(what)
			return 0
		return method


# Globals the scripts use unqualified (from CvPythonExtensions import *).
true = True
false = False

def NiTextOut(text):
	pass


class CyPythonMgr(object):
	def allowDefaultImpl(self):
		oc4host.allow_default_impl()

	# CvAppInterface.init() points sys.stdout/stderr at these (CvUtil.RedirectDebug/Error).
	def debugMsg(self, msg):
		oc4host.log('PythonDbg.log', msg)

	def errorMsg(self, msg):
		oc4host.log('PythonErr.log', msg)
		sys.__stderr__.write(msg.encode('utf-8', 'replace') if isinstance(msg, unicode) else msg)

	debugMsgWide = debugMsg
	errorMsgWide = errorMsg


class CyTranslator(object):
	_tags = re.compile(r'<[^>]*>')

	def getText(self, key, args=()):
		return oc4host.get_text(key, args)

	def getObjectText(self, key, form):
		return oc4host.get_object_text(key, form)

	def getColorText(self, key, args, color):
		return self.changeTextColor(self.getText(key, args), color)

	def changeTextColor(self, text, color):
		# The DLL's SETCOLR/ENDCOLR markup with the colour info's RGBA in 0..255.
		c = _cpe.CyGlobalContext().getColorInfo(color).getColor()
		return u"<color=%d,%d,%d,%d>%s</color>" % (int(c.r * 255), int(c.g * 255), int(c.b * 255), int(c.a * 255), text)

	def stripHTML(self, text):
		return self._tags.sub(u'', text)


class FracVals(object):
	FRAC_WRAP_X = 0x0001
	FRAC_WRAP_Y = 0x0002
	FRAC_PERCENT = 0x0004
	FRAC_POLAR = 0x0008
	FRAC_CENTER_RIFT = 0x0010
	FRAC_INVERT_HEIGHTS = 0x0020
	DEFAULT_FRAC_X_EXP = 7
	DEFAULT_FRAC_Y_EXP = 6

_cpe.CyFractal.FracVals = FracVals


# Mods' scripts compare getNotifyCode() with 0 (a click) and 11 (a list item chosen); the other
# codes only need to differ.
class NotifyCode(object):
	NOTIFY_CLICKED, NOTIFY_LISTBOX_ITEM_SELECTED = 0, 11

for _value, _name in zip([v for v in range(20) if v not in (0, 11)], ['CHARACTER', 'CURSOR_MOVE_OFF', 'CURSOR_MOVE_ON',
		'DBL_CLICKED', 'FLYOUT_ITEM_SELECTED', 'FOCUS', 'LINKEXECUTE', 'MOUSEMOVE', 'MOUSEWHEELDOWN', 'MOUSEWHEELUP', 'MOVIE_DONE',
		'NEW_HORIZONTAL_STOP', 'NEW_VERTICAL_STOP', 'SCROLL_DOWN', 'SCROLL_UP', 'SLIDER_NEWSTOP', 'TABLE_HEADER_SELECTED', 'UNFOCUS']):
	setattr(NotifyCode, 'NOTIFY_' + _name, _value)


class MouseFlags(object):
	pass

# Distinct bits: scripts test them with & and use them as dictionary keys.
for _i, _name in enumerate(['CLICKED', 'CONTROL', 'DBLCLICKED', 'EVENT', 'LBUTTON', 'LBUTTONDBLCLK', 'LBUTTONDOWN', 'LBUTTONUP',
		'MBUTTON', 'MBUTTONDBLCLK', 'MBUTTONDOWN', 'MBUTTONUP', 'MOUSEMOVE', 'MOUSEWHEELDOWN', 'MOUSEWHEELUP', 'RBUTTON',
		'RBUTTONDBLCLK', 'RBUTTONDOWN', 'RBUTTONUP', 'RELEASED', 'SHIFT', 'STATE', 'XBUTTON1', 'XBUTTON2', 'XBUTTONDBLCLK',
		'XBUTTONDOWN', 'XBUTTONUP']):
	setattr(MouseFlags, 'MOUSE_' + _name, 1 << _i)


class InputTypes(object):
	pass

# The SDK's FInputDevice::InputType, the codes kbdEvent hands the scripts.
for _i, _name in enumerate(['ESCAPE'] + [str(d) for d in range(10)] + ['MINUS'] + [chr(c) for c in range(ord('A'), ord('Z') + 1)] + (
		'EQUALS BACKSPACE TAB LBRACKET RBRACKET RETURN LCONTROL SEMICOLON APOSTROPHE GRAVE LSHIFT BACKSLASH COMMA PERIOD SLASH '
		'RSHIFT NUMPADSTAR LALT SPACE CAPSLOCK F1 F2 F3 F4 F5 F6 F7 F8 F9 F10 F11 F12 NUMLOCK SCROLL NUMPAD0 NUMPAD1 NUMPAD2 '
		'NUMPAD3 NUMPAD4 NUMPAD5 NUMPAD6 NUMPAD7 NUMPAD8 NUMPAD9 NUMPADMINUS NUMPADPLUS NUMPADPERIOD NUMPADEQUALS AT UNDERLINE COLON '
		'NUMPADENTER RCONTROL VOLUMEDOWN VOLUMEUP NUMPADCOMMA NUMPADSLASH SYSRQ RALT PAUSE HOME UP PGUP LEFT RIGHT END DOWN PGDN '
		'INSERT DELETE').split()):
	setattr(InputTypes, 'KB_' + _name, _i + 1)


# Engine classes the host does not implement yet. Graphics and UI have nothing to do
# headless; the others log their first use.
for _name in ['CyAudioGame', 'CyCamera', 'CyEngine', 'CyGFlyoutMenu', 'CyGInterfaceScreen', 'CyGTabCtrl', 'CyGlobeLayerManager',
		'CyInterface', 'CyInterfaceScreen', 'CyPopup', 'CyPopupInfo', 'CyUnitEntity']:
	globals()[_name] = type(_name, (_Stub,), {'_silent': True})
for _name in ['CyDiplomacy', 'CyMessageControl', 'CyPitboss', 'CyUserProfile', 'CyVariableSystem', 'CyGet']:
	globals()[_name] = type(_name, (_Stub,), {})


class CyStatistics(object):
	"""A player's game statistics as the game core records them from its events (CvStatistics, saved
	with the game): units built, killed and lost by type, buildings built, cities built and razed,
	religions founded, golden ages. Players never alive have none (asking would make a record)."""

	def _get(self, iPlayer, index):
		"""Value number index of the game core's report (oc4host.player_statistics): 0 victory, 1 time
		played, 2 leader, 3 cities built, 4 razed, 5 golden ages, then built, killed, lost for each
		unit, built for each building, founded for each religion."""
		gc = _cpe.CyGlobalContext()
		iPlayer = int(iPlayer)
		if index < 0 or iPlayer < 0 or iPlayer >= gc.getMAX_PLAYERS() or not gc.getPlayer(iPlayer).isEverAlive():
			return 0
		values = oc4host.player_statistics(iPlayer)
		if len(values) != 6 + 3 * gc.getNumUnitInfos() + gc.getNumBuildingInfos() + gc.getNumReligionInfos():
			return 0
		return values[index] if index < len(values) else 0

	def _unit(self, iUnit, offset):
		return 6 + 3 * int(iUnit) + offset if 0 <= int(iUnit) < _cpe.CyGlobalContext().getNumUnitInfos() else -1

	def getPlayerNumUnitsBuilt(self, iPlayer, iUnit):
		return self._get(iPlayer, self._unit(iUnit, 0))

	def getPlayerNumUnitsKilled(self, iPlayer, iUnit):
		return self._get(iPlayer, self._unit(iUnit, 1))

	def getPlayerNumUnitsLost(self, iPlayer, iUnit):
		return self._get(iPlayer, self._unit(iUnit, 2))

	def getPlayerNumBuildingsBuilt(self, iPlayer, iBuilding):
		gc = _cpe.CyGlobalContext()
		if not 0 <= int(iBuilding) < gc.getNumBuildingInfos():
			return 0
		return self._get(iPlayer, 6 + 3 * gc.getNumUnitInfos() + int(iBuilding))

	def getPlayerNumCitiesBuilt(self, iPlayer):
		return self._get(iPlayer, 3)

	def getPlayerNumCitiesRazed(self, iPlayer):
		return self._get(iPlayer, 4)

	def getPlayerNumGoldenAges(self, iPlayer):
		return self._get(iPlayer, 5)

	def getPlayerReligionFounded(self, iPlayer, iReligion):
		gc = _cpe.CyGlobalContext()
		if not 0 <= int(iReligion) < gc.getNumReligionInfos():
			return False
		return self._get(iPlayer, 6 + 3 * gc.getNumUnitInfos() + gc.getNumBuildingInfos() + int(iReligion)) != 0

	def getPlayerTimePlayed(self, iPlayer):
		return self._get(iPlayer, 1)

# No tech splash popups without a screen.
CyInterface.noTechSplash = lambda self: True


for _name in ['true', 'false', 'NiTextOut', 'CyPythonMgr', 'CyStatistics', 'CyTranslator', 'NotifyCode', 'MouseFlags', 'InputTypes'] +[n for n, v in globals().items() if isinstance(v, type) and issubclass(v, _Stub) and v is not _Stub]:
	if not hasattr(_cpe, _name):
		setattr(_cpe, _name, globals()[_name])


# CyMap.rebuild as the WorldBuilder's scripts call it (CvWBDesc.applyMap): the wraps as 0 or 1,
# no custom map options as None; the binding takes bools and an int there.
if hasattr(_cpe, 'CyMap') and hasattr(_cpe.CyMap, 'rebuild'):
	_cyMapRebuild = _cpe.CyMap.rebuild
	def _rebuild(self, iGridW, iGridH, iTop, iBottom, bWrapX, bWrapY, eWorldSize, eClimate, eSeaLevel, iNumCustom=0, aeCustom=None):
		return _cyMapRebuild(self, int(iGridW), int(iGridH), int(iTop), int(iBottom), bool(bWrapX), bool(bWrapY), int(eWorldSize), int(eClimate),
			int(eSeaLevel), int(iNumCustom or 0) if aeCustom is not None else 0, 0)
	_cpe.CyMap.rebuild = _rebuild
