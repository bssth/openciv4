#include "Screens.h"

#include "GameFont.h"
#include "Interface.h"
#include "Utility.h"
#include "Sounds.h"

#include "core/FileUtil.h"
#include "media/Movie.h"
#include "render/Input.h"
#include "render/UiScale.h"

#include "CvGameCoreDLL.h"
#include "CvCity.h"
#include "CvDLLWidgetData.h"
#include "CvGameAI.h"
#include "CvInfos.h"
#include "CvMessageControl.h"
#include "CvGameTextMgr.h"
#include "CvMap.h"
#include "CvPlayerAI.h"
#include "CvPlot.h"
#include "CvPopupInfo.h"
#include "CvSelectionGroup.h"
#include "CvTalkingHeadMessage.h"
#include "CvUnit.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/eval.h>

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <utility>
#include <unordered_map>

namespace py = pybind11;

namespace oc4::host
{
	namespace
	{
		std::wstring toWide(py::handle o)
		{
			PyObject* p = o.ptr();
			if (PyUnicode_Check(p))
			{
				std::wstring w(size_t(PyUnicode_GetSize(p)), L'\0');
				PyUnicode_AsWideChar(reinterpret_cast<PyUnicodeObject*>(p), w.data(), Py_ssize_t(w.size()));
				return w;
			}
			if (PyString_Check(p)) // game text is Latin-1 on the Python 2 side
			{
				std::wstring w;
				for (const char* s = PyString_AsString(p); *s; ++s)
					w.push_back(static_cast<unsigned char>(*s));
				return w;
			}
			if (p == Py_None)
				return {};
			return toWide(py::str(o));
		}

		py::object wide(const std::wstring& s)
		{
			return py::reinterpret_steal<py::object>(PyUnicode_FromWideChar(s.data(), Py_ssize_t(s.size())));
		}

		// A script's arguments for a text: numbers, and texts (text keys among them).
		std::vector<TextManager::Arg> textArgs(py::handle args)
		{
			std::vector<TextManager::Arg> out;
			if (args.is_none() || !PySequence_Check(args.ptr()))
				return out;
			for (py::handle h : py::reinterpret_borrow<py::sequence>(args))
			{
				if (PyUnicode_Check(h.ptr()) || PyString_Check(h.ptr()))
					out.emplace_back(toWide(h));
				else if (PyFloat_Check(h.ptr()))
					out.emplace_back(h.cast<double>());
				else
					out.emplace_back(h.cast<long long>());
			}
			return out;
		}

		// The engine's own diplomacy screen (oc4screens.py): its file id, past the scripts' screens.
		constexpr int kDiplomacyScreen = 9000;
		constexpr int kTurnLogScreen = 31001; // python/oc4turnlog.py's
		constexpr int kOptionsScreen = 31002; // python/oc4options.py's

		std::string narrow(py::handle o)
		{
			if (PyString_Check(o.ptr()))
				return PyString_AsString(o.ptr());
			if (PyInt_Check(o.ptr()))
				return std::to_string(PyInt_AsLong(o.ptr()));
			if (PyLong_Check(o.ptr()))
				return std::to_string(PyLong_AsLong(o.ptr()));
			const std::wstring w = toWide(o);
			return std::string(w.begin(), w.end());
		}

		CvUnit* unitOf(int owner, int id)
		{
			if (owner < 0 || owner >= GC.getMAX_PLAYERS())
				return nullptr;
			return CvPlayerAI::getPlayerNonInl(static_cast<PlayerTypes>(owner)).getUnit(id);
		}

		CvCity* cityOf(int owner, int id)
		{
			if (owner < 0 || owner >= GC.getMAX_PLAYERS())
				return nullptr;
			return CvPlayerAI::getPlayerNonInl(static_cast<PlayerTypes>(owner)).getCity(id);
		}

		py::tuple idOf(const CvUnit* u) { return u ? py::make_tuple(int(u->getOwner()), u->getID()) : py::make_tuple(-1, -1); }

		std::wstring g_help; // the help shown now (CyInterface.getHelpString)
		std::function<int(const std::string&, int)> g_textWidth; // how wide a text is in the window
		render::MapWindow::UiDisplay g_display; // the window's, for the scale's choices
		std::function<int(const std::string&, int, int)> g_textHeight; // how high it is, wrapped at a width

		// (before the window is up: its markup left out, a symbol about two letters wide)
		int estimatedWidth(const std::wstring& text)
		{
			int width = 0;
			bool tag = false;
			for (const wchar_t c : text)
				if (c == L'<' || c == L'>')
					tag = c == L'<';
				else if (!tag)
					width += c < 128 ? 7 : 16;
			return width;
		}

		void printPythonError(py::error_already_set& e)
		{
			e.restore();
			PyErr_Print(); // (to the console and PythonErr.log, as the engine's errors go)
		}
	}

	SoundscapePlaces soundscapePlaces(Utility& utility)
	{
		SoundscapePlaces places;
		AudioScripts& audio = utility.audioScripts();
		try
		{
			py::gil_scoped_acquire gil;
			py::object gc = py::module_::import("CvPythonExtensions").attr("CyGlobalContext")();
			const auto types = [&](const char* count, const char* info, std::vector<int>& out) {
				const int n = gc.attr(count)().cast<int>();
				for (int i = 0; i < n; ++i)
				{
					const std::string type = py::str(gc.attr(info)(i).attr("getType")());
					const std::string tag = audio.worldSoundscape(type);
					out.push_back(tag.empty() ? -1 : audio.index(tag, AUDIOTAG_SOUNDSCAPE));
				}
			};
			types("getNumTerrainInfos", "getTerrainInfo", places.terrain);
			types("getNumFeatureInfos", "getFeatureInfo", places.feature);
			types("getNumImprovementInfos", "getImprovementInfo", places.improvement);
			const int eras = gc.attr("getNumEraInfos")().cast<int>();
			for (int e = 0; e < eras; ++e)
			{
				py::object era = gc.attr("getEraInfo")(e);
				std::vector<int> sizes;
				for (int s = 0; s < NUM_CITYSIZE_TYPES; ++s)
					sizes.push_back(era.attr("getCitySoundscapeSciptId")(s).cast<int>());
				places.city.push_back(std::move(sizes));
			}
		}
		catch (py::error_already_set& e)
		{
			printPythonError(e);
		}
		return places;
	}

