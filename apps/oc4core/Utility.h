#pragma once

// CvDLLUtilityIFaceBase: the engine services the game DLL calls through gDLL.

#include "Audio.h"
#include "Diplomacy.h"
#include "FAStar.h"
#include "Interface.h"
#include "PythonHost.h"
#include "Text.h"
#include "Xml.h"
#include "generated/IFaceStubs.h"

#include "core/AssetResolver.h"
#include "render/EdgeScroll.h"

#include <filesystem>
#include <fstream>
#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <map>
#include <string>

namespace oc4::host
{
	// The engine's heap as the game DLL sees it (mods route their operator new through it).
	class EngineMemory : public stubs::UtilityIFace
	{
	public:
		void delMem(void* p) override { std::free(p); }
		void* newMem(size_t size) override { return std::malloc(size); }
		void delMem(void* p, const char*, int) override { std::free(p); }
		void* newMem(size_t size, const char*, int) override { return std::malloc(size); }
		void delMemArray(void* p, const char*, int) override { std::free(p); }
		void* newMemArray(size_t size, const char*, int) override { return std::malloc(size); }
		void* reallocMem(void* a, unsigned int uiBytes, const char*, int) override { return std::realloc(a, uiBytes); }
		unsigned int memSize(void* a) override { return static_cast<unsigned int>(_msize(a)); }
	};

	// The game core's interface once the engine is gone: its static destructors still free
	// memory through it when the process exits (AND2's operator delete). Never destroyed.
	CvDLLUtilityIFaceBase& engineMemoryAfterShutdown();

	// The engine's landscape as the game core asks for it: the map's plots of PLOT_SIZE, the map's
	// middle at 0,0 (CvMap::plotXToPointX, AND2's viewports place their points by it), flat
	// ground.
	class Engine : public stubs::EngineIFace
	{
	public:
		void GetLandscapeDimensions(float& fWidth, float& fHeight) override;
		void GetLandscapeGameDimensions(float& fWidth, float& fHeight) override;
		float GetHeightmapZ(const NiPoint3&, bool) override { return 0; }
		// The game's autosave (each turn's start, and the game's first): whoever plays saves it.
		void AutoSave(bool bInitial) override
		{
			if (onAutoSave)
				onAutoSave(bInitial);
		}
		std::function<void(bool initial)> onAutoSave;

		// The map's display the game's controls switch (Ctrl+R, the unit icons, Ctrl+T): the
		// window shows them so.
		std::atomic<bool> resourceLayer = false, unitLayer = false, grid = false;
		void toggleResourceLayer() override { resourceLayer = !resourceLayer; }
		void setResourceLayer(bool bOn) override { resourceLayer = bOn; }
		void toggleUnitLayer() override { unitLayer = !unitLayer; }

		// The globe view (CONTROL_GLOBELAYER toggles it: the camera back over the whole map) and
		// the globe's layer shown (CvGame::getGlobeLayers' index, -1 none) with its option: the
		// units' and resources' are the icon layers above, the others colour the plots
		// (CvPlayer::getGlobeLayerColors). version changes with any of them.
		std::atomic<bool> globeUp = false;
		std::atomic<int> globeLayer = -1, globeOption = 0;
		std::atomic<uint64_t> globeVersion = 0;
		void toggleGlobeview() override;
		bool isGlobeviewUp() override { return globeUp; }
		bool GetGridMode() override { return grid; }
		void SetGridMode(bool bVal) override { grid = bVal; }

