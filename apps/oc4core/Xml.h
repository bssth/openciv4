#pragma once

// The engine's XML interface (CvDLLXmlIFaceBase): the DLL loads a document and
// walks it with a cursor. The cursor only visits elements: the DLL relies on
// SetToChild landing on the first element and GetNumChildren counting elements
// even where files have comments in between (e.g. CIV4GameText_BTS_PatchText.xml),
// so comments are dropped and text lives on its element. The data type of an
// element comes from the document's XDR schema (x-schema:...).

#include "generated/IFaceStubs.h"

#include "core/AssetResolver.h"

#include <map>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace oc4::host
{
	struct XmlNode
	{
		enum class Type : uint8_t
		{
			Document,
			Element,
			Comment,
			Text,
		};

		Type type = Type::Element;
		std::string name;  // element tag
		std::wstring text; // text content (element: concatenated direct text)
		XmlNode* parent = nullptr;
		size_t index = 0; // position in parent->children
		std::vector<std::unique_ptr<XmlNode>> children;
		std::vector<std::pair<std::string, std::wstring>> attributes;

		XmlNode* child(size_t i) const { return i < children.size() ? children[i].get() : nullptr; }
		XmlNode* nextSibling() const { return parent ? parent->child(index + 1) : nullptr; }
		XmlNode* prevSibling() const { return parent && index > 0 ? parent->child(index - 1) : nullptr; }
		XmlNode* firstChildElement(std::string_view tag) const;
		const std::wstring* attribute(std::string_view name) const;
	};

	// Parses XML 1.0 as used by Civ4 (ISO-8859-1 or UTF-8). Returns the document node.
	std::unique_ptr<XmlNode> parseXml(std::span<const std::byte> data, std::string& error);

	using SchemaTypes = std::unordered_map<std::string, std::string>; // element name -> dt:type

	class Xml : public stubs::XmlIFace
	{
	public:
		explicit Xml(const AssetResolver& assets) : m_assets(assets) {}

		FXml* CreateFXml(FXmlSchemaCache* pSchemaCache) override;
		void DestroyFXml(FXml*& xml) override;
		void DestroyFXmlSchemaCache(FXmlSchemaCache*& cache) override;
		FXmlSchemaCache* CreateFXmlSchemaCache() override;
		bool LoadXml(FXml* xml, const TCHAR* pszXmlFile) override;
		bool Validate(FXml* xml, TCHAR* pszError) override;
		bool LocateNode(FXml* xml, const TCHAR* pszXmlNode) override;
		bool LocateFirstSiblingNodeByTagName(FXml* xml, const TCHAR* pszTagName) override;
		bool LocateNextSiblingNodeByTagName(FXml* xml, const TCHAR* pszTagName) override;
		bool NextSibling(FXml* xml) override;
		bool PrevSibling(FXml* xml) override;
		bool SetToChild(FXml* xml) override;
		bool SetToChildByTagName(FXml* xml, const TCHAR* szTagName) override;
		bool SetToParent(FXml* xml) override;
		int GetLastNodeTextSize(FXml* xml) override;
		bool GetLastNodeText(FXml* xml, TCHAR* pszText) override;
		bool GetLastNodeValue(FXml* xml, std::string& pszText) override;
		bool GetLastNodeValue(FXml* xml, std::wstring& pszText) override;
		bool GetLastNodeValue(FXml* xml, char* pszText) override;
		bool GetLastNodeValue(FXml* xml, wchar* pszText) override;
		bool GetLastNodeValue(FXml* xml, bool* pbVal) override;
		bool GetLastNodeValue(FXml* xml, int* piVal) override;
		bool GetLastNodeValue(FXml* xml, float* pfVal) override;
		bool GetLastNodeValue(FXml* xml, unsigned int* puiVal) override;
		bool GetLastLocatedNodeType(FXml* xml, TCHAR* pszType) override;
		bool IsLastLocatedNodeCommentNode(FXml* xml) override;
		int NumOfElementsByTagName(FXml* xml, const TCHAR* pszTagName) override;
		int NumOfChildrenByTagName(FXml* xml, const TCHAR* pszTagName) override;
		int GetNumSiblings(FXml* xml) override;
		int GetNumChildren(FXml* xml) override;
		bool GetLastLocatedNodeTagName(FXml* xml, TCHAR* pszTagName) override;
		bool IsAllowXMLCaching() override { return false; }
		void MapChildren(FXml*) override {}

		size_t filesLoaded() const { return m_filesLoaded; }

	private:
		const SchemaTypes* schemaFor(const AssetLocation& xmlFile, const XmlNode& root);

		const AssetResolver& m_assets;
		std::map<std::string, std::unique_ptr<SchemaTypes>> m_schemas; // by asset key
		size_t m_filesLoaded = 0;
	};
}

// Opaque to the DLL; defined by the engine.
class FXml
{
public:
	std::unique_ptr<oc4::host::XmlNode> document;
	oc4::host::XmlNode* cursor = nullptr;
	const oc4::host::SchemaTypes* schema = nullptr;
	std::string file;
};

class FXmlSchemaCache
{
};
