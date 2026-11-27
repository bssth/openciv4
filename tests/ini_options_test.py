# Checks the options screen's OpenCiv4 tab for CivilizationIV.ini's settings the original sets only
# in its ini (apps/oc4core/python/oc4options.py): NoTechSplash, NoEventMessages, AutoSaveInterval,
# MaxAutoSaves (and openciv4's own autosaves), FIELD_OF_VIEW, SetMaxFrameRate. Their defaults are
# the ini's, a choice goes to the host and into profile.ini at once, and a restart reads it back.
#
# oc4options runs with player_options_test's stand-ins for the host and the modules it imports.
#   python3 ini_options_test.py <apps/oc4core/python>

import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import player_options_test as po

check = po.check


def start(folder):
	# (oc4screens' hook for typing into an edit box, in player_options_test's stand-in)
	real = po.Anything
	class Screens(real):
		pass
	po.Anything = Screens
	Screens.editTyped = []
	try:
		host, options = po.start(folder)
	finally:
		po.Anything = real
	host.ini = None
	host.set_ini_options = lambda *args: setattr(host, 'ini', args)
	defines = {'FIELD_OF_VIEW': 42.0}
	context = sys.modules['CvPythonExtensions'].CyGlobalContext
	context.getDefineFLOAT = lambda self, name: defines.get(name, 0.0)
	context.setDefineFLOAT = lambda self, name, value: defines.__setitem__(name, value)
	options._applyIniOptions() # (as at the program's start, the host's stand-in in place now)
	check(host.ini is not None and defines['FIELD_OF_VIEW'] == 42.0, "before a game: the host has them, the defines untouched")
	options.gameStarted()
	return host, options, defines


def typed(options, ctrl, key, text):
	"""Typed into the number box of the profile's key, as oc4screens tells after the keys."""
	n = ctrl._byName['Oc4Edit' + key]

	class Box(object):
		pass
	w = Box()
	w.name, w.text = 'OptW%d' % n.id, text
	options._editTyped(w)


def main():
	sys.path.insert(0, sys.argv[1])
	folder = tempfile.mkdtemp()
	try:
		# No profile: the ini's defaults.
		host, options, defines = start(folder)
		check(host.ini == (False, False, 4, 5, False, 0), "defaults to the host: %s" % (host.ini,))
		check(defines['FIELD_OF_VIEW'] == 42.0, "the XML's field of view kept")
		ctrl = po.optionsScreen(options)
		edits = sorted(n.name for n in ctrl._nodes.values() if n.kind == 'edit')
		check(edits == sorted('Oc4Edit' + k for k in ('autoSaveInterval', 'autoSavesKept', 'fieldOfView', 'maxFrameRate')), "number boxes: %s" % edits)
		check(ctrl._byName['Oc4EditautoSaveInterval'].text == u'4', "the interval box shows 4")

		# Chosen: both check boxes, an autosave every turn, 10 kept, openciv4's, field of view 60, 30 frames.
		po.click(options, ctrl, options.NO_TECH_SPLASH_CHECKBOX)
		po.click(options, ctrl, options.NO_EVENT_MESSAGES_CHECKBOX)
		po.click(options, ctrl, options.OPENCIV4_AUTOSAVES_CHECKBOX)
		typed(options, ctrl, 'autoSaveInterval', u'1')
		typed(options, ctrl, 'autoSavesKept', u'10')
		typed(options, ctrl, 'fieldOfView', u'60')
		typed(options, ctrl, 'maxFrameRate', u'3x0') # (digits only)
		check(host.ini == (True, True, 1, 10, True, 30), "chosen, to the host: %s" % (host.ini,))
		check(defines['FIELD_OF_VIEW'] == 60.0, "the field of view replaced: %s" % defines['FIELD_OF_VIEW'])
		lines = po.profileLines(folder)
		check({'noTechSplash=1', 'noEventMessages=1', 'openciv4AutoSaves=1', 'autoSaveInterval=1', 'autoSavesKept=10',
			'fieldOfView=60', 'maxFrameRate=30'} <= lines, "in profile.ini at once: %s" % sorted(lines))

		# A restart: the profile's again.
		host, options, defines = start(folder)
		check(host.ini == (True, True, 1, 10, True, 30), "after a restart: %s" % (host.ini,))
		check(defines['FIELD_OF_VIEW'] == 60.0, "after a restart, the field of view: %s" % defines['FIELD_OF_VIEW'])

		# An empty box is 0: no autosaves; the field of view the XML's again.
		ctrl = po.optionsScreen(options)
		typed(options, ctrl, 'autoSaveInterval', u'')
		typed(options, ctrl, 'fieldOfView', u'0')
		check(host.ini[2] == 0, "an empty interval: none (%s)" % (host.ini,))
		check(defines['FIELD_OF_VIEW'] == 42.0, "field of view 0: the XML's again (%s)" % defines['FIELD_OF_VIEW'])
	finally:
		shutil.rmtree(folder, ignore_errors=True)

	if po.failures:
		print("ini_options_test: %d failed" % len(po.failures))
		return 1
	print("ini_options_test: all passed")
	return 0


if __name__ == '__main__':
	sys.exit(main())
