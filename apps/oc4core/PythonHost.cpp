#include "PythonHost.h"

#include "core/FileUtil.h"

#include <cstdio>
#include <stdexcept>

// Exported by the game DLL (CvDLLPython.cpp).
__declspec(dllimport) void DLLPublishToPython();

namespace oc4::host
{
	namespace
	{
		PythonHost* g_host = nullptr;

		std::string lower(std::string s)
		{
			for (char& c : s)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}

		// ---------------------------------------------------------------- oc4host module

		PyObject* pyFind(PyObject*, PyObject* args)
		{
			const char* name = nullptr;
			if (!PyArg_ParseTuple(args, "s", &name))
				return nullptr;
			const auto* s = g_host ? g_host->findScript(name) : nullptr;
			if (!s)
				Py_RETURN_NONE;
			return Py_BuildValue("(si)", s->location.display().c_str(), s->package ? 1 : 0);
		}

		PyObject* pySource(PyObject*, PyObject* args)
		{
			const char* name = nullptr;
			if (!PyArg_ParseTuple(args, "s", &name))
				return nullptr;
			const auto* s = g_host ? g_host->findScript(name) : nullptr;
			if (!s)
			{
				PyErr_SetString(PyExc_ImportError, name);
				return nullptr;
			}
			const std::string src = g_host->readScript(*s);
			return PyString_FromStringAndSize(src.data(), Py_ssize_t(src.size()));
		}

		PyObject* pyAllowDefaultImpl(PyObject*, PyObject*)
		{
			PythonHost::allowDefaultImpl();
			Py_RETURN_NONE;
		}

		PyMethodDef g_methods[] = {
			{ "find", pyFind, METH_VARARGS, "find(name) -> (filename, is_package) or None" },
			{ "source", pySource, METH_VARARGS, "source(name) -> module source" },
			{ "allow_default_impl", pyAllowDefaultImpl, METH_NOARGS, "let the DLL run its default implementation" },
			{ nullptr, nullptr, 0, nullptr },
		};

		// Game scripts import each other by bare name from any Python sub-folder.
		constexpr const char* kImporter = R"py(
import sys, imp, oc4host

class _Oc4AssetImporter(object):
    def find_module(self, fullname, path=None):
        if oc4host.find(fullname) is not None:
            return self
        return None

