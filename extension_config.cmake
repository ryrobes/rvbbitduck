# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo. Static linking makes LOAD rvbbit work in the
# duckdb binary this build produces. The loadable .so is still built.
duckdb_extension_load(rvbbit
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)
duckdb_extension_statically_link(rvbbit)
