#include "expect.hpp"
#include "lakemon_sql.hpp"

#include <iostream>
#include <string>

static bool Contains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

static void TestFlushSchemaQualifiedUsesNamedArgs() {
	const auto ref = lakemon::ParseTableRef("dp_gold.deal_chat_bridge");
	const std::string sql = lakemon::FlushInlinedDataCall("dlw", ref);
	Expect(sql == "CALL ducklake_flush_inlined_data('dlw', table_name => 'deal_chat_bridge', "
	              "schema_name => 'dp_gold')",
	       "schema.table flush uses named table_name and schema_name");
	Expect(!Contains(sql, "('dlw', 'deal_chat_bridge'"),
	       "flush must not emit ducklake_flush_inlined_data(VARCHAR, VARCHAR)");
	Expect(!Contains(sql, "schema =>"), "flush uses schema_name, not schema =>");
}

static void TestFlushCatalogOnlyOmitsTable() {
	lakemon::TableRef ref;
	Expect(ref.table.empty(), "default TableRef has no table");
	Expect(ref.schema == "main", "default TableRef schema is main");
	Expect(lakemon::FlushInlinedDataCall("dlw", ref) == "CALL ducklake_flush_inlined_data('dlw')",
	       "catalog-only flush is a single positional catalog argument");
}

static void TestFlushMainAndEmptySchemaOmitSchemaName() {
	const auto bare = lakemon::ParseTableRef("events");
	Expect(lakemon::FlushInlinedDataCall("dlw", bare) ==
	           "CALL ducklake_flush_inlined_data('dlw', table_name => 'events')",
	       "unqualified table uses named table_name and omits main schema");

	const auto main_qualified = lakemon::ParseTableRef("main.events");
	Expect(lakemon::FlushInlinedDataCall("dlw", main_qualified) ==
	           "CALL ducklake_flush_inlined_data('dlw', table_name => 'events')",
	       "schema main is omitted from flush");

	lakemon::TableRef empty_schema;
	empty_schema.schema.clear();
	empty_schema.table = "events";
	Expect(lakemon::FlushInlinedDataCall("dlw", empty_schema) ==
	           "CALL ducklake_flush_inlined_data('dlw', table_name => 'events')",
	       "empty schema is omitted from flush");
}

static void TestRewriteMergeKeepPositionalTableAndSchema() {
	const auto ref = lakemon::ParseTableRef("dp_gold.deal_chat_bridge");
	Expect(lakemon::TableArg(ref) == ", 'deal_chat_bridge'",
	       "rewrite/merge keep a positional table VARCHAR");
	Expect(lakemon::SchemaNamed(ref) == ", schema => 'dp_gold'",
	       "rewrite/merge keep schema => for non-main schemas");
	Expect(lakemon::SchemaNamed(lakemon::ParseTableRef("events")).empty(),
	       "main schema is omitted for rewrite/merge");
	Expect(lakemon::RewriteDataFilesCall("dlw", ref, 0.05) ==
	           "CALL ducklake_rewrite_data_files('dlw', 'deal_chat_bridge', schema => 'dp_gold', "
	           "delete_threshold => 0.05)",
	       "rewrite CALL keeps positional table plus data-driven threshold");
}

int main() {
	TestFlushSchemaQualifiedUsesNamedArgs();
	TestFlushCatalogOnlyOmitsTable();
	TestFlushMainAndEmptySchemaOmitSchemaName();
	TestRewriteMergeKeepPositionalTableAndSchema();
	if (TestFailures()) {
		std::cerr << TestFailures() << " failure(s)" << std::endl;
		return 1;
	}
	std::cout << "sql tests ok" << std::endl;
	return 0;
}
