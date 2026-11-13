# _winreg for builds of openciv4 off Windows (registered under that name only there; on Windows
# Python's own module is used). Mods' scripts ask the registry one question that matters, where
# "My Documents" is (BUG's BugPath, MapScriptTools), and expect a WindowsError for the rest.
# The answer is the engine's documents folder: OC4_DOCUMENTS, else ~/Documents, as for its saves.
import os

HKEY_CLASSES_ROOT = 0x80000000
HKEY_CURRENT_USER = 0x80000001
HKEY_LOCAL_MACHINE = 0x80000002
HKEY_USERS = 0x80000003
HKEY_CURRENT_CONFIG = 0x80000005

REG_NONE = 0
REG_SZ = 1
REG_EXPAND_SZ = 2
REG_BINARY = 3
REG_DWORD = 4

KEY_READ = 0x20019
KEY_QUERY_VALUE = 0x0001


class error(EnvironmentError):
	pass

WindowsError = error


def _documents():
	return os.environ.get("OC4_DOCUMENTS") or os.path.join(os.environ.get("HOME", "."), "Documents")

_SHELL_FOLDERS = ("software\\microsoft\\windows\\currentversion\\explorer\\shell folders",
	"software\\microsoft\\windows\\currentversion\\explorer\\user shell folders")
_VALUES = {}
for _key in _SHELL_FOLDERS:
	_VALUES[(HKEY_CURRENT_USER, _key, "personal")] = lambda: (_documents(), REG_SZ)


def _notFound():
	return error(2, "The system cannot find the file specified")


class HKEYType(object):
	def __init__(self, root, path):
		self.root = root
		self.path = path

	def Close(self):
		pass

	def Detach(self):
		return 0

	def __enter__(self):
		return self

	def __exit__(self, *args):
		return False


def OpenKey(key, sub_key, reserved=0, sam=KEY_READ):
	if isinstance(key, HKEYType):
		root, path = key.root, key.path + "\\" + sub_key
	else:
		root, path = key, sub_key
	path = path.strip("\\").lower()
	for (r, p, n) in _VALUES:
		if r == root and p == path:
			return HKEYType(root, path)
	raise _notFound()

OpenKeyEx = OpenKey


def QueryValueEx(key, value_name):
	f = _VALUES.get((key.root, key.path, (value_name or "").lower()))
	if f is None:
		raise _notFound()
	return f()


def QueryValue(key, sub_key):
	raise _notFound()


def EnumKey(key, index):
	raise _notFound()


def EnumValue(key, index):
	raise _notFound()


def CloseKey(key):
	pass


def ExpandEnvironmentStrings(s):
	return os.path.expandvars(s)