		// The plots the game core and the scripts mark on the map, by layer: coloured
		// (addColoredPlot: a circle on the plot, the recommended city sites, the city's worked
		// plots) or bordered (fillAreaBorderPlot: the outline round a set of plots, a city's
		// radius, a ranged unit's reach), and the signs and landmarks with their captions.
		// The game core's plots are in its own coordinates (core: a mod's viewport moves
		// them in the window). Any thread; version changes with them.
		struct PlotMark
		{
			int x = 0, y = 0;
			float r = 1, g = 1, b = 1, a = 1;
			int layer = 0;
			bool border = false, core = true;
		};
		struct Sign
		{
			int x = 0, y = 0, player = -1;
			std::wstring caption;
			bool core = true;
		};
		void addColoredPlot(int plotX, int plotY, const NiColorA& color, PlotStyles, PlotLandscapeLayers layer) override
		{
			mark({ plotX, plotY, color.r, color.g, color.b, color.a, int(layer), false });
		}
		void clearColoredPlots(PlotLandscapeLayers layer) override { clear(int(layer), false); }
		void fillAreaBorderPlot(int plotX, int plotY, const NiColorA& color, AreaBorderLayers layer) override
		{
			mark({ plotX, plotY, color.r, color.g, color.b, color.a, int(layer), true });
		}
		void clearAreaBorderPlots(AreaBorderLayers layer) override { clear(int(layer), true); }
		void updateFoundingBorder() override { foundingDirty = true; }
		std::atomic<bool> foundingDirty = false; // (the selected settler's city radius: whoever plays draws it again)
		void addLandmark(CvPlot* plot, const wchar* caption) override;
		void clearSigns() override
		{
			std::lock_guard lock(marksMutex);
			signs.clear();
			++marksVersion;
		}
		void mark(const PlotMark& m)
		{
			std::lock_guard lock(marksMutex);
			marks.push_back(m);
			++marksVersion;
		}
		void clear(int layer, bool border)
		{
			std::lock_guard lock(marksMutex);
			std::erase_if(marks, [&](const PlotMark& m) { return m.border == border && (layer < 0 || m.layer == layer); });
			++marksVersion;
		}
		void addSign(Sign s)
		{
			std::lock_guard lock(marksMutex);
			std::erase_if(signs, [&](const Sign& o) { return o.x == s.x && o.y == s.y && o.player == s.player && o.core == s.core; });
			signs.push_back(std::move(s));
			++marksVersion;
		}
		// The effects the game core sets off (TriggerEffect: a battle's smoke, an explosion, a
		// feature's birds): at a plot of its own coordinates (and where in it, 0..1), for whoever
		// draws the map to take. Any thread.
		struct Effect
		{
			int type = -1, x = 0, y = 0;
			float fx = 0.5f, fy = 0.5f, rotation = 0;
		};
		void TriggerEffect(int iEffect, NiPoint3 pt3Point, float rotation) override;
		std::vector<Effect> takeEffects()
		{
			std::lock_guard lock(marksMutex);
			return std::exchange(effects, {});
		}
		std::vector<Effect> effects;
		// The Great Wall built or lost somewhere: whoever draws the map looks again.
		void AddGreatWall(CvCity*) override { ++greatWallVersion; }
		void RemoveGreatWall(CvCity*) override { ++greatWallVersion; }
		std::atomic<uint64_t> greatWallVersion = 1;
		std::mutex marksMutex;
		std::vector<PlotMark> marks;
		std::vector<Sign> signs;
		std::atomic<uint64_t> marksVersion = 0;
	};

	class Utility : public EngineMemory
	{
	public:
		Utility(const AssetResolver& assets, PythonHost& python, Xml& xml, std::filesystem::path logDir);

		TextManager& text() { return m_text; }
		const AssetResolver& assets() const { return m_assets; }
		Engine& engine() { return m_engine; }

		// Interfaces
		CvDLLEntityIFaceBase* getEntityIFace() override { return &m_entity; }
		CvDLLInterfaceIFaceBase* getInterfaceIFace() override { return &m_interface; }
		Interface& gameInterface() { return m_interface; }
		CvDLLEngineIFaceBase* getEngineIFace() override { return &m_engine; }
		CvDLLIniParserIFaceBase* getIniParserIFace() override { return &m_iniParser; }
		CvDLLSymbolIFaceBase* getSymbolIFace() override { return &m_symbol; }
		CvDLLFeatureIFaceBase* getFeatureIFace() override { return &m_feature; }
		CvDLLRouteIFaceBase* getRouteIFace() override { return &m_route; }
		CvDLLPlotBuilderIFaceBase* getPlotBuilderIFace() override { return &m_plotBuilder; }
		CvDLLRiverIFaceBase* getRiverIFace() override { return &m_river; }
		CvDLLFAStarIFaceBase* getFAStarIFace() override { return &m_faStar; }
		CvDLLXmlIFaceBase* getXMLIFace() override { return &m_xml; }
		CvDLLFlagEntityIFaceBase* getFlagEntityIFace() override { return &m_flagEntity; }
		CvDLLPythonIFaceBase* getPythonIFace() override { return &m_python; }

