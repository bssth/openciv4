#include "Xml.h"
#include "XmlDecode.h"

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <format>

namespace oc4::host
{
	XmlNode* XmlNode::firstChildElement(std::string_view tag) const
	{
		for (const auto& c : children)
			if (c->type == Type::Element && c->name == tag)
				return c.get();
		return nullptr;
	}

	const std::wstring* XmlNode::attribute(std::string_view n) const
	{
		for (const auto& [k, v] : attributes)
			if (k == n)
				return &v;
		return nullptr;
	}

	namespace
	{
		// ---------------------------------------------------------------- parser

		class Parser
		{
		public:
			Parser(std::span<const std::byte> data) : m_data(reinterpret_cast<const unsigned char*>(data.data())), m_size(data.size()) {}

			std::unique_ptr<XmlNode> run(std::string& error)
			{
				auto doc = std::make_unique<XmlNode>();
				doc->type = XmlNode::Type::Document;
				try
				{
					if (m_size >= 3 && m_data[0] == 0xEF && m_data[1] == 0xBB && m_data[2] == 0xBF)
					{
						m_pos = 3;
						m_utf8 = true;
					}
					parseContent(*doc, true);
				}
				catch (const std::exception& e)
				{
					error = std::format("{} (byte {})", e.what(), m_pos);
					return nullptr;
				}
				return doc;
			}

		private:
			[[noreturn]] void fail(const char* what) { throw std::runtime_error(what); }

			bool atEnd() const { return m_pos >= m_size; }
			unsigned char peek(size_t off = 0) const { return m_pos + off < m_size ? m_data[m_pos + off] : 0; }
			bool startsWith(const char* s) const
			{
				const size_t n = std::strlen(s);
				return m_pos + n <= m_size && std::memcmp(m_data + m_pos, s, n) == 0;
			}
			void expect(const char* s)
			{
				if (!startsWith(s))
					fail("unexpected input");
				m_pos += std::strlen(s);
			}
			void skipSpace()
			{
				while (!atEnd() && std::isspace(peek()))
					++m_pos;
			}

			// Decodes one character of the document encoding.
			wchar_t decodeChar() { return decodeXmlChar(m_data, m_size, m_pos, m_utf8); }

			std::string name()
			{
				const size_t start = m_pos;
				while (!atEnd() && (std::isalnum(peek()) || peek() == '_' || peek() == ':' || peek() == '-' || peek() == '.'))
					++m_pos;
				if (m_pos == start)
					fail("expected a name");
				return std::string(reinterpret_cast<const char*>(m_data + start), m_pos - start);
			}

			void appendEntity(std::wstring& out)
			{
				const size_t start = m_pos;
				while (!atEnd() && peek() != ';' && m_pos - start < 12)
					++m_pos;
				if (peek() != ';')
				{
					out.push_back(L'&'); // stray ampersand: keep it
					m_pos = start;
					return;
				}
				const std::string ent(reinterpret_cast<const char*>(m_data + start), m_pos - start);
				++m_pos;
				if (ent == "amp")
					out.push_back(L'&');
				else if (ent == "lt")
					out.push_back(L'<');
				else if (ent == "gt")
					out.push_back(L'>');
				else if (ent == "quot")
					out.push_back(L'"');
				else if (ent == "apos")
					out.push_back(L'\'');
				else if (ent.size() > 1 && ent[0] == '#')
					out.push_back(static_cast<wchar_t>(ent[1] == 'x' ? std::strtoul(ent.c_str() + 2, nullptr, 16) : std::strtoul(ent.c_str() + 1, nullptr, 10)));
				else
					out += L"&" + std::wstring(ent.begin(), ent.end()) + L";";
			}

			std::wstring attributeValue()
			{
				const unsigned char quote = peek();
				if (quote != '"' && quote != '\'')
					fail("expected a quoted attribute value");
				++m_pos;
				std::wstring v;
				while (!atEnd() && peek() != quote)
				{
					if (peek() == '&')
					{
						++m_pos;
						appendEntity(v);
					}
					else
						v.push_back(decodeChar());
				}
				++m_pos;
				return v;
			}

