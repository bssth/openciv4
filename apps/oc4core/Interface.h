#pragma once

// CvDLLInterfaceIFaceBase without a screen of its own: no screen is up and there are no
// splashes. The units the player has selected are kept as the original's interface keeps
// them (the DLL gives orders to that list: CvGame::selectionListMove and friends, and picks
// the next unit to move with it). Void methods are no-ops in the generated base.

#include <atomic>
#include <bitset>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "generated/IFaceStubs.h"

#include "LinkedList.h"
#include "Sounds.h"

namespace oc4::host
{
	class Interface : public stubs::InterfaceIFace
	{
	public:
		// Selection
		CvPlot* getSelectionPlot() override;
		CvUnit* getSelectionUnit(int iIndex) override;
		CvUnit* getHeadSelectedUnit() override;
		void selectUnit(CvUnit* pUnit, bool bClear, bool bToggle, bool bSound) override;
		void selectGroup(CvUnit* pUnit, bool bShift, bool bCtrl, bool bAlt) override;
		void selectAll(CvPlot* pPlot) override;
		bool removeFromSelectionList(CvUnit* pUnit) override;
		bool mirrorsSelectionGroup() override;
		CvSelectionGroup* getSelectionList() override;
		void clearSelectionList() override
		{
			m_selection.clear();
			selectionChanged();
		}
		void insertIntoSelectionList(CvUnit* pUnit, bool bClear, bool bToggle, bool bGroup, bool bSound, bool bMinimalChange) override;
		CLLNode<IDInfo>* deleteSelectionListNode(CLLNode<IDInfo>* pNode) override { return m_selection.deleteNode(pNode); }
		CLLNode<IDInfo>* nextSelectionListNode(CLLNode<IDInfo>* pNode) override { return m_selection.next(pNode); }
		int getLengthSelectionList() override { return m_selection.getLength(); }
		CLLNode<IDInfo>* headSelectionListNode() override { return m_selection.head(); }
		CvUnit* getLastSelectedUnit() override { return m_lastSelected; }
		void setLastSelectedUnit(CvUnit* pUnit) override { m_lastSelected = pUnit; }
		CvPlot* getOriginalPlot() override { return nullptr; }
		int getOriginalPlotCount() override { return 0; }
		bool canSelectionListFound() override;
		void lookAtSelectionPlot(bool bRelease) override;
		// The camera to a point of the map (the DLL's lookAt: a city, a battle, a mod's viewport
		// moved to the selected unit).
		void lookAt(NiPoint3 pt3Target, CameraLookAtTypes type, NiPoint3 attackDirection) override;
		// Cities: one selected is the city screen (more, with addSelectedCity, only a selection).
		void selectCity(CvCity* pNewValue, bool bTestProduction) override;
		void addSelectedCity(CvCity* pNewValue, bool bToggle) override;
		void clearSelectedCities() override;
		bool isCitySelected(CvCity* pCity) override;
		CvCity* getHeadSelectedCity() override;
		bool isCitySelection() override { return getHeadSelectedCity() != nullptr; }
		bool isCityScreenUp() override { return m_cityScreen && isCitySelection(); }
		CLLNode<IDInfo>* headSelectedCitiesNode() override { return m_cities.head(); }
		CLLNode<IDInfo>* nextSelectedCitiesNode(CLLNode<IDInfo>* pNode) override { return m_cities.next(pNode); }

		// Where the DLL asked the camera to go (lookAtSelectionPlot, lookAtCityOffset), for the
		// window; -1 none.
		int lookAtX = -1, lookAtY = -1;
		bool isSelected(const CvUnit* pUnit) const { return selected(pUnit); }
		void lookAtCityOffset(int iCity) override;

		// Actions as the original's interface asks the game about them (its action buttons and
		// hotkeys run CvGame::handleAction), and the interface mode that waits for a plot (go
		// to, air strike, ...: the host takes the click).
		bool canHandleAction(int iAction, CvPlot* pPlot, bool bTestVisible) override;
		bool canDoInterfaceMode(InterfaceModeTypes eInterfaceMode, CvSelectionGroup* pSelectionGroup) override;
		void setInterfaceMode(InterfaceModeTypes eNewValue) override { m_interfaceMode = eNewValue; }
		InterfaceModeTypes getInterfaceMode() override { return m_interfaceMode; }

