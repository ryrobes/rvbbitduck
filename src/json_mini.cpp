#include "json_mini.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace duckdb {
namespace rvbbit {

Json Json::Null() {
	return Json();
}

Json Json::Bool(bool value) {
	Json json;
	json.type = Type::BOOL;
	json.b = value;
	return json;
}

Json Json::Number(double value) {
	Json json;
	json.type = Type::NUMBER;
	json.num = value;
	return json;
}

Json Json::String(string value) {
	Json json;
	json.type = Type::STRING;
	json.str = std::move(value);
	return json;
}

Json Json::Array() {
	Json json;
	json.type = Type::ARRAY;
	return json;
}

Json Json::Object() {
	Json json;
	json.type = Type::OBJECT;
	return json;
}

Json &Json::Set(const string &key, Json value) {
	if (type != Type::OBJECT) {
		type = Type::OBJECT;
		obj.clear();
	}
	for (auto &entry : obj) {
		if (entry.first == key) {
			entry.second = std::move(value);
			return entry.second;
		}
	}
	obj.emplace_back(key, std::move(value));
	return obj.back().second;
}

const Json *Json::Find(const string &key) const {
	if (type != Type::OBJECT) {
		return nullptr;
	}
	for (auto &entry : obj) {
		if (entry.first == key) {
			return &entry.second;
		}
	}
	return nullptr;
}

Json *Json::Find(const string &key) {
	return const_cast<Json *>(static_cast<const Json *>(this)->Find(key));
}

namespace {

struct Parser {
	const string &text;
	idx_t i = 0;

	explicit Parser(const string &text_p) : text(text_p) {
	}

	[[noreturn]] void Fail(const string &message) const {
		throw std::runtime_error(message + " at offset " + std::to_string(i));
	}

	void Skip() {
		while (i < text.size() && (text[i] == ' ' || text[i] == '\n' || text[i] == '\r' || text[i] == '\t')) {
			i++;
		}
	}

	char Peek() {
		Skip();
		if (i >= text.size()) {
			Fail("unexpected end of JSON");
		}
		return text[i];
	}

	Json ParseValue() {
		char c = Peek();
		if (c == 'n') {
			return Literal("null", Json::Null());
		}
		if (c == 't') {
			return Literal("true", Json::Bool(true));
		}
		if (c == 'f') {
			return Literal("false", Json::Bool(false));
		}
		if (c == '"') {
			return Json::String(ParseString());
		}
		if (c == '[') {
			return ParseArray();
		}
		if (c == '{') {
			return ParseObject();
		}
		if (c == '-' || (c >= '0' && c <= '9')) {
			return ParseNumber();
		}
		Fail("unexpected JSON value");
	}

	Json Literal(const char *word, Json value) {
		auto len = std::char_traits<char>::length(word);
		if (text.compare(i, len, word) != 0) {
			Fail(string("expected ") + word);
		}
		i += len;
		return value;
	}

	string ParseString() {
		if (Peek() != '"') {
			Fail("expected string");
		}
		i++;
		string out;
		while (i < text.size()) {
			char c = text[i++];
			if (c == '"') {
				return out;
			}
			if (c == '\\') {
				if (i >= text.size()) {
					Fail("bad escape");
				}
				char e = text[i++];
				switch (e) {
				case '"':
				case '\\':
				case '/':
					out.push_back(e);
					break;
				case 'b':
					out.push_back('\b');
					break;
				case 'f':
					out.push_back('\f');
					break;
				case 'n':
					out.push_back('\n');
					break;
				case 'r':
					out.push_back('\r');
					break;
				case 't':
					out.push_back('\t');
					break;
				case 'u': {
					if (i + 4 > text.size()) {
						Fail("bad unicode escape");
					}
					int code = 0;
					for (int k = 0; k < 4; k++) {
						char h = text[i++];
						code <<= 4;
						if (h >= '0' && h <= '9') {
							code += h - '0';
						} else if (h >= 'a' && h <= 'f') {
							code += h - 'a' + 10;
						} else if (h >= 'A' && h <= 'F') {
							code += h - 'A' + 10;
						} else {
							Fail("bad unicode escape");
						}
					}
					if (code < 0x80) {
						out.push_back(static_cast<char>(code));
					} else if (code < 0x800) {
						out.push_back(static_cast<char>(0xC0 | (code >> 6)));
						out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
					} else {
						out.push_back(static_cast<char>(0xE0 | (code >> 12)));
						out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
						out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
					}
					break;
				}
				default:
					Fail("bad escape");
				}
			} else if (static_cast<unsigned char>(c) < 0x20) {
				Fail("raw control character in string");
			} else {
				out.push_back(c);
			}
		}
		Fail("unterminated string");
	}

