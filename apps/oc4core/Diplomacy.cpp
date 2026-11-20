#include "Diplomacy.h"

#include "CvDeal.h"
#include "CvDiploParameters.h"
#include "CvGameAI.h"
#include "CvGameCoreUtils.h"
#include "CvGlobals.h"
#include "CvMessageControl.h"
#include "CvPlayerAI.h"
#include "CvTeamAI.h"

#include <windows.h>

namespace oc4::host
{
	namespace
	{
		CvPlayerAI& playerOf(PlayerTypes p) { return CvPlayerAI::getPlayerNonInl(p); }

		void copy(const CLinkList<TradeData>& from, CLinkList<TradeData>& to)
		{
			to.clear();
			for (CLLNode<TradeData>* n = from.head(); n; n = from.next(n))
				to.insertAtEnd(n->m_data);
		}

		// The same item of a list (gold whatever its amount).
		bool same(const TradeData& item, int type, int data)
		{
			return item.m_eItemType == type && (CvDeal::isGold(item.m_eItemType) || !CvDeal::hasData(item.m_eItemType) || item.m_iData == data);
		}

		CLLNode<TradeData>* find(const CLinkList<TradeData>& list, int type, int data)
		{
			for (CLLNode<TradeData>* n = list.head(); n; n = list.next(n))
				if (same(n->m_data, type, data))
					return n;
			return nullptr;
		}

		void remove(CLinkList<TradeData>& list, int type, int data)
		{
			if (CLLNode<TradeData>* n = find(list, type, data))
				list.deleteNode(n);
		}

		std::wstring widen(const char* s)
		{
			if (!s || !*s)
				return {};
			const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
			std::wstring w(size_t(std::max(n, 1)) - 1, L'\0');
			MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
			return w;
		}
	}

	Diplomacy::~Diplomacy()
	{
		// (a talk still up when the engine stops goes with the process: the game core may be gone)
	}

	void Diplomacy::begin(CvDiploParameters* params, PlayerTypes p)
	{
		// (shown at the next frame when nothing else is up; a talk waits in the player's queue, as
		// the original keeps it, saved with the game)
		if (params && p != NO_PLAYER)
			playerOf(p).addDiplomacy(params);
	}

	void Diplomacy::ask(CvDiploParameters* params, PlayerTypes p)
	{
		if (params && p != NO_PLAYER)
			m_asked.push_back({ params, p });
	}

	bool Diplomacy::waiting() const
	{
		const PlayerTypes p = GC.getGame().getActivePlayer();
		return p != NO_PLAYER && !playerOf(p).getDiplomacy().empty();
	}

	bool Diplomacy::next(bool popupUp)
	{
		if (!active() && !m_asked.empty())
		{
			const auto [params, p] = m_asked.front();
			m_asked.erase(m_asked.begin());
			start(params, p);
			return true;
		}
		if (active() || popupUp)
			return false;
		const PlayerTypes p = GC.getGame().getActivePlayer();
		if (p == NO_PLAYER || playerOf(p).getDiplomacy().empty())
			return false;
		if (CvDiploParameters* params = playerOf(p).popFrontDiplomacy())
		{
			start(params, p);
			return true;
		}
		return false;
	}

	int g_lastDiplomacyPlayer = -1;
	thread_local int g_diplomacyPlayerOverride = -1;

	void Diplomacy::start(CvDiploParameters* params, PlayerTypes p)
	{
		m_params = params;
		m_player = p;
		m_who = params->getWhoTalkingTo();
		g_lastDiplomacyPlayer = m_who;
		// (the offers as the DLL gives them: "ours" are the player's, "theirs" the leader's)
		copy(params->getOurOfferList(), m_ourOffer);
		copy(params->getTheirOfferList(), m_theirOffer);
		m_aiOffer = !ourOfferEmpty() || !theirOfferEmpty();
		m_comment = params->getDiploComment();
		// The player's contact: the leader's greeting, or its refusal to talk.
		if (m_comment == NO_DIPLOCOMMENT)
			m_comment = playerOf(m_who).AI_isWillingToTalk(p) ? playerOf(m_who).AI_getGreeting(p) : GC.getInfoTypeForString("AI_DIPLOCOMMENT_REFUSE_TO_TALK");
		// (trading: the player's Ctrl+click, and an AI's proposal, its items on the table)
		m_trading = m_comment == GC.getInfoTypeForString("AI_DIPLOCOMMENT_TRADING") || m_aiOffer;
		m_showAll = true;
		m_current = false;
		m_aiText.clear();
		m_userComments.clear();
		rebuild();
		++m_version;
	}