		// Popups as the original's interface runs them: the DLL queues them (addPopup, or the
		// player's own list the host pulls with CvPlayer::popFrontPopup); the next one is launched
		// through CvDLLButtonPopup::launchButtonPopup, which fills it with the popup* calls below
		// (the CvPopup it is given is a Popup); the player's answer goes back through the clicked
		// button's widget (CvDLLWidgetData::executeAction: what to build, what to research) and
		// CvDLLButtonPopup::OnOkClicked. While one is up, OnFocus may cancel it (the city has
		// chosen meanwhile). The scripts' own popups (CyPopup) come filled, without a CvPopupInfo:
		// their answer goes to onPythonAnswer.
		struct Popup
		{
			struct Item
			{
				enum class Kind : uint8_t // (in the window's order)
				{
					Text, Button, Radio, Check, Separator, Image, Edit
				};
				enum class Control : uint8_t // what a radio item is one of
				{
					RadioButtons, PullDown, ListBox
				};
				Kind kind = Kind::Text;
				std::wstring text, help; // (an edit box: its text)
				std::string icon;     // button art: a DDS, or ",<file>,<atlas>,<column>,<row>"
				int id = -1;          // the button's id; the radio button's or checkbox's index; a pulldown entry's id
				int group = 0;        // radio buttons, checkboxes, edit boxes, pulldowns, list boxes
				Control control = Control::RadioButtons;
				CvWidgetDataStruct widget{ -1, -1, false, WIDGET_GENERAL };
				bool checked = false; // checkbox; the radio item chosen
			};
			CvPopupInfo* info = nullptr; // owned (deleted through its virtual destructor, in the DLL)
			int python = -1;             // a script's popup: its number there
			PlayerTypes player = NO_PLAYER;
			std::wstring header;
			std::vector<Item> items;
			bool launched = false, okButton = false, cancelled = false;
			std::string style; // popupSetStyle's: "Window_NoTitleBar_Style" for the game menu
			uint64_t serial = 0; // tells one popup from the next
		};
		// The answer to a script's popup: the button clicked (0 for OK), per pulldown, list box
		// and set of radio buttons the id chosen, per group of checkboxes the bits ticked, per edit
		// box its text.
		struct PopupAnswer
		{
			int button = 0;
			std::map<std::pair<Popup::Item::Control, int>, int> chosen;
			std::map<int, int> checked;
			std::map<int, std::wstring> edits;
		};
		std::function<void(const Popup&, const PopupAnswer&)> onPythonAnswer;
		// Whether a script's screen is up (an advisor, a wonder's movie): the next of the DLL's
		// screens to show (BUTTONPOPUP_PYTHON_SCREEN) waits for it to close.
		std::function<bool()> screenUp;
		// A script's popup, filled: shown when its turn comes (first when it is to be shown at once).
		void queuePopup(Popup popup, bool front);
		void addPopup(CvPopupInfo* pInfo, PlayerTypes ePlayer, bool bImmediate, bool bFront) override;
		void getDisplayedButtonPopups(CvPopupQueue& infos) override;
		void clearQueuedPopups() override;
		bool isPopupUp() override { return m_popup != nullptr; }
		void popupSetStyle(CvPopup* pPopup, const char* styleId) override
		{
			if (pPopup && styleId)
				popupOf(pPopup)->style = styleId;
		}
		bool isPopupQueued() override;
		bool isDiploOrPopupWaiting() override { return isPopupUp() || isPopupQueued() || (diplomacyWaiting && diplomacyWaiting()); }
		// A talk is up or waits (the host's diplomacy).
		std::function<bool()> diplomacyWaiting;
		std::function<void(CvDiploParameters*, PlayerTypes)> diplomacyAsked; // (a talk the player asked for)
		// The player's contact with a leader (CvPlayer::contact): the talk waits in the player's
		// queue for the screen, as the AI's do.
		void setDiploQueue(CvDiploParameters* pDiploParams, PlayerTypes ePlayer) override;
		void popupSetHeaderString(CvPopup* pPopup, CvWString szText, uint uiFlags) override;
		void popupSetBodyString(CvPopup* pPopup, CvWString szText, uint uiFlags, char* szName, CvWString szHelpText) override;
		void popupLaunch(CvPopup* pPopup, bool bCreateOkButton, PopupStates bState, int iNumPixelScroll) override;
		void popupAddDDS(CvPopup* pPopup, const char* szIconFilename, int iWidth, int iHeight, CvWString szHelpText) override;
		void popupAddSeparator(CvPopup* pPopup, int iSpace) override;
		void popupCreateEditBox(CvPopup* pPopup, CvWString szDefaultString, WidgetTypes eWidgetType, CvWString szHelpText, int iGroup,
			PopupControlLayout ctrlLayout, unsigned int preferredCharWidth, unsigned int maxCharCount) override;
		void popupAddGenericButton(CvPopup* pPopup, CvWString szText, const char* szIcon, int iButtonId, WidgetTypes eWidgetType, int iData1, int iData2,
			bool bOption, PopupControlLayout ctrlLayout, unsigned int textJustifcation) override;
		void popupCreateRadioButtons(CvPopup* pPopup, int iNumButtons, int iGroup, WidgetTypes eWidgetType, PopupControlLayout ctrlLayout) override;
		void popupSetRadioButtonText(CvPopup* pPopup, int iRadioButtonID, CvWString szText, int iGroup, CvWString szHelpText) override;
		void popupCreateCheckBoxes(CvPopup* pPopup, int iNumBoxes, int iGroup, WidgetTypes eWidgetType, PopupControlLayout ctrlLayout) override;
		void popupSetCheckBoxText(CvPopup* pPopup, int iCheckBoxID, CvWString szText, int iGroup, CvWString szHelpText) override;
		void popupSetCheckBoxState(CvPopup* pPopup, int iCheckBoxID, bool bChecked, int iGroup) override;
		void popupSetAsCancelled(CvPopup* pPopup) override;
		bool popupIsDying(CvPopup* pPopup) override;

