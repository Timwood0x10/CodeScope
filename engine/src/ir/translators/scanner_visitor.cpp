#include "scanner_visitor.h"
#include <cstring>
#include <sstream>

namespace ir
{

ScannerVisitor::ScannerVisitor()
{
}

SemanticUnit *ScannerVisitor::scan(const std::string &source,
				   const char *file_path)
{
	unit_ = new SemanticUnit();
	SemanticEmitter emitter(unit_);
	emitter_ = &emitter;
	unit_->setFilePath(file_path);
	unit_->setLanguage(language_);

	std::istringstream stream(source);
	std::string line;
	int line_num = 0;

	while (std::getline(stream, line)) {
		line_num++;
		std::string_view sv(line);
		sv = trimLeft(sv);
		if (sv.empty() || sv[0] == '/' || sv[0] == '#' || sv[0] == '*')
			continue;

		std::string kind = detectDeclLine(sv);
		if (kind.empty())
			continue;

		std::string name = extractDeclName(sv, kind);
		if (name.empty())
			continue;

		// Skip false positives
		if (name == "if" || name == "for" || name == "while" ||
		    name == "switch" || name == "catch" || name == "return" ||
		    name == "else")
			continue;

		// Find end column from the name + signature length
		uint32_t end_col =
			static_cast<uint32_t>(sv.size() > 80 ? 80 : sv.size());
		SourceRange loc{ static_cast<uint32_t>(line_num - 1), 0,
				 static_cast<uint32_t>(line_num - 1), end_col };

		emitDecl(kind, name, loc, 0);
	}

	emitter_ = nullptr;
	return unit_;
}

void ScannerVisitor::emitDecl(const std::string &kind, const std::string &name,
			      SourceRange loc, uint64_t parent_id)
{
	if (kind == "function" || kind == "method")
		emitter_->emitFunction(name, loc, parent_id);
	else if (kind == "class")
		emitter_->emitClass(name, loc, parent_id);
	else if (kind == "struct")
		emitter_->emitClass(name, loc, parent_id);
	else if (kind == "interface")
		emitter_->emitInterface(name, loc, parent_id);
	else if (kind == "enum")
		emitter_->emitEnum(name, loc, parent_id);
	else if (kind == "type_alias")
		emitter_->emitTypeAlias(name, loc, parent_id);
	else if (kind == "const" || kind == "variable")
		emitter_->emitVariable(name, loc, parent_id);
}

// ── Line detection helpers ──────────────────────────────────────

std::string_view ScannerVisitor::trimLeft(std::string_view s)
{
	while (!s.empty() && (s[0] == ' ' || s[0] == '\t'))
		s.remove_prefix(1);
	return s;
}

bool ScannerVisitor::startsWithKW(std::string_view line, const char *kw)
{
	line = trimLeft(line);
	auto klen = std::strlen(kw);
	if (line.size() < klen)
		return false;
	if (line.substr(0, klen) != kw)
		return false;
	// kw ending with space: space was delimiter
	if (klen > 0 && kw[klen - 1] == ' ')
		return true;
	// Otherwise must be followed by non-word char
	if (line.size() > klen) {
		char c = line[klen];
		return (c == ' ' || c == '(' || c == '<' || c == '\t' ||
			c == '{' || c == '[' || c == ':' || c == ';');
	}
	return true;
}

std::string ScannerVisitor::detectDeclLine(std::string_view line)
{
	line = trimLeft(line);
	if (line.empty() || line[0] == '/' || line[0] == '#' || line[0] == '*')
		return "";

	// C/C++ detection
	if (language_ == "c" || language_ == "cpp") {
		if (startsWithKW(line, "class "))
			return "class";
		if (startsWithKW(line, "struct "))
			return "struct";
		if (startsWithKW(line, "enum "))
			return "enum";
		if (startsWithKW(line, "union "))
			return "struct";
		if (startsWithKW(line, "namespace "))
			return "";
		if (startsWithKW(line, "using namespace "))
			return "";
		if (startsWithKW(line, "template "))
			return "";
		// C function detection via looksLikeCFunction logic
		if (line.find('(') != std::string_view::npos &&
		    line.find('(') > 0 && line.back() != ';' &&
		    line.back() != ',') {
			// More careful: skip if/while/for/switch
			static const char *skip[] = {
				"if",	  "while",  "for",    "switch", "catch",
				"return", "sizeof", "typeof", nullptr
			};
			auto name_end = line.find('(');
			auto name_start = name_end;
			while (name_start > 0 &&
			       (isalnum(line[name_start - 1]) ||
				line[name_start - 1] == '_' ||
				line[name_start - 1] == ':'))
				name_start--;
			if (name_start == name_end)
				return "";
			auto name_tok =
				line.substr(name_start, name_end - name_start);
			for (const char **s = skip; *s; s++)
				if (name_tok == *s)
					return "";
			// Must have a known type keyword before the name
			auto before = trimLeft(line.substr(0, name_start));
			static const char *types[] = {
				"int ",	    "void ",	 "char ",   "long ",
				"short ",   "float ",	 "double ", "bool ",
				"signed ",  "unsigned ", "const ",  "struct ",
				"union ",   "enum ",	 "class ",  "size_t ",
				"ssize_t ", nullptr
			};
			for (const char **t = types; *t; t++)
				if (before.find(*t) != std::string_view::npos)
					return "function";
		}
	}

	// Rust
	if (language_ == "rust") {
		if (startsWithKW(line, "pub unsafe fn ") ||
		    startsWithKW(line, "pub async fn ") ||
		    startsWithKW(line, "pub fn ") || startsWithKW(line, "fn "))
			return "function";
		if (startsWithKW(line, "pub struct ") ||
		    startsWithKW(line, "struct "))
			return "struct";
		if (startsWithKW(line, "pub enum ") ||
		    startsWithKW(line, "enum "))
			return "enum";
		if (startsWithKW(line, "pub trait ") ||
		    startsWithKW(line, "trait "))
			return "interface";
		if (startsWithKW(line, "pub type ") ||
		    startsWithKW(line, "type "))
			return "type_alias";
	}

	// Python
	if (language_ == "python") {
		if (startsWithKW(line, "async def ") ||
		    startsWithKW(line, "def "))
			return "function";
		if (startsWithKW(line, "class "))
			return "class";
	}

	// JS/TS
	if (language_ == "javascript" || language_ == "typescript") {
		if (startsWithKW(line, "export async function ") ||
		    startsWithKW(line, "export function ") ||
		    startsWithKW(line, "async function ") ||
		    startsWithKW(line, "function "))
			return "function";
		if (startsWithKW(line, "export class ") ||
		    startsWithKW(line, "class "))
			return "class";
		if (startsWithKW(line, "export interface ") ||
		    startsWithKW(line, "interface "))
			return "interface";
		if (startsWithKW(line, "export enum ") ||
		    startsWithKW(line, "enum "))
			return "enum";
	}

	// Go
	if (language_ == "go") {
		if (startsWithKW(line, "func "))
			return "function";
		if (startsWithKW(line, "type ")) {
			if (line.find("struct") != std::string_view::npos)
				return "struct";
			if (line.find("interface") != std::string_view::npos)
				return "interface";
			return "type_alias";
		}
	}

	// Java
	if (language_ == "java") {
		if (startsWithKW(line, "class ") ||
		    startsWithKW(line, "public class ") ||
		    startsWithKW(line, "private class ") ||
		    startsWithKW(line, "protected class "))
			return "class";
		if (startsWithKW(line, "interface ") ||
		    startsWithKW(line, "public interface "))
			return "interface";
		if (startsWithKW(line, "enum ") ||
		    startsWithKW(line, "public enum "))
			return "enum";
	}

	// Swift
	if (language_ == "swift") {
		if (startsWithKW(line, "func ") ||
		    startsWithKW(line, "public func ") ||
		    startsWithKW(line, "open func "))
			return "function";
		if (startsWithKW(line, "class ") ||
		    startsWithKW(line, "public class ") ||
		    startsWithKW(line, "open class "))
			return "class";
		if (startsWithKW(line, "struct ") ||
		    startsWithKW(line, "public struct "))
			return "struct";
		if (startsWithKW(line, "enum ") ||
		    startsWithKW(line, "public enum "))
			return "enum";
		if (startsWithKW(line, "protocol ") ||
		    startsWithKW(line, "public protocol "))
			return "interface";
	}

	return "";
}

std::string ScannerVisitor::extractDeclName(std::string_view line,
					    const std::string &kind)
{
	line = trimLeft(line);

	// Skip language-specific keyword prefixes to find the name token
	auto skip_prefix = [&](const char **keywords) {
		for (const char **kw = keywords; *kw; kw++) {
			if (startsWithKW(line, *kw)) {
				line = trimLeft(line.substr(std::strlen(*kw)));
				return true;
			}
		}
		return false;
	};

	// For struct/class/enum declarations, the name is the next identifier
	if (kind == "struct" || kind == "class" || kind == "enum" ||
	    kind == "interface" || kind == "type_alias") {
		if (language_ == "rust") {
			static const char *rk[] = {
				"pub struct", "struct", "pub enum", "enum",
				"pub trait",  "trait",	"pub type", "type",
				"pub union",  "union",	nullptr
			};
			skip_prefix(rk);
		} else if (language_ == "c" || language_ == "cpp") {
			static const char *ck[] = { "class ",	   "struct ",
						    "enum ",	   "union ",
						    "enum class ", nullptr };
			skip_prefix(ck);
		} else if (language_ == "java") {
			static const char *jk[] = { "public class",
						    "private class",
						    "protected class",
						    "class",
						    "public interface",
						    "interface",
						    "public enum",
						    "enum",
						    nullptr };
			skip_prefix(jk);
		} else if (language_ == "swift") {
			static const char *sk[] = {
				"open class", "public class",
				"class",      "public struct",
				"struct",     "public enum",
				"enum",	      "public protocol",
				"protocol",   nullptr
			};
			skip_prefix(sk);
		} else if (language_ == "go") {
			if (line.substr(0, 5) == "type ")
				line = trimLeft(line.substr(5));
		} else if (language_ == "python") {
			static const char *pk[] = { "class", nullptr };
			skip_prefix(pk);
		} else if (language_ == "javascript" ||
			   language_ == "typescript") {
			static const char *jk2[] = { "export class",
						     "class",
						     "export interface",
						     "interface",
						     "export enum",
						     "enum",
						     nullptr };
			skip_prefix(jk2);
		}
		// Take first word as name
		size_t pos = 0;
		while (pos < line.size() &&
		       (isalnum(line[pos]) || line[pos] == '_'))
			pos++;
		if (pos == 0)
			return "";
		return std::string(line.substr(0, pos));
	}

	// For function/method declarations, find name before '('
	if (language_ == "rust") {
		static const char *rk[] = {
			"pub unsafe fn", "pub async fn", "pub fn",   "fn",
			"pub struct",	 "struct",	 "pub enum", "enum",
			"pub trait",	 "trait",	 "pub type", "type",
			nullptr
		};
		skip_prefix(rk);
	} else if (language_ == "python") {
		static const char *pk[] = { "async def", "def", "class",
					    nullptr };
		skip_prefix(pk);
	} else if (language_ == "javascript" || language_ == "typescript") {
		static const char *jk[] = { "export async function",
					    "export function",
					    "async function",
					    "function",
					    "export class",
					    "class",
					    "export interface",
					    "interface",
					    "export enum",
					    "enum",
					    nullptr };
		skip_prefix(jk);
	} else if (language_ == "go") {
		if (line.substr(0, 5) == "func ")
			line = trimLeft(line.substr(5));
		else if (line.substr(0, 5) == "type ")
			line = trimLeft(line.substr(5));
	} else if (language_ == "swift") {
		static const char *sk[] = {
			"open func",	 "public func",	    "func",
			"open class",	 "public class",    "class",
			"public struct", "struct",	    "public enum",
			"enum",		 "public protocol", "protocol",
			nullptr
		};
		skip_prefix(sk);
	} else if (language_ == "java") {
		static const char *jk2[] = { "public class",
					     "private class",
					     "protected class",
					     "class",
					     "public interface",
					     "interface",
					     "public enum",
					     "enum",
					     nullptr };
		skip_prefix(jk2);
	} else if (language_ == "c" || language_ == "cpp") {
		// For C/C++, skip storage class or type prefix, then find name before '('
		static const char *ck[] = { "static ",	  "extern ",
					    "inline ",	  "virtual ",
					    "constexpr ", "const ",
					    "volatile ",  nullptr };
		skip_prefix(ck);
		// If there's a '(' in the line, the name is the token just before it
		auto paren = line.find('(');
		if (paren != std::string_view::npos && paren > 0) {
			auto end = paren;
			auto start = end;
			while (start > 0 && (isalnum(line[start - 1]) ||
					     line[start - 1] == '_' ||
					     line[start - 1] == ':'))
				start--;
			if (start < end) {
				// Skip backwards over * and & (pointer/reference)
				line = line.substr(start, end - start);
				// Strip leading type keywords
				static const char *tk[] = { "unsigned ",
							    "signed ", "long ",
							    "short ", nullptr };
				skip_prefix(tk);
				// Now find the last word (the actual name)
				auto last_space = line.rfind(' ');
				if (last_space != std::string_view::npos)
					line = trimLeft(
						line.substr(last_space + 1));
				// Clean up any trailing *
				while (!line.empty() && line.back() == '*')
					line.remove_suffix(1);
				line = trimLeft(line);
			}
		}
	}

	// Extract identifier: alphanumeric + underscore + scope
	size_t pos = 0;
	while (pos < line.size() &&
	       (isalnum(line[pos]) || line[pos] == '_' || line[pos] == ':'))
		pos++;
	if (pos == 0)
		return "";

	std::string name(line.substr(0, pos));
	// Remove trailing ':' from scope operators
	while (!name.empty() && name.back() == ':')
		name.pop_back();
	return name;
}

} // namespace ir