		// Memory (and EngineMemory)
		void clearVector(std::vector<int>& vec) override { std::vector<int>().swap(vec); }
		void clearVector(std::vector<byte>& vec) override { std::vector<byte>().swap(vec); }
		void clearVector(std::vector<float>& vec) override { std::vector<float>().swap(vec); }

		// Session / lobby: a local single-player engine
		int getAssignedNetworkID(int player) override;
		bool isConnected(int player) override;
		bool isGameActive() override { return m_gameActive; }
		int GetLocalNetworkID() override { return m_pitbossHost ? -1 : m_pitbossSeat; }
		bool IsPitbossHost() const override { return m_pitbossHost; }
		void setPitboss(bool host, int seat = -1) { m_pitboss = true; m_pitbossHost = host; m_pitbossSeat = seat; }
		CvString GetPitbossSmtpHost() const override { return {}; }
		CvWString GetPitbossSmtpLogin() const override { return {}; }
		CvWString GetPitbossSmtpPassword() const override { return {}; }
		CvString GetPitbossEmail() const override { return {}; }
		int getChtLvl() override { return 0; }
		bool GetWorldBuilderMode() override { return m_interface.worldBuilder(); }
		int getCurrentLanguage() const override { return m_language; }
		void setCurrentLanguage(int iNewLanguage) override { m_language = iNewLanguage; }
		bool isModularXMLLoading() const override { return false; }
		bool GetAutorun() override { return false; }
		void callUpdater() override {}
		void openSlot(PlayerTypes) override {}
		// Length of a turn slice; the DLL scales mission animations with it.
		int getMillisecsPerTurn() override { return m_pitboss ? 250 : 1000; }
		float getSecsPerTurn() override { return m_pitboss ? 0.25f : 1.0f; }
		int getTurnsPerMinute() override { return 60000 / getMillisecsPerTurn(); } // turn slices; CvGame divides by it
		void reportStatistics() override {} // online stat reporting
		CvWString getMapScriptName() override { return CvWString(std::string(m_python.getMapScriptModule())); }
		bool getTransferredMap() override { return false; }
		void stripSpecialCharacters(CvWString&) override {}