			void declaration()
			{
				// <?xml version="1.0" encoding="ISO-8859-1"?>
				const size_t end = findAfter("?>");
				const std::string decl(reinterpret_cast<const char*>(m_data + m_pos), end - m_pos);
				if (decl.starts_with("<?xml"))
				{
					std::string lower;
					for (char c : decl)
						lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
					m_utf8 = lower.find("utf-8") != std::string::npos || lower.find("encoding") == std::string::npos;
				}
				m_pos = end;
			}

			size_t findAfter(const char* terminator) const
			{
				const size_t n = std::strlen(terminator);
				for (size_t i = m_pos; i + n <= m_size; ++i)
					if (std::memcmp(m_data + i, terminator, n) == 0)
						return i + n;
				throw std::runtime_error(std::string("unterminated construct, expected ") + terminator);
			}

			static bool isBlank(const std::wstring& s)
			{
				for (wchar_t c : s)
					if (!std::iswspace(c))
						return false;
				return true;
			}

			static std::wstring trim(const std::wstring& s)
			{
				size_t b = 0, e = s.size();
				while (b < e && std::iswspace(s[b]))
					++b;
				while (e > b && std::iswspace(s[e - 1]))
					--e;
				return s.substr(b, e - b);
			}

			XmlNode& addChild(XmlNode& parent, XmlNode::Type type)
			{
				auto node = std::make_unique<XmlNode>();
				node->type = type;
				node->parent = &parent;
				node->index = parent.children.size();
				parent.children.push_back(std::move(node));
				return *parent.children.back();
			}

			void flushText(XmlNode& parent, std::wstring& text)
			{
				if (!isBlank(text))
					parent.text += trim(text);
				text.clear();
			}

			// Parses children until the matching end tag (or EOF for the document).
			void parseContent(XmlNode& parent, bool isDocument)
			{
				std::wstring text;
				while (!atEnd())
				{
					if (peek() != '<')
					{
						if (peek() == '&')
						{
							++m_pos;
							appendEntity(text);
						}
						else
							text.push_back(decodeChar());
						continue;
					}
					if (startsWith("<!--"))
					{
						// Comments are not part of the tree: the original engine's cursor
						// only ever visits elements (see Xml.h).
						m_pos = findAfter("-->");
					}
					else if (startsWith("<![CDATA["))
					{
						m_pos += 9;
						const size_t end = findAfter("]]>");
						while (m_pos < end - 3)
							text.push_back(decodeChar());
						m_pos = end;
					}
					else if (startsWith("<?"))
						declaration();
					else if (startsWith("<!"))
						m_pos = findAfter(">"); // DOCTYPE
					else if (startsWith("</"))
					{
						if (isDocument)
							fail("unexpected end tag");
						flushText(parent, text);
						m_pos += 2;
						const std::string n = name();
						if (n != parent.name)
							fail("mismatched end tag");
						skipSpace();
						expect(">");
						return;
					}
					else
					{
						flushText(parent, text);
						++m_pos;
						XmlNode& e = addChild(parent, XmlNode::Type::Element);
						e.name = name();
						while (true)
						{
							skipSpace();
							if (startsWith("/>"))
							{
								m_pos += 2;
								break;
							}
							if (peek() == '>')
							{
								++m_pos;
								parseContent(e, false);
								break;
							}
							std::string an = name();
							skipSpace();
							expect("=");
							skipSpace();
							e.attributes.emplace_back(std::move(an), attributeValue());
						}
					}
				}
				if (!isDocument)
					fail("unexpected end of document");
				flushText(parent, text);
			}

			const unsigned char* m_data;
			size_t m_size;
			size_t m_pos = 0;
			bool m_utf8 = true;
		};

		// ---------------------------------------------------------------- helpers

		std::string narrow(const std::wstring& w)
		{
			// The DLL's narrow strings are Windows-1252; Civ4 text is Latin-1 compatible.
			std::string s;
			s.reserve(w.size());
			for (wchar_t c : w)
				s.push_back(c < 256 ? static_cast<char>(c) : '?');
			return s;
		}