	void Diplomacy::end()
	{
		delete m_params;
		m_params = nullptr;
		m_who = NO_PLAYER;
		m_ourOffer.clear();
		m_theirOffer.clear();
		m_ourInventory.clear();
		m_theirInventory.clear();
		m_userComments.clear();
		m_aiText.clear();
		m_trading = m_current = m_aiOffer = false;
		m_showAll = true;
		++m_version;
	}

	std::vector<TextManager::Arg> Diplomacy::commentArgs() const
	{
		std::vector<TextManager::Arg> args;
		if (!m_params)
			return args;
		for (const FVariable& v : m_params->getDiploCommentArgs())
			switch (v.m_eType)
			{
			case FVARTYPE_WSTRING:
				args.emplace_back(std::wstring(v.m_wszValue ? v.m_wszValue : L""));
				break;
			case FVARTYPE_STRING:
				args.emplace_back(widen(v.m_szValue));
				break;
			case FVARTYPE_FLOAT:
				args.emplace_back(double(v.m_fValue));
				break;
			case FVARTYPE_DOUBLE:
				args.emplace_back(v.m_dValue);
				break;
			case FVARTYPE_UINT:
				args.emplace_back((long long)v.m_uiValue);
				break;
			default:
				args.emplace_back((long long)v.m_iValue);
				break;
			}
		return args;
	}

	int Diplomacy::data() const { return m_params ? m_params->getData() : -1; }

	void Diplomacy::setAIComment(int comment)
	{
		m_comment = comment;
		if (active())
			GC.getGame().handleDiplomacySetAIComment(static_cast<DiploCommentTypes>(comment));
		++m_version;
	}

	void Diplomacy::startTrade(bool renegotiate)
	{
		m_trading = true;
		if (renegotiate && active())
		{
			// The deals the two have: their items in the offers, as they stand.
			m_ourOffer.clear();
			m_theirOffer.clear();
			CvGame& game = GC.getGame();
			int i = 0;
			for (CvDeal* d = game.firstDeal(&i); d; d = game.nextDeal(&i))
			{
				const bool first = d->getFirstPlayer() == m_player && d->getSecondPlayer() == m_who, second = d->getFirstPlayer() == m_who && d->getSecondPlayer() == m_player;
				if (!first && !second)
					continue;
				for (CLLNode<TradeData>* n = d->headFirstTradesNode(); n; n = d->nextFirstTradesNode(n))
					(first ? m_ourOffer : m_theirOffer).insertAtEnd(n->m_data);
				for (CLLNode<TradeData>* n = d->headSecondTradesNode(); n; n = d->nextSecondTradesNode(n))
					(first ? m_theirOffer : m_ourOffer).insertAtEnd(n->m_data);
			}
			m_current = true;
			m_aiOffer = false;
		}
		rebuild();
		++m_version;
	}

	void Diplomacy::endTrade()
	{
		// The talk leaves the table, and the original clears it: an offer left on it ("Never
		// mind", an AI's proposal renegotiated and then dropped) is gone at the next proposal.
		m_trading = false;
		if (m_current || !ourOfferEmpty() || !theirOfferEmpty())
		{
			m_ourOffer.clear();
			m_theirOffer.clear();
			m_current = m_aiOffer = false;
			rebuild();
		}
		++m_version;
	}

	void Diplomacy::showAllTrade(bool all)
	{
		m_trading = true;
		m_showAll = all;
		if (all && m_current)
		{
			m_ourOffer.clear();
			m_theirOffer.clear();
			m_current = false;
		}
		rebuild();
		++m_version;
	}

	bool Diplomacy::atWar() const
	{
		return active() && ::atWar(playerOf(m_player).getTeam(), playerOf(m_who).getTeam());
	}

	bool Diplomacy::offerDeal()
	{
		if (!active())
			return false;
		rebuild();
		// (the leader's view: "theirs" is what the player gives)
		const bool accepted = playerOf(m_who).AI_considerOffer(m_player, &m_ourOffer, &m_theirOffer, 1);
		if (accepted)
			implementDeal();
		++m_version;
		return accepted;
	}