    def load_module(self, fullname):
        if fullname in sys.modules:
            return sys.modules[fullname]
        filename, ispkg = oc4host.find(fullname)
        source = oc4host.source(fullname).replace('\r\n', '\n')
        mod = imp.new_module(fullname)
        mod.__file__ = filename
        mod.__loader__ = self
        if ispkg:
            mod.__path__ = []
        sys.modules[fullname] = mod
        try:
            exec compile(source, filename, 'exec') in mod.__dict__
        except:
            del sys.modules[fullname]
            raise
        return mod

sys.meta_path.append(_Oc4AssetImporter())
if not hasattr(sys, 'argv'):
    sys.argv = ['']
)py";

#ifndef _WIN32
		// Windows reads a file opened in text mode ('r') with its CRLF line ends as LF.
		// Mods ship such files and rely on it: BUG's Domestic Advisor pickles its pages
		// into UserSettings/CustomDomAdv/CustomDomAdv.txt with CRLF ends, and pickle.load
		// fails on the '\r' elsewhere, so BUG fell back to its built-in pages.
		constexpr const char* kTextModeOpen = R"py(
import __builtin__
def _oc4_text_open(name, mode='r', buffering=-1, _open=__builtin__.open):
    if mode in ('r', 'rt'):
        mode = 'rU'
    return _open(name, mode, buffering)
__builtin__.open = _oc4_text_open
del _oc4_text_open
)py";
#endif

		template <class T, class Convert>
		bool toList(PyObject* o, std::vector<T>& out, Convert convert)
		{
			PyObject* seq = PySequence_Fast(o, "expected a sequence");
			if (!seq)
			{
				PyErr_Clear();
				return false;
			}
			const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
			out.clear();
			out.reserve(size_t(n));
			for (Py_ssize_t i = 0; i < n; ++i)
				out.push_back(convert(PySequence_Fast_GET_ITEM(seq, i)));
			Py_DECREF(seq);
			return true;
		}
	}

	PythonHost::PythonHost(const AssetResolver& assets) : m_assets(assets) { g_host = this; }

	PythonHost::~PythonHost()
	{
		if (g_host == this)
			g_host = nullptr;
	}

	void PythonHost::indexScripts()
	{
		for (const auto& loc : m_assets.list(".py"))
		{
			if (!loc.key.starts_with("python/"))
				continue;
			const std::string file = loc.key.substr(loc.key.rfind('/') + 1);
			if (file == "__init__.py")
			{
				const std::string dir = loc.key.substr(0, loc.key.rfind('/'));
				m_modules.try_emplace(dir.substr(dir.rfind('/') + 1), Script{ loc, true });
			}
			else
				m_modules.try_emplace(file.substr(0, file.size() - 3), Script{ loc, false });
		}
		for (const auto& dir : m_mapScriptDirs)
		{
			std::error_code ec;
			for (const auto& e : std::filesystem::directory_iterator(dir, ec))
			{
				if (!e.is_regular_file() || lower(utf8Path(e.path().extension())) != ".py")
					continue;
				AssetLocation loc;
				loc.key = "publicmaps/" + lower(utf8Path(e.path().filename()));
				loc.file = e.path();
				m_modules.try_emplace(lower(utf8Path(e.path().stem())), Script{ loc, false });
			}
		}
	}

	void PythonHost::addMapScriptDir(const std::filesystem::path& dir) { m_mapScriptDirs.push_back(dir); }

	const PythonHost::Script* PythonHost::findScript(const std::string& moduleName) const
	{
		std::string name = lower(moduleName);
		if (const size_t dot = name.rfind('.'); dot != std::string::npos)
			name = name.substr(dot + 1); // pkg.module: modules are found by bare name
		const auto it = m_modules.find(name);
		return it == m_modules.end() ? nullptr : &it->second;
	}

	std::string PythonHost::readScript(const Script& script) const
	{
		const auto bytes = m_assets.read(script.location);
		return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	}

	void* PythonHost::releaseInterpreter() { return Py_IsInitialized() ? PyEval_SaveThread() : nullptr; }

	void PythonHost::takeInterpreter(void* state)
	{
		if (state)
			PyEval_RestoreThread(static_cast<PyThreadState*>(state));
	}

	int PythonHost::enterThread() { return Py_IsInitialized() ? int(PyGILState_Ensure()) + 1 : 0; }

	void PythonHost::leaveThread(int entered)
	{
		if (entered)
			PyGILState_Release(PyGILState_STATE(entered - 1));
	}

	void PythonHost::start(const std::filesystem::path& pythonHome)
	{
		if (m_started)
			return;
		indexScripts();
		m_home = pythonHome.string();
		Py_NoSiteFlag = 1;
		Py_IgnoreEnvironmentFlag = 1; // a PYTHONHASHSEED or PYTHONPATH of an installed Python would change dict order or modules
		Py_SetPythonHome(m_home.data());
		Py_SetProgramName(const_cast<char*>("oc4core"));
		Py_Initialize();
		PyEval_InitThreads(); // (the game also runs on a thread of its own: TurnWorker)
		if (!m_executable.empty())
		{
			PyObject* exe = PyString_FromString(m_executable.c_str());
			PySys_SetObject(const_cast<char*>("executable"), exe);
			Py_DECREF(exe);
		}
		Py_InitModule("oc4host", g_methods);
		// python27.dll has its own CRT and stdio buffers: make its stdout unbuffered
		// so script output interleaves with the host's in order.
		PyRun_SimpleString("import sys, os\nsys.stdout = sys.__stdout__ = os.fdopen(1, 'w', 0)\nsys.stderr = sys.__stderr__ = os.fdopen(2, 'w', 0)\n");
		if (PyRun_SimpleString(kImporter) != 0)
			throw std::runtime_error("failed to install the asset importer");
#ifndef _WIN32
		if (PyRun_SimpleString(kTextModeOpen) != 0)
			throw std::runtime_error("failed to install the text-mode open");
#endif

		// The DLL publishes its classes into CvPythonExtensions.
		PyImport_AddModule("CvPythonExtensions");
		try
		{
			DLLPublishToPython();
		}
		catch (const std::exception& e)
		{
			throw std::runtime_error(std::string("DLLPublishToPython failed: ") + e.what());
		}
		m_started = true;
	}

	bool PythonHost::run(const std::string& source)
	{
		return PyRun_SimpleString(source.c_str()) == 0;
	}

	PyObject* PythonHost::intern(const char* name)
	{
		const auto it = m_interned.find(std::string_view(name));
		if (it != m_interned.end())
			return it->second;
		PyObject* s = PyString_InternFromString(name); // kept for the life of the interpreter
		m_interned.emplace(name, s);
		return s;
	}

	PyObject* PythonHost::MakeFunctionArgs(void** args, int argc)
	{
		// Entry points take the argument tuple as their single parameter (def fn(argsList)),
		// so the call arguments are a 1-tuple holding it. A "generic" event embeds such a
		// tuple in its own arguments, which is why its handler unpacks argsList[0][0].
		PyObject* tuple = PyTuple_New(argc);
		for (int i = 0; i < argc; ++i)
			PyTuple_SET_ITEM(tuple, i, static_cast<PyObject*>(args[i])); // steals the references
		PyObject* callArgs = PyTuple_New(1);
		PyTuple_SET_ITEM(callArgs, 0, tuple);
		return callArgs;
	}

	bool PythonHost::moduleExists(const char* moduleName, bool bLoadIfNecessary)
	{
		if (!moduleName || !*moduleName)
			return false;
		if (PyObject* modules = PyImport_GetModuleDict(); PyDict_GetItemString(modules, moduleName))
			return true;
		if (!findScript(moduleName))
			return false;
		if (!bLoadIfNecessary)
			return true;
		PyObject* m = PyImport_ImportModule(moduleName);
		if (!m)
		{
			PyErr_Print();
			return false;
		}
		Py_DECREF(m);
		return true;
	}

	PyObject* PythonHost::call(const char* moduleName, const char* fxnName, void* fxnArg, bool& found)
	{
		// The argument tuple comes from CyArgsList::makeFunctionArgs() and is owned by the call.
		struct ArgsRef
		{
			PyObject* p;
			~ArgsRef() { Py_XDECREF(p); }
		} owned{ static_cast<PyObject*>(fxnArg) };

		found = false;
		if (!m_started || !moduleName || !*moduleName || !fxnName)
			return nullptr;
		s_usingDefaultImpl = false;
		// The DLL calls into Python thousands of times a turn: take loaded modules straight
		// from sys.modules and look names up as interned strings.
		PyObject* module = PyDict_GetItem(PyImport_GetModuleDict(), intern(moduleName)); // borrowed
		if (module)
			Py_INCREF(module);
		else if (!(module = PyImport_ImportModule(moduleName)))
		{
			PyErr_Print();
			return nullptr;
		}
		PyObject* fn = PyObject_GetAttr(module, intern(fxnName));
		Py_DECREF(module);
		if (!fn)
		{
			PyErr_Clear(); // optional callbacks are allowed to be missing
			return nullptr;
		}
		found = true;
		PyObject* args = static_cast<PyObject*>(fxnArg); // from MakeFunctionArgs, or null
		PyObject* result = PyObject_CallObject(fn, args && PyTuple_Check(args) ? args : nullptr);
		Py_DECREF(fn);
		if (!result)
			PyErr_Print();
		return result;
	}

	bool PythonHost::callFunction(const char* moduleName, const char* fxnName, void* fxnArg)
	{
		bool found = false;
		PyObject* r = call(moduleName, fxnName, fxnArg, found);
		Py_XDECREF(r);
		return r != nullptr;
	}

	bool PythonHost::callFunction(const char* moduleName, const char* fxnName, void* fxnArg, long* result)
	{
		bool found = false;
		PyObject* r = call(moduleName, fxnName, fxnArg, found);
		if (!r)
			return false;
		// Always written, the way the original engine converts results (PyInt_AsLong):
		// callbacks that fall off their end return None, which becomes -1, i.e. true.
		// The DLL tests such results without initialising them (e.g. the PythonCanDoCity
		// checks of event triggers), so leaving *result alone would read stack garbage.
		const long v = PyInt_AsLong(r);
		if (result)
			*result = v;
		PyErr_Clear();
		Py_DECREF(r);
		return true;
	}

	bool PythonHost::callFunction(const char* moduleName, const char* fxnName, void* fxnArg, CvString* result)
	{
		bool found = false;
		PyObject* r = call(moduleName, fxnName, fxnArg, found);
		if (!r)
			return false;
		if (result)
		{
			if (PyString_Check(r))
				*result = PyString_AsString(r);
			else if (PyUnicode_Check(r))
				if (PyObject* b = PyUnicode_AsEncodedString(r, "cp1252", "replace"))
				{
					*result = PyString_AsString(b);
					Py_DECREF(b);
				}
		}
		PyErr_Clear();
		Py_DECREF(r);
		return true;
	}

	bool PythonHost::callFunction(const char* moduleName, const char* fxnName, void* fxnArg, CvWString* result)
	{
		bool found = false;
		PyObject* r = call(moduleName, fxnName, fxnArg, found);
		if (!r)
			return false;
		if (result)
		{
			if (PyUnicode_Check(r))
			{
				std::wstring w(size_t(PyUnicode_GetSize(r)), L'\0');
				PyUnicode_AsWideChar(reinterpret_cast<PyUnicodeObject*>(r), w.data(), Py_ssize_t(w.size()));
				*result = w;
			}
			else if (PyString_Check(r))
				*result = CvWString(std::string(PyString_AsString(r)));
		}
		PyErr_Clear();
		Py_DECREF(r);
		return true;
	}

	bool PythonHost::callFunction(const char* moduleName, const char* fxnName, void* fxnArg, std::vector<byte>* pList)
	{
		bool found = false;
		PyObject* r = call(moduleName, fxnName, fxnArg, found);
		if (!r)
			return false;
		if (pList)
		{
			if (PyString_Check(r)) // pickled script data comes back as a str
				pList->assign(reinterpret_cast<const byte*>(PyString_AsString(r)),
					reinterpret_cast<const byte*>(PyString_AsString(r)) + PyString_Size(r));
			else
				toList(r, *pList, [](PyObject* o) { return static_cast<byte>(PyInt_AsLong(o)); });
		}
		PyErr_Clear();
		Py_DECREF(r);
		return true;
	}

	bool PythonHost::callFunction(const char* moduleName, const char* fxnName, void* fxnArg, std::vector<int>* pIntList)
	{
		bool found = false;
		PyObject* r = call(moduleName, fxnName, fxnArg, found);
		if (!r)
			return false;
		if (pIntList)
			toList(r, *pIntList, [](PyObject* o) { return static_cast<int>(PyInt_AsLong(o)); });
		PyErr_Clear();
		Py_DECREF(r);
		return true;
	}

	bool PythonHost::callFunction(const char* moduleName, const char* fxnName, void* fxnArg, int* pIntList, int* iListSize)
	{
		bool found = false;
		PyObject* r = call(moduleName, fxnName, fxnArg, found);
		if (!r)
			return false;
		std::vector<int> values;
		toList(r, values, [](PyObject* o) { return static_cast<int>(PyInt_AsLong(o)); });
		// The caller's array is sized by *iListSize on input.
		const int capacity = iListSize ? *iListSize : 0;
		const int n = std::min<int>(int(values.size()), capacity > 0 ? capacity : int(values.size()));
		for (int i = 0; i < n && pIntList; ++i)
			pIntList[i] = values[size_t(i)];
		if (iListSize)
			*iListSize = n;
		PyErr_Clear();
		Py_DECREF(r);
		return true;
	}

	bool PythonHost::callFunction(const char* moduleName, const char* fxnName, void* fxnArg, std::vector<float>* pFloatList)
	{
		bool found = false;
		PyObject* r = call(moduleName, fxnName, fxnArg, found);
		if (!r)
			return false;
		if (pFloatList)
			toList(r, *pFloatList, [](PyObject* o) { return static_cast<float>(PyFloat_AsDouble(o)); });
		PyErr_Clear();
		Py_DECREF(r);
		return true;
	}

	bool PythonHost::callPythonFunction(const char* szModName, const char* szFxnName, int iArg, long* result)
	{
		PyObject* arg = PyInt_FromLong(iArg);
		void* args[] = { arg };
		return callFunction(szModName, szFxnName, MakeFunctionArgs(args, 1), result);
	}
}
