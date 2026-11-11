// Checks the VC7.1-compatible stdext::hash_map (gamecore/compat/hash_map): iteration order
// against a key order read from a save of the original game, and against a direct model
// of its rules under random inserts and erases.

#include <hash_map>

#include <algorithm>
#include <cstdio>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

struct ThrowingHash : stdext::hash_compare<int>
{
    static inline int remaining = -1;
    using stdext::hash_compare<int>::operator();
    std::size_t operator()(const int& key) const
    {
        if (remaining == 0) throw std::runtime_error("hash");
        if (remaining > 0) --remaining;
        return stdext::hash_value(key);
    }
};
struct ThrowingValue
{
    static inline bool fail = false;
    ThrowingValue() = default;
    ThrowingValue(const ThrowingValue&)
    {
        if (fail) throw std::runtime_error("copy");
    }
};


// All rebinds share a state so bucket storage and list nodes are both observed.
struct AllocationState
{
    int allocations = 0;
    int outstanding = 0;
    int copySelections = 0;
    std::vector<std::size_t> counts;
};

template<class T>
struct TrackingAllocator
{
    using value_type = T;
    using propagate_on_container_copy_assignment = std::true_type;
    AllocationState* state;
    explicit TrackingAllocator(AllocationState* s = nullptr) : state(s) {}
    template<class U>
    TrackingAllocator(const TrackingAllocator<U>& other) : state(other.state) {}
    T* allocate(std::size_t n)
    {
        T* result = std::allocator<T>{}.allocate(n);
        if (state)
        {
            ++state->allocations;
            ++state->outstanding;
            state->counts.push_back(n);
        }
        return result;
    }
    void deallocate(T* p, std::size_t n)
    {
        if (state) --state->outstanding;
        std::allocator<T>{}.deallocate(p, n);
    }
    TrackingAllocator select_on_container_copy_construction() const
    {
        if (state) ++state->copySelections;
        return TrackingAllocator();
    }
    template<class U>
    bool operator==(const TrackingAllocator<U>& other) const { return state == other.state; }
};


struct KeyOrder
{
    bool descending = false;
    bool operator()(int a, int b) const { return descending ? a > b : a < b; }
};
struct DerivedHashCompare : stdext::hash_compare<int, KeyOrder>
{
    using stdext::hash_compare<int, KeyOrder>::hash_compare;
    bool descending() const { return comp.descending; }
};


struct StringLookupOperations
{
    bool descending = false;
    enum { bucket_size = 4, min_buckets = 8 };
    std::size_t operator()(std::string_view key) const { return stdext::hash_value(key); }
    bool operator()(std::string_view a, std::string_view b) const
    {
        return descending ? a > b : a < b;
    }
};
struct TransparentStringCompare : StringLookupOperations
{
    using is_transparent = void;
};
struct MissingLookupHash : TransparentStringCompare
{
    using TransparentStringCompare::operator();
    std::size_t operator()(const std::string& key) const { return stdext::hash_value(key); }
    std::size_t operator()(std::string_view) const = delete;
};
struct LookupProbe { int value; };
struct OneWayLookup : stdext::hash_compare<int>
{
    using is_transparent = void;
    using stdext::hash_compare<int>::operator();
    std::size_t operator()(const LookupProbe& q) const { return stdext::hash_value(q.value); }
    bool operator()(int key, const LookupProbe& q) const { return key < q.value; }
};

template<class M, class Q>
constexpr bool has_all_lookup_templates = requires(M& m, const M& cm, const Q& q)
{
    m.template find<Q>(q); cm.template find<Q>(q);
    cm.template count<Q>(q);
    m.template lower_bound<Q>(q); cm.template lower_bound<Q>(q);
    m.template upper_bound<Q>(q); cm.template upper_bound<Q>(q);
    m.template equal_range<Q>(q); cm.template equal_range<Q>(q);
};
template<class M, class Q>
constexpr bool has_any_lookup_template =
    requires(M& m, const Q& q) { m.template find<Q>(q); }
    || requires(const M& m, const Q& q) { m.template find<Q>(q); }
    || requires(const M& m, const Q& q) { m.template count<Q>(q); }
    || requires(M& m, const Q& q) { m.template lower_bound<Q>(q); }
    || requires(const M& m, const Q& q) { m.template lower_bound<Q>(q); }
    || requires(M& m, const Q& q) { m.template upper_bound<Q>(q); }
    || requires(const M& m, const Q& q) { m.template upper_bound<Q>(q); }
    || requires(M& m, const Q& q) { m.template equal_range<Q>(q); }
    || requires(const M& m, const Q& q) { m.template equal_range<Q>(q); };