	bool Diplomacy::counterPropose()
	{
		if (!active())
			return false;
		rebuild();
		CLinkList<TradeData> ourCounter, theirCounter;
		// (the leader's view: "theirs" is the player's; what it adds to either side)
		if (!playerOf(m_who).AI_counterPropose(m_player, &m_ourOffer, &m_theirOffer, &m_ourInventory, &m_theirInventory, &ourCounter, &theirCounter))
			return false;
		for (CLLNode<TradeData>* n = ourCounter.head(); n; n = ourCounter.next(n))
			m_ourOffer.insertAtEnd(n->m_data);
		for (CLLNode<TradeData>* n = theirCounter.head(); n; n = theirCounter.next(n))
			m_theirOffer.insertAtEnd(n->m_data);
		m_aiOffer = true;
		rebuild();
		++m_version;
		return true;
	}

	void Diplomacy::implementDeal()
	{
		if (!active() || (ourOfferEmpty() && theirOfferEmpty()))
			return;
		// (as the original: a message, in a multiplayer game every game's in the same frame)
		gDLL->sendImplementDealMessage(m_who, &m_ourOffer, &m_theirOffer);
		m_ourOffer.clear();
		m_theirOffer.clear();
		m_aiOffer = m_current = false;
		rebuild();
		++m_version;
	}

	void Diplomacy::declareWar()
	{
		// (the game's message, as the original sends it: the war ends the talk, endDiplomacy)
		if (active())
			CvMessageControl::getInstance().sendChangeWar(playerOf(m_who).getTeam(), true);
	}

	void Diplomacy::diploEvent(int event, int data1, int data2)
	{
		if (active())
			gDLL->sendDiploEvent(m_who, static_cast<DiploEventTypes>(event), data1, data2);
		++m_version;
	}

	bool Diplomacy::hasAnnualDeal() const
	{
		if (!active())
			return false;
		CvGame& game = GC.getGame();
		int i = 0;
		for (CvDeal* d = game.firstDeal(&i); d; d = game.nextDeal(&i))
		{
			if (!((d->getFirstPlayer() == m_player && d->getSecondPlayer() == m_who) || (d->getFirstPlayer() == m_who && d->getSecondPlayer() == m_player)))
				continue;
			for (CLLNode<TradeData>* n = d->headFirstTradesNode(); n; n = d->nextFirstTradesNode(n))
				if (CvDeal::isAnnual(n->m_data.m_eItemType))
					return true;
			for (CLLNode<TradeData>* n = d->headSecondTradesNode(); n; n = d->nextSecondTradesNode(n))
				if (CvDeal::isAnnual(n->m_data.m_eItemType))
					return true;
		}
		return false;
	}

	bool Diplomacy::theirVassalTribute() const { return CvDeal::isVassalTributeDeal(&m_theirOffer); }

	void Diplomacy::rebuild()
	{
		m_ourInventory.clear();
		m_theirInventory.clear();
		if (!active())
			return;
		playerOf(m_player).buildTradeTable(m_who, m_ourInventory);
		playerOf(m_who).buildTradeTable(m_player, m_theirInventory);
		playerOf(m_player).updateTradeList(m_who, m_ourInventory, m_ourOffer, m_theirOffer);
		playerOf(m_who).updateTradeList(m_player, m_theirInventory, m_theirOffer, m_ourOffer);
		// (what is on offer is marked so in its list)
		for (auto [inventory, offer] : { std::pair{ &m_ourInventory, &m_ourOffer }, std::pair{ &m_theirInventory, &m_theirOffer } })
			for (CLLNode<TradeData>* n = inventory->head(); n; n = inventory->next(n))
				n->m_data.m_bOffering = find(*offer, n->m_data.m_eItemType, n->m_data.m_iData) != nullptr;
	}

	Diplomacy::Item Diplomacy::describe(PlayerTypes owner, const TradeData& data, bool offered) const
	{
		Item item;
		item.type = data.m_eItemType;
		item.data = data.m_iData;
		const PlayerTypes other = owner == m_player ? m_who : m_player;
		CvWString text;
		CvString icon;
		// (an item on offer is described by who gets it, "offered" to it by its owner; one of a
		// list by its owner)
		if (offered)
			playerOf(other).getItemTradeString(owner, true, m_current, data, text, icon);
		else
			playerOf(owner).getItemTradeString(other, false, m_current, data, text, icon);
		item.text = text.GetCString();
		item.icon = icon.GetCString();
		// (what its owner will not give: red in its list, the reason in its help)
		item.denied = !offered && playerOf(owner).getTradeDenial(other, data) != NO_DENIAL;
		return item;
	}

