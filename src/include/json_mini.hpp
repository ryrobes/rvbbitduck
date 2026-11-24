#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/unordered_map.hpp"

#include <utility>

namespace duckdb {
namespace rvbbit {

//! Minimal JSON value used for OpenRouter payloads and decision-tree nodes.
//! Objects keep insertion order. Dump() emits canonical JSON: sorted keys, no
//! insignificant whitespace, so cache keys and tree ids are stable.
struct Json {
	enum class Type : uint8_t { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT };

	Type type = Type::NUL;
	bool b = false;
	double num = 0;
	string str;
	vector<Json> arr;
	vector<std::pair<string, Json>> obj;

	static Json Null();
	static Json Bool(bool value);
	static Json Number(double value);
	static Json String(string value);
	static Json Array();
	static Json Object();

	bool IsNull() const {
		return type == Type::NUL;
	}
	bool IsBool() const {
		return type == Type::BOOL;
	}
	bool IsNumber() const {
		return type == Type::NUMBER;
	}
	bool IsString() const {
		return type == Type::STRING;
	}
	bool IsArray() const {
		return type == Type::ARRAY;
	}
	bool IsObject() const {
		return type == Type::OBJECT;
	}

	Json &Set(const string &key, Json value);
	const Json *Find(const string &key) const;
	Json *Find(const string &key);
	bool Has(const string &key) const {
		return Find(key) != nullptr;
	}

	//! Parse one JSON value. Throws std::runtime_error on malformed input.
	static Json Parse(const string &text);

	//! Canonical compact JSON.
	string Dump() const;

	//! All string values and keys, used by the mock scorer.
	string TextBlob() const;
};

} // namespace rvbbit
} // namespace duckdb