		XmlNode* elementCursor(FXml* xml)
		{
			return xml ? xml->cursor : nullptr;
		}

		void countElements(const XmlNode& n, std::string_view tag, int& count)
		{
			for (const auto& c : n.children)
			{
				if (c->type == XmlNode::Type::Element && c->name == tag)
					++count;
				countElements(*c, tag, count);
			}
		}
	}

	std::unique_ptr<XmlNode> parseXml(std::span<const std::byte> data, std::string& error)
	{
		return Parser(data).run(error);
	}

	// ------------------------------------------------------------------ schema

	const SchemaTypes* Xml::schemaFor(const AssetLocation& xmlFile, const XmlNode& root)
	{
		const std::wstring* ns = root.attribute("xmlns");
		if (!ns || !ns->starts_with(L"x-schema:"))
			return nullptr;
		const std::string schemaName = narrow(ns->substr(9));
		const std::string folder = xmlFile.key.substr(0, xmlFile.key.rfind('/') + 1);
		const auto loc = m_assets.locate(folder + schemaName);
		if (!loc)
			return nullptr;
		auto& slot = m_schemas[loc->key];
		if (!slot)
		{
			slot = std::make_unique<SchemaTypes>();
			std::string error;
			if (auto doc = parseXml(m_assets.read(*loc), error))
			{
				// <ElementType name="iDefineIntVal" content="textOnly" dt:type="int"/>
				std::vector<const XmlNode*> stack{ doc.get() };
				while (!stack.empty())
				{
					const XmlNode* n = stack.back();
					stack.pop_back();
					for (const auto& c : n->children)
						stack.push_back(c.get());
					if (n->type == XmlNode::Type::Element && n->name == "ElementType")
						if (const auto* nameAttr = n->attribute("name"))
							if (const auto* typeAttr = n->attribute("dt:type"))
								(*slot)[narrow(*nameAttr)] = narrow(*typeAttr);
				}
			}
		}
		return slot.get();
	}

	// ------------------------------------------------------------------ interface

	FXml* Xml::CreateFXml(FXmlSchemaCache*) { return new FXml; }

	void Xml::DestroyFXml(FXml*& xml)
	{
		delete xml;
		xml = nullptr;
	}

	FXmlSchemaCache* Xml::CreateFXmlSchemaCache() { return new FXmlSchemaCache; }

	void Xml::DestroyFXmlSchemaCache(FXmlSchemaCache*& cache)
	{
		delete cache;
		cache = nullptr;
	}

	bool Xml::LoadXml(FXml* xml, const TCHAR* pszXmlFile)
	{
		if (!xml || !pszXmlFile)
			return false;
		std::string path = pszXmlFile;
		if (path.starts_with("Assets//") || path.starts_with("Assets\\\\"))
			path = path.substr(8);
		const auto loc = m_assets.locate(path);
		if (!loc)
		{
			std::fprintf(stderr, "[oc4core] xml not found: %s\n", pszXmlFile);
			return false;
		}
		std::string error;
		auto doc = parseXml(m_assets.read(*loc), error);
		if (!doc)
		{
			std::fprintf(stderr, "[oc4core] xml parse error in %s: %s\n", loc->display().c_str(), error.c_str());
			return false;
		}
		XmlNode* root = nullptr;
		for (const auto& c : doc->children)
			if (c->type == XmlNode::Type::Element)
				root = c.get();
		xml->schema = root ? schemaFor(*loc, *root) : nullptr;
		xml->document = std::move(doc);
		xml->cursor = root;
		xml->file = loc->key;
		++m_filesLoaded;
		return true;
	}

	bool Xml::Validate(FXml*, TCHAR* pszError)
	{
		if (pszError)
			pszError[0] = 0;
		return true;
	}