		// The host's side: take the active player's popups, show the next one; the one up.
		void updatePopups(PlayerTypes active);
		const Popup* shownPopup() const { return m_popup.get(); }
		// The player chose: a button (index into items), or OK (-1), with the radio buttons and
		// checkboxes as they are set (the items chosen, the items ticked) and the edit boxes'
		// texts (item, text).
		void answerPopup(int item, const std::vector<int>& radioItems, const std::vector<int>& checkedItems,
			const std::vector<std::pair<int, std::wstring>>& edits = {});
		// Esc: closed without an answer.
		void dismissPopup();

		// The plot list (the units on the selection's plot, as the main interface lists them):
		// its rows fill from the bottom (on the city screen only its last is used), so its first
		// unit is on the button after the offset; its units are those the active team sees, in
		// the plot's order. The main interface says how many buttons a row has.
		int getPlotListColumn() override { return 0; }
		int getPlotListOffset() override;
		CvUnit* getInterfacePlotUnit(const CvPlot* pPlot, int iIndex) override;
		std::function<int()> plotListButtonsPerRow;

		// Screens and widgets
		static constexpr int kDirtyBits = Advanced_Start_DIRTY_BIT + 1; // (the count is the DLL's own)
		// What the Python interface redraws: the DLL sets the bits, CvMainInterface.redraw reads
		// and clears them; selections set theirs as the original's interface does.
		bool isDirty(InterfaceDirtyBits eDirtyItem) override { return eDirtyItem >= 0 && eDirtyItem < kDirtyBits && m_dirty[eDirtyItem]; }
		void setDirty(InterfaceDirtyBits eDirtyItem, bool bNewValue) override
		{
			if (eDirtyItem >= 0 && eDirtyItem < kDirtyBits)
				m_dirty[eDirtyItem] = bNewValue;
		}
		void makeInterfaceDirty() override { m_dirty.set(); }
		bool anyDirty() const { return m_dirty.any(); }
		InterfaceVisibility getShowInterface() override { return m_show; }
		void setShowInterface(InterfaceVisibility eInterfaceVisibility) { m_show = eInterfaceVisibility; } // (CyInterface only)

