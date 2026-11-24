#pragma once

#include "duckdb.hpp"

namespace duckdb {

class RvbbitExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

void LoadInternal(ExtensionLoader &loader);

} // namespace duckdb