	bool Xml::LocateNode(FXml* xml, const TCHAR* pszXmlNode)
	{
		if (!xml || !xml->document || !pszXmlNode)
			return false;
		// Absolute path from the document: "Civ4UnitInfos/UnitInfos/UnitInfo".
		XmlNode* n = xml->document.get();
		std::string_view path = pszXmlNode;
		while (!path.empty() && path.front() == '/')
			path.remove_prefix(1);
		while (!path.empty())
		{
			const size_t slash = path.find('/');
			const std::string_view part = path.substr(0, slash);
			n = n->firstChildElement(part);
			if (!n)
				return false;
			path = slash == std::string_view::npos ? std::string_view() : path.substr(slash + 1);
		}
		xml->cursor = n;
		return true;
	}

	bool Xml::LocateFirstSiblingNodeByTagName(FXml* xml, TCHAR* pszTagName)
	{
		XmlNode* c = elementCursor(xml);
		if (!c || !c->parent || !pszTagName)
			return false;
		if (XmlNode* n = c->parent->firstChildElement(pszTagName))
		{
			xml->cursor = n;
			return true;
		}
		return false;
	}

	bool Xml::LocateNextSiblingNodeByTagName(FXml* xml, TCHAR* pszTagName)
	{
		XmlNode* c = elementCursor(xml);
		if (!c || !pszTagName)
			return false;
		for (XmlNode* n = c->nextSibling(); n; n = n->nextSibling())
			if (n->type == XmlNode::Type::Element && n->name == pszTagName)
			{
				xml->cursor = n;
				return true;
			}
		return false;
	}

	bool Xml::NextSibling(FXml* xml)
	{
		XmlNode* c = elementCursor(xml);
		XmlNode* n = c ? c->nextSibling() : nullptr;
		if (!n)
			return false;
		xml->cursor = n;
		return true;
	}

	bool Xml::PrevSibling(FXml* xml)
	{
		XmlNode* c = elementCursor(xml);
		XmlNode* n = c ? c->prevSibling() : nullptr;
		if (!n)
			return false;
		xml->cursor = n;
		return true;
	}

	bool Xml::SetToChild(FXml* xml)
	{
		XmlNode* c = elementCursor(xml);
		if (!c || c->type != XmlNode::Type::Element || c->children.empty())
			return false;
		xml->cursor = c->children.front().get();
		return true;
	}

	bool Xml::SetToChildByTagName(FXml* xml, const TCHAR* szTagName)
	{
		XmlNode* c = elementCursor(xml);
		if (!c || !szTagName)
			return false;
		if (XmlNode* n = c->firstChildElement(szTagName))
		{
			xml->cursor = n;
			return true;
		}
		return false;
	}

	bool Xml::SetToParent(FXml* xml)
	{
		XmlNode* c = elementCursor(xml);
		if (!c || !c->parent || c->parent->type == XmlNode::Type::Document)
			return false;
		xml->cursor = c->parent;
		return true;
	}

	int Xml::GetLastNodeTextSize(FXml* xml)
	{
		XmlNode* c = elementCursor(xml);
		return c ? int(narrow(c->text).size()) + 1 : 0;
	}

	bool Xml::GetLastNodeText(FXml* xml, TCHAR* pszText)
	{
		XmlNode* c = elementCursor(xml);
		if (!c || !pszText)
			return false;
		const std::string s = narrow(c->text);
		std::memcpy(pszText, s.c_str(), s.size() + 1);
		return true;
	}

	bool Xml::GetLastNodeValue(FXml* xml, std::string& value)
	{
		XmlNode* c = elementCursor(xml);
		if (!c)
			return false;
		value = narrow(c->text);
		return true;
	}

	bool Xml::GetLastNodeValue(FXml* xml, std::wstring& value)
	{
		XmlNode* c = elementCursor(xml);
		if (!c)
			return false;
		value = c->text;
		return true;
	}

	bool Xml::GetLastNodeValue(FXml* xml, char* value)
	{
		XmlNode* c = elementCursor(xml);
		if (!c || !value)
			return false;
		const std::string s = narrow(c->text);
		std::memcpy(value, s.c_str(), s.size() + 1);
		return true;
	}

	bool Xml::GetLastNodeValue(FXml* xml, wchar* value)
	{
		XmlNode* c = elementCursor(xml);
		if (!c || !value)
			return false;
		std::wmemcpy(value, c->text.c_str(), c->text.size() + 1);
		return true;
	}