	void registerScreenFunctions(Utility& utility)
	{
		auto host = py::reinterpret_borrow<py::module_>(PyImport_AddModule("oc4host"));
		Interface& ui = utility.gameInterface();

		// dirty bits, what shows
		host.def("ui_is_dirty", [&ui](int bit) { return ui.isDirty(static_cast<InterfaceDirtyBits>(bit)); });
		host.def("ui_set_dirty", [&ui](int bit, bool value) { ui.setDirty(static_cast<InterfaceDirtyBits>(bit), value); });
		host.def("ui_make_dirty", [&ui] { ui.makeInterfaceDirty(); });
		host.def("ui_show_interface", [&ui] { return int(ui.getShowInterface()); });
		host.def("ui_set_show_interface", [&ui](int v) { ui.setShowInterface(static_cast<InterfaceVisibility>(v)); });

		// cities
		host.def("ui_city_screen_up", [&ui] { return ui.isCityScreenUp(); });
		// whether the flag and the selected unit's model show (the DLL's: not in a talk, ...)
		host.def("ui_display_flag", [] { return GC.getGame().shouldDisplayFlag(); });
		host.def("ui_display_unit_model", [] { return GC.getGame().shouldDisplayUnitModel(); });
		host.def("ui_city_selection", [&ui] { return ui.isCitySelection(); });
		host.def("ui_head_city", [&ui] {
			const CvCity* c = ui.getHeadSelectedCity();
			return c ? py::make_tuple(int(c->getOwner()), c->getID()) : py::make_tuple(-1, -1);
		});
		host.def("ui_select_city", [&ui](int owner, int id, bool test) {
			if (CvCity* c = cityOf(owner, id))
				ui.selectCity(c, test);
		});
		host.def("ui_add_city", [&ui](int owner, int id, bool toggle) {
			if (CvCity* c = cityOf(owner, id))
				ui.addSelectedCity(c, toggle);
		});
		host.def("ui_clear_cities", [&ui] { ui.clearSelectedCities(); });
		host.def("ui_city_selected", [&ui](int owner, int id) { return ui.isCitySelected(cityOf(owner, id)); });
		host.def("ui_look_at_city", [&ui](int id) { ui.lookAtCityOffset(id); });
		// the turn log: the active player's messages the game core keeps, (turn, text, plot x, y,
		// type), oldest first; a look at one's plot (the game core's coordinates)
		host.def("turnlog_messages", [] {
			py::list out;
			const PlayerTypes active = GC.getGame().getActivePlayer();
			if (active == NO_PLAYER)
				return out;
			for (const CvTalkingHeadMessage& m : CvPlayerAI::getPlayerNonInl(active).getGameMessages())
				if (m.getDescription() && *m.getDescription())
					out.append(py::make_tuple(m.getTurn(), wide(m.getDescription()), m.getX(), m.getY(), int(m.getMessageType())));
			return out;
		});
		host.def("turnlog_look", [&ui](int x, int y) {
			const auto [vx, vy] = windowPlot({ x, y });
			ui.lookAtX = vx, ui.lookAtY = vy;
		});
		// CyEngine's marks on the map (the game core's coordinates): coloured plots, outlines,
		// signs and landmarks
		Engine& engine = utility.engine();
		host.def("engine_mark", [&engine](int x, int y, float r, float g, float b, float a, int layer, bool border) {
			engine.mark({ x, y, r, g, b, a, layer, border, true });
		});
		host.def("engine_effect", [&engine](int effect, float x, float y, float z, float rotation) { engine.TriggerEffect(effect, NiPoint3(x, y, z), rotation); });
		// The player's profile (oc4options): where it is kept, how loud the sounds play.
		host.def("profile_path", [] {
			PWSTR documents = nullptr;
			std::string path;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)))
			{
				const std::filesystem::path folder = std::filesystem::path(documents) / L"My Games" / L"openciv4";
				std::error_code ec;
				std::filesystem::create_directories(folder, ec);
				path = (folder / L"profile.ini").string();
			}
			CoTaskMemFree(documents);
			return path;
		});
		// (the graphic options it has: the game core and the window ask the utility)
		host.def("set_graphic_option", [&utility](int option, bool on) { utility.setGraphicOption(option, on); });
		host.def("reset_graphic_options", [&utility] { utility.resetGraphicOptions(); });
		host.def("set_ui_scale", [&utility](int percent) { utility.setUiScale(percent); });
		// W A S D panning: the profile's, as loaded; the player's, from the options' check box.
		host.def("set_wasd_pan", [&utility](bool on) { utility.setWasdPan(on); });
		host.def("choose_wasd_pan", [&utility](bool on) { utility.chooseWasdPan(on); });
		host.def("wasd_pan", [&utility] { return utility.wasdPan(); });
		// Ending idle turns: the same.
		host.def("set_auto_end_turn", [&utility](bool on) { utility.setAutoEndTurn(on); });
		host.def("choose_auto_end_turn", [&utility](bool on) { utility.chooseAutoEndTurn(on); });
		host.def("auto_end_turn", [&utility] { return utility.autoEndTurn(); });
		// Edge scrolling: the same (on by default).
		host.def("set_edge_scroll", [&utility](bool on) { utility.setEdgeScroll(on); });
		host.def("choose_edge_scroll", [&utility](bool on) { utility.chooseEdgeScroll(on); });
		host.def("edge_scroll", [&utility] { return utility.edgeScroll(); });
		host.def("ui_size_at", [](int percent) {
			const float scale = render::uiScaleFor(percent, g_display.density, g_display.width, g_display.height);
			return std::pair{ render::uiUnits(g_display.width, scale), render::uiUnits(g_display.height, scale) };
		});
		// The globe view's layers (oc4screens' CyGlobeLayerManager): the game core's, the one
		// shown and its option, chosen.
		host.def("globe_up", [&engine] { return bool(engine.globeUp); });
		host.def("globe_layers", [] {
			std::vector<CvGlobeLayerData> layers;
			GC.getGame().getGlobeLayers(layers);
			py::list out;
			for (const CvGlobeLayerData& l : layers)
				out.append(py::make_tuple(int(l.m_eType), std::string(l.m_strName.GetCString()), std::string(l.m_strButtonHelpTag.GetCString()),
					std::string(l.m_strButtonStyle.GetCString()), l.m_iNumOptions, l.m_bGlobeViewRequired));
			return out;
		});
		host.def("globe_layer", [&engine] {
			// (the icon layers the game's controls switch are the units' and resources' layers)
			int layer = engine.globeLayer;
			if (layer < 0 && (engine.resourceLayer || engine.unitLayer))
			{
				std::vector<CvGlobeLayerData> layers;
				GC.getGame().getGlobeLayers(layers);
				for (size_t i = 0; i < layers.size(); ++i)
					if ((engine.resourceLayer && layers[i].m_eType == GLOBE_LAYER_RESOURCE) || (engine.unitLayer && layers[i].m_eType == GLOBE_LAYER_UNIT))
						layer = int(i);
			}
			return py::make_tuple(layer, int(engine.globeOption));
		});
		host.def("globe_option_name", [](int type, int option) {
			CvWString name;
			GAMETEXT.getGlobeLayerName(static_cast<GlobeLayerTypes>(type), option, name);
			return wide(name.GetCString());
		});
		// (the spaceship screen's launch: as the engine sends it)
		host.def("spaceship_launch", [](int victory) {
			if (const PlayerTypes active = GC.getGame().getActivePlayer(); active != NO_PLAYER && victory >= 0)
				CvMessageControl::getInstance().sendLaunch(active, static_cast<VictoryTypes>(victory));
		});
		host.def("set_volumes", [](std::vector<float> volumes) {
			float v[6] = { 1, 1, 1, 1, 1, 1 };
			for (size_t i = 0; i < 6 && i < volumes.size(); ++i)
				v[i] = volumes[i];
			Sounds::setVolumes(v);
		});
		host.def("engine_clear", [&engine](int layer, bool border) { engine.clear(layer, border); });
		host.def("engine_sign", [&engine](int x, int y, int player, py::object caption) { engine.addSign({ x, y, player, toWide(caption), true }); });
		host.def("engine_unsign", [&engine](int x, int y, int player, bool anyPlayer) {
			std::lock_guard lock(engine.marksMutex);
			std::erase_if(engine.signs, [&](const Engine::Sign& s) { return s.core && s.x == x && s.y == y && (anyPlayer || s.player == player); });
			++engine.marksVersion;
		});
		host.def("engine_signs", [&engine] {
			py::list out;
			std::lock_guard lock(engine.marksMutex);
			for (const Engine::Sign& s : engine.signs)
				if (s.core)
					out.append(py::make_tuple(s.x, s.y, s.player, wide(s.caption)));
			return out;
		});
		// (CyCamera's looks: a point of the map, as CvPlot::getPoint gives it)
		host.def("camera_look_at", [&ui](float x, float y) { ui.lookAt(NiPoint3(x, y, 0), CAMERALOOKAT_NORMAL, NiPoint3(0, 0, 0)); });
		host.def("ui_orders", [&ui] {
			const CvCity* c = ui.getHeadSelectedCity();
			return c ? c->getNumOrdersQueued() : 0;
		});
		host.def("ui_order", [&ui](int i) {
			const CvCity* c = ui.getHeadSelectedCity();
			if (!c || i < 0 || i >= c->getNumOrdersQueued())
				return py::make_tuple(-1, -1, -1, false);
			const OrderData o = c->getOrderData(i);
			return py::make_tuple(int(o.eOrderType), o.iData1, o.iData2, o.bSave);
		});

		// units
		host.def("ui_head_unit", [&ui] { return idOf(ui.getHeadSelectedUnit()); });
		host.def("ui_selection_unit", [&ui](int i) { return idOf(ui.getSelectionUnit(i)); });
		host.def("ui_selection_length", [&ui] { return ui.getLengthSelectionList(); });
		host.def("ui_mirrors_selection_group", [&ui] { return ui.mirrorsSelectionGroup(); });
		host.def("ui_selection_plot", [&ui] {
			const CvPlot* p = ui.getSelectionPlot();
			if (!p)
				return py::make_tuple(-1, -1);
			const auto [x, y] = gameCorePlot({ p->getX(), p->getY() });
			return py::make_tuple(x, y);
		});
		host.def("ui_select_unit", [&ui](int owner, int id, bool clear, bool toggle) {
			if (CvUnit* u = unitOf(owner, id))
				ui.selectUnit(u, clear, toggle, false);
		});
		host.def("ui_select_group", [&ui](int owner, int id, bool shift, bool ctrl, bool alt) {
			if (CvUnit* u = unitOf(owner, id))
				ui.selectGroup(u, shift, ctrl, alt);
		});
		host.def("ui_clear_selection", [&ui] { ui.clearSelectionList(); });
		host.def("ui_insert_selection", [&ui](int owner, int id, bool clear, bool toggle, bool group) {
			if (CvUnit* u = unitOf(owner, id))
				ui.insertIntoSelectionList(u, clear, toggle, group, false, false);
		});
		host.def("ui_can_create_group", [&ui] { return ui.getLengthSelectionList() > 1 && !ui.mirrorsSelectionGroup(); });
		host.def("ui_can_delete_group", [&ui] {
			const CvUnit* head = ui.getHeadSelectedUnit();
			return head && head->getGroup() && head->getGroup()->getNumUnits() > 1;
		});

		// actions
		host.def("ui_interface_mode", [&ui] { return int(ui.getInterfaceMode()); });
		host.def("ui_set_interface_mode", [&ui](int m) { ui.setInterfaceMode(static_cast<InterfaceModeTypes>(m)); });
		host.def("ui_can_handle_action", [](int action, bool visible) {
			return action >= 0 && action < GC.getNumActionInfos() && GC.getGame().canHandleAction(action, nullptr, visible);
		});
		host.def("ui_actions_to_show", [&ui] {
			// What the selected units can do, as the action buttons show it: in the game's order.
			std::vector<std::pair<int, int>> order;
			if (ui.getHeadSelectedUnit() && !ui.isCitySelection())
				for (int i = 0; i < GC.getNumActionInfos(); ++i)
				{
					const CvActionInfo& a = GC.getActionInfo(i);
					if (a.isVisible() && a.getControlType() == NO_CONTROL && GC.getGame().canHandleAction(i, nullptr, true))
						order.push_back({ a.getOrderPriority(), i });
				}
			std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			std::vector<int> actions;
			for (const auto& [priority, i] : order)
				actions.push_back(i);
			return actions;
		});

		// the end of the turn
		host.def("ui_turn_active", [] {
			const PlayerTypes me = GC.getGame().getActivePlayer();
			return me != NO_PLAYER && CvPlayerAI::getPlayerNonInl(me).isTurnActive();
		});
		host.def("ui_end_turn_message", [&ui] {
			const PlayerTypes me = GC.getGame().getActivePlayer();
			return me != NO_PLAYER && CvPlayerAI::getPlayerNonInl(me).isTurnActive() && ui.isEndTurnMessage();
		});
		host.def("ui_end_turn_state", [&ui] { return int(ui.isEndTurnMessage() ? END_TURN_GO : END_TURN_OVER_HIGHLIGHT); });
		host.def("ui_should_show_research", [&ui] {
			const PlayerTypes me = GC.getGame().getActivePlayer();
			return me != NO_PLAYER && CvPlayerAI::getPlayerNonInl(me).getCurrentResearch() == NO_TECH && !ui.isCityScreenUp();
		});
		host.def("ui_help_string", [] { return py::reinterpret_steal<py::object>(PyUnicode_FromWideChar(g_help.data(), Py_ssize_t(g_help.size()))); });
		// The WorldBuilder: its mode, the mouse over the map (oc4wb, CyInterface)
		host.def("ui_set_world_builder", [&ui](bool on) { ui.setWorldBuilder(on); });
		host.def("ui_world_builder", [&ui] { return ui.worldBuilder(); });
		host.def("ui_mouse_plot", [&ui] {
			if (ui.mouseX < 0)
				return py::make_tuple(ui.mouseX, ui.mouseY);
			const auto [x, y] = gameCorePlot({ ui.mouseX, ui.mouseY });
			return py::make_tuple(x, y);
		});
		host.def("ui_mouse_buttons", [&ui] { return py::make_tuple(ui.mouseLeft, ui.mouseRight); });
		host.def("ui_mouse_over_map", [&ui] { return ui.mouseOverMap; });
		host.def("ui_key", [](int which) {
			const render::Modifiers m = render::heldModifiers();
			return which == 0 ? m.shift : which == 1 ? m.ctrl : which == 2 && m.alt;
		});
		// sounds (CyInterface's, CyAudioGame's): by tag, by script id (type 1: 2D, 2: 3D), at a plot;
		// a talk's music
		Sounds& sounds = utility.sounds();
		host.def("audio_play", [&sounds](const std::string& tag) { sounds.play(tag); });
		host.def("movie_playable", [&utility](const std::string& file) {
			const auto at = utility.assets().locate(file);
			return media::available() && at && !at->file.empty();
		});
		// the scripts' messages (CyInterface.addMessage; addImmediateMessage: the active player's,
		// only shown)
		host.def("ui_add_combat_message", [&ui](int player, py::object text) {
			ui.addCombatMessage(static_cast<PlayerTypes>(player), CvWString(toWide(text).c_str()));
		});
		host.def("ui_add_message", [&ui](int player, bool force, int length, py::object text, py::object sound, int type, py::object icon, int color,
									   int x, int y, bool offArrows, bool onArrows) {
			const std::string s = sound.is_none() ? std::string() : narrow(sound), i = icon.is_none() ? std::string() : narrow(icon);
			ui.addMessage(player < 0 ? GC.getGame().getActivePlayer() : static_cast<PlayerTypes>(player), force, length < 0 ? GC.getEVENT_MESSAGE_TIME() : length,
				CvWString(toWide(text).c_str()), s.c_str(), static_cast<InterfaceMessageTypes>(type), i.c_str(), static_cast<ColorTypes>(color), x, y, offArrows,
				onArrows);
		});
		host.def("audio_play_id", [&sounds](int script, int type) {
			sounds.play(type == AUDIOTAG_3DSCRIPT ? AudioScripts::Script3D : AudioScripts::Script2D, script);
		});
		host.def("audio_play_at", [&sounds](const std::string& tag, int x, int y) {
			const CvPlot* plot = GC.getMap().plot(x, y);
			if (const TeamTypes team = GC.getGame().getActiveTeam(); plot && team != NO_TEAM && !plot->isVisible(team, false))
				return;
			sounds.play(tag, Sounds::Place{ float(x) + 0.5f, float(y) + 0.5f });
		});
		host.def("audio_talk_music", [&sounds](int intro, int loop) { sounds.setTalkMusic(intro, loop); });
		// the diplomacy screen: CyDiplomacy (what the script says and does) and what the screen shows
		Diplomacy& diplo = utility.diplomacy();
		TextManager& texts = utility.text();
		host.def("diplo_version", [&diplo] { return diplo.version(); });
		host.def("diplo_who", [&diplo] { return int(diplo.who()); });
		host.def("diplo_player", [&diplo] { return int(diplo.player()); });
		host.def("diplo_data", [&diplo] { return diplo.data(); });
		host.def("diplo_comment", [&diplo] { return diplo.comment(); });
		host.def("diplo_set_ai_string", [&diplo, &texts](py::object text, py::object args) { diplo.setAIString(texts.formatRaw(toWide(text), textArgs(args))); });
		host.def("diplo_set_ai_comment", [&diplo](int comment) { diplo.setAIComment(comment); });
		host.def("diplo_ai_text", [&diplo] { return wide(diplo.aiText()); });
		host.def("diplo_clear_user", [&diplo] { diplo.clearUserComments(); });
		host.def("diplo_add_user", [&diplo, &texts](int comment, int data1, int data2, py::object text, py::object args) {
			diplo.addUserComment({ comment, data1, data2, texts.formatRaw(toWide(text), textArgs(args)) });
		});
		host.def("diplo_user", [&diplo] {
			py::list out;
			for (const Diplomacy::Comment& c : diplo.userComments())
				out.append(py::make_tuple(c.comment, c.data1, c.data2, wide(c.text)));
			return out;
		});
		host.def("diplo_start_trade", [&diplo](bool renegotiate) { diplo.startTrade(renegotiate); });
		host.def("diplo_end_trade", [&diplo] { diplo.endTrade(); });
		host.def("diplo_show_all", [&diplo](bool all) { diplo.showAllTrade(all); });
		host.def("diplo_trading", [&diplo] { return py::make_tuple(diplo.trading(), diplo.showingAll(), diplo.showingCurrent()); });
		host.def("diplo_our_empty", [&diplo] { return diplo.ourOfferEmpty(); });
		host.def("diplo_their_empty", [&diplo] { return diplo.theirOfferEmpty(); });
		host.def("diplo_at_war", [&diplo] { return diplo.atWar(); });
		host.def("diplo_is_ai_offer", [&diplo] { return diplo.isAIOffer(); });
		host.def("diplo_set_ai_offer", [&diplo](bool ai) { diplo.setAIOffer(ai); });
		host.def("diplo_offer", [&diplo] { return diplo.offerDeal(); });
		host.def("diplo_counter", [&diplo] { return diplo.counterPropose(); });
		host.def("diplo_implement", [&diplo] { diplo.implementDeal(); });
		host.def("diplo_declare_war", [&diplo] { diplo.declareWar(); });
		host.def("diplo_event", [&diplo](int event, int data1, int data2) { diplo.diploEvent(event, data1, data2); });
		host.def("diplo_annual", [&diplo] { return diplo.hasAnnualDeal(); });
		host.def("diplo_vassal_tribute", [&diplo] { return diplo.theirVassalTribute(); });
		host.def("diplo_close", [&diplo] { diplo.end(); });
		host.def("diplo_items", [&diplo](bool theirs, bool offered) {
			py::list out;
			for (const Diplomacy::Item& i : offered ? diplo.offer(theirs) : diplo.inventory(theirs))
				out.append(py::make_tuple(i.type, i.data, wide(i.text), i.icon, i.heading, i.denied));
			return out;
		});
		host.def("diplo_cancel", [&diplo](int type, int data) {
			std::wstring reason;
			const bool done = diplo.cancelDeal(type, data, reason);
			return py::make_tuple(done, wide(reason));
		});
		host.def("diplo_toggle", [&diplo](bool theirs, bool fromOffer, int type, int data, int amount) { diplo.toggle(theirs, fromOffer, type, data, amount); });
		host.def("diplo_max_gold", [&diplo](bool theirs, bool perTurn) { return diplo.maxGold(theirs, perTurn); });

		// how wide a text is (CyInterface.determineWidth), in the window's font
		host.def("ui_text_width", [](py::object text, int font) {
			return g_textWidth ? g_textWidth(gameMarkup(toWide(text).c_str()), font) : estimatedWidth(toWide(text));
		}, py::arg("text"), py::arg("font") = -1);
		host.def("ui_text_measured", [] { return bool(g_textWidth); });
		// how high a text is when it wraps at a width, in a widget's font (FontTypes)
		host.def("ui_text_height", [](py::object text, int width, int font) {
			return g_textHeight ? g_textHeight(gameMarkup(toWide(text).c_str()), width, font) : (int(toWide(text).size()) * 7 / std::max(1, width) + 1) * 16;
		});

		// the plot list (the main interface's row size for the DLL's clicks on it)
		host.def("ui_plot_list_offset", [&ui] { return ui.getPlotListOffset(); });
		host.def("ui_plot_list_column", [&ui] { return ui.getPlotListColumn(); });
		ui.plotListButtonsPerRow = [] {
			try
			{
				return py::module_::import("CvScreensInterface").attr("numPlotListButtons")().cast<int>();
			}
			catch (py::error_already_set& e)
			{
				printPythonError(e);
				return 1;
			}
		};

		// popups: a script's own (CyPopup), filled; the DLL's Python ones (CyPopupInfo)
		host.def("popup_launch", [&ui](int number, py::object header, py::list items, bool okButton, bool front) {
			Interface::Popup p;
			p.python = number;
			p.header = toWide(header);
			p.okButton = okButton;
			p.launched = true;
			for (py::handle h : items)
			{
				const py::tuple t = py::reinterpret_borrow<py::tuple>(h);
				Interface::Popup::Item& it = p.items.emplace_back();
				it.kind = static_cast<Interface::Popup::Item::Kind>(t[0].cast<int>());
				it.text = toWide(t[1]);
				it.help = toWide(t[2]);
				it.icon = narrow(t[3]);
				it.id = t[4].cast<int>();
				it.group = t[5].cast<int>();
				it.checked = t[6].cast<bool>();
				it.control = static_cast<Interface::Popup::Item::Control>(t[7].cast<int>());
			}
			ui.queuePopup(std::move(p), front);
		});
		host.def("popup_info_add", [&ui](int player, int type, int data1, int data2, int data3, int flags, bool option1, bool option2, py::object text,
									   std::string onFocus, std::string onClicked, std::string module, py::list buttons) {
			auto* info = new CvPopupInfo(static_cast<ButtonPopupTypes>(type), data1, data2, data3, flags, option1, option2);
			info->setText(toWide(text).c_str());
			info->setOnFocusPythonCallback(onFocus.c_str());
			info->setOnClickedPythonCallback(onClicked.c_str());
			info->setPythonModule(module.c_str());
			for (py::handle h : buttons)
			{
				const py::tuple t = py::reinterpret_borrow<py::tuple>(h);
				info->addPythonButton(toWide(t[0]).c_str(), narrow(t[1]).c_str());
			}
			ui.addPopup(info, static_cast<PlayerTypes>(player), false, false);
		});
		ui.screenUp = [] {
			try
			{
				return py::module_::import("oc4screens").attr("modalUp")().cast<bool>();
			}
			catch (py::error_already_set& e)
			{
				printPythonError(e);
			}
			return false;
		};
		// A script's popup answered: to the script (oc4screens: CvEventInterface.applyEvent).
		ui.onPythonAnswer = [](const Interface::Popup& p, const Interface::PopupAnswer& a) {
			try
			{
				py::list chosen, checked, edits;
				for (const auto& [key, id] : a.chosen)
					chosen.append(py::make_tuple(int(key.first), key.second, id));
				for (const auto& [group, bits] : a.checked)
					checked.append(py::make_tuple(group, bits));
				for (const auto& [group, text] : a.edits)
					edits.append(py::make_tuple(group, py::cast(text)));
				py::module_::import("oc4screens").attr("popupAnswered")(p.python, a.button, chosen, checked, edits);
			}
			catch (py::error_already_set& e)
			{
				printPythonError(e);
			}
		};
		host.def("screen_closed", [](int fileId) {
			try
			{
				py::module_::import("CvScreensInterface").attr("onClose")(py::make_tuple(fileId));
			}
			catch (py::error_already_set& e)
			{
				printPythonError(e);
			}
		});
	}

	namespace
	{
		std::pair<int, int> translatePlot(std::pair<int, int> p, const char* xName, const char* yName)
		{
			try
			{
				py::object map = py::module_::import("CvPythonExtensions").attr("CyMap")();
				if (py::hasattr(map, xName))
					return { map.attr(xName)(p.first).cast<int>(), map.attr(yName)(p.second).cast<int>() };
			}
			catch (py::error_already_set& e)
			{
				printPythonError(e);
			}
			return p;
		}
	}

	std::pair<int, int> gameCorePlot(std::pair<int, int> window) { return translatePlot(window, "getMapXFromViewportX", "getMapYFromViewportY"); }
	std::pair<int, int> windowPlot(std::pair<int, int> gameCore) { return translatePlot(gameCore, "getViewportXFromMapX", "getViewportYFromMapY"); }

	std::pair<std::string, float> effectArt(int effect)
	{
		static std::unordered_map<int, std::pair<std::string, float>> cache;
		if (const auto it = cache.find(effect); it != cache.end())
			return it->second;
		std::pair<std::string, float> art{ {}, 1.0f };
		try
		{
			py::object info = py::module_::import("CvPythonExtensions").attr("CyGlobalContext")().attr("getEffectInfo")(effect);
			if (!info.is_none())
			{
				art.first = info.attr("getPath")().cast<std::string>();
				if (py::hasattr(info, "getScale"))
					art.second = info.attr("getScale")().cast<float>();
			}
		}
		catch (py::error_already_set& e)
		{
			printPythonError(e);
		}
		return cache[effect] = art;
	}

	std::vector<std::pair<int, int>> greatWallCities()
	{
		std::vector<std::pair<int, int>> out;
		try
		{
			py::dict scope;
			py::exec(R"(
import CvPythonExtensions as _c
_gc = _c.CyGlobalContext()
_walls = [b for b in range(_gc.getNumBuildingInfos()) if _gc.getBuildingInfo(b).isAreaBorderObstacle()]
found = []
if _walls:
	for _p in range(_gc.getMAX_PLAYERS()):
		_pl = _gc.getPlayer(_p)
		if not _pl.isAlive():
			continue
		(_city, _it) = _pl.firstCity(False)
		while _city:
			if [b for b in _walls if _city.getNumRealBuilding(b) > 0]:
				found.append((_city.getX(), _city.getY()))
			(_city, _it) = _pl.nextCity(_it, False)
)", scope);
			for (const auto& item : scope["found"])
			{
				const auto t = item.cast<py::tuple>();
				out.push_back({ t[0].cast<int>(), t[1].cast<int>() });
			}
		}
		catch (py::error_already_set& e)
		{
			printPythonError(e);
		}
		return out;
	}

	// ---------------------------------------------------------------- the driver

	void Screens::setTextWidth(std::function<int(const std::string&, int)> width) { g_textWidth = std::move(width); }
	void Screens::setTextHeight(std::function<int(const std::string&, int, int)> height) { g_textHeight = std::move(height); }

	bool Screens::start(int width, int height)
	{
		try
		{
			py::module_ screens = py::module_::import("oc4screens");
			screens.attr("setResolution")(width, height);
			m_mainId = py::module_::import("CvScreenEnums").attr("MAIN_INTERFACE").cast<int>();
			py::module_::import("CvScreensInterface").attr("showMainInterface")();
			m_utility.gameInterface().makeInterfaceDirty();
			m_active = true;
			m_laidOut = m_seenSize = { width, height };
		}
		catch (py::error_already_set& e)
		{
			printPythonError(e);
			m_active = false;
		}
		return m_active;
	}

	bool Screens::startMenu(int width, int height, const std::string& title, const std::vector<std::string>& mapScripts,
		const std::vector<std::pair<std::filesystem::path, std::string>>& saveFolders, const std::vector<std::string>& mods,
		const std::string& message, const std::string& page)
	{
		try
		{
			py::module_::import("oc4screens").attr("setResolution")(width, height);
			py::module_ menu = py::module_::import("oc4menu");
			py::list maps, folders, others;
			for (const std::string& m : mapScripts)
				maps.append(py::bytes(m));
			for (const auto& [folder, label] : saveFolders)
				folders.append(py::make_tuple(py::bytes(utf8Path(folder)), py::bytes(label)));
			for (const std::string& m : mods)
				others.append(py::bytes(m));
			menu.attr("show")(py::bytes(title), maps, folders, py::bytes(message), py::bytes(page), others);
			m_mainId = menu.attr("MENU").cast<int>();
			m_active = m_menu = true;
			m_laidOut = m_seenSize = { width, height };
		}
		catch (py::error_already_set& e)
		{
			printPythonError(e);
			m_active = m_menu = false;
		}
		return m_active;
	}

	std::optional<MenuChoice> Screens::takeMenuChoice()
	{
		if (!m_menu)
			return std::nullopt;
		try
		{
			const py::object c = py::module_::import("oc4menu").attr("takeChoice")();
			if (c.is_none())
				return std::nullopt;
			const py::tuple t = c.cast<py::tuple>();
			const std::string kind = t[0].cast<std::string>();
			MenuChoice choice;
			if (kind == "load")
			{
				choice.kind = MenuChoice::Kind::Load;
				const std::string path = t[1].cast<std::string>();
				choice.save = std::filesystem::path(std::u8string(path.begin(), path.end()));
				if (py::len(t) > 2)
					choice.host = t[2].cast<int>();
			}
			else if (kind == "mod")
			{
				choice.kind = MenuChoice::Kind::Mod;
				choice.mod = t[1].cast<std::string>();
			}
			else if (kind == "join")
			{
				choice.kind = MenuChoice::Kind::Join;
				choice.join = t[1].cast<std::string>();
			}
			else if (kind == "new")
			{
				choice.kind = MenuChoice::Kind::NewGame;
				const py::dict d = t[1].cast<py::dict>();
				const auto text = [&](const char* key, std::string& to) {
					if (d.contains(key))
						to = d[key].cast<std::string>();
				};
				text("map", choice.game.mapScript);
				text("size", choice.game.worldSize);
				text("climate", choice.game.climate);
				text("sea", choice.game.seaLevel);
				text("era", choice.game.era);
				text("speed", choice.game.gameSpeed);
				text("handicap", choice.game.handicap);
				text("civ", choice.game.civilization);
				text("leader", choice.game.leader);
				if (d.contains("seed"))
					choice.game.seed = d["seed"].cast<unsigned>();
				if (d.contains("players"))
					choice.game.players = d["players"].cast<int>();
				if (d.contains("advancedStartPoints"))
					choice.game.advancedStartPoints = d["advancedStartPoints"].cast<int>();
				// {GAMEOPTION_...: on}, [VICTORY_...], {option: value}, {slot: {status, civ, leader, team}}
				if (d.contains("options"))
					for (const auto& [name, on] : d["options"].cast<py::dict>())
						choice.game.options.emplace_back(name.cast<std::string>(), on.cast<bool>());
				if (d.contains("victories"))
				{
					for (const py::handle v : d["victories"])
						choice.game.victories.push_back(v.cast<std::string>());
					if (choice.game.victories.empty())
						choice.game.victories.push_back("NONE");
				}
				if (d.contains("mapOptions"))
					for (const auto& [option, value] : d["mapOptions"].cast<py::dict>())
						choice.game.mapOptions[option.cast<int>()] = value.cast<int>();
				if (d.contains("slots"))
					for (const auto& [slot, value] : d["slots"].cast<py::dict>())
					{
						const py::dict v = value.cast<py::dict>();
						SlotSetting set;
						const std::string status = v.contains("status") ? v["status"].cast<std::string>() : "ai";
						set.status = status == "human" ? SlotSetting::Status::Human
							: status == "closed"       ? SlotSetting::Status::Closed
													   : SlotSetting::Status::AI;
						if (v.contains("civ"))
							set.civilization = v["civ"].cast<std::string>();
						if (v.contains("leader"))
							set.leader = v["leader"].cast<std::string>();
						if (v.contains("team"))
							set.team = v["team"].cast<int>();
						choice.game.slots[slot.cast<int>()] = std::move(set);
					}
				if (d.contains("host"))
					choice.host = d["host"].cast<int>();
			}
			return choice;
		}
		catch (py::error_already_set& e)
		{
			printPythonError(e);
		}
		return std::nullopt;
	}

	void Screens::closeMenu()
	{
		if (!m_menu)
			return;
		try
		{
			py::module_::import("oc4menu").attr("close")();
		}
		catch (py::error_already_set& e)
		{
			printPythonError(e);
		}
		m_active = m_menu = false;
		m_items.clear();
		m_version = -1;
	}

	bool Screens::menuUp()
	{
		if (!m_active)
			return false;
		if (m_menu || m_utility.diplomacy().active())
			return true;
		try
		{
			return py::module_::import("oc4screens").attr("modalUp")().cast<bool>();
		}
		catch (py::error_already_set& e)
		{
			printPythonError(e);
			return false;
		}
	}

	void Screens::frame(render::MapWindow& window, int width, int height)
	{
		// (a new scale: the next frame's size, which lays the screens out again)
		window.setUiScale(m_utility.uiScale());
		// (Ctrl+Shift+W in the window: the player's choice, kept in the profile)
		if (const auto toggled = window.takeWasdPanToggle())
		{
			m_utility.chooseWasdPan(*toggled);
			try
			{
				py::module_::import("oc4options").attr("setWasdPan")(*toggled);
			}
			catch (py::error_already_set& e)
			{
				printPythonError(e);
			}
		}
		window.setWasdPan(m_utility.wasdPan());
		// (Ctrl+Shift+E: the same)
		if (const auto toggled = window.takeAutoEndTurnToggle())
		{
			m_utility.chooseAutoEndTurn(*toggled);
			try
			{
				py::module_::import("oc4options").attr("setAutoEndTurn")(*toggled);
			}
			catch (py::error_already_set& e)
			{
				printPythonError(e);
			}
		}
		window.setAutoEndTurn(m_utility.autoEndTurn());
		if (!m_menu) // (the main menu has no map to scroll)
			window.setEdgeScroll(m_utility.edgeScroll());
		g_display = window.uiDisplay();
		window.setUiTakesKeys(menuUp(), !m_menu);
		// (new answers in a talk: the keyboard's choice was among the old ones, and Return is the
		// talk's own again)
		{
			std::vector<std::pair<int, std::wstring>> answers;
			for (const Diplomacy::Comment& c : m_utility.diplomacy().userComments())
				answers.emplace_back(c.comment, c.text);
			if (answers != m_talkAnswers)
			{
				window.clearKeyChoice();
				m_talkAnswers = std::move(answers);
			}
		}
		if (!m_active)
			return;
		if (m_menu)
		{
			// (the menu is no game's screen: nothing to update, no talks; another window size
			// lays it out again once the size settles)
			try
			{
				py::module_ screens = py::module_::import("oc4screens");
				const auto now = std::chrono::steady_clock::now();
				if (std::pair{ width, height } != m_seenSize)
				{
					m_seenSize = { width, height };
					m_sizeChanged = now;
				}
				if (m_seenSize != m_laidOut && now - m_sizeChanged > std::chrono::milliseconds(300))
				{
					m_laidOut = m_seenSize;
					screens.attr("setResolution")(width, height);
					py::module_::import("oc4menu").attr("relayout")();
					py::module_::import("oc4options").attr("resized")();
				}
				const auto [mouseX, mouseY] = window.mousePosition();
				screens.attr("setMouse")(std::max(mouseX, 0), std::max(mouseY, 0));
				if (const long long version = screens.attr("version")().cast<long long>(); version != m_version)
				{
					m_version = version;
					rebuild(window);
				}
			}
			catch (py::error_already_set& e)
			{
				printPythonError(e);
			}
			return;
		}
		Interface& ui = m_utility.gameInterface();
		try
		{
			py::module_ screens = py::module_::import("oc4screens");
			// Another window size: once it settles, the main interface is laid out again for it (the
			// scripts place everything by the resolution when they make their screens).
			const auto now = std::chrono::steady_clock::now();
			if (std::pair{ width, height } != m_seenSize)
			{
				m_seenSize = { width, height };
				m_sizeChanged = now;
			}
			if (m_seenSize != m_laidOut && now - m_sizeChanged > std::chrono::milliseconds(300))
			{
				m_laidOut = m_seenSize;
				screens.attr("reset")(m_mainId);
				screens.attr("setResolution")(width, height);
				py::module_::import("CvScreensInterface").attr("showMainInterface")();
				py::module_::import("oc4options").attr("resized")();
				m_utility.gameInterface().makeInterfaceDirty();
			}
			screens.attr("setResolution")(m_laidOut.first, m_laidOut.second);
			const auto [mouseX, mouseY] = window.mousePosition();
			screens.attr("setMouse")(std::max(mouseX, 0), std::max(mouseY, 0));
			py::module_ entry = py::module_::import("CvScreensInterface");
			entry.attr("forceScreenUpdate")(py::make_tuple(m_mainId));
			// The turn log, as the game core asks for it.
			switch (ui.turnLogRequest.exchange(Interface::TurnLog::None))
			{
			case Interface::TurnLog::Toggle: py::module_::import("oc4turnlog").attr("toggle")(); break;
			case Interface::TurnLog::Show: py::module_::import("oc4turnlog").attr("show")(); break;
			case Interface::TurnLog::Refresh: py::module_::import("oc4turnlog").attr("refresh")(); break;
			default: break;
			}
			// The screens over it, as the original engine updates them every frame (the replay
			// plays its turns so, the military advisor's flashing plots end so).
			const float dt = m_lastUpdate.time_since_epoch().count() ? std::min(std::chrono::duration<float>(now - m_lastUpdate).count(), 0.25f) : 0.0f;
			m_lastUpdate = now;
			for (py::handle id : screens.attr("shownScreens")().cast<py::list>())
				if (const int screen = id.cast<int>(); screen != kDiplomacyScreen)
					entry.attr("update")(py::make_tuple(screen, dt));
			if (ui.anyDirty())
				entry.attr("forceScreenRedraw")(py::make_tuple(m_mainId));
			// Diplomacy: the next talk waiting, when nothing is up (the game's script says what the
			// leader says and what the player can answer), and its screen again when it changed.
			Diplomacy& diplomacy = m_utility.diplomacy();
			if (diplomacy.next(ui.isPopupUp() || ui.isPopupQueued()))
			{
				py::list args;
				args.append(diplomacy.comment());
				const auto commentArgs = diplomacy.commentArgs();
				args.append(int(commentArgs.size()));
				for (const TextManager::Arg& a : commentArgs)
					if (const auto* s = std::get_if<std::wstring>(&a))
						args.append(wide(*s));
					else if (const auto* d = std::get_if<double>(&a))
						args.append(*d);
					else
						args.append(std::get<long long>(a));
				py::module_::import("CvDiplomacyInterface").attr("beginDiplomacy")(py::tuple(args));
			}
			if (diplomacy.version() != m_diploVersion)
			{
				m_diploVersion = diplomacy.version();
				screens.attr("diploShow")();
				// (a talk begun or ended: the interface shows what it hides in one, the flag and the
				// unit's model, again, or hides it)
				if (diplomacy.active() != m_diploActive)
				{
					m_diploActive = diplomacy.active();
					ui.makeInterfaceDirty();
				}
			}
			if (const long long version = screens.attr("version")().cast<long long>(); version != m_version)
			{
				m_version = version;
				rebuild(window);
			}
			// A screen's own minimap, when it changed (its plots where the window shows them, or as
			// they are on another map).
			if (const py::object mm = screens.attr("minimapOverlay")(m_minimapVersion); !mm.is_none())
			{
				render::MapWindow::ScreenMinimap s;
				const py::tuple t = mm.cast<py::tuple>();
				if (t.size() >= 7)
				{
					s.version = m_minimapVersion = t[0].cast<long long>();
					s.width = t[1].cast<int>(), s.height = t[2].cast<int>();
					const bool sameMap = t[3].cast<bool>();
					s.territory = t[4].cast<float>();
					s.terrain = sameMap;
					s.wholeMap = t.size() >= 8 && t[7].cast<bool>();
					CvMap& map = GC.getMap();
					const int W = sameMap ? map.getGridWidth() : s.width, H = sameMap ? map.getGridHeight() : s.height;
					const auto place = [&](int x, int y) -> std::optional<std::pair<int, int>> {
						return x >= 0 && y >= 0 && x < W && y < H ? std::optional(std::pair{ x, y }) : std::nullopt;
					};
					if (sameMap)
						s.width = W, s.height = H;
					s.colors.assign(size_t(std::max(W, 0)) * size_t(std::max(H, 0)), 0);
					for (py::handle h : t[5].cast<py::list>())
					{
						const py::tuple c = py::reinterpret_borrow<py::tuple>(h);
						if (const auto at = place(c[0].cast<int>(), c[1].cast<int>()); at && at->first >= 0 && at->first < W && at->second >= 0 && at->second < H)
							s.colors[size_t(at->first) + size_t(at->second) * size_t(W)] = c[2].cast<uint32_t>();
					}
					for (py::handle h : t[6].cast<py::list>())
					{
						const py::tuple c = py::reinterpret_borrow<py::tuple>(h);
						if (const auto at = place(c[0].cast<int>(), c[1].cast<int>()))
							s.flashes.push_back({ at->first, at->second, c[2].cast<uint32_t>() });
					}
				}
				else
					m_minimapVersion = 0;
				window.setScreenMinimap(std::move(s));
			}
		}
		catch (py::error_already_set& e)
		{
			printPythonError(e);
			if (++m_failures > 20)
			{
				std::printf("  the game's screens failed too often: the host's own interface again\n");
				m_active = false;
				window.setUi({});
			}
		}
		// The help for the widget under the mouse, in the screens' help area.
		if (const int hovered = window.uiHovered(); hovered != m_hovered)
		{
			m_hovered = hovered;
			const std::string text = hovered >= 0 && size_t(hovered) < m_items.size() ? help(m_items[size_t(hovered)]) : std::string();
			float x = 7, y = float(height) - 172, w = 350;
			bool right = true, down = false;
			try
			{
				if (py::object area = py::module_::import("oc4screens").attr("helpArea")(); !area.is_none())
				{
					auto t = area.cast<py::tuple>();
					x = t[0].cast<float>();
					y = t[1].cast<float>();
					w = t[2].cast<float>();
					right = t[4].cast<bool>();
					down = t[5].cast<bool>();
				}
			}
			catch (py::error_already_set& e)
			{
				printPythonError(e);
			}
			window.setUiHelp(text, x, y, w, right, down);
		}
	}

	std::string Screens::help(const Item& item) const
	{
		if (item.widget >= 0 && item.widget != WIDGET_GENERAL)
		{
			CvWStringBuffer text;
			CvWidgetDataStruct w{ item.data1, item.data2, item.option, static_cast<WidgetTypes>(item.widget) };
			CvDLLWidgetData::getInstance().parseHelp(text, w);
			g_help = text.getCString();
			return gameMarkup(text.getCString());
		}
		try
		{
			const std::wstring text = toWide(py::module_::import("oc4screens").attr("widgetText")(item.screen, item.name));
			g_help = text;
			return gameMarkup(text.c_str());
		}
		catch (py::error_already_set& e)
		{
			printPythonError(e);
		}
		return {};
	}

	void Screens::rebuild(render::MapWindow& window)
	{
		using Kind = render::MapWindow::UiItem::Kind;
		static const std::pair<const char*, Kind> kKinds[] = { { "panel", Kind::Panel }, { "image", Kind::Image }, { "imagebutton", Kind::ImageButton },
			{ "button", Kind::Button }, { "checkbox", Kind::CheckBox }, { "label", Kind::Label }, { "text", Kind::Text }, { "multiline", Kind::Multiline },
			{ "bar", Kind::Bar }, { "line", Kind::Line }, { "header", Kind::Header }, { "cell", Kind::Cell }, { "dropdown", Kind::Dropdown },
			{ "endturn", Kind::EndTurnText }, { "endbutton", Kind::EndTurnButton }, { "minimap", Kind::Minimap }, { "graphic", Kind::Graphic },
			{ "separator", Kind::Separator }, { "edit", Kind::Edit }, { "slider", Kind::Slider }, { "option", Kind::Cell }, { "flag", Kind::Flag }, { "scrollbar", Kind::Scrollbar },
			{ "leaderhead", Kind::Leaderhead }, { "model", Kind::Model }, { "movie", Kind::Movie } };
		std::vector<render::MapWindow::UiItem> items;
		m_items.clear();
		if (const char* dump = std::getenv("OC4_UI_DUMP"))
			py::module_::import("oc4screens").attr("dump")(std::string(dump));
		const py::list list = py::module_::import("oc4screens").attr("display")();
		for (py::handle h : list)
		{
			const py::tuple t = py::reinterpret_borrow<py::tuple>(h);
			const std::string kindName = narrow(t[1]);
			const auto kind = std::ranges::find_if(kKinds, [&](const auto& k) { return kindName == k.first; });
			if (kind == std::end(kKinds))
				continue;
			render::MapWindow::UiItem item;
			item.kind = kind->second;
			item.x0 = t[2].cast<float>();
			item.y0 = t[3].cast<float>();
			item.x1 = t[4].cast<float>();
			item.y1 = t[5].cast<float>();
			if (!t[6].is_none())
			{
				const py::tuple c = t[6].cast<py::tuple>();
				item.clipped = true;
				for (size_t k = 0; k < 4; ++k)
					item.clip[k] = c[k].cast<float>();
			}
			item.style = narrow(t[7]);
			item.text = gameMarkup(toWide(t[8]).c_str());
			item.texture = narrow(t[9]);
			item.texture2 = narrow(t[10]);
			item.font = t[11].cast<int>();
			item.flags = t[12].cast<int>();
			item.state = t[13].cast<bool>();
			item.enabled = t[14].cast<bool>();
			const int hitTest = t[15].cast<int>();
			if (!t[16].is_none())
			{
				const py::tuple c = t[16].cast<py::tuple>();
				item.hasColor = true;
				item.color = { c[0].cast<float>(), c[1].cast<float>(), c[2].cast<float>() };
				item.alpha = c[3].cast<float>();
			}
			Item info;
			const py::tuple key = t[0].cast<py::tuple>();
			info.screen = key[0].cast<int>();
			info.name = narrow(key[1]);
			info.part = key[2].cast<int>();
			info.kind = kindName;
			info.widget = t[17].cast<int>();
			info.data1 = t[18].cast<int>();
			info.data2 = t[19].cast<int>();
			info.option = t[20].cast<bool>();
			info.checkbox = item.kind == Kind::CheckBox;
			if (item.kind == Kind::Movie)
				info.movie = item.texture;
			if ((item.kind == Kind::Bar || item.kind == Kind::Scrollbar) && t.size() >= 25)
			{
				item.barStart = t[23].cast<float>();
				item.barPercent = t[24].cast<float>();
				item.barLast = t.size() >= 26 && t[25].cast<bool>();
			}
			if (item.kind == Kind::Leaderhead)
			{
				// (the leader's art: its model, its animations, its scene; "attitude action count")
				const int leader = std::atoi(item.texture.c_str());
				item.texture.clear();
				if (leader >= 0 && leader < GC.getNumLeaderHeadInfos())
				{
					const CvLeaderHeadInfo& info = GC.getLeaderHeadInfo(LeaderHeadTypes(leader));
					item.leaderButton = info.getButton();
					if (const CvArtInfoLeaderhead* art = info.getArtInfo())
					{
						item.texture = art->getNIF() ? art->getNIF() : "";
						item.texture2 = art->getKFM() ? art->getKFM() : "";
						item.leaderBackground = art->getBackgroundKFM() ? art->getBackgroundKFM() : "";
					}
				}
				std::sscanf(item.text.c_str(), "%d %d %d", &item.attitude, &item.action, &item.actionSerial);
				item.text.clear();
			}
			if (item.kind == Kind::Model)
			{
				// ("tilt turn scale animated|button", "...|button|layers")
				int animated = 1;
				std::sscanf(item.text.c_str(), "%f %f %f %d", &item.modelTilt, &item.modelTurn, &item.modelScale, &animated);
				item.modelAnimated = animated != 0;
				if (const size_t bar = item.text.find('|'); bar != std::string::npos)
				{
					item.leaderButton = item.text.substr(bar + 1);
					if (const size_t second = item.leaderButton.find('|'); second != std::string::npos)
					{
						item.modelLayers = item.leaderButton.substr(second + 1);
						item.leaderButton.resize(second);
					}
				}
				item.text.clear();
			}
			const bool buttonLike = item.kind == Kind::ImageButton || item.kind == Kind::Button || item.kind == Kind::CheckBox ||
				item.kind == Kind::Dropdown || item.kind == Kind::EndTurnButton || item.kind == Kind::Cell || item.kind == Kind::Scrollbar;
			// (a text, setText, is clicked as a button is: the scripts' "EXIT"s are texts)
			item.mainInterface = !m_menu && info.screen == m_mainId;
			item.interactive = hitTest != HITTEST_NOHIT && (buttonLike || item.kind == Kind::Text || item.kind == Kind::Movie || (info.widget >= 0 && info.widget != WIDGET_GENERAL) ||
				(item.kind == Kind::Header && info.part <= -100)); // (a sorting table's header)
			item.blocks = (info.screen != m_mainId && (item.kind == Kind::Panel || item.kind == Kind::Image || item.kind == Kind::Graphic || item.kind == Kind::Leaderhead ||
				item.kind == Kind::Model || item.kind == Kind::Movie)) ||
				item.style == "__droplist";
			items.push_back(std::move(item));
			m_items.push_back(std::move(info));
		}
		window.setUi(std::move(items));
		m_hovered = -2; // (the help again, the items are new)
	}

	void Screens::input(render::MapWindow& window)
	{
		if (!m_active)
			return;
		m_mouse = window.mousePosition();
		// A movie at its end: its screen hears NOTIFY_MOVIE_DONE.
		constexpr int kMovieDone = 17;
		for (const std::string& file : window.takeMoviesDone())
			if (const auto it = std::ranges::find(m_items, file, &Item::movie); it != m_items.end())
				scriptInput(*it, kMovieDone, 0, 0, false, false, false, false);
		for (const auto& click : window.takeUiClicks())
			if (click.item >= 0 && size_t(click.item) < m_items.size())
			{
				if (std::getenv("OC4_PLAY_LOG"))
					std::printf("  mouse on %d/%s/%d\n", m_items[size_t(click.item)].screen, m_items[size_t(click.item)].name.c_str(), m_items[size_t(click.item)].part);
				this->click(m_items[size_t(click.item)], click.right, click.shift, click.ctrl, click.alt);
			}
	}

	bool Screens::click(const std::string& name, int part, bool shift, bool ctrl, bool alt)
	{
		for (const Item& it : m_items)
			if (it.name == name && (part == -1 || it.part == part))
			{
				if (std::getenv("OC4_PLAY_LOG"))
					std::printf("  clicked %s/%d (widget %d, %d, %d): %s\n", name.c_str(), it.part, it.widget, it.data1, it.data2, help(it).c_str());
				click(Item(it), false, shift, ctrl, alt);
				return true;
			}
		return false;
	}

	bool Screens::hover(render::MapWindow& window, const std::string& name, int part)
	{
		for (size_t i = 0; i < m_items.size(); ++i)
			if (m_items[i].name == name && (part == -1 || m_items[i].part == part))
			{
				window.hoverUi(int(i));
				return true;
			}
		return false;
	}

	void Screens::click(const Item& it, bool right, bool shift, bool ctrl, bool alt)
	{
		// What the script hears: a click (NOTIFY_CLICKED, the part), or an item of a list
		// chosen (NOTIFY_LISTBOX_ITEM_SELECTED, its index; a table's column as the mouse's x).
		constexpr int kClicked = 0, kListItem = 11;
		int notify = kClicked, data = it.part < 0 ? 0 : (it.part & 0xFFFF), column = 0;
		try
		{
			py::module_ screens = py::module_::import("oc4screens");
			// (a button's click is heard, as the original's interface plays it)
			if (it.kind == "button" || it.kind == "imagebutton" || it.kind == "checkbox" || it.kind == "endbutton" || it.kind == "dropdown" || it.kind == "option")
				m_utility.sounds().play(it.kind == "checkbox" ? "AS2D_IF_CHECKBOX" : "AS2D_IF_SELECT");
			if (it.kind == "header")
			{
				// A sorting table's header: its rows by that column (the engine's own; the script hears nothing).
				screens.attr("closeDropdown")();
				if (!right)
					screens.attr("sortBy")(it.screen, it.name, it.part);
				return;
			}
			if (it.kind == "scrollbar")
			{
				// A scroll bar: what it scrolls moves so that its grip is under the mouse.
				screens.attr("closeDropdown")();
				if (!right)
					screens.attr("scrollTo")(it.screen, it.name, it.part, m_mouse.first, m_mouse.second);
				return;
			}
			if (it.screen == kDiplomacyScreen)
			{
				// The engine's own screen: its clicks are its own (an answer, an item to trade).
				screens.attr("closeDropdown")();
				screens.attr("diploClick")(it.name, it.part, right, shift);
				return;
			}
			if (it.kind == "option")
			{
				// An item of an open pull down's list: the pull down's.
				if (right || !screens.attr("chooseOption")(it.screen, it.name, it.part).cast<bool>())
					return;
				notify = kListItem;
				data = it.part;
			}
			else
			{
				// (any other click closes the open list; one on a pull down opens its own)
				screens.attr("closeDropdown")();
				if (it.kind == "dropdown")
				{
					if (!right)
						screens.attr("openDropdown")(it.screen, it.name);
					return;
				}
			}
			if (it.kind == "cell" && !right)
				if (const py::object row = screens.attr("rowClicked")(it.screen, it.name, it.part, shift, ctrl); !row.is_none())
				{
					notify = kListItem;
					data = row[py::int_(0)].cast<int>();
					column = row[py::int_(1)].cast<int>();
				}
			// The globe's layer buttons and their options: the engine's own, not the DLL's.
			if ((it.widget == WIDGET_GLOBELAYER || it.widget == WIDGET_GLOBELAYER_OPTION) && !right)
			{
				m_utility.chooseGlobeLayer(it.widget == WIDGET_GLOBELAYER, it.data1);
				return;
			}
			// The WorldBuilder's buttons (the engine's own: save, load, its modes, ...): oc4wb.
			if (it.widget >= WIDGET_WB_UNITNAME_EDITBOX && it.widget <= WIDGET_WB_REGENERATE_MAP && !right)
			{
				py::module_::import("oc4wb").attr("widgetClicked")(it.widget, it.data1, it.data2);
				return;
			}
			// The engine's own "close this screen" (a wonder's movie's OK): the screen it is on closes.
			if (it.widget == WIDGET_CLOSE_SCREEN && !right)
			{
				screens.attr("hide")(it.screen);
				py::module_::import("CvScreensInterface").attr("onClose")(py::make_tuple(it.screen));
				return;
			}
			// The widget's own action in the DLL, as the original's widgets have it.
			if (it.widget >= 0 && it.widget != WIDGET_GENERAL)
			{
				CvWidgetDataStruct w{ it.data1, it.data2, it.option, static_cast<WidgetTypes>(it.widget) };
				m_utility.setModifierKeys(shift, ctrl, alt);
				if (right)
					CvDLLWidgetData::getInstance().executeAltAction(w);
				else
					CvDLLWidgetData::getInstance().executeAction(w);
				m_utility.setModifierKeys(false, false, false);
			}
			if (it.checkbox && !right)
				screens.attr("toggle")(it.screen, it.name);
		}
		catch (py::error_already_set& e)
		{
			printPythonError(e);
			return;
		}
		scriptInput(it, notify, data, column, right, shift, ctrl, alt);
	}

	void Screens::scriptInput(const Item& it, int notify, int data, int column, bool right, bool shift, bool ctrl, bool alt)
	{
		try
		{
			// The name's digits are its item id ("CityTab2": "CityTab", 2; a name without them 0, as
			// BUG's domestic advisor finds its pull down's, "DomPagesDD" "0").
			size_t digits = it.name.size();
			while (digits > 0 && std::isdigit(static_cast<unsigned char>(it.name[digits - 1])))
				--digits;
			const std::string function = it.name.substr(0, digits);
			const int id = digits < it.name.size() ? std::atoi(it.name.c_str() + digits) : 0;
			const py::object flags = py::module_::import("CvPythonExtensions").attr("MouseFlags").attr(right ? "MOUSE_RBUTTONUP" : "MOUSE_LBUTTONUP");
			py::list args;
			for (const py::object& o : { py::cast(notify), py::cast(data), flags, py::cast(id),
					 py::cast(it.screen), py::cast(function), py::cast(shift), py::cast(ctrl), py::cast(alt), py::cast(column), py::cast(0),
					 py::cast(it.widget), py::cast(it.data1), py::cast(it.data2), py::cast(it.option) })
				args.append(o);
			// (the host's own screens: the menu's, the turn log's)
			const char* handler = it.name.starts_with("OC4WB") ? "oc4wb" : it.screen == kOptionsScreen ? "oc4options" : m_menu ? "oc4menu" :
				it.screen == kTurnLogScreen ? "oc4turnlog" : "CvScreensInterface";
			py::module_::import(handler).attr("handleInput")(py::tuple(args));
		}
		catch (py::error_already_set& e)
		{
			printPythonError(e);
		}
	}

	bool Screens::wheel(int x, int y, int delta)
	{
		if (!m_active)
			return false;
		try
		{
			return py::module_::import("oc4screens").attr("wheel")(x, y, delta, render::heldModifiers().shift).cast<bool>();
		}
		catch (py::error_already_set& e)
		{
			printPythonError(e);
		}
		return false;
	}

	bool Screens::keyEvent(int key, bool shift, bool ctrl, bool alt, int mouseX, int mouseY, int plotX, int plotY)
	{
		bool taken = false;
		m_utility.setModifierKeys(shift, ctrl, alt);
		try
		{
			py::module_ events = py::module_::import("CvEventInterface");
			for (const int type : { 6 /*EventKeyDown*/, 7 /*EventKeyUp*/ })
			{
				// (the handlers read the last six: debug, unused, Alt, Ctrl, Shift, cheats)
				const py::object r = events.attr("onEvent")(py::make_tuple("kbdEvent", type, key, mouseX, mouseY, plotX, plotY, false, false, alt, ctrl, shift, false));
				if (type == 6)
					taken = !r.is_none() && PyObject_IsTrue(r.ptr()) == 1;
			}
		}
		catch (py::error_already_set& e)
		{
			printPythonError(e);
		}
		m_utility.setModifierKeys(false, false, false);
		return taken;
	}

	bool Screens::diploKey(unsigned key)
	{
		if (!m_active || !m_utility.diplomacy().active())
			return false;
		try
		{
			return py::module_::import("oc4screens").attr("diploKey")(key).cast<bool>();
		}
		catch (py::error_already_set& e)
		{
			printPythonError(e);
		}
		return false;
	}

	bool Screens::escape()
	{
		if (!m_active)
			return false;
		try
		{
			py::module_ screens = py::module_::import("oc4screens");
			if (screens.attr("closeDropdown")().cast<bool>())
				return true;
			if (m_menu)
				return py::module_::import("oc4menu").attr("escape")().cast<bool>();
			// (a talk: its way out, when it has one)
			if (m_utility.diplomacy().active())
				return screens.attr("diploEscape")().cast<bool>();
			for (auto entry : py::reinterpret_borrow<py::dict>(screens.attr("_screens")))
			{
				const int id = entry.first.cast<int>();
				if (screens.attr("closeOnEscape")(id).cast<bool>())
				{
					screens.attr("hide")(id);
					py::module_::import("CvScreensInterface").attr("onClose")(py::make_tuple(id));
					return true;
				}
			}
		}
		catch (py::error_already_set& e)
		{
			printPythonError(e);
		}
		return false;
	}
}