	Json ParseNumber() {
		Skip();
		idx_t start = i;
		if (text[i] == '-') {
			i++;
		}
		if (i >= text.size() || text[i] < '0' || text[i] > '9') {
			Fail("bad number");
		}
		if (text[i] == '0') {
			i++;
		} else {
			while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
				i++;
			}
		}
		if (i < text.size() && text[i] == '.') {
			i++;
			if (i >= text.size() || text[i] < '0' || text[i] > '9') {
				Fail("bad number");
			}
			while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
				i++;
			}
		}
		if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
			i++;
			if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
				i++;
			}
			if (i >= text.size() || text[i] < '0' || text[i] > '9') {
				Fail("bad number");
			}
			while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
				i++;
			}
		}
		auto token = text.substr(start, i - start);
		char *end = nullptr;
		double value = std::strtod(token.c_str(), &end);
		if (!end || *end != '\0' || !std::isfinite(value)) {
			Fail("bad number");
		}
		return Json::Number(value);
	}

	Json ParseArray() {
		if (Peek() != '[') {
			Fail("expected array");
		}
		i++;
		Json json = Json::Array();
		Skip();
		if (i < text.size() && text[i] == ']') {
			i++;
			return json;
		}
		while (true) {
			json.arr.push_back(ParseValue());
			char c = Peek();
			if (c == ',') {
				i++;
				continue;
			}
			if (c == ']') {
				i++;
				break;
			}
			Fail("expected comma or end of array");
		}
		return json;
	}

	Json ParseObject() {
		if (Peek() != '{') {
			Fail("expected object");
		}
		i++;
		Json json = Json::Object();
		Skip();
		if (i < text.size() && text[i] == '}') {
			i++;
			return json;
		}
		while (true) {
			if (Peek() != '"') {
				Fail("expected object key");
			}
			auto key = ParseString();
			if (Peek() != ':') {
				Fail("expected colon");
			}
			i++;
			auto value = ParseValue();
			if (json.Has(key)) {
				Fail("duplicate object key");
			}
			json.Set(key, std::move(value));
			char c = Peek();
			if (c == ',') {
				i++;
				continue;
			}
			if (c == '}') {
				i++;
				break;
			}
			Fail("expected comma or end of object");
		}
		return json;
	}
};

void DumpString(const string &value, string &out) {
	out.push_back('"');
	for (unsigned char c : value) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out.push_back(static_cast<char>(c));
			}
		}
	}
	out.push_back('"');
}

string FormatNumber(double value) {
	if (!std::isfinite(value)) {
		throw std::runtime_error("JSON number is not finite");
	}
	double truncated = std::trunc(value);
	if (truncated == value && std::fabs(value) < 1e15) {
		return std::to_string(static_cast<long long>(truncated));
	}
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.16g", value);
	return buf;
}

void DumpInto(const Json &json, string &out) {
	switch (json.type) {
	case Json::Type::NUL:
		out += "null";
		break;
	case Json::Type::BOOL:
		out += json.b ? "true" : "false";
		break;
	case Json::Type::NUMBER:
		out += FormatNumber(json.num);
		break;
	case Json::Type::STRING:
		DumpString(json.str, out);
		break;
	case Json::Type::ARRAY: {
		out.push_back('[');
		for (idx_t i = 0; i < json.arr.size(); i++) {
			if (i) {
				out.push_back(',');
			}
			DumpInto(json.arr[i], out);
		}
		out.push_back(']');
		break;
	}
	case Json::Type::OBJECT: {
		vector<idx_t> order;
		order.reserve(json.obj.size());
		for (idx_t i = 0; i < json.obj.size(); i++) {
			order.push_back(i);
		}
		std::sort(order.begin(), order.end(), [&](idx_t a, idx_t b) { return json.obj[a].first < json.obj[b].first; });
		out.push_back('{');
		for (idx_t i = 0; i < order.size(); i++) {
			if (i) {
				out.push_back(',');
			}
			DumpString(json.obj[order[i]].first, out);
			out.push_back(':');
			DumpInto(json.obj[order[i]].second, out);
		}
		out.push_back('}');
		break;
	}
	}
}

void BlobInto(const Json &json, string &out) {
	switch (json.type) {
	case Json::Type::STRING:
		out.push_back(' ');
		out += json.str;
		break;
	case Json::Type::ARRAY:
		for (auto &child : json.arr) {
			BlobInto(child, out);
		}
		break;
	case Json::Type::OBJECT:
		for (auto &entry : json.obj) {
			out.push_back(' ');
			out += entry.first;
			BlobInto(entry.second, out);
		}
		break;
	default:
		break;
	}
}

} // namespace

Json Json::Parse(const string &text) {
	Parser parser(text);
	auto value = parser.ParseValue();
	parser.Skip();
	if (parser.i != text.size()) {
		parser.Fail("trailing data after JSON");
	}
	return value;
}

string Json::Dump() const {
	string out;
	out.reserve(64);
	DumpInto(*this, out);
	return out;
}

string Json::TextBlob() const {
	string out;
	BlobInto(*this, out);
	return out;
}

} // namespace rvbbit
} // namespace duckdb
