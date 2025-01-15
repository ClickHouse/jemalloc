#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/clickhouse.h"

__thread JEMALLOC_TLS_MODEL je_clickhouse_tls_t je_clickhouse_tls = {0};
atomic_zd_t je_clickhouse_resident_bytes = {0};
atomic_zd_t je_clickhouse_active_bytes = {0};
