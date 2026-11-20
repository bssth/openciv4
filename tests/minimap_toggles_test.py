# Checks that the minimap's toggles the original keeps in its player profile (the plot grid, the
# bare map, the yields, the scores) are kept in OpenCiv4's profile (apps/oc4core/python/oc4options.py):
# switched in one run, they come back after a restart and with a loaded game.
#
# oc4options runs here with a stand-in for the host (oc4host: the toggles as the C++ side holds
# them, Screens::frame telling the profile when one switched) and for the modules it imports.
#   python3 minimap_toggles_test.py <apps/oc4core/python>

import importlib
import os
import shutil
import sys
import tempfile
import types

failures = []


def check(ok, what):
	if not ok:
		print("FAILED: %s" % what)
		failures.append(what)


class Anything(types.ModuleType):
	"""A module whose missing names are do-nothing functions (the host calls oc4options makes
	that this test does not look at)."""

	def __getattr__(self, name):
		if name.startswith('__'):
			raise AttributeError(name)
		return lambda *args, **kwargs: None


class Host(Anything):
	"""oc4host: where the profile is, and the toggles as the game's controls leave them."""

	def __init__(self, path):
		Anything.__init__(self, 'oc4host')
		self.path = path
		self.toggles = [False, False, False, True] # (the C++ side's own start: Interface.h, Utility.h)
		self.profileToggles = None # (Screens.cpp's g_profileToggles: none before the profile is read)

	def profile_path(self):
		return self.path

	def map_toggles(self):
		return list(self.toggles)

	def set_map_toggles(self, grid, bareMap, yields, scores):
		self.toggles = [grid, bareMap, yields, scores]

	def load_map_toggles(self, grid, bareMap, yields, scores):
		self.toggles = [grid, bareMap, yields, scores]
		self.profileToggles = list(self.toggles)

	def control(self, i):
		"""A toggle's button or key (CvGame::doControl), then the next frame (Screens::frame)."""
		self.toggles[i] = not self.toggles[i]
		if self.toggles != self.profileToggles:
			self.profileToggles = list(self.toggles)
			sys.modules['oc4options'].setMapToggles(*self.toggles)


def start(folder):
	"""The program starting: a new host, oc4options loaded afresh (it reads the profile)."""
	host = Host(os.path.join(folder, 'profile.ini'))
	sys.modules['oc4host'] = host
	sys.modules['CvPythonExtensions'] = Anything('CvPythonExtensions')
	screens = Anything('oc4screens')
	screens.beforeDisplay = []
	sys.modules['oc4screens'] = screens
	sys.modules.pop('oc4options', None)
	return host, importlib.import_module('oc4options')


def profileLines(folder):
	with open(os.path.join(folder, 'profile.ini')) as f:
		return set(f.read().split())


def main():
	sys.path.insert(0, sys.argv[1])
	folder = tempfile.mkdtemp()
	try:
		# A first start, no profile yet: the original's defaults (the scores shown, nothing else).
		host, options = start(folder)
		check(host.toggles == [False, False, False, True], "no profile: grid, bare map, yields off, scores on")
		check(not os.path.exists(os.path.join(folder, 'profile.ini')), "nothing written before a toggle")
		profile = options._UserProfile()
		check((profile.getGrid(), profile.getMap(), profile.getYields(), profile.getScores()) == (False, False, False, True),
			"CyUserProfile reads the toggles as they are")

		# The grid and the yields on, the scores off: kept at once (the original writes its .pfl at each click).
		host.control(0)
		host.control(2)
		host.control(3)
		lines = profileLines(folder)
		check({'grid=1', 'map=0', 'yields=1', 'scores=0'} <= lines, "the toggles are in profile.ini at once: %s" % sorted(lines))
		check(profile.getGrid() and profile.getYields() and not profile.getScores() and not profile.getMap(), "CyUserProfile follows the controls")

		# A restart (and a loaded game: OpenCiv4 starts afresh to load one): the profile's toggles again.
		host, options = start(folder)
		check(host.toggles == [True, False, True, False], "after a restart: grid and yields on, scores off (%s)" % host.toggles)

		# The game's toggles switched in the loaded game (the bare map on, the grid off), and the
		# profile read again (CyUserProfile.readFromFile, as the options screen's profile does):
		# the toggles as the player left them, not as the game started.
		host.control(1)
		host.control(0)
		host.toggles = [False, False, False, True] # (the host's own start, say a new game)
		options._UserProfile().readFromFile('')
		check(host.toggles == [False, True, True, False], "readFromFile gives the host the profile's toggles (%s)" % host.toggles)

		# The other options are kept beside them.
		check({'scores=0', 'edgeScroll=1'} <= profileLines(folder), "the rest of the profile is kept")

		# A profile written before this change (no toggle keys): the defaults.
		with open(os.path.join(folder, 'profile.ini'), 'w') as f:
			f.write('name=openciv4\nclock=1\n')
		host, options = start(folder)
		check(host.toggles == [False, False, False, True], "an older profile: the defaults (%s)" % host.toggles)
	finally:
		shutil.rmtree(folder, ignore_errors=True)

	if failures:
		print("minimap_toggles_test: %d failed" % len(failures))
		return 1
	print("minimap_toggles_test: all passed")
	return 0


if __name__ == '__main__':
	sys.exit(main())