		// Messages: single player executes them locally, in order, before the next game update.
		void sendMessageData(CvMessageData* pData) override;
		void sendPlayerOption(PlayerOptionTypes eOption, bool bValue) override;
		bool getPlayerOption(PlayerOptionTypes ePlayerOption) override;
		// The XML defaults: there is no options screen.
		bool getGraphicOption(GraphicOptionTypes eGraphicOption) override;
		// The profile's (oc4options); the XML's default for one it does not have.
		void setGraphicOption(int option, bool on)
		{
			if (option >= 0 && size_t(option) < m_graphicOptions.size())
				m_graphicOptions[size_t(option)] = on ? 2 : 1;
		}
		// Whether the profile has it on (not the XML's default: the window is not full screen
		// until asked).
		bool graphicOptionChosen(int option) const { return option >= 0 && size_t(option) < m_graphicOptions.size() && m_graphicOptions[size_t(option)] == 2; }
		// The interface's scale in percent (the profile's; 0: the window's density). A forced
		// one (--ui-scale) stays whatever the profile says.
		int uiScale() const { return m_forcedUiScale >= 0 ? m_forcedUiScale : m_uiScale; }
		void setUiScale(int percent) { m_uiScale = percent; }
		void forceUiScale(int percent) { m_forcedUiScale = percent; }
		// W A S D pan the map (the profile's, or --wasd-pan's for the session until the player
		// chooses: the options' check box, or Ctrl+Shift+W to turn it off).
		bool wasdPan() const { return m_forcedWasdPan || m_wasdPan; }
		void setWasdPan(bool on) { m_wasdPan = on; }
		void forceWasdPan() { m_forcedWasdPan = true; }
		void chooseWasdPan(bool on) { m_forcedWasdPan = false; m_wasdPan = on; }
		// End idle turns by themselves (AutoEndTurn.h; the profile's, or --auto-end-turn's for the
		// session until the player chooses: Ctrl+Shift+E or the options' check box).
		bool autoEndTurn() const { return m_forcedAutoEndTurn || m_autoEndTurn; }
		void setAutoEndTurn(bool on) { m_autoEndTurn = on; }
		void forceAutoEndTurn() { m_forcedAutoEndTurn = true; }
		void chooseAutoEndTurn(bool on) { m_forcedAutoEndTurn = false; m_autoEndTurn = on; }
		// The mouse at the window's edge scrolls the map (render/EdgeScroll.h; on unless the profile
		// says off, or --no-edge-scroll / a scripted run for the session until the player chooses).
		bool edgeScroll() const { return m_edgeScroll.on(); }
		void setEdgeScroll(bool on) { m_edgeScroll.set(on); }
		void forceNoEdgeScroll() { m_edgeScroll.forceOff(); }
		void chooseEdgeScroll(bool on) { m_edgeScroll.choose(on); }
		void resetGraphicOptions()
		{
			for (auto& o : m_graphicOptions)
				o = 0;
		}
		bool ChangeINIKeyValue(const char*, const char*, const char*) override { return false; } // the engine writes no INI
		// The user profile's player options (wait at end of turn, ...): XML defaults for a
		// new game, the active player's own after loading a save.
		void setProfileOptionsFromPlayer(PlayerTypes ePlayer);
		void pumpMessages();
		// Multiplayer (Net): a frame's command run, and a player who left made an AI player.
		void runCommand(const std::vector<std::byte>& command);
		void sendMakeAI(PlayerTypes ePlayer);

		// Diplomacy (the screen: Diplomacy; no multiplayer one)
		void beginDiplomacy(CvDiploParameters* pDiploParams, PlayerTypes ePlayer) override { m_diplomacy.begin(pDiploParams, ePlayer); }
		bool isDiplomacy() override { return m_diplomacy.active(); }
		int getDiplomacyPlayer() override { return g_diplomacyPlayerOverride >= 0 ? g_diplomacyPlayerOverride : m_diplomacy.who(); }

