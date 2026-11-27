# Checks the options' Other tab (apps/oc4core/python/oc4options.py) against what the original does
# (measured in the Wine game, docs/options-other.md):
# - profiles: one file each (Profiles/<name>.ini beside profile.ini, the original's Profiles\<name>.pfl),
#   the loaded one named in profile.ini (the original's CivilizationIV.ini UserProfile); Create New
#   Profile writes the options under the new name and loads it, the drop-down loads another (its
#   options to the sounds, the map and the game core), Delete Profile deletes a file (the loaded one:
#   the first in the list is loaded); OpenCiv4's own options stay whichever is loaded;
# - an older profile.ini (every option in it) becomes the profile "openciv4";
# - the clock: setClockOn marks it just turned on (BUG's clock starts over), 24 Hour Clock;
# - the alarm: set so many minutes ahead, a * before the clock while set, it rings at that minute;
# - the connection speed: greyed with its reason, a click does nothing.
#   python3 options_other_test.py <apps/oc4core/python>

import importlib
import os
import shutil
import sys
import tempfile
import time
import types

failures = []


def check(ok, what):
	if not ok:
		print("FAILED: %s" % what)
		failures.append(what)


class Anything(types.ModuleType):
	def __getattr__(self, name):
		if name.startswith('__'):
			raise AttributeError(name)
		return lambda *args, **kwargs: None


class Host(Anything):
	def __init__(self, path, pattern='HH.mm'):
		Anything.__init__(self, 'oc4host')
		self.path, self.pattern = path, pattern
		self.states = [0] * 64
		self.sounds = []
		self.graphics = {}

	def profile_path(self): return self.path
	def clock_pattern(self): return self.pattern
	def set_player_option(self, i, on): self.states[i] = 2 if on else 1
	def reset_player_options(self): self.states = [0] * 64
	def player_option_state(self, i): return self.states[i]
	def audio_play(self, tag): self.sounds.append(tag)
	def reset_graphic_options(self): self.graphics = {}
	def set_graphic_option(self, i, on): self.graphics[i] = on


popups = []


def cpe(host):
	m = Anything('CvPythonExtensions')

	class Translator(object):
		def getText(self, key, args):
			return key + ('(%s)' % args[0] if args else '')

	m.CyTranslator = Translator
	m.TabGroupTypes = types.SimpleNamespace(TABGROUP_GAME=0, TABGROUP_GRAPHICS=1, TABGROUP_AUDIO=2, TABGROUP_CLOCK=3)
	m.EventContextTypes = types.SimpleNamespace(NO_EVENTCONTEXT=-1)
	m.PopupStates = types.SimpleNamespace(POPUPSTATE_IMMEDIATE=1)
	return m


class Popup(object):
	def __init__(self, *args):
		self.header, self.body = u'', u''
	def setHeaderString(self, s, *args): self.header = s
	def setBodyString(self, s, *args): self.body = s
	def launch(self, *args): popups.append((self.header, self.body))


def start(folder, pattern='HH.mm'):
	host = Host(os.path.join(folder, 'profile.ini'), pattern)
	sys.modules['oc4host'] = host
	sys.modules['CvPythonExtensions'] = cpe(host)
	screens = Anything('oc4screens')
	screens.beforeDisplay = []
	screens._clockText = [None]
	screens._text = lambda s: u"%s" % (s,)
	screens.CyPopup = Popup
	sys.modules['oc4screens'] = screens
	sys.modules.pop('oc4options', None)
	return host, importlib.import_module('oc4options')


def listed(profile):
	"""The drop-down's names, as CvOptionsScreen cuts them from the file names."""
	profile.loadProfileFileNames()
	out = []
	for i in range(profile.getNumProfileFiles()):
		f = profile.getProfileFileName(i)
		out.append(f[f.find("PROFILES\\") + 9:-4])
	return out


def read(path):
	with open(path) as f:
		return set(f.read().split())