namespace
{
	int failures = 0;

	void check(bool ok, const char* what)
	{
		if (!ok)
		{
			std::printf("FAILED: %s\n", what);
			++failures;
		}
	}

	template <class Map>
	std::vector<int> keys(const Map& m)
	{
		std::vector<int> out;
		for (const auto& kv : m)
			out.push_back(kv.first);
		return out;
	}

	// The rules spelled out without the container's machinery.
	struct Model
	{
		std::uint32_t mask = 1, maxidx = 1, table = 8;
		std::vector<int> keys;

		std::uint32_t bucket(int k) const
		{
			std::uint32_t n = (static_cast<std::uint32_t>(k) ^ 0xDEADBEEFu) & mask;
			return maxidx <= n ? n - ((mask >> 1) + 1) : n;
		}
		void insert(int k)
		{
			if (maxidx <= keys.size() / 4)
			{
				if (table <= maxidx)
				{
					mask = (table << 1) - 1;
					table = mask + 1;
				}
				else if (mask < maxidx)
					mask = (mask << 1) + 1;
				++maxidx;
			}
			if (std::find(keys.begin(), keys.end(), k) == keys.end())
				keys.push_back(k);
		}
		void erase(int k)
		{
			keys.erase(std::remove(keys.begin(), keys.end(), k), keys.end());
			if (keys.empty()) clear();
		}
		void clear() { *this = Model(); }
		std::vector<int> order() const
		{
			std::vector<int> out = keys;
			std::sort(out.begin(), out.end(), [&](int a, int b) { return bucket(a) != bucket(b) ? bucket(a) < bucket(b) : a < b; });
			return out;
		}
	};
}