		// The WorldBuilder (CONTROL_WORLD_BUILDER, the scripts' setWorldBuilder): its screen up
		// (CvScreensInterface.showWorldBuilderScreen), the main interface away, the game still.
		// The mouse as its screen asks for it: the plot under it (the game core's coordinates,
		// -1 none), its buttons, whether it is over the map rather than a widget.
		void setWorldBuilder(bool bTurnOn) override;
		bool worldBuilder() const { return m_worldBuilder; }
		int mouseX = -1, mouseY = -1;
		bool mouseLeft = false, mouseRight = false, mouseOverMap = false;
		CvPlot* getMouseOverPlot() override;
		bool updateCursorType() override { return false; }
		bool isFocusedWidget() override { return false; }
		bool isFocused() override { return false; }
		bool isScoresVisible() override { return false; }
		// The tech splash (the scripts' CvTechSplashScreen, with the tech's quote): only with the
		// game's screens up (Screens::start); without them the game core plays the short sound instead.
		bool techSplash = false;
		bool noTechSplash() override { return !techSplash; }
		bool isInAdvancedStart() const override { return false; }
		CvPlot* getLookAtPlot() override { return nullptr; }
		CvPlot* getGotoPlot() override { return m_gotoPlot; }
		// Where the right button held would send the selection (the game core's plot help then gives the odds of an attack there).
		void setGotoPlot(CvPlot* plot) { m_gotoPlot = plot; }

		// The end of the player's turn as CvGame::updateTestEndTurn works it out with the
		// interface: when no unit waits for orders, it raises the "press Enter" message or, after
		// a few calls, ends the turn itself; the cycle counter holds back selecting the next unit
		// while one is still moving.
		bool isEndTurnMessage() override { return m_endTurnMessage; }
		void setEndTurnMessage(bool bNewValue) override { m_endTurnMessage = bNewValue; }
		bool isHasMovedUnit() override { return m_hasMovedUnit; }
		void setHasMovedUnit(bool bNewValue) override { m_hasMovedUnit = bNewValue; }
		bool isForcePopup() override { return m_forcePopup; }
		void setForcePopup(bool bNewValue) override { m_forcePopup = bNewValue; }
		int getEndTurnCounter() override { return m_endTurnCounter; }
		void setEndTurnCounter(int iNewValue) override { m_endTurnCounter = iNewValue; }
		void changeEndTurnCounter(int iChange) override { m_endTurnCounter += iChange; }
		int getCycleSelectionCounter() override { return m_cycleSelectionCounter; }
		void setCycleSelectionCounter(int iNewValue) override { m_cycleSelectionCounter = iNewValue; }
		void changeCycleSelectionCounter(int iChange) override { m_cycleSelectionCounter += iChange; }

		// Game font character of a FontSymbols value: the DLL places them after its own glyphs
		// when the font is assigned (CvGameTextMgr::assignFontIds tells them with setSymbolID,
		// see GameFont.h). Before that, consecutive codes from the font's first one, which keep
		// scripts that compute symbols from others (BUG's FontUtil: "ss casing" is HAPPY - 25)
		// working. Text only.
		static inline std::vector<int> s_symbols;
		static int symbolID(int iSymbol)
		{
			return iSymbol >= 0 && size_t(iSymbol) < s_symbols.size() && s_symbols[size_t(iSymbol)] ? s_symbols[size_t(iSymbol)] : 8483 + iSymbol;
		}
		int getSymbolID(int iSymbol) override { return symbolID(iSymbol); }

		// The minimap's bare map for the replay a save keeps (CvReplayInfo copies
		// MINIMAP_RENDER_SIZE² / 2 bytes of it): the terrain at MINIMAP_RENDER_SIZE pixels square,
		// north up, as DXT1 blocks. Owners are not in it: the replay screen colours them over it.
		// The window gives each terrain's colour (BGRA8, by terrain type); without one, grey land
		// and blue water.
		unsigned char* getMinimapBaseTexture() const override;

