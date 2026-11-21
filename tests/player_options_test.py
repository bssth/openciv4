# Checks that the player options (Options > Game: Wait at End of Turn, Quick Combat, ...) are kept in
# OpenCiv4's profile (apps/oc4core/python/oc4options.py) as the original keeps them in its player
# profile: a choice on the options screen is written at once, and after a restart the host has the
# profile's options again (the game core sends them to the active player of every game, new or
# loaded). Also that the game's options screen gets OpenCiv4's own options on a tab of their own.
#
# oc4options runs here with a stand-in for the host (oc4host: the profile's player options as
# Utility holds them) and for the modules it imports.
#   python3 player_options_test.py <apps/oc4core/python>

import importlib
import os
import shutil
import sys
import tempfile
import types

failures = []
DEFAULTS = {0: True, 1: True, 2: True} # (CIV4PlayerOptionInfos.xml: advisor pop-ups, Sid's tips, wait at end of turn)
NUM = 22


def check(ok, what):
	if not ok:
		print("FAILED: %s" % what)
		failures.append(what)


class Anything(types.ModuleType):
	"""A module whose missing names are do-nothing functions."""

	def __getattr__(self, name):
		if name.startswith('__'):
			raise AttributeError(name)
		return lambda *args, **kwargs: None


class Host(Anything):
	"""oc4host: where the profile is, and Utility's profile player options (0 default, 1 off, 2 on)."""

	def __init__(self, path):
		Anything.__init__(self, 'oc4host')
		self.path = path
		self.states = [0] * 64
		self.player = {} # (the active player's options, as sendPlayerOption sets them)

	def profile_path(self):
		return self.path

	def set_player_option(self, i, on):
		self.states[i] = 2 if on else 1

	def reset_player_options(self):
		self.states = [0] * 64

	def player_option_state(self, i):
		return self.states[i]


class Info(object):
	def __init__(self, i):
		self.i = i

	def getDefault(self):
		return DEFAULTS.get(self.i, False)


def cpe(host):
	m = Anything('CvPythonExtensions')

	class Context(object):
		def getPlayerOptionsInfoByIndex(self, i): return Info(i)
		def getNumPlayerOptionInfos(self): return NUM

	class MessageControl(object):
		def sendPlayerOption(self, i, on):
			# (Utility::sendPlayerOption: into the profile, then to the active player)
			host.set_player_option(i, on)
			host.player[i] = bool(on)

	m.CyGlobalContext = Context
	m.CyMessageControl = MessageControl
	m.TabGroupTypes = types.SimpleNamespace(TABGROUP_GAME=0, TABGROUP_GRAPHICS=1, TABGROUP_AUDIO=2, TABGROUP_CLOCK=3)
	return m


def start(folder):
	"""The program starting: a new host, oc4options loaded afresh (it reads the profile)."""
	host = Host(os.path.join(folder, 'profile.ini'))
	sys.modules['oc4host'] = host
	sys.modules['CvPythonExtensions'] = cpe(host)
	screens = Anything('oc4screens')
	screens.beforeDisplay = []
	screens._text = lambda s: u"%s" % (s,)
	sys.modules['oc4screens'] = screens
	# (the game's callbacks module: CvOptionsScreenCallbackInterface.handleGameOptionsClicked)
	callbacks = types.ModuleType('optionscallbacks')

	def handleGameOptionsClicked(args):
		on, name = args
		sys.modules['CvPythonExtensions'].CyMessageControl().sendPlayerOption(int(name[name.find('_') + 1:]), on)
		return 1
	callbacks.handleGameOptionsClicked = handleGameOptionsClicked
	sys.modules['optionscallbacks'] = callbacks
	sys.modules.pop('oc4options', None)
	return host, importlib.import_module('oc4options')


def profileLines(folder):
	with open(os.path.join(folder, 'profile.ini')) as f:
		return set(f.read().split())


def optionsScreen(options):
	"""The game's options screen as CvOptionsScreen.py builds it (its tabs, two player options)."""
	ctrl = options.CyGTabCtrl('Options', False, False)
	for form, label in (('GameForm', 'Game'), ('GraphicsForm', 'Graphics'), ('AudioForm', 'Audio'), ('OtherForm', 'Other')):
		ctrl.attachTabItem(form, label)
	ctrl.attachVBox('GameForm', 'GameVBox')
	ctrl.attachPanel('GameVBox', 'GamePanelCenter')
	profile = options._UserProfile()
	for i in (2, 6):
		name = 'GameOptionCheckBox_%d' % i
		ctrl.attachCheckBox('GamePanelCenter', name, 'option %d' % i, 'optionscallbacks', 'handleGameOptionsClicked', name, profile.getPlayerOption(i))
	ctrl.attachHBox('GamePanelCenter', 'LangHBox')
	return ctrl