	std::vector<Diplomacy::Item> Diplomacy::inventory(bool theirs) const
	{
		std::vector<Item> out;
		if (!active())
			return out;
		const PlayerTypes owner = theirs ? m_who : m_player, other = theirs ? m_player : m_who;
		const CLinkList<TradeData>& list = theirs ? m_theirInventory : m_ourInventory;
		int heading = -1;
		for (CLLNode<TradeData>* n = list.head(); n; n = list.next(n))
		{
			if (n->m_data.m_bHidden || n->m_data.m_bOffering)
				continue;
			if (n->m_data.m_eItemType != heading)
			{
				heading = n->m_data.m_eItemType;
				CvWString text;
				CvString icon;
				if (playerOf(owner).getHeadingTradeString(other, n->m_data.m_eItemType, text, icon))
				{
					Item h;
					h.type = heading;
					h.heading = true;
					h.text = text.GetCString();
					h.icon = icon.GetCString();
					out.push_back(std::move(h));
				}
			}
			out.push_back(describe(owner, n->m_data, false));
		}
		return out;
	}

	std::vector<Diplomacy::Item> Diplomacy::offer(bool theirs) const
	{
		std::vector<Item> out;
		if (!active())
			return out;
		const CLinkList<TradeData>& list = theirs ? m_theirOffer : m_ourOffer;
		for (CLLNode<TradeData>* n = list.head(); n; n = list.next(n))
			out.push_back(describe(theirs ? m_who : m_player, n->m_data, true));
		return out;
	}

	void Diplomacy::toggle(bool theirs, bool fromOffer, int type, int data, int amount)
	{
		if (!active() || m_current)
			return;
		CLinkList<TradeData>& offer = theirs ? m_theirOffer : m_ourOffer;
		CLinkList<TradeData>& other = theirs ? m_ourOffer : m_theirOffer;
		// (a two-sided item, peace or open borders or a pact, is on both sides or on none)
		const bool dual = CvDeal::isDual(static_cast<TradeableItems>(type));
		if (fromOffer)
		{
			remove(offer, type, data);
			if (dual)
				remove(other, type, data);
		}
		else if (!find(offer, type, data))
		{
			TradeData item;
			setTradeItem(&item, static_cast<TradeableItems>(type), CvDeal::isGold(static_cast<TradeableItems>(type)) ? amount : data);
			offer.insertAtEnd(item);
			if (dual && !find(other, type, data))
				other.insertAtEnd(item);
		}
		m_aiOffer = false;
		rebuild();
		++m_version;
	}

	bool Diplomacy::cancelDeal(int type, int data, std::wstring& reason)
	{
		if (!active() || !m_current)
			return false;
		CvGame& game = GC.getGame();
		int i = 0;
		for (CvDeal* d = game.firstDeal(&i); d; d = game.nextDeal(&i))
		{
			if (!((d->getFirstPlayer() == m_player && d->getSecondPlayer() == m_who) || (d->getFirstPlayer() == m_who && d->getSecondPlayer() == m_player)))
				continue;
			bool has = false;
			for (CLLNode<TradeData>* n = d->headFirstTradesNode(); n && !has; n = d->nextFirstTradesNode(n))
				has = n->m_data.m_eItemType == type && (CvDeal::isGold(TradeableItems(type)) || n->m_data.m_iData == data);
			for (CLLNode<TradeData>* n = d->headSecondTradesNode(); n && !has; n = d->nextSecondTradesNode(n))
				has = n->m_data.m_eItemType == type && (CvDeal::isGold(TradeableItems(type)) || n->m_data.m_iData == data);
			if (!has)
				continue;
			CvWString why;
			if (!d->isCancelable(m_player, &why))
			{
				reason = gDLL->getText("TXT_KEY_POPUP_CANNOT_CANCEL_DEAL").GetCString();
				if (!why.empty())
					reason += L"\n" + std::wstring(why.GetCString());
				return false;
			}
			gDLL->sendKillDeal(d->getID(), true);
			startTrade(true); // (the deals left)
			return true;
		}
		return false;
	}

	int Diplomacy::maxGold(bool theirs, bool perTurn) const
	{
		if (!active())
			return 0;
		const PlayerTypes owner = theirs ? m_who : m_player, other = theirs ? m_player : m_who;
		return perTurn ? playerOf(owner).AI_maxGoldPerTurnTrade(other) : playerOf(owner).AI_maxGoldTrade(other);
	}
}