		// The game menu's and the controls' saving, loading and leaving (CONTROL_SAVE_NORMAL,
		// CONTROL_QUICK_SAVE, ...): for whoever plays the game to do (nothing without one).
		struct Session
		{
			std::function<void(SaveGameTypes)> save;
			std::function<void()> quickSave, quickLoad, load, quit;
		} session;
		void SaveGame(SaveGameTypes eSaveGame) override
		{
			if (session.save)
				session.save(eSaveGame);
		}
		void LoadGame() override
		{
			if (session.load)
				session.load();
		}
		void QuickSave() override
		{
			if (session.quickSave)
				session.quickSave();
		}
		void QuickLoad() override
		{
			if (session.quickLoad)
				session.quickLoad();
		}
		void SetDone(bool bDone) override
		{
			m_done = bDone;
			if (bDone && session.quit)
				session.quit();
		}
		bool GetDone() override { return m_done; }
		void updateDiplomacyAttitude(bool) override {} // (the screen reads it when it is drawn)
		bool isMPDiplomacy() override { return false; }
		bool isMPDiplomacyScreenUp() override { return false; }
		void endDiplomacy() override { m_diplomacy.end(); }
		void endMPDiplomacy() override {}
		Diplomacy& diplomacy() { return m_diplomacy; }
		// A globe layer's button clicked (again: none), or one of its options.
		void chooseGlobeLayer(bool layer, int index);
		// The game's messages of a talk, as the original sends them over the network: run for
		// the active player at once.
		void sendDiploEvent(PlayerTypes eWhoTradingWith, DiploEventTypes eDiploEvent, int iData1, int iData2) override;
		void sendImplementDealMessage(PlayerTypes eOtherWho, CLinkList<TradeData>* pOurList, CLinkList<TradeData>* pTheirList) override;
		void sendKillDeal(int iDealID, bool bFromDiplomacy) override;
		// The modifier keys of the player's input being handled (set by the host around it; the
		// game's actions read them: Shift queues a mission, Alt applies to the whole stack).
		// A single player game: everyone is connected.
		PlayerTypes getFirstBadConnection() override { return NO_PLAYER; }
		int getConnState(PlayerTypes) override { return 0; }
		bool altKey() override { return m_alt; }
		bool shiftKey() override { return m_shift; }
		bool ctrlKey() override { return m_ctrl; }
		void setModifierKeys(bool shift, bool ctrl, bool alt)
		{
			m_shift = shift;
			m_ctrl = ctrl;
			m_alt = alt;
		}
		bool scrollLock() override { return false; }
		bool capsLock() override { return false; }
		bool numLock() override { return false; }

		// Text
		CvWString getText(CvWString szIDTag, ...) override;
		CvWString getObjectText(CvWString szIDTag, uint uiForm, bool bNoSubs) override;
		void addText(const TCHAR* szIDTag, const wchar* szString, const wchar* szGender, const wchar* szPlural) override;
		uint getNumForms(CvWString szIDTag) override;
		int getSymbolID(int iID) override { return Interface::symbolID(iID); } // (see Interface)
		void setSymbolID(int iID, int iValue) override
		{
			if (iID < 0)
				return;
			if (size_t(iID) >= Interface::s_symbols.size())
				Interface::s_symbols.resize(size_t(iID) + 1, 0);
			Interface::s_symbols[size_t(iID)] = iValue;
		}

		// Files
		bool fileManagerEnabled() override { return true; }
		void enumerateFiles(std::vector<CvString>& files, const char* szPattern) override;
		// "Mods\Name\" (relative to the Beyond the Sword folder) or the full path of a mod that
		// is not installed; just "Name" without bFullPath.
		const char* getModName(bool bFullPath) const override { return bFullPath ? m_modFolder.c_str() : m_modName.c_str(); }
		// With a trailing backslash; empty without a mod.
		void setModFolder(const std::string& folder);
		bool isDescFileName(const char* szFileName) override;
		bool pythonMapExists(const char* szMapName) override { return szMapName && m_python.findScript(szMapName); }

		// Audio: the script ids resolve with or without a device (the eras' soundtracks are only
		// looked up when audio is not disabled); what plays, plays where a window has started
		// the sounds.
		bool getAudioDisabled() override { return false; }
		int getAudioTagIndex(const TCHAR* szTag, int iScriptType) override { return szTag ? m_audio.index(szTag, iScriptType) : -1; }
		void DoSound(int iScriptId) override { m_sounds.play(AudioScripts::Script2D, iScriptId); }
		void Do3DSound(int iScriptId, NiPoint3 vPosition) override;
		Sounds& sounds() { return m_sounds; }
		AudioScripts& audioScripts() { return m_audio; }