		// Sounds: the DLL's (by tag or script id, at a point of the map or not) and those of
		// its messages to the active player. Where a point is on the map, in plot units; false
		// for a plot the active team does not see (nothing to hear there).
		Sounds* sounds = nullptr;
		// A unit of the active player's sound when picked, or (order) sent somewhere.
		void unitSound(const CvUnit* pUnit, bool order);
		static bool soundPlace(const NiPoint3& point, std::optional<Sounds::Place>& place);
		void playGeneralSound(LPCTSTR pszSound, NiPoint3 vPos) override;
		void playGeneralSound(int iSoundId, int iSoundType, NiPoint3 vPos) override;
		void addMessage(PlayerTypes ePlayer, bool bForce, int iLength, CvWString szString, LPCTSTR pszSound, InterfaceMessageTypes eType,
			LPCSTR pszIcon, ColorTypes eFlashColor, int iFlashX, int iFlashY, bool bShowOffScreenArrows, bool bShowOnScreenArrows) override;
		void addCombatMessage(PlayerTypes ePlayer, CvWString szString) override;
		// The game menu's way out to the main menu (a save to load there, or none): for whoever
		// plays the game to do.
		std::function<void(const std::string& load)> onExitToMainMenu;
		void exitingToMainMenu(const char* szLoadFile) override
		{
			m_exiting = true;
			if (onExitToMainMenu)
				onExitToMainMenu(szLoadFile ? szLoadFile : "");
		}
		bool isExitingToMainMenu() override { return m_exiting; }
		// The turn log (Ctrl+Tab): the game core asks for it to come and go, to be shown, or
		// says it changed; the screens see to it (python/oc4turnlog.py). Any thread.
		void toggleTurnLog() override { turnLogRequest.store(TurnLog::Toggle); }
		void showTurnLog(ChatTargetTypes) override { turnLogRequest.store(TurnLog::Show); }
		void dirtyTurnLog(PlayerTypes ePlayer) override
		{
			if (ePlayer == GC.getGame().getActivePlayer() && turnLogRequest.load() == TurnLog::None)
				turnLogRequest.store(TurnLog::Refresh);
		}
		// The plots' yields (Ctrl+Y) and the bare map (Ctrl+B), as the game's controls switch them.
		std::atomic<bool> showYields = false, bareMap = false;
		bool isShowYields() override { return showYields; }
		void toggleYieldVisibleMode() override { showYields = !showYields; }
		bool isBareMapMode() override { return bareMap; }
		void toggleBareMapMode() override { bareMap = !bareMap; }
		enum class TurnLog
		{
			None, Toggle, Show, Refresh
		};
		std::atomic<TurnLog> turnLogRequest = TurnLog::None;
		void addQuestMessage(PlayerTypes ePlayer, CvWString szString, int iQuestId) override;
		void showMessage(CvTalkingHeadMessage& msg) override;
		void clearEventMessages() override;
		// The messages on the screen now (the active player's, as the original shows them at the
		// top for their length in seconds), oldest first, each with how much of it is left to
		// fade (1 shown, towards 0 going). Any thread.
		struct ShownMessage
		{
			std::wstring text;
			float alpha = 1;
		};
		std::vector<ShownMessage> shownMessages() const;
		std::vector<uint32_t> minimapTerrainColors;

	private:
		void show(const std::wstring& text, int seconds);
		bool m_exiting = false;
		struct Shown
		{
			std::wstring text;
			std::chrono::steady_clock::time_point until;
			int count = 1; // (the same message again while it is up: one line, "(x3)")
		};
		mutable std::mutex m_shownMutex;
		std::deque<Shown> m_shown;
		mutable std::vector<unsigned char> m_minimapBase;
		bool selected(const CvUnit* pUnit) const;

		CLinkList<IDInfo> m_selection;
		CvPlot* m_gotoPlot = nullptr;
		CvUnit* m_lastSelected = nullptr;
		bool m_endTurnMessage = false, m_hasMovedUnit = false, m_forcePopup = false;
		int m_endTurnCounter = 0, m_cycleSelectionCounter = 0;