	bool Xml::GetLastNodeValue(FXml* xml, bool* value)
	{
		XmlNode* c = elementCursor(xml);
		if (!c || !value)
			return false;
		const std::wstring& t = c->text;
		*value = t == L"1" || _wcsicmp(t.c_str(), L"true") == 0;
		return true;
	}

	bool Xml::GetLastNodeValue(FXml* xml, int* value)
	{
		XmlNode* c = elementCursor(xml);
		if (!c || !value)
			return false;
		*value = static_cast<int>(std::wcstol(c->text.c_str(), nullptr, 10));
		return true;
	}

	bool Xml::GetLastNodeValue(FXml* xml, float* value)
	{
		XmlNode* c = elementCursor(xml);
		if (!c || !value)
			return false;
		*value = std::wcstof(c->text.c_str(), nullptr);
		return true;
	}

	bool Xml::GetLastNodeValue(FXml* xml, unsigned int* value)
	{
		XmlNode* c = elementCursor(xml);
		if (!c || !value)
			return false;
		*value = static_cast<unsigned int>(std::wcstoul(c->text.c_str(), nullptr, 10));
		return true;
	}

	bool Xml::GetLastLocatedNodeType(FXml* xml, TCHAR* pszType)
	{
		XmlNode* c = elementCursor(xml);
		if (!c || !pszType)
			return false;
		std::string type;
		if (xml->schema)
			if (const auto it = xml->schema->find(c->name); it != xml->schema->end())
				type = it->second;
		std::memcpy(pszType, type.c_str(), type.size() + 1);
		return true;
	}

	bool Xml::IsLastLocatedNodeCommentNode(FXml* xml)
	{
		XmlNode* c = elementCursor(xml);
		return c && c->type == XmlNode::Type::Comment;
	}

	int Xml::NumOfElementsByTagName(FXml* xml, TCHAR* pszTagName)
	{
		if (!xml || !xml->document || !pszTagName)
			return 0;
		std::string_view path = pszTagName;
		if (path.find('/') == std::string_view::npos)
		{
			int count = 0;
			countElements(*xml->document, path, count);
			return count;
		}
		// The DLL passes absolute paths ("Civ4Types/FlavorTypes/FlavorType"): count
		// every element the path selects, like an XPath selectNodes().
		while (!path.empty() && path.front() == '/')
			path.remove_prefix(1);
		std::vector<const XmlNode*> level{ xml->document.get() };
		while (!path.empty())
		{
			const size_t slash = path.find('/');
			const std::string_view part = path.substr(0, slash);
			std::vector<const XmlNode*> next;
			for (const XmlNode* n : level)
				for (const auto& c : n->children)
					if (c->type == XmlNode::Type::Element && c->name == part)
						next.push_back(c.get());
			level = std::move(next);
			path = slash == std::string_view::npos ? std::string_view() : path.substr(slash + 1);
		}
		return int(level.size());
	}

	int Xml::NumOfChildrenByTagName(FXml* xml, const TCHAR* pszTagName)
	{
		XmlNode* c = elementCursor(xml);
		if (!c || !pszTagName)
			return 0;
		int count = 0;
		for (const auto& n : c->children)
			if (n->type == XmlNode::Type::Element && n->name == pszTagName)
				++count;
		return count;
	}

	int Xml::GetNumSiblings(FXml* xml)
	{
		// The other nodes next to the cursor (the caller adds one for itself).
		XmlNode* c = elementCursor(xml);
		return c && c->parent ? int(c->parent->children.size()) - 1 : 0;
	}

	int Xml::GetNumChildren(FXml* xml)
	{
		XmlNode* c = elementCursor(xml);
		return c ? int(c->children.size()) : 0;
	}

	bool Xml::GetLastLocatedNodeTagName(FXml* xml, TCHAR* pszTagName)
	{
		XmlNode* c = elementCursor(xml);
		if (!c || !pszTagName)
			return false;
		std::memcpy(pszTagName, c->name.c_str(), c->name.size() + 1);
		return true;
	}
}