		// Caches: always read the XML
		CvCacheObject* createGlobalTextCacheObject(const TCHAR*) override { return nullptr; }
		CvCacheObject* createGlobalDefinesCacheObject(const TCHAR*) override { return nullptr; }
		CvCacheObject* createTechInfoCacheObject(const TCHAR*) override { return nullptr; }
		CvCacheObject* createBuildingInfoCacheObject(const TCHAR*) override { return nullptr; }
		CvCacheObject* createUnitInfoCacheObject(const TCHAR*) override { return nullptr; }
		CvCacheObject* createLeaderHeadInfoCacheObject(const TCHAR*) override { return nullptr; }
		CvCacheObject* createCivilizationInfoCacheObject(const TCHAR*) override { return nullptr; }
		CvCacheObject* createPromotionInfoCacheObject(const TCHAR*) override { return nullptr; }
		CvCacheObject* createDiplomacyInfoCacheObject(const TCHAR*) override { return nullptr; }
		CvCacheObject* createEventInfoCacheObject(const TCHAR*) override { return nullptr; }
		CvCacheObject* createEventTriggerInfoCacheObject(const TCHAR*) override { return nullptr; }
		CvCacheObject* createCivicInfoCacheObject(const TCHAR*) override { return nullptr; }
		CvCacheObject* createHandicapInfoCacheObject(const TCHAR*) override { return nullptr; }
		CvCacheObject* createBonusInfoCacheObject(const TCHAR*) override { return nullptr; }
		CvCacheObject* createImprovementInfoCacheObject(const TCHAR*) override { return nullptr; }
		bool cacheRead(CvCacheObject*, const TCHAR*) override { return false; }
		bool cacheWrite(CvCacheObject*) override { return true; } // the DLL reports a failed write as an error
		void destroyCache(CvCacheObject*& pCache) override { pCache = nullptr; }

		// Diagnostics
		void logMsg(const TCHAR* pLogFileName, const TCHAR* pBuf, bool bWriteToConsole, bool bTimeStamp) override;
		// Appends text as is (Python's redirected stdout/stderr); dropped without a log folder.
		void logRaw(const std::string& fileName, std::string_view text);
		void logMemState(const char*) override {}
		void messageControlLog(char* s) override;
		void NiTextOut(const TCHAR* szText) override;
		void MessageBox(const TCHAR* szText, const TCHAR* szCaption) override;
		void ProfilerBegin() override {}
		void ProfilerEnd() override {}
		void BeginSample(ProfileSample*) override {}
		void EndSample(ProfileSample*) override {}

		// Globals the original engine allocated: nothing to do so far
		void initGlobals() override {}
		void uninitGlobals() override {}

		bool isGameInitializing() override { return m_gameInitializing; }
		void setGameActive(bool active) { m_gameActive = active; }
		void setGameInitializing(bool initializing) { m_gameInitializing = initializing; }

	private:
		bool m_pitboss = false, m_pitbossHost = false;
		int m_pitbossSeat = -1;
		bool m_done = false;
		const AssetResolver& m_assets;
		PythonHost& m_python;
		Xml& m_xml;
		TextManager m_text;
		std::vector<CvMessageData*> m_messages;
		std::vector<char> m_profileOptions; // empty: XML defaults
		int m_uiScale = 0, m_forcedUiScale = -1;
		bool m_wasdPan = false, m_forcedWasdPan = false;
		bool m_autoEndTurn = false, m_forcedAutoEndTurn = false;
		render::EdgeScrollOption m_edgeScroll;
		std::array<std::atomic<int8_t>, 64> m_graphicOptions{}; // 0 the XML's default, 1 off, 2 on (set by the window's thread, read by the game's)
		AudioScripts m_audio{ m_assets };
		Sounds m_sounds{ m_assets, m_audio };
		std::filesystem::path m_logDir;
		std::map<std::string, std::ofstream> m_logs;
		std::string m_modFolder, m_modName;
		int m_language = 0;
		bool m_gameActive = false, m_gameInitializing = false;
		bool m_shift = false, m_ctrl = false, m_alt = false;

		Interface m_interface;
		Diplomacy m_diplomacy;
		Entities m_entity{ m_interface };
		Engine m_engine;
		stubs::IniParserIFace m_iniParser;
		stubs::SymbolIFace m_symbol;
		stubs::FeatureIFace m_feature;
		stubs::RouteIFace m_route;
		stubs::PlotBuilderIFace m_plotBuilder;
		stubs::RiverIFace m_river;
		PathFinder m_faStar;
		stubs::FlagEntityIFace m_flagEntity;
	};
}