		Popup* popupOf(CvPopup* pPopup) { return reinterpret_cast<Popup*>(pPopup); }
		void closePopup();
		struct Queued
		{
			CvPopupInfo* info;
			PlayerTypes player;
			std::unique_ptr<Popup> ready; // a script's, filled
			bool added = false; // by addPopup, not from a save's queue
		};
		std::deque<Queued> m_queued;
		std::unique_ptr<Popup> m_popup;
		uint64_t m_popupSerial = 0;
		bool m_quietFocus = false; // re-checking the popup up: no camera moves
		InterfaceModeTypes m_interfaceMode = INTERFACEMODE_SELECTION;
		std::bitset<kDirtyBits> m_dirty;
		InterfaceVisibility m_show = INTERFACE_SHOW;
		bool m_worldBuilder = false;
		InterfaceVisibility m_showBeforeWorldBuilder = INTERFACE_SHOW;
		CLinkList<IDInfo> m_cities;
		std::vector<IDInfo> m_citiesShown; // (asked for production when the city screen closes, if idle)
		bool m_cityScreen = false;
		void selectionChanged(); // (the dirty bits a new selection sets)
		void cityScreenChanged(bool wasUp); // (what the main interface hides in the city screen is redrawn)
	};

	// CvDLLEntityIFaceBase for units: the DLL asks a unit's entity whether the unit is selected
	// (CvDLLEntity::IsSelected), and that is how it moves on to the next unit when one is done.
	// A unit's entity is the unit itself; nothing is drawn through it.
	class Entities : public stubs::EntityIFace
	{
	public:
		explicit Entities(const Interface& selection) : m_selection(selection) {}
		void createUnitEntity(CvUnit* pUnit) override;
		void destroyEntity(CvEntity*& pEntity, bool) override;
		bool IsSelected(const CvEntity* pEntity) const override;
		// The mission a unit's entity is told it does (fortify, sleep, ..., idle): how the
		// original engine knows which animation the unit plays.
		void NotifyEntity(CvUnitEntity* pEntity, MissionTypes eMission) override;

		// Whether a unit rests (fortified, asleep, on sentry, healing): told so last and
		// still waiting; one never told (in a loaded game) when it is waiting.
		static bool resting(const CvUnit& unit);

		// A battle the game shows (MISSION_BEGIN_COMBAT, a CvBattleDefinition: the DLL sends
		// one where the active player sees it): kept while it plays, for the window.
		void AddMission(const CvMissionDefinition* pDefinition) override;
		struct Battle
		{
			uint64_t serial = 0;
			IDInfo attacker, defender;
			int attackerX = 0, attackerY = 0, defenderX = 0, defenderY = 0;
			struct Round
			{
				bool ranged = false;
				int killed[2]{}, alive[2]{};
			};
			std::vector<Round> rounds;
			int slices = 0; // the game updates it lasts (CvUnit::planBattle's turns)
			int collateral = 0; // the other units on the defender's plot the attacker's collateral damage hit
			std::chrono::steady_clock::time_point start;
		};
		static std::vector<Battle> battles(); // those still on (any thread)

		// A settler that founded a city where the active player sees (MISSION_FOUND; the game
		// removes it right after): its figures, kept a few seconds to play their founding.
		struct Founding
		{
			int x = 0, y = 0, owner = -1, direction = 4;
			std::vector<const CvArtInfoUnit*> figures;
			std::chrono::steady_clock::time_point at;
		};
		static std::vector<Founding> foundings();
		// A unit's action the game shows (a mission's animation, AddMission: pillaging,
		// bombarding, a ranged or an air strike, ...): the mission, the plot it aims at, the
		// effect its entity event sets off there (CIV4EffectInfos type), since when, how long.
		struct Action
		{
			IDInfo unit;
			int mission = -1, targetX = -1, targetY = -1;
			std::string effect;
			float seconds = 1;
			std::chrono::steady_clock::time_point start;
		};
		static std::vector<Action> actions();
		// While one is on the game is updated once a battle slice, as its animation goes:
		// true while it is not yet time for the next update (false: go, counted).
		static bool battleWait();

	private:
		const Interface& m_selection;
		static std::unordered_map<const void*, MissionTypes> s_missions;
	};
}
