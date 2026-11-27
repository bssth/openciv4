#pragma once

// The diplomacy screen's engine side, as the original EXE has it. The DLL opens it: for an AI
// that contacts the player (beginDiplomacy) and for the player who contacts an AI (the
// interface's diplomacy queue); the leader greets with the AI's greeting when the DLL gives
// no comment, or refuses to talk. A talk that comes while another is up, or a popup, waits in
// the player's own queue (CvPlayer::addDiplomacy, kept in saves as in the original).
//
// What the leader says and what the player can answer are the game's script's
// (CvDiplomacyInterface and CvDiplomacy.py, through the engine's CyDiplomacy: oc4screens.py
// draws the screen); this keeps them and the trade table: both players' items
// (CvPlayer::buildTradeTable and updateTradeList, the items' texts and headings from the DLL),
// the two offers ("ours" the player's, "theirs" the leader's) and whose proposal they are. The
// AI judges an offer and makes one through its virtual functions (AI_considerOffer,
// AI_counterPropose) as the original EXE calls them; a deal is the game's (implementDeal).

#include "Text.h"

#include "CvGameCoreDLL.h"
#include "LinkedList.h"

#include <string>
#include <vector>

class CvDiploParameters;

namespace oc4::host
{
	// The last leader talked to (a leader's text read outside its talk fills [OUR_...] with it),
	// and the one the game core is told is talking while such a tag is filled (-1 none).
	extern int g_lastDiplomacyPlayer;
	extern thread_local int g_diplomacyPlayerOverride;

	class Diplomacy
	{
	public:
		struct Comment
		{
			int comment = -1, data1 = -1, data2 = -1;
			std::wstring text;
		};
		// A line of a trade list: a heading (Technologies, Resources, ...) or an item.
		struct Item
		{
			int type = -1, data = 0;
			std::wstring text;
			std::string icon;
			bool heading = false, denied = false;
		};

		~Diplomacy();

		// From the DLL: shown when nothing else is up (it waits in the player's queue).
		void begin(CvDiploParameters* params, PlayerTypes player);
		// A talk the player asked for (CvPlayer::contact): up at the next frame, whatever popup
		// is up or waiting.
		void ask(CvDiploParameters* params, PlayerTypes player);
		// The next talk waiting for the active player, when nothing is up; true if one began
		// (its comment and arguments are then the script's to answer: comment(), commentArgs()).
		bool next(bool popupUp);
		bool active() const { return m_params != nullptr; }
	private:
		std::vector<std::pair<CvDiploParameters*, PlayerTypes>> m_asked;
	public:
		bool waiting() const;
		PlayerTypes who() const { return m_who; }
		PlayerTypes player() const { return m_player; }
		void end();
		// Something on the screen changed.
		unsigned version() const { return m_version; }

		// What the talk began with.
		int comment() const { return m_comment; }
		std::vector<TextManager::Arg> commentArgs() const;

		// CyDiplomacy
		int data() const;
		void setAIString(std::wstring text) { m_aiText = std::move(text), ++m_version; }
		void setAIComment(int comment);
		void clearUserComments() { m_userComments.clear(), ++m_version; }
		void addUserComment(Comment c) { m_userComments.push_back(std::move(c)), ++m_version; }
		void startTrade(bool renegotiate);
		void endTrade();
		void showAllTrade(bool all);
		bool ourOfferEmpty() const { return m_ourOffer.getLength() == 0; }
		bool theirOfferEmpty() const { return m_theirOffer.getLength() == 0; }
		bool atWar() const;
		bool isAIOffer() const { return m_aiOffer; }
		void setAIOffer(bool ai) { m_aiOffer = ai, ++m_version; }
		bool offerDeal();
		bool counterPropose();
		void implementDeal();
		void declareWar();
		void diploEvent(int event, int data1, int data2);
		bool hasAnnualDeal() const;
		bool theirVassalTribute() const;

		// The screen
		const std::wstring& aiText() const { return m_aiText; }
		const std::vector<Comment>& userComments() const { return m_userComments; }
		bool trading() const { return m_trading; }
		bool showingAll() const { return m_showAll; }
		bool showingCurrent() const { return m_current; }
		// Whether the player changed the table since the trade began (CLEAR TABLE shows then).
		bool edited() const { return m_edited; }
		// A player's items (theirs: the leader's) as the trade list shows them: its headings and
		// what is not offered or hidden; an offer's items.
		std::vector<Item> inventory(bool theirs) const;
		std::vector<Item> offer(bool theirs) const;
		// An offer's items as they are in it: (item type, data; gold its amount).
		std::vector<std::pair<int, int>> offerData(bool theirs) const;
		// A click on an item: from a list into its offer (gold: `amount` of it), or back.
		void toggle(bool theirs, bool fromOffer, int type, int data, int amount);
		// Viewing the current deals: the deal of an offer's row (theirs: the leader's offer)
		// cancelled, if it can be (else why not, in reason). The talk then starts again at the
		// leader's greeting, as the original's does (seen in Wine).
		bool cancelDeal(bool theirs, int row, std::wstring& reason);
		// Whether an offer's row is one of the current deals' items, which a click cancels.
		bool isDealRow(bool theirs, int row) const;
		// The most gold (per turn) a side can give.
		int maxGold(bool theirs, bool perTurn) const;

	private:
		void start(CvDiploParameters* params, PlayerTypes player);
		// Both inventories again, the offered items marked (as AI_counterPropose reads them).
		void rebuild();
		Item describe(PlayerTypes owner, const TradeData& item, bool offered) const;

		CvDiploParameters* m_params = nullptr;
		PlayerTypes m_player = NO_PLAYER, m_who = NO_PLAYER;
		CLinkList<TradeData> m_ourOffer, m_theirOffer, m_ourInventory, m_theirInventory;
		// (the current deals shown: each offer row's deal)
		std::vector<int> m_ourDeals, m_theirDeals;
		bool m_aiOffer = false, m_trading = false, m_showAll = true, m_current = false, m_edited = false;
		int m_comment = -1;
		std::wstring m_aiText;
		std::vector<Comment> m_userComments;
		unsigned m_version = 1;
	};
}