int main()
{
	// Hash values use VC7.1's 32-bit arithmetic even on a 64-bit host.
	{
		static_assert(std::is_same_v<decltype(stdext::hash_value(-1)), std::uint32_t>);
		static_assert(std::is_same_v<decltype(stdext::hash_value(std::string{})), std::uint32_t>);
		check(stdext::hash_value(-1) == 0x21524110u, "negative key hashes as 32 bits");
		check(stdext::hash_value(std::uint64_t{0x100000001ULL}) == 0xdeadbeeeu,
		      "wide key is truncated to 32 bits");
		const std::u32string wrap(2, static_cast<char32_t>(0xffffffffu));
		check(stdext::hash_value(wrap) == 0xdeadbeedu, "string hash wraps at 32 bits");
		check(stdext::hash_compare<int>{}(-1) == 0x21524110u, "hash comparator preserves 32-bit hash");
	}

	// Score history of a player in AutoSave_BC-3520 (BtS 3.19 hotseat), keys = turns 0..16.
	{
		stdext::hash_map<int, int> m;
		for (int turn = 0; turn <= 16; ++turn)
			m[turn] = turn * 10;
		check(keys(m) == std::vector<int>{ 7, 15, 2, 6, 10, 14, 1, 5, 9, 13, 0, 4, 8, 12, 16, 3, 11 }, "order of an original save");
	}

	// Key erase resets a full matching range; iterator erase retains the buckets.
	for (int mode = 0; mode < 3; ++mode)
	{
		stdext::hash_map<int, int> m;
		for (int i = 0; i < 5; ++i) m[i] = i;
		for (int i = 0; i < 4; ++i)
			check(m.erase(i) == 1, "key erase returns one");
		check(m.erase(99) == 0 && m.count(4) == 1, "missing key preserves nonempty map");
		if (mode == 0)
			check(m.erase(4) == 1, "erase sole key returns one");
		else
			m.erase(m.begin());
		if (mode == 2)
			check(m.erase(99) == 0, "missing key in empty map returns zero");
		m[0] = 0;
		m[1] = 1;
		check(keys(m) == (mode == 1 ? std::vector<int>{1, 0} : std::vector<int>{0, 1}),
		      "key erase resets empty table but iterator erase retains it");
	}

	// Random inserts, erases and clears against the model; finds and stable references.
	std::mt19937 rng(12345);
	for (int round = 0; round < 200; ++round)
	{
		stdext::hash_map<int, int> m;
		Model model;
		std::map<int, const int*> addresses;
		const int range = 1 + int(rng() % 400);
		for (int op = 0; op < 2000; ++op)
		{
			const int k = int(rng() % unsigned(range)) - range / 4; // negative keys too
			const unsigned what = rng() % 100;
			if (what < 60)
			{
				m.insert(std::make_pair(k, k * 3));
				model.insert(k);
				addresses.emplace(k, &m.find(k)->second);
			}
			else if (what < 95)
			{
				m.erase(k);
				model.erase(k);
				addresses.erase(k);
			}
			else if (what == 99)
			{
				m.clear();
				model.clear();
				addresses.clear();
			}
		}
		check(keys(m) == model.order(), "order after random operations");
		bool found = true, stable = true;
		for (const auto& [k, p] : addresses)
		{
			const auto it = m.find(k);
			found = found && it != m.end() && it->second == k * 3;
			stable = stable && it != m.end() && &it->second == p;
		}
		check(found, "find after random operations");
		check(stable, "element addresses stay valid");
		check(m.find(range * 10) == m.end(), "find of a missing key");

		const stdext::hash_map<int, int> copy = m;
		check(keys(copy) == keys(m), "copy keeps the order");
		stdext::hash_map<int, int> moved = std::move(stdext::hash_map<int, int>(m));
		check(keys(moved) == keys(m), "move keeps the order");
		moved.insert(std::make_pair(range * 10, 1));
		model.insert(range * 10);
		check(keys(moved) == model.order(), "insert after move");
	}

	// String keys, looked up by std::string and by C string.
	{
		stdext::hash_map<std::string, int> m;
		for (int i = 0; i < 500; ++i)
			m["TYPE_" + std::to_string(i * 7)] = i;
		bool ok = true;
		for (int i = 0; i < 500; ++i)
		{
			const std::string key = "TYPE_" + std::to_string(i * 7);
			ok = ok && m.find(key) != m.end() && m.find(key.c_str()) == m.find(key) && m.find(key.c_str())->second == i;
		}
		const auto& cm = m;
		ok = ok && m.find("TYPE_1") == m.end() && cm.find("TYPE_7") != cm.end() && cm.find("") == cm.end();
		check(ok, "string keys");
	}

	// Erasing while iterating, as CvPlayer::doEvents does.
	{
		stdext::hash_map<int, int> m;
		for (int i = 0; i < 100; ++i)
			m[i] = i;
		for (auto it = m.begin(); it != m.end();)
			it = (it->first % 3 == 0) ? m.erase(it) : std::next(it);
		bool ok = m.size() == 66;
		for (int i = 0; i < 100; ++i)
			ok = ok && ((m.find(i) != m.end()) == (i % 3 != 0));
		check(ok, "erase while iterating");
	}

    {
        stdext::hash_map<int, int> source, target;
        for (int i = 0; i < 80; ++i) source[i] = i;
        target[-1] = -1;
        target = source;
        check(keys(target) == keys(source), "copy assignment preserves boundaries");
        target = target;
        target.erase(10);
        check(source.count(10) && !target.count(10), "copy assignment is independent");
        stdext::hash_map<int, int> empty;
        target.swap(empty);
        check(target.empty() && empty.size() == 79, "swap with empty map");
        target[999] = 1;
        target = std::move(empty);
        check(target.size() == 79 && empty.size() == 79, "rvalue assignment copies like VC7.1");
        empty[42] = 42;
        check(empty.count(42) == 1, "rvalue assignment source remains usable");
        target = source;
        check(target.find(79)->second == 79, "lookup after assignment");
    }
    {
        stdext::hash_map<int, ThrowingValue> source, target;
        source[1]; target[2];
        ThrowingValue::fail = true;
        bool threw = false;
        try { target = source; } catch (const std::runtime_error&) { threw = true; }
        ThrowingValue::fail = false;
        check(threw && target.empty(), "failed copy leaves empty map like VC7.1");
        target[3];
        check(target.count(3) == 1, "reuse after failed copy");
    }
    {
        stdext::hash_map<int, int, ThrowingHash> map;
        for (int i = 0; i < 4; ++i) map[i] = i;
        ThrowingHash::remaining = 1;
        bool threw = false;
        try { map.insert({4, 4}); } catch (const std::runtime_error&) { threw = true; }
        ThrowingHash::remaining = -1;
        check(threw && keys(map) == std::vector<int>{1, 2, 3, 0} && map.size() == 4,
              "failed split retains VC7.1 incremental rearrangement");
        // The original does not roll back the split; clear before reusing it.
        map.clear();
        map.insert({4, 4});
        check(map.count(4) == 1, "retry after failed split");
    }


    {
        AllocationState first, second;
        using Pair = std::pair<const int, int>;
        using Alloc = TrackingAllocator<Pair>;
        using Map = stdext::hash_map<int, int, stdext::hash_compare<int>, Alloc>;
        {
            Map source(stdext::hash_compare<int>{}, Alloc(&first));
            check(std::find(first.counts.begin(), first.counts.end(), 9) != first.counts.end(),
                  "initial boundary vector uses rebound value allocator");
            for (int i = 0; i < 4; ++i) source[i] = i;
            const auto* address = &*source.find(0);
            const int before = first.allocations;
            check(!source.insert({0, 99}).second, "duplicate insert still splits");
            check(first.allocations == before && &*source.find(0) == address,
                  "split within existing table allocates no nodes or boundaries");
            check(keys(source) == std::vector<int>{1, 3, 0, 2}, "split preserves bucket order");

            first.counts.clear();
            Map copy(source);
            check(copy.get_allocator().state == &first && first.copySelections == 0,
                  "copy constructor directly copies allocator without selection");
            check(keys(copy) == keys(source) && &*copy.find(0) != address,
                  "copy constructor allocates independent nodes");
            auto boundaries = std::find(first.counts.begin(), first.counts.end(), 9);
            check(boundaries != first.counts.end()
                  && std::count(std::next(boundaries), first.counts.end(), 1) >= 4,
                  "copy allocates boundaries before element nodes");
            Map fromRvalue(std::move(copy));
            check(keys(copy) == keys(source) && keys(fromRvalue) == keys(source),
                  "rvalue construction copies without stealing storage");

            Map target(stdext::hash_compare<int>{}, Alloc(&second));
            target[99] = 99;
            target = source;
            check(target.get_allocator().state == &second && keys(target) == keys(source),
                  "assignment retains target allocator even if propagation is enabled");
            target.erase(0);
            source.swap(target);
            check(source.get_allocator().state == &first && target.get_allocator().state == &second
                  && !source.count(0) && target.count(0), "unequal allocator swap copies contents");

            const auto* node = &*fromRvalue.begin();
            const int allocations = first.allocations;
            source.swap(fromRvalue);
            check(first.allocations == allocations && &*source.begin() == node,
                  "equal allocator swap transfers nodes without allocation");
            first.counts.clear();
            for (int i = 4; i < 80; ++i) source[i] = i;
            // The table grows to 17 entries, but vector's capacity growth policy
            // may allocate more. List nodes allocate one entry at a time.
            check(std::any_of(first.counts.begin(), first.counts.end(),
                              [](std::size_t n) { return n >= 17; }),
                  "boundary expansion uses rebound value allocator");
        }
        check(first.outstanding == 0 && second.outstanding == 0,
              "all allocator-owned storage is released");
    }


    // Legacy API: constructors, observers, bounds, reverse iteration, and overloads.
    {
        using Map = stdext::hash_map<int, int>;
        static_assert(std::is_same_v<Map::referent_type, int>);
        static_assert(std::is_same_v<Map::pointer, Map::value_type*>);
        static_assert(std::is_same_v<Map::const_pointer, const Map::value_type*>);
        static_assert(std::is_same_v<Map::value_compare::first_argument_type, Map::value_type>);
        static_assert(std::is_same_v<Map::value_compare::second_argument_type, Map::value_type>);
        static_assert(std::is_same_v<Map::value_compare::result_type, bool>);
        static_assert(Map::bucket_size == 4 && Map::min_buckets == 8);
        const std::pair<int, int> values[] = {{3, 30}, {1, 10}, {2, 20}, {1, 99}};
        Map m(std::begin(values), std::end(values));
        check(m.size() == 3 && m[1] == 10, "range construction preserves first equivalent value");
        Map::value_compare valueCompare(m.key_comp());
        check(valueCompare({1, 999}, {2, 0}) && !valueCompare({1, 0}, {1, 999}),
              "value_compare compares keys only");
        check(m.value_comp()({1, 9}, {2, 8}) && m.key_comp()(1, 2), "comparison observers");
        check(m.max_size() == std::list<Map::value_type>(m.get_allocator()).max_size(), "max_size delegates to list");
        for (auto it = m.rbegin(); it != m.rend(); ++it) ++it->second;
        const Map& cm = m;
        std::vector<int> reversed;
        for (Map::const_reverse_iterator it = cm.rbegin(); it != cm.rend(); ++it)
            reversed.push_back(it->first);
        check(reversed == std::vector<int>{3, 2, 1} && m[1] == 11, "mutable and const reverse iteration");
        Map empty;
        const Map& ce = empty;
        check(empty.rbegin() == empty.rend() && ce.rbegin() == ce.rend(), "empty reverse range");

        using Compare = stdext::hash_compare<int, KeyOrder>;
        Compare descending = KeyOrder{true}; // VC7.1 permits implicit predicate conversion.
        check(DerivedHashCompare(KeyOrder{true}).descending(), "hash_compare exposes protected predicate");
        using DescMap = stdext::hash_map<int, int, Compare>;
        DescMap descendingMap(std::begin(values), std::end(values), descending);
        check(keys(descendingMap) == std::vector<int>{3, 2, 1}
              && descendingMap.key_comp()(3, 1)
              && descendingMap.value_comp()({3, 0}, {1, 0}), "range constructor preserves stateful comparator");
        for (int k = 0; k <= 4; ++k)
        {
            const auto& cd = descendingMap;
            auto found = descendingMap.find(k);
            auto next = found == descendingMap.end() ? found : std::next(found);
            check(descendingMap.lower_bound(k) == found && descendingMap.upper_bound(k) == next
                  && cd.lower_bound(k) == found && cd.upper_bound(k) == next,
                  "bounds respect descending comparator");
        }
        AllocationState state;
        {
            using Alloc = TrackingAllocator<Map::value_type>;
            using AllocMap = stdext::hash_map<int, int, Compare, Alloc>;
            AllocMap allocated(std::begin(values), std::end(values), descending, Alloc(&state));
            check(allocated.get_allocator().state == &state && keys(allocated) == keys(descendingMap),
                  "range constructor preserves explicit allocator and comparator");
        }
        check(state.outstanding == 0, "range constructor storage released");

        // Exercise both overloads across bucket boundaries, including absent keys.
        for (int i = 0; i < 40; ++i) m[i * 2] = i;
        for (int k = -1; k <= 81; ++k)
        {
            auto found = m.find(k);
            auto next = found == m.end() ? found : std::next(found);
            auto range = m.equal_range(k);
            auto constRange = cm.equal_range(k);
            check(m.lower_bound(k) == found && cm.lower_bound(k) == found
                  && m.upper_bound(k) == next && cm.upper_bound(k) == next
                  && range.first == found && range.second == next
                  && constRange.first == found && constRange.second == next,
                  "legacy bounds return matching range or end/end");
        }
        check(empty.lower_bound(1) == empty.end() && empty.upper_bound(1) == empty.end()
              && ce.equal_range(1).first == ce.end() && ce.equal_range(1).second == ce.end(),
              "empty bounds");
        const int remove[] = {0, 2, 500, 2};
        m.erase(std::begin(remove), std::end(remove));
        check(!m.count(0) && !m.count(2) && m.count(4), "erase array of keys handles duplicates and misses");
        Map hint;
        for (int i = 0; i < 4; ++i) hint[i] = i;
        auto duplicate = hint.insert(hint.begin(), Map::value_type(0, 99));
        check(duplicate->second == 0 && keys(hint) == std::vector<int>{1, 3, 0, 2},
              "hint ignored and duplicate hinted insertion still splits");
        auto inserted = hint.insert(hint.begin(), Map::value_type(5, 50));
        check(inserted == hint.find(5) && inserted->second == 50, "hinted insertion returns element");

        // Identical contents with different split histories compare by sequence.
        Map a, b;
        for (int i = 0; i < 4; ++i) { a[i] = i; b[i] = i; }
        check(a == b && !(a != b) && !(a < b) && !(a > b) && a <= b && a >= b,
              "all relational operators on equal sequences");
        b.insert({0, 0});
        check(a != b && !(a == b) && a < b && b > a && a <= b && b >= a
              && !(b <= a) && !(a >= b), "relational operators use iteration order");
        const auto* node = &*a.begin();
        const auto original = keys(a);
        using std::swap;
        swap(a, b);
        check(keys(b) == original && &*b.begin() == node, "ADL swap transfers nodes");
        stdext::swap(a, b);
        check(keys(a) == original && &*a.begin() == node, "qualified nonmember swap");
    }


    // Transparent lookup must neither construct keys nor change legacy overloads.
    {
        using Map = stdext::hash_map<std::string, int, TransparentStringCompare>;
        using OpaqueMap = stdext::hash_map<std::string, int, StringLookupOperations>;
        static_assert(has_all_lookup_templates<Map, std::string_view>);
        static_assert(has_all_lookup_templates<Map, const char*>);
        static_assert(!has_any_lookup_template<OpaqueMap, std::string_view>);
        static_assert(!has_any_lookup_template<stdext::hash_map<std::string, int>, std::string_view>);
        static_assert(!has_any_lookup_template<Map, LookupProbe>);
        static_assert(!has_any_lookup_template<stdext::hash_map<std::string, int, MissingLookupHash>, std::string_view>);
        static_assert(!has_any_lookup_template<stdext::hash_map<int, int, OneWayLookup>, LookupProbe>);
        static_assert(std::is_same_v<decltype(std::declval<Map&>().find(std::string_view{})), Map::iterator>);
        static_assert(std::is_same_v<decltype(std::declval<const Map&>().find(std::string_view{})), Map::const_iterator>);
        // The pre-existing typed overloads can still be selected by exact signature.
        Map::iterator (Map::*oldFind)(const std::string&) = &Map::find;
        Map::const_iterator (Map::*oldConstFind)(const std::string&) const = &Map::find;
        for (bool descending : {false, true})
        {
            TransparentStringCompare traits;
            traits.descending = descending;
            Map m(traits);
            const Map& cm = m;
            const auto verify = [&](std::string_view query)
            {
                const std::string owning(query); // only the reference lookup constructs a key
                auto found = (m.*oldFind)(owning);
                auto after = found == m.end() ? found : std::next(found);
                auto range = m.equal_range(query);
                auto crange = cm.equal_range(query);
                check(m.find(query) == found && cm.find(query) == (cm.*oldConstFind)(owning)
                      && m.count(query) == (found == m.end() ? 0u : 1u)
                      && cm.count(query) == m.count(owning)
                      && m.lower_bound(query) == found && cm.lower_bound(query) == found
                      && m.upper_bound(query) == after && cm.upper_bound(query) == after
                      && range.first == found && range.second == after
                      && crange.first == found && crange.second == after,
                      "all heterogeneous overloads match typed lookup");
            };
            verify(""); verify("absent");
            m[""] = -1;
            m[std::string("a\0b", 3)] = -2;
            for (int i = 0; i < 80; ++i) m["key_" + std::to_string(i * 2)] = i;
            for (int i = 0; i < 161; ++i) verify("key_" + std::to_string(i));
            verify(""); verify(std::string_view("a\0b", 3));
            // The view must honor its length, even without a terminator at that length.
            const char buffer[] = {'k', 'e', 'y', '_', '2', 'x'};
            verify(std::string_view(buffer, 5));
            check(m.find("key_2") == m.find(std::string_view(buffer, 5)), "literal lookup hashes string contents");
        }
        // A counted key allocator detects accidental construction of a long owned key.
        AllocationState keyAllocations;
        using String = std::basic_string<char, std::char_traits<char>, TrackingAllocator<char>>;
        using CountedMap = stdext::hash_map<String, int, TransparentStringCompare>;
        {
            CountedMap m;
            String key(200, 'x', TrackingAllocator<char>(&keyAllocations));
            m[key] = 7;
            const CountedMap& cm = m;
            const std::string_view view(key.data(), key.size());
            const int before = keyAllocations.allocations;
            auto found = m.find(view);
            check(found != m.end() && found->second == 7 && cm.find(view) == found
                  && m.count(view) == 1 && m.lower_bound(view) == found && cm.lower_bound(view) == found
                  && m.upper_bound(view) == m.end() && cm.upper_bound(view) == cm.end()
                  && m.equal_range(view).first == found && cm.equal_range(view).first == found,
                  "heterogeneous lookup supports allocator-aware string keys");
            check(keyAllocations.allocations == before, "heterogeneous lookup allocates no owned key");
        }
        check(keyAllocations.outstanding == 0, "counted string storage released");
    }

	std::printf(failures ? "%d check(s) failed\n" : "all hash_map checks passed\n", failures);
	return failures ? 1 : 0;
}