def main():
	sys.path.insert(0, sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'apps', 'oc4core', 'python'))
	folder = tempfile.mkdtemp()
	try:
		# An older profile.ini: every option in it; it becomes the profile "openciv4".
		with open(os.path.join(folder, 'profile.ini'), 'w') as f:
			f.write('version=2\nname=openciv4\nclock=1\nwasdPan=1\nvolume.Music=7\nplayer.6=1\n')
		host, options = start(folder)
		profile = options._UserProfile()
		own = os.path.join(folder, 'Profiles', 'openciv4.ini')
		check(os.path.isfile(own), "the old profile.ini's options moved to Profiles/openciv4.ini")
		check({'clock=1', 'volume.Music=7', 'player.6=1'} <= read(own), "with its options: %s" % sorted(read(own)))
		check('clock=1' not in read(os.path.join(folder, 'profile.ini')) and {'UserProfile=openciv4', 'wasdPan=1'} <= read(os.path.join(folder, 'profile.ini')),
			"profile.ini keeps the name and OpenCiv4's own: %s" % sorted(read(os.path.join(folder, 'profile.ini'))))
		check(profile.isClockOn() and profile.getMusicVolume() == 7 and host.states[6] == 2, "and they are loaded")
		check(listed(profile) == ['openciv4'], "one profile listed: %s" % listed(profile))

		# Create New Profile (CvOptionsScreenCallbackInterface.handleNewProfileButtonInput).
		profile.set24Hours(True)
		profile.setProfileName('Second')
		check(profile.writeToFile('Second'), "Create New Profile writes it")
		profile.loadProfileFileNames()
		options._save()
		check(listed(profile) == ['openciv4', 'Second'], "two listed: %s" % listed(profile))
		check('UserProfile=Second' in read(os.path.join(folder, 'profile.ini')), "the new one is the loaded one")
		check({'hours24=1', 'clock=1', 'player.6=1'} <= read(os.path.join(folder, 'Profiles', 'Second.ini')), "with the options as they were")

		# Its own options; then the drop-down's openciv4 (saveProfile, then loadProfile).
		profile.setClockOn(False)
		profile.setMusicVolume(3)
		profile.setGraphicOption(4, True)
		options._UserProfile().setPlayerOption(6, False)
		options._save()
		host.audio = []
		check(profile.readFromFile('openciv4'), "the drop-down loads openciv4")
		check(profile.getProfileName() == 'openciv4' and profile.isClockOn() and not profile.is24Hours() and profile.getMusicVolume() == 7,
			"openciv4's options back")
		check(host.states[6] == 2 and 4 not in host.graphics, "its player and graphic options to the host (%s, %s)" % (host.states[6], host.graphics))
		check(profile.wasClockJustTurnedOn(), "a load marks the clock just turned on")
		check(options._profile['wasdPan'] is True, "OpenCiv4's own option stays")
		check('UserProfile=openciv4' in read(os.path.join(folder, 'profile.ini')), "profile.ini names it")

		# A restart: the loaded one again.
		host, options = start(folder)
		profile = options._UserProfile()
		check(profile.getProfileName() == 'openciv4' and profile.isClockOn(), "after a restart, openciv4 is loaded")
		host, options = start(folder)
		profile = options._UserProfile()
		profile.readFromFile('Second')
		host, options = start(folder)
		profile = options._UserProfile()
		check(profile.getProfileName() == 'Second' and not profile.isClockOn() and profile.getMusicVolume() == 3 and host.states[6] == 1,
			"after a restart, Second is loaded with its options")

		# Delete Profile: one with no file, the loaded one, the last one.
		check(not profile.deleteProfileFile('NoSuch'), "no such profile: nothing deleted")
		check(not profile.writeToFile('a/b') and not profile.writeToFile(''), "a name a file cannot have is not written")
		check(profile.deleteProfileFile('Second'), "the loaded one deleted")
		profile.setProfileName('')
		check(listed(profile) == ['openciv4'], "one left: %s" % listed(profile))
		check(profile.readFromFile('openciv4') and profile.getProfileName() == 'openciv4', "the first loaded then")
		check(not profile.deleteProfileFile('openciv4'), "the last one is kept")

		# The clock.
		profile.setClockJustTurnedOn(False)
		profile.setClockOn(False)
		check(profile.wasClockJustTurnedOn(), "setClockOn marks it just turned on, as the original (off too)")
		lt = time.localtime()
		check(options.clockText() == u'%02d.%02d' % (lt.tm_hour, lt.tm_min), "the user's format: %s" % options.clockText())
		host, options = start(folder, 'h:mm a')
		profile = options._UserProfile()
		profile.set24Hours(False)
		lt = time.localtime()
		check(options.clockText() == u'%d:%02d %s' % ((lt.tm_hour + 11) % 12 + 1, lt.tm_min, 'PM' if lt.tm_hour >= 12 else 'AM'),
			"a 12-hour format: %s" % options.clockText())
		profile.set24Hours(True)
		check(options.clockText() == u'%02d:%02d' % (lt.tm_hour, lt.tm_min), "24 Hour Clock: %s" % options.clockText())

		# The alarm: one minute ahead, from this minute.
		cp = sys.modules['CvPythonExtensions']
		del popups[:]
		cp.toggleAlarm(True, 0, 1)
		at = time.localtime((int(time.time()) // 60 + 1) * 60)
		check(cp.isAlarmOn() and (cp.getAlarmHour(), cp.getAlarmMin()) == (at.tm_hour, at.tm_min), "set: goes off at %d:%02d" % (cp.getAlarmHour(), cp.getAlarmMin()))
		check((cp.getAlarmHourLeft(), cp.getAlarmMinLeft()) == (0, 1), "1 minute left")
		check(popups and popups[-1] == ('TXT_KEY_POPUP_ALARM_TITLE', 'TXT_KEY_MISC_ALARM_WILL_GO_OFF(%d:%02d)' % (at.tm_hour, at.tm_min)),
			"its popup: %s" % popups)
		check(options.clockText().startswith(u'*'), "a * before the clock")
		options._checkAlarm()
		check(cp.isAlarmOn() and 'AS2D_ALARM' not in host.sounds, "not yet")
		options._alarm['at'] = time.time() - 1
		options._checkAlarm()
		check(not cp.isAlarmOn() and host.sounds == ['AS2D_ALARM'] and popups[-1][1].startswith('TXT_KEY_MISC_ALARM_GOES_OFF'),
			"it rings: %s %s" % (host.sounds, popups[-1]))
		check((cp.getAlarmHour(), cp.getAlarmMin(), cp.getAlarmHourLeft(), cp.getAlarmMinLeft()) == (-1, -1, 0, 0), "and is off")
		check(not options.clockText().startswith(u'*'), "no * then")
		cp.toggleAlarm(True, 2, 5)
		check((cp.getAlarmHourLeft(), cp.getAlarmMinLeft()) == (2, 5), "2 hours 5 left")
		cp.toggleAlarm(False, 0, 0)
		check(not cp.isAlarmOn(), "turned off")

		# The connection speed: greyed, with why; a click does nothing.
		ctrl = options.CyGTabCtrl('Options', False, False)
		ctrl.attachTabItem('OtherForm', 'Other')
		ctrl.attachVBox('OtherForm', 'NetPanelVBox')
		ctrl.attachRadioButton('NetPanelVBox', 'BroadbandSelection', 'Broadband', 'cb', 'handleBroadbandSelected', 'BroadbandSelection', True)
		ctrl.attachRadioButton('NetPanelVBox', 'ModemSelection', 'Modem', 'cb', 'handleModemSelected', 'ModemSelection', False)
		for name in ('BroadbandSelection', 'ModemSelection'):
			n = ctrl._byName[name]
			check(n.disabled and 'OpenCiv4' in n.tip, "%s greyed with its reason" % name)
		options._state['ctrl'] = ctrl
		n = ctrl._byName['ModemSelection']
		options.handleInput((0, 0, 0, n.id, 0, 'OptW'))
		check(n.value is False, "a click on Modem does nothing")
	finally:
		shutil.rmtree(folder, ignore_errors=True)

	# A profile and a music folder named in what isn't ASCII, in a folder so named: kept as UTF-8,
	# written whole (no file left beside it).
	folder = tempfile.mkdtemp(suffix=u'_J\u00f8rgen')
	try:
		host, options = start(folder)
		profile = options._UserProfile()
		name = u'\u6771\u4eac'
		profile.setProfileName(name)
		check(profile.writeToFile(name), "a profile named %r written" % name)
		check(os.path.isfile(os.path.join(folder, u'Profiles', name + u'.ini')), "as Profiles/%s.ini" % name)
		check(options._utf8(name) in [options._utf8(n) for n in listed(profile)], "and listed: %r" % listed(profile))
		check(profile.readFromFile(name), "and loaded")
		music = os.path.join(folder, u'\u6771\u4eac musik')
		profile.setMusicPath(music)
		check(profile.getMusicPath() == options._utf8(music), "the music folder kept as UTF-8")
		host, options = start(folder)
		profile = options._UserProfile()
		check(profile.getProfileName() == options._utf8(name) and profile.getMusicPath() == options._utf8(music),
			"both read back after a restart: %r %r" % (profile.getProfileName(), profile.getMusicPath()))
		left = [f for f in os.listdir(folder) + os.listdir(os.path.join(folder, u'Profiles')) if f.endswith(u'.tmp')]
		check(not left, "no file left from writing: %r" % left)
	finally:
		shutil.rmtree(folder, ignore_errors=True)

	if failures:
		print("options_other_test: %d failed" % len(failures))
		return 1
	print("options_other_test: all passed")
	return 0


if __name__ == '__main__':
	sys.exit(main())