def click(options, ctrl, name):
	n = ctrl._byName[name]
	n.value = not n.value
	options._call(n, (n.value, n.callback[2]))


def main():
	sys.path.insert(0, sys.argv[1])
	folder = tempfile.mkdtemp()
	try:
		# No profile yet: the XML's defaults, nothing for the host.
		host, options = start(folder)
		profile = options._UserProfile()
		check(profile.getPlayerOption(2) and not profile.getPlayerOption(6), "no profile: the XML's defaults")
		check(host.states[:NUM] == [0] * NUM, "no profile: the host has none chosen")

		# The screen's tabs: the game's four, then OpenCiv4's with its three options.
		ctrl = optionsScreen(options)
		check([label for form, label in ctrl._tabs] == ['Game', 'Graphics', 'Audio', 'Other', 'OpenCiv4'], "tabs: %s" % [l for f, l in ctrl._tabs])
		oc4 = [n.name for n in ctrl._nodes.values() if n.kind == 'checkbox' and n.name.startswith('Oc4')]
		check(sorted(oc4) == sorted([options.WASD_PAN_CHECKBOX, options.AUTO_END_TURN_CHECKBOX, options.EDGE_SCROLL_CHECKBOX]), "OpenCiv4's options: %s" % oc4)
		game = [n.name for n in ctrl._byName['GamePanelCenter'].children]
		check(game == ['GameOptionCheckBox_2', 'GameOptionCheckBox_6', 'LangHBox'], "the Game tab has only the game's: %s" % game)

		# Wait at End of Turn off, Quick Moves on: sent to the player and kept at once.
		click(options, ctrl, 'GameOptionCheckBox_2')
		click(options, ctrl, 'GameOptionCheckBox_6')
		check(host.player == {2: False, 6: True}, "sent to the active player: %s" % host.player)
		lines = profileLines(folder)
		check({'player.2=0', 'player.6=1'} <= lines, "in profile.ini at once: %s" % sorted(lines))
		check(not profile.getPlayerOption(2) and profile.getPlayerOption(6), "CyUserProfile reads them back")

		# A restart (a new game, or a save loaded: the game core sends the host's to the player).
		host, options = start(folder)
		check(host.states[2] == 1 and host.states[6] == 2, "after a restart the host has the profile's (%s)" % host.states[:8])
		profile = options._UserProfile()
		check(not profile.getPlayerOption(2) and profile.getPlayerOption(6) and profile.getPlayerOption(0), "and the screen shows them")

		# Restore Defaults on the Game tab: the XML's, to the profile and the player.
		profile.resetOptions(0)
		options._save()
		check(host.player.get(2) is True and host.player.get(6) is False, "defaults sent to the player: %s" % host.player)
		host, options = start(folder)
		profile = options._UserProfile()
		check(profile.getPlayerOption(2) and not profile.getPlayerOption(6), "defaults kept after a restart")

		# A profile written before this change: the XML's defaults, the rest kept.
		with open(os.path.join(folder, 'profile.ini'), 'w') as f:
			f.write('name=openciv4\nedgeScroll=0\n')
		host, options = start(folder)
		check(host.states[:NUM] == [0] * NUM, "an older profile: none chosen")
		check(options._profile['edgeScroll'] is False, "the other keys still read")

		# A profile the old options screen wrote (no version): its "off" for the options on by
		# default (it showed them off) gives way to the default once; the other choices stay.
		with open(os.path.join(folder, 'profile.ini'), 'w') as f:
			f.write('name=openciv4\nplayer.0=0\nplayer.1=0\nplayer.2=0\nplayer.6=1\nplayer.13=0\n')
		host, options = start(folder)
		profile = options._UserProfile()
		check(profile.getPlayerOption(2) and profile.getPlayerOption(0) and profile.getPlayerOption(6) and not profile.getPlayerOption(13),
			"an old screen's profile: Wait at End of Turn on again, Quick Moves kept")
		check(host.states[2] == 0 and host.states[6] == 2, "... and the host has those (%s)" % host.states[:8])
		options._save()
		lines = profileLines(folder)
		check('version=2' in lines and 'player.6=1' in lines and 'player.2=0' not in lines, "saved with its version: %s" % sorted(lines))
		# Once: then Wait at End of Turn off is the player's choice and stays.
		click(options, optionsScreen(options), 'GameOptionCheckBox_2')
		host, options = start(folder)
		check(not options._UserProfile().getPlayerOption(2), "after that, off is kept")
	finally:
		shutil.rmtree(folder, ignore_errors=True)

	if failures:
		print("player_options_test: %d failed" % len(failures))
		return 1
	print("player_options_test: all passed")
	return 0


if __name__ == '__main__':
	sys.exit(main())
