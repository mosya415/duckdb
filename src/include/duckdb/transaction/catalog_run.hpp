//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/transaction/catalog_run.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/parser/parsed_data/parse_info.hpp"

namespace duckdb {

struct CatalogRunEntry {
	CatalogRunEntry(CatalogEntry &entry, idx_t change, unique_ptr<ParseInfo> alter_info)
	    : entry(entry), change(change), alter_info(std::move(alter_info)) {
	}

	reference<CatalogEntry> entry;
	idx_t change;
	unique_ptr<ParseInfo> alter_info;
};

struct CatalogRun {
	idx_t catalog_oid;
	reference<const vector<CatalogRunEntry>> entries;
};

} // namespace duckdb
