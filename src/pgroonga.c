#include "pgroonga.h"

#include "pgrn-compatible.h"

#include "pgrn-alias.h"
#include "pgrn-auto-close.h"
#include "pgrn-command-escape-value.h"
#include "pgrn-convert.h"
#include "pgrn-crash-safer-statuses.h"
#include "pgrn-create.h"
#include "pgrn-ctid.h"
#include "pgrn-file.h"
#include "pgrn-full-text-search-condition.h"
#include "pgrn-global.h"
#include "pgrn-groonga.h"
#include "pgrn-groonga-tuple-is-alive.h"
#include "pgrn-highlight-html.h"
#include "pgrn-index-status.h"
#include "pgrn-jsonb.h"
#include "pgrn-keywords.h"
#include "pgrn-match-positions-byte.h"
#include "pgrn-match-positions-character.h"
#include "pgrn-normalize.h"
#include "pgrn-options.h"
#include "pgrn-pg.h"
#include "pgrn-portable.h"
#include "pgrn-query-expand.h"
#include "pgrn-query-extract-keywords.h"
#include "pgrn-row-level-security.h"
#include "pgrn-search.h"
#include "pgrn-sequential-search.h"
#include "pgrn-string.h"
#include "pgrn-tokenize.h"
#include "pgrn-value.h"
#include "pgrn-variables.h"
#include "pgrn-wal.h"
#include "pgrn-writable.h"

#include <access/amapi.h>
#include <access/reloptions.h>
#include <access/relscan.h>
#ifdef PGRN_SUPPORT_TABLEAM
#	include <access/tableam.h>
#endif
#include <catalog/catalog.h>
#include <catalog/index.h>
#include <catalog/pg_type.h>
#ifdef PGRN_INDEX_AM_ROUTINE_HAVE_AM_PARALLEL_VACUUM_OPTIONS
#	include <commands/vacuum.h>
#endif
#include <mb/pg_wchar.h>
#include <miscadmin.h>
#include <nodes/nodeFuncs.h>
#ifdef PGRN_HAVE_OPTIMIZER_H
#	include <optimizer/optimizer.h>
#else
#	include <optimizer/clauses.h>
#	include <optimizer/cost.h>
#endif
#include <pgstat.h>
#include <storage/bufmgr.h>
#include <storage/ipc.h>
#include <storage/latch.h>
#include <utils/array.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/selfuncs.h>
#include <utils/snapmgr.h>
#include <utils/timestamp.h>
#include <utils/typcache.h>

#include <lib/ilist.h>

#include <groonga.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>

PG_MODULE_MAGIC;

static bool PGrnInitialized = false;
bool PGrnGroongaInitialized = false;
static bool PGrnCrashSaferInitialized = false;

typedef struct PGrnBuildStateData
{
	grn_obj	*sourcesTable;
	grn_obj	*sourcesCtidColumn;
	double nIndexedTuples;
	bool needMaxRecordSizeUpdate;
	uint32_t maxRecordSize;
	MemoryContext memoryContext;
} PGrnBuildStateData;

typedef PGrnBuildStateData *PGrnBuildState;

typedef struct PGrnPrimaryKeyColumn
{
	slist_node node;
	AttrNumber number;
	Oid type;
	grn_id domain;
	unsigned char flags;
	grn_obj *column;
} PGrnPrimaryKeyColumn;

typedef struct PGrnScanOpaqueData
{
	Relation index;

	MemoryContext memoryContext;

	Oid dataTableID;
	grn_obj *sourcesTable;
	grn_obj *sourcesCtidColumn;
	grn_obj *ctidResolveTable;
	grn_obj minBorderValue;
	grn_obj maxBorderValue;
	grn_obj *searched;
	grn_obj *sorted;
	grn_obj *targetTable;
	grn_obj *indexCursor;
	grn_table_cursor *tableCursor;
	grn_obj *ctidAccessor;
	grn_obj *scoreAccessor;
	grn_id currentID;

	grn_obj canReturns;

	dlist_node node;
	slist_head primaryKeyColumns;
	grn_obj *scoreTargetRecords;
} PGrnScanOpaqueData;

typedef PGrnScanOpaqueData *PGrnScanOpaque;

typedef struct PGrnPrefixRKSequentialSearchData
{
	grn_obj *table;
	grn_obj *key;
	grn_obj *resultTable;
} PGrnPrefixRKSequentialSearchData;

typedef struct PGrnParallelScanDescData {
	slock_t mutex;
	bool scanning;
} PGrnParallelScanDescData;
typedef PGrnParallelScanDescData *PGrnParallelScanDesc;

static bool PGrnParallelScanAcquire(IndexScanDesc scan);

static dlist_head PGrnScanOpaques = DLIST_STATIC_INIT(PGrnScanOpaques);
static unsigned int PGrnNScanOpaques = 0;

extern PGDLLEXPORT void _PG_init(void);

PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_score);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_score_row);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_score_ctid);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_table_name);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_command);

/*
 * Naming conversions:
 *
 *   * pgroonga_#{operation}_#{type}(operand1 #{type}, operand2 #{type})
 *     * e.g.: pgroonga_match_text(target text, term text)
 *     * true when #{operand1} #{operation} #{operand2} is true,
 *       false otherwise.
 *     * e.g.: "PGroonga is great" match "great" -> true
 *
 *   * pgroonga_#{operation}_#{type}_array(operands1 #{type}[],
 *                                         operand2 #{type})
 *     * e.g.: pgroonga_match_text_array(targets text[], term text)
 *     * true when #{one of operands1} #{operation} #{operand2} is true,
 *       false otherwise.
 *     * e.g.: ["PGroonga is great", "PostgreSQL is great"] match "PGroonga"
 *       -> true
 *
 *   * pgroonga_#{operation}_in_#{type}(operand1 #{type}, operands2 #{type}[])
 *     * e.g.: pgroonga_match_in_text(target text, terms text[])
 *     * true when #{operand1} #{operation} #{one of operands2} is true,
 *       false otherwise.
 *     * e.g.: "PGroonga is great" match ["PGroonga", "PostgreSQL"]
 *       -> true
 *
 *   * pgroonga_#{operation}_in_#{type}_array(operands1 #{type}[],
 *                                            operands2 #{type}[])
 *     * e.g.: pgroonga_match_in_text_array(targets1 text[], terms2 text[])
 *     * true when #{one of operands1} #{operation} #{one of operands2} is true,
 *       false otherwise.
 *     * e.g.: ["PGroonga is great", "PostgreSQL is great"] match
 *       ["Groonga", "PostgreSQL"] -> true
 */

PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_term_text);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_term_text_array);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_term_varchar);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_term_varchar_array);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_query_text);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_query_text_array);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_query_varchar);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_regexp_text);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_regexp_varchar);

/* v2 */
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_text);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_text_condition);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_text_condition_with_scorers);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_text_array);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_text_array_condition);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_text_array_condition_with_scorers);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_varchar);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_varchar_condition);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_varchar_condition_with_scorers);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_contain_varchar_array);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_query_text);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_query_text_condition);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_query_text_condition_with_scorers);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_query_text_array);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_query_text_array_condition);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_query_text_array_condition_with_scorers);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_query_varchar);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_query_varchar_condition);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_query_varchar_condition_with_scorers);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_similar_text);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_similar_text_array);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_similar_varchar);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_script_text);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_script_text_array);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_script_varchar);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_prefix_text);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_prefix_text_array);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_prefix_varchar);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_prefix_varchar_array);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_prefix_contain_text_array);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_prefix_rk_text);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_prefix_rk_text_array);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_prefix_rk_varchar);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_prefix_rk_varchar_array);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_prefix_rk_contain_text_array);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_in_text);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_contain_text);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_in_text_array);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_match_in_varchar);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_query_in_text);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_query_contain_text);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_query_in_text_array);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_query_in_varchar);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_prefix_in_text);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_not_prefix_in_text);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_prefix_in_text_array);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_prefix_in_varchar);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_prefix_in_varchar_array);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_prefix_rk_in_text);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_prefix_rk_in_text_array);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_prefix_rk_in_varchar);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_prefix_rk_in_varchar_array);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_regexp_text);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_regexp_varchar);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_regexp_in_text);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_regexp_in_varchar);

PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_insert);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_beginscan);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_gettuple);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_getbitmap);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_rescan);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_endscan);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_build);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_buildempty);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_bulkdelete);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_vacuumcleanup);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_canreturn);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_costestimate);
PGDLLEXPORT PG_FUNCTION_INFO_V1(pgroonga_handler);

static grn_ctx *ctx = NULL;
static struct PGrnBuffers *buffers = &PGrnBuffers;
static PGrnSequentialSearchData sequentialSearchData;
static PGrnPrefixRKSequentialSearchData prefixRKSequentialSearchData;

static uint32_t
PGrnGetThreadLimit(void *data)
{
	return 1;
}

static grn_encoding
PGrnGetEncoding(void)
{
	int	enc = GetDatabaseEncoding();

	switch (enc)
	{
	case PG_SQL_ASCII:
	case PG_UTF8:
		return GRN_ENC_UTF8;
	case PG_EUC_JP:
	case PG_EUC_JIS_2004:
		return GRN_ENC_EUC_JP;
	case PG_LATIN1:
	case PG_WIN1252:
		return GRN_ENC_LATIN1;
	case PG_KOI8R:
		return GRN_ENC_KOI8R;
	case PG_SJIS:
	case PG_SHIFT_JIS_2004:
		return GRN_ENC_SJIS;
	default:
		elog(WARNING,
			 "pgroonga: use default encoding instead of '%s'",
			 GetDatabaseEncodingName());
		return GRN_ENC_DEFAULT;
	}
}

static void PGrnScanOpaqueFin(PGrnScanOpaque so);

static void
PGrnReleaseScanOpaques(ResourceReleasePhase phase,
					   bool isCommit,
					   bool isTopLevel,
					   void *arg)
{
	const char *tag = "pgroonga: [release][scan-opaques]";
	const char *top_level_tag = isTopLevel ? "[top-level]" : "";
	dlist_mutable_iter iter;

	switch (phase)
	{
	case RESOURCE_RELEASE_BEFORE_LOCKS:
		GRN_LOG(ctx, GRN_LOG_DEBUG,
				"%s%s%s %u: skip",
				tag,
				top_level_tag,
				"[before-locks]",
				PGrnNScanOpaques);
		return;
	case RESOURCE_RELEASE_LOCKS:
		GRN_LOG(ctx, GRN_LOG_DEBUG,
				"%s%s%s %u: skip",
				tag,
				top_level_tag,
				"[locks]",
				PGrnNScanOpaques);
		return;
	case RESOURCE_RELEASE_AFTER_LOCKS:
		GRN_LOG(ctx, GRN_LOG_DEBUG,
				"%s%s%s[start] %u",
				tag,
				top_level_tag,
				"[after-locks]",
				PGrnNScanOpaques);
		if (!isTopLevel)
			return;
		break;
	}

	dlist_foreach_modify(iter, &PGrnScanOpaques)
	{
		PGrnScanOpaque so;
		so = dlist_container(PGrnScanOpaqueData, node, iter.cur);
		PGrnScanOpaqueFin(so);
	}

	GRN_LOG(ctx, GRN_LOG_DEBUG,
			"%s%s%s[end] %u",
			tag,
			top_level_tag,
			"[after-locks]",
			PGrnNScanOpaques);
}

static void
PGrnInitializeGroongaFunctions(void)
{
	PGrnInitializeGroongaTupleIsAlive();
}

static void
PGrnFinalizeSequentialSearchData(void)
{
	PGrnSequentialSearchDataFinalize(&sequentialSearchData);
}

static void
PGrnFinalizePrefixRKSequentialSearchData(void)
{
	grn_obj_close(ctx, prefixRKSequentialSearchData.resultTable);
	grn_obj_close(ctx, prefixRKSequentialSearchData.key);
	grn_obj_close(ctx, prefixRKSequentialSearchData.table);
}

static void
PGrnBeforeShmemExit(int code, Datum arg)
{
	const char *tag = "pgroonga: [exit]";

	if (ctx)
	{
		grn_obj *db;

		db = grn_ctx_db(ctx);
		GRN_LOG(ctx, GRN_LOG_DEBUG,
				"%s[db][%s]",
				tag,
				db ? "opened" : "not-opened");
		if (db)
		{
			GRN_LOG(ctx, GRN_LOG_DEBUG, "%s[finalize][auto-close]", tag);
			PGrnFinalizeAutoClose();

			GRN_LOG(ctx, GRN_LOG_DEBUG,
					"%s[finalize][normalize]", tag);
			PGrnFinalizeNormalize();

			GRN_LOG(ctx, GRN_LOG_DEBUG,
					"%s[finalize][tokenize]", tag);
			PGrnFinalizeTokenize();

			GRN_LOG(ctx, GRN_LOG_DEBUG,
					"%s[finalize][query-extract-keywords]", tag);
			PGrnFinalizeQueryExtractKeywords();

			GRN_LOG(ctx, GRN_LOG_DEBUG,
					"%s[finalize][match-positions-byte]", tag);
			PGrnFinalizeMatchPositionsByte();
			GRN_LOG(ctx, GRN_LOG_DEBUG,
					"%s[finalize][match-positions-character]", tag);
			PGrnFinalizeMatchPositionsCharacter();

			GRN_LOG(ctx, GRN_LOG_DEBUG, "%s[finalize][highlight-html]", tag);
			PGrnFinalizeHighlightHTML();

			GRN_LOG(ctx, GRN_LOG_DEBUG, "%s[finalize][keywords]", tag);
			PGrnFinalizeKeywords();

			GRN_LOG(ctx, GRN_LOG_DEBUG, "%s[finalize][jsonb]", tag);
			PGrnFinalizeJSONB();

			GRN_LOG(ctx, GRN_LOG_DEBUG,
					"%s[finalize][sequential-search-data]", tag);
			PGrnFinalizeSequentialSearchData();
			GRN_LOG(ctx, GRN_LOG_DEBUG,
					"%s[finalize][prefix-rk-sequential-search-data]", tag);
			PGrnFinalizePrefixRKSequentialSearchData();

			GRN_LOG(ctx, GRN_LOG_DEBUG, "%s[finalize][options]", tag);
			PGrnFinalizeOptions();

			GRN_LOG(ctx, GRN_LOG_DEBUG, "%s[db][close]", tag);
			grn_obj_close(ctx, db);
		}

		if (PGrnCrashSaferInitialized)
		{
			pgrn_crash_safer_statuses_release(NULL,
											  MyDatabaseId,
											  MyDatabaseTableSpace);
			PGrnCrashSaferInitialized = false;
		}

		GRN_LOG(ctx, GRN_LOG_DEBUG, "%s[finalize][buffers]", tag);
		PGrnFinalizeBuffers();

		GRN_LOG(ctx, GRN_LOG_DEBUG, "%s[finalize][context]", tag);
		grn_ctx_fin(ctx);
	}

	GRN_LOG(ctx, GRN_LOG_DEBUG, "%s[finalize]", tag);
	grn_fin();

	PGrnGroongaInitialized = false;
	PGrnInitialized = false;
}

static void
PGrnInitializeSequentialSearchData(void)
{
	PGrnSequentialSearchDataInitialize(&sequentialSearchData);
}

static void
PGrnInitializePrefixRKSequentialSearchData(void)
{
	prefixRKSequentialSearchData.table =
		grn_table_create(ctx,
						 NULL, 0,
						 NULL,
						 GRN_OBJ_TABLE_PAT_KEY,
						 grn_ctx_at(ctx, GRN_DB_SHORT_TEXT),
						 NULL);

	prefixRKSequentialSearchData.key =
		grn_obj_column(ctx,
					   prefixRKSequentialSearchData.table,
					   GRN_COLUMN_NAME_KEY,
					   GRN_COLUMN_NAME_KEY_LEN);

	prefixRKSequentialSearchData.resultTable =
		grn_table_create(ctx,
						 NULL, 0,
						 NULL,
						 GRN_OBJ_TABLE_HASH_KEY | GRN_OBJ_WITH_SUBREC,
						 prefixRKSequentialSearchData.table,
						 NULL);
}

static void
PGrnInitializeDatabase(void)
{
	PGrnInitializeGroongaFunctions();

	PGrnInitializeAlias();

	PGrnInitializeIndexStatus();

	PGrnInitializeSequentialSearchData();
	PGrnInitializePrefixRKSequentialSearchData();

	PGrnInitializeJSONB();

	PGrnInitializeKeywords();

	PGrnInitializeHighlightHTML();

	PGrnInitializeMatchPositionsByte();
	PGrnInitializeMatchPositionsCharacter();

	PGrnInitializeQueryExpand();

	PGrnInitializeQueryExtractKeywords();

	PGrnInitializeTokenize();

	PGrnInitializeNormalize();

	PGrnInitializeAutoClose();
}

void
PGrnEnsureDatabase(void)
{
	char *databasePath;
	char path[MAXPGPATH];

	if (grn_ctx_db(ctx))
		return;

	if (!OidIsValid(MyDatabaseId))
		return;

	GRN_CTX_SET_ENCODING(ctx, PGrnGetEncoding());
	databasePath = GetDatabasePath(MyDatabaseId, MyDatabaseTableSpace);
	join_path_components(path,
						 databasePath,
						 PGrnDatabaseBasename);
	pfree(databasePath);

	if (grn_ctx_get_wal_role(ctx) == GRN_WAL_ROLE_SECONDARY)
	{
		HTAB *statuses;
		pid_t crashSaferPID;
		statuses = pgrn_crash_safer_statuses_get();
		crashSaferPID = pgrn_crash_safer_statuses_get_main_pid(statuses);
		if (crashSaferPID == 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pgroonga: "
							"pgroonga_crash_safer process doesn't exist: "
							"shared_preload_libraries may not include "
							"pgroonga_crash_safer")));
		}
		pgrn_crash_safer_statuses_use(statuses,
									  MyDatabaseId,
									  MyDatabaseTableSpace);
		PGrnCrashSaferInitialized = true;
		while (true)
		{
			int conditions;
			long timeout = 1000;

			if (pgrn_crash_safer_statuses_is_flushing(statuses,
													  MyDatabaseId,
													  MyDatabaseTableSpace))
			{
				break;
			}

			kill(crashSaferPID, SIGUSR1);

			conditions = WaitLatch(MyLatch,
								   WL_LATCH_SET |
								   WL_TIMEOUT |
								   PGRN_WL_EXIT_ON_PM_DEATH,
								   timeout,
								   PG_WAIT_EXTENSION);
			if (conditions & WL_LATCH_SET)
			{
				ResetLatch(MyLatch);
			}
			CHECK_FOR_INTERRUPTS();
		}
	}

	if (pgrn_file_exist(path))
	{
		grn_db_open(ctx, path);
		PGrnCheck("failed to open database: <%s>", path);
	}
	else
	{
		grn_db_create(ctx, path, NULL);
		PGrnCheck("failed to create database: <%s>", path);
	}

	PGrnInitializeDatabase();
}

void
_PG_init(void)
{
	grn_rc rc;

	if (PGrnInitialized)
		PGrnCheckRC(GRN_UNKNOWN_ERROR,
					"already tried to initialize and failed");

	PGrnInitialized = true;
	PGrnGroongaInitialized = false;

	PGrnInitializeVariables();

	grn_thread_set_get_limit_func(PGrnGetThreadLimit, NULL);

	grn_default_logger_set_flags(grn_default_logger_get_flags() | GRN_LOG_PID);

	rc = grn_init();
	PGrnCheckRC(rc,
				"failed to initialize Groonga");

	grn_set_segv_handler();

	before_shmem_exit(PGrnBeforeShmemExit, 0);

	RegisterResourceReleaseCallback(PGrnReleaseScanOpaques, NULL);

	grn_set_default_match_escalation_threshold(PGrnMatchEscalationThreshold);

	rc = grn_ctx_init(&PGrnContext, 0);
	PGrnCheckRC(rc,
				"failed to initialize Groonga context");

	PGrnGroongaInitialized = true;

	ctx = &PGrnContext;

	GRN_LOG(ctx, GRN_LOG_NOTICE, "pgroonga: initialize: <%s>", PGRN_VERSION);

	PGrnInitializeBuffers();

	PGrnInitializeGroongaInformation();

	PGrnVariablesApplyInitialValues();

	PGrnInitializeOptions();

	PGrnEnsureDatabase();
}

static grn_id
PGrnGetType(Relation index, AttrNumber n, unsigned char *flags)
{
	TupleDesc desc = RelationGetDescr(index);
	Form_pg_attribute attr;

	attr = TupleDescAttr(desc, n);
	return PGrnPGTypeToGrnType(attr->atttypid, flags);
}

static Datum PGrnConvertToDatum(grn_obj *value, Oid typeID);

static Datum
PGrnConvertToDatumArrayType(grn_obj *vector, Oid typeID)
{
	const char *tag = "[vector][groonga->postgresql-type]";
	Oid elementTypeID;
	int elementLength;
	bool elementByValue;
	char elementAlign;
	int i, n;
	Datum *values;

	switch (typeID)
	{
	case INT4ARRAYOID:
		elementTypeID = INT4OID;
		elementLength = 4;
		elementByValue = true;
		elementAlign = 'i';
		break;
	case VARCHARARRAYOID:
		elementTypeID = VARCHAROID;
		elementLength = -1;
		elementByValue = false;
		elementAlign = 'i';
		break;
	case TEXTARRAYOID:
		elementTypeID = TEXTOID;
		elementLength = -1;
		elementByValue = false;
		elementAlign = 'i';
		break;
	default:
		PGrnCheckRC(GRN_FUNCTION_NOT_IMPLEMENTED,
					"%s unsupported datum array type: %u",
					tag,
					typeID);
		break;
	}

	n = grn_vector_size(ctx, vector);
	if (n == 0)
		PG_RETURN_POINTER(construct_empty_array(elementTypeID));

	values = palloc(sizeof(Datum) * n);
	if (vector->header.domain == GRN_VECTOR)
	{
		for (i = 0; i < n; i++)
		{
			const char *element;
			unsigned int elementSize;
			text *value;

			elementSize = grn_vector_get_element(ctx, vector, i,
												 &element,
												 NULL,
												 NULL);
			value = cstring_to_text_with_len(element, elementSize);
			values[i] = PointerGetDatum(value);
		}
	}
	else
	{
		unsigned int elementSize = grn_uvector_element_size(ctx, vector);
		grn_obj element;
		GRN_VALUE_FIX_SIZE_INIT(&element,
								GRN_OBJ_DO_SHALLOW_COPY,
								vector->header.domain);
		for (i = 0; i < n; i++)
		{
			const char *rawElement = GRN_BULK_HEAD(vector) + (elementSize * i);
			GRN_TEXT_SET(ctx, &element, rawElement, elementSize);
			values[i] = PGrnConvertToDatum(&element, elementTypeID);
		}
		GRN_OBJ_FIN(ctx, &element);
	}

	{
		int	dims[1];
		int	lbs[1];

		dims[0] = n;
		lbs[0] = 1;
		PG_RETURN_POINTER(construct_md_array(values, NULL,
											 1, dims, lbs,
											 elementTypeID,
											 elementLength,
											 elementByValue,
											 elementAlign));
	}
}

static Datum
PGrnConvertToDatum(grn_obj *value, Oid typeID)
{
	const char *tag = "[data][groonga->postgresql]";

	switch (typeID)
	{
	case BOOLOID:
		PG_RETURN_BOOL(GRN_BOOL_VALUE(value));
		break;
	case INT2OID:
		PG_RETURN_INT16(GRN_INT16_VALUE(value));
		break;
	case INT4OID:
		PG_RETURN_INT32(GRN_INT32_VALUE(value));
		break;
	case INT8OID:
		PG_RETURN_INT64(GRN_INT64_VALUE(value));
		break;
	case FLOAT4OID:
		PG_RETURN_FLOAT4(GRN_FLOAT_VALUE(value));
		break;
	case FLOAT8OID:
		PG_RETURN_FLOAT8(GRN_FLOAT_VALUE(value));
		break;
	case TIMESTAMPOID:
	case TIMESTAMPTZOID:
	{
		int64 grnTime;
		int64 sec;
		int64 usec;
		TimestampTz timestamp;

		grnTime = GRN_TIME_VALUE(value);
		GRN_TIME_UNPACK(grnTime, sec, usec);
		if (typeID == TIMESTAMPOID)
		{
			timestamp = PGrnPGLocalTimeToTimestamp(sec);
		}
		else
		{
			/* TODO: Support not localtime time zone. */
			pg_time_t unixTimeUTC = sec;
			timestamp = time_t_to_timestamptz(unixTimeUTC);
		}
#ifdef HAVE_INT64_TIMESTAMP
		timestamp += usec;
#else
		timestamp += ((double) usec) / USECS_PER_SEC;
#endif
		if (typeID == TIMESTAMPOID)
			PG_RETURN_TIMESTAMP(timestamp);
		else
			PG_RETURN_TIMESTAMPTZ(timestamp);
		break;
	}
	case TEXTOID:
	case XMLOID:
	{
		text *text = cstring_to_text_with_len(GRN_TEXT_VALUE(value),
											  GRN_TEXT_LEN(value));
		PG_RETURN_TEXT_P(text);
		break;
	}
	case VARCHAROID:
	{
		text *text = cstring_to_text_with_len(GRN_TEXT_VALUE(value),
											  GRN_TEXT_LEN(value));
		PG_RETURN_VARCHAR_P((VarChar *) text);
		break;
	}
#ifdef NOT_USED
	case POINTOID:
		/* GRN_DB_TOKYO_GEO_POINT or GRN_DB_WGS84_GEO_POINT; */
		break;
#endif
	case INT4ARRAYOID:
	case VARCHARARRAYOID:
	case TEXTARRAYOID:
		return PGrnConvertToDatumArrayType(value, typeID);
		break;
	default:
		PGrnCheckRC(GRN_FUNCTION_NOT_IMPLEMENTED,
					"%s unsupported datum type: %u",
					tag,
					typeID);
		break;
	}

	return 0;
}

static bool
PGrnIsQueryStrategyIndex(Relation index, int nthAttribute)
{
	Oid strategyOID;
	Oid leftType;
	Oid rightType;

	leftType = index->rd_opcintype[nthAttribute];

	switch (leftType)
	{
	case VARCHARARRAYOID:
		rightType = VARCHAROID;
		break;
	case TEXTARRAYOID:
		rightType = TEXTOID;
		break;
	default:
		rightType = leftType;
		break;
	}

	strategyOID = get_opfamily_member(index->rd_opfamily[nthAttribute],
									  leftType,
									  rightType,
									  PGrnQueryStrategyNumber);
	if (OidIsValid(strategyOID))
		return true;

	strategyOID = get_opfamily_member(index->rd_opfamily[nthAttribute],
									  leftType,
									  rightType,
									  PGrnQueryStrategyV2Number);
	if (OidIsValid(strategyOID))
		return true;

	strategyOID = get_opfamily_member(index->rd_opfamily[nthAttribute],
									  leftType,
									  rightType,
									  PGrnQueryStrategyV2DeprecatedNumber);
	if (OidIsValid(strategyOID))
		return true;

	return false;
}

static bool
PGrnIsQueryInStrategyIndex(Relation index, int nthAttribute)
{
	Oid strategyOID;
	Oid leftType;
	Oid rightType;

	leftType = index->rd_opcintype[nthAttribute];

	switch (leftType)
	{
	case VARCHAROID:
		rightType = VARCHARARRAYOID;
		break;
	case TEXTOID:
		rightType = TEXTARRAYOID;
		break;
	default:
		rightType = leftType;
		break;
	}

	strategyOID = get_opfamily_member(index->rd_opfamily[nthAttribute],
									  leftType,
									  rightType,
									  PGrnQueryInStrategyV2Number);
	if (OidIsValid(strategyOID))
		return true;

	strategyOID = get_opfamily_member(index->rd_opfamily[nthAttribute],
									  leftType,
									  rightType,
									  PGrnQueryInStrategyV2DeprecatedNumber);
	if (OidIsValid(strategyOID))
		return true;

	strategyOID = get_opfamily_member(index->rd_opfamily[nthAttribute],
									  leftType,
									  rightType,
									  PGrnQueryInStrategyV2Deprecated2Number);
	if (OidIsValid(strategyOID))
		return true;

	return false;
}

static bool
PGrnIsScriptStrategyIndex(Relation index, int nthAttribute)
{
	Oid strategyOID;
	Oid leftType;
	Oid rightType;

	leftType = index->rd_opcintype[nthAttribute];

	switch (leftType)
	{
	case VARCHARARRAYOID:
		rightType = VARCHAROID;
		break;
	case TEXTARRAYOID:
		rightType = TEXTOID;
		break;
	default:
		rightType = leftType;
		break;
	}

	strategyOID = get_opfamily_member(index->rd_opfamily[nthAttribute],
									  leftType,
									  rightType,
									  PGrnScriptStrategyV2Number);
	if (OidIsValid(strategyOID))
		return true;

	return false;
}

static bool
PGrnIsForFullTextSearchIndex(Relation index, int nthAttribute)
{
	if (PGrnIsQueryStrategyIndex(index, nthAttribute))
		return true;

	if (PGrnIsQueryInStrategyIndex(index, nthAttribute))
		return true;

	if (PGrnIsScriptStrategyIndex(index, nthAttribute))
		return true;

	return false;
}

static bool
PGrnIsForRegexpSearchIndex(Relation index, int nthAttribute)
{
	Oid regexpStrategyOID;
	Oid leftType;
	Oid rightType;

	leftType = index->rd_opcintype[nthAttribute];
	rightType = leftType;
	regexpStrategyOID = get_opfamily_member(index->rd_opfamily[nthAttribute],
											leftType,
											rightType,
											PGrnRegexpStrategyNumber);
	return OidIsValid(regexpStrategyOID);
}

static bool
PGrnIsForPrefixSearchIndex(Relation index, int nthAttribute)
{
	Oid prefixStrategyOID;
	Oid prefixInStrategyOID;
	Oid leftType;
	Oid rightType;

	leftType = index->rd_opcintype[nthAttribute];

	switch (leftType)
	{
	case VARCHARARRAYOID:
		rightType = VARCHAROID;
		break;
	case TEXTARRAYOID:
		rightType = TEXTOID;
		break;
	default:
		rightType = leftType;
		break;
	}

	prefixStrategyOID = get_opfamily_member(index->rd_opfamily[nthAttribute],
											leftType,
											rightType,
											PGrnPrefixStrategyV2Number);
	if (OidIsValid(prefixStrategyOID))
		return true;

	prefixStrategyOID = get_opfamily_member(index->rd_opfamily[nthAttribute],
											leftType,
											rightType,
											PGrnPrefixStrategyV2DeprecatedNumber);
	if (OidIsValid(prefixStrategyOID))
		return true;

	prefixInStrategyOID =
		get_opfamily_member(index->rd_opfamily[nthAttribute],
							leftType,
							rightType,
							PGrnPrefixInStrategyV2Number);
	if (OidIsValid(prefixInStrategyOID))
		return true;

	return false;
}

static void
PGrnCreateCheckType(PGrnCreateData *data)
{
	const char *tag = "[create][type][check]";
	TupleDesc desc = RelationGetDescr(data->index);
	Form_pg_attribute attr;
	int32 maxLength;

	attr = TupleDescAttr(desc, data->i);
	if (data->forFullTextSearch)
		return;
	if (data->forRegexpSearch)
		return;

	switch (attr->atttypid)
	{
	case VARCHAROID:
	case VARCHARARRAYOID:
		maxLength = type_maximum_size(VARCHAROID, attr->atttypmod);
		if (maxLength > 4096)
		{
			PGrnCheckRC(GRN_FUNCTION_NOT_IMPLEMENTED,
						"%s 4097bytes over size varchar isn't supported: %d",
						tag,
						maxLength);
		}
		break;
	default:
		break;
	}
}

static void PGrnRemoveUnusedTable(Oid relationFileNodeID);

static void
PGrnCreate(PGrnCreateData *data)
{
	/*
	 * If CREATE TABLE and TRUNCATE TABLE exist in the same
	 * transaction, PostgreSQL reuses the created table instead of
	 * creating a new table. We need to remove existing Groonga tables
	 * and columns in the case.
	 */
	{
		char sourcesTableName[GRN_TABLE_MAX_KEY_SIZE];
		grn_obj *sourcesTable;

		snprintf(sourcesTableName, sizeof(sourcesTableName),
				 PGrnSourcesTableNameFormat, data->relNode);
		sourcesTable = grn_ctx_get(ctx, sourcesTableName, -1);
		if (sourcesTable) {
			grn_obj_unlink(ctx, sourcesTable);
			PGrnRemoveUnusedTable(data->relNode);
		}
	}

	PGrnCreateSourcesTable(data);

	for (data->i = 0; data->i < data->desc->natts; data->i++)
	{
#ifdef PGRN_INDEX_AM_ROUTINE_HAVE_AM_CAN_INCLUDE
		bool forInclude =
			(data->i >=
			 IndexRelationGetNumberOfKeyAttributes(data->index));
#else
		bool forInclude = false;
#endif
		Form_pg_attribute attribute = TupleDescAttr(data->desc, data->i);
		if (PGrnAttributeIsJSONB(attribute->atttypid))
		{
			if (forInclude)
				continue;
			data->forFullTextSearch = false;
			data->forRegexpSearch = false;
			data->forPrefixSearch = false;
			PGrnJSONBCreate(data);
		}
		else
		{
			data->forFullTextSearch =
				PGrnIsForFullTextSearchIndex(data->index, data->i);
			data->forRegexpSearch =
				PGrnIsForRegexpSearchIndex(data->index, data->i);
			data->forPrefixSearch =
				PGrnIsForPrefixSearchIndex(data->index, data->i);
			data->attributeTypeID =
				PGrnGetType(data->index, data->i, &(data->attributeFlags));
			PGrnCreateCheckType(data);
			if (!forInclude || data->forPrefixSearch)
				PGrnCreateLexicon(data);
			PGrnCreateDataColumn(data);
			if (forInclude)
				continue;
			PGrnCreateIndexColumn(data);
		}
	}
}

static void
PGrnSetSources(Relation index, grn_obj *sourcesTable)
{
	TupleDesc desc;
	unsigned int i;

	desc = RelationGetDescr(index);
	for (i = 0; i < desc->natts; i++)
	{
#ifdef PGRN_INDEX_AM_ROUTINE_HAVE_AM_CAN_INCLUDE
		bool forInclude =
			(i >= IndexRelationGetNumberOfKeyAttributes(index));
#else
		bool forInclude = false;
#endif
		Form_pg_attribute attribute = TupleDescAttr(desc, i);
		NameData *name = &(attribute->attname);
		grn_obj *source;
		grn_obj *indexColumn;

		if (forInclude)
			continue;

		if (PGrnAttributeIsJSONB(attribute->atttypid))
		{
			indexColumn = PGrnJSONBSetSource(index, i);
		}
		else
		{
			indexColumn = PGrnLookupIndexColumn(index, i, ERROR);
		}

		source = PGrnLookupColumn(sourcesTable, name->data, ERROR);
		PGrnIndexColumnSetSource(index, indexColumn, source);
		grn_obj_unlink(ctx, source);
		grn_obj_unlink(ctx, indexColumn);
	}
}

static double
PGrnCollectScoreGetScore(Relation table,
						 PGrnScanOpaque so,
						 grn_id recordID)
{
	double score = 0.0;
	grn_id id;

	id = grn_table_get(ctx, so->searched, &recordID, sizeof(grn_id));
	if (id == GRN_ID_NIL)
		return 0.0;

	GRN_BULK_REWIND(&(buffers->ctid));
	grn_obj_get_value(ctx, so->ctidAccessor, id, &(buffers->ctid));
	if (GRN_BULK_VSIZE(&(buffers->ctid)) == 0)
		return 0.0;

	{
		ItemPointerData ctid;
		ctid = PGrnCtidUnpack(GRN_UINT64_VALUE(&(buffers->ctid)));
		if (!PGrnCtidIsAlive(table, &ctid))
			return 0.0;
	}

	GRN_BULK_REWIND(&(buffers->score));
	grn_obj_get_value(ctx, so->scoreAccessor, id, &(buffers->score));
	if (buffers->score.header.domain == GRN_DB_FLOAT)
	{
		score = GRN_FLOAT_VALUE(&(buffers->score));
	}
	else
	{
		score = GRN_INT32_VALUE(&(buffers->score));
	}

	return score;
}

static double
PGrnCollectScoreOneColumnPrimaryKey(Relation table,
									HeapTuple tuple,
									PGrnScanOpaque so)
{
	double score = 0.0;
	TupleDesc desc;
	PGrnPrimaryKeyColumn *primaryKeyColumn;
	grn_index_datum indexDatum;
	grn_obj *lexicon;
	grn_id termID;
	grn_ii_cursor *iiCursor;
	int iiNElements = 2;
	grn_posting *posting;

	desc = RelationGetDescr(table);
	primaryKeyColumn = slist_container(PGrnPrimaryKeyColumn,
									   node,
									   so->primaryKeyColumns.head.next);

	{
		unsigned int nIndexData;

		nIndexData = grn_column_find_index_data(ctx,
												primaryKeyColumn->column,
												GRN_OP_EQUAL,
												&indexDatum,
												1);
		if (nIndexData == 0)
			return 0.0;
	}

	lexicon = grn_ctx_at(ctx, indexDatum.index->header.domain);
	if (!lexicon)
		return 0.0;

	{
		bool isNULL;
		Datum primaryKeyValue;

		grn_obj_reinit(ctx,
					   &(buffers->general),
					   primaryKeyColumn->domain,
					   primaryKeyColumn->flags);
		primaryKeyValue = heap_getattr(tuple,
									   primaryKeyColumn->number,
									   desc,
									   &isNULL);
		PGrnConvertFromData(primaryKeyValue,
							primaryKeyColumn->type,
							&(buffers->general));
	}
	termID = grn_table_get(ctx,
						   lexicon,
						   GRN_BULK_HEAD(&(buffers->general)),
						   GRN_BULK_VSIZE(&(buffers->general)));
	if (termID == GRN_ID_NIL)
		return 0.0;

	iiCursor = grn_ii_cursor_open(ctx,
								  (grn_ii *)(indexDatum.index),
								  termID,
								  GRN_ID_NIL,
								  GRN_ID_NIL,
								  iiNElements,
								  0);
	if (!iiCursor)
		return 0.0;

	while ((posting = grn_ii_cursor_next(ctx, iiCursor)))
	{
		score += PGrnCollectScoreGetScore(table, so, posting->rid);
	}
	grn_ii_cursor_close(ctx, iiCursor);

	return score;
}

static double
PGrnCollectScoreMultiColumnPrimaryKey(Relation table,
									  HeapTuple tuple,
									  PGrnScanOpaque so)
{
	double score = 0.0;
	TupleDesc desc;
	grn_obj *expression;
	grn_obj *variable;
	slist_iter iter;
	unsigned int nPrimaryKeyColumns = 0;

	desc = RelationGetDescr(table);

	if (!so->scoreTargetRecords)
	{
		so->scoreTargetRecords =
			grn_table_create(ctx, NULL, 0, NULL,
							 GRN_OBJ_TABLE_HASH_KEY | GRN_OBJ_WITH_SUBREC,
							 so->sourcesTable, NULL);
	}

	GRN_EXPR_CREATE_FOR_QUERY(ctx, so->sourcesTable, expression, variable);

	slist_foreach(iter, &(so->primaryKeyColumns))
	{
		PGrnPrimaryKeyColumn *primaryKeyColumn;
		bool isNULL;
		Datum primaryKeyValue;

		primaryKeyColumn = slist_container(PGrnPrimaryKeyColumn, node, iter.cur);

		grn_obj_reinit(ctx,
					   &(buffers->general),
					   primaryKeyColumn->domain,
					   primaryKeyColumn->flags);

		primaryKeyValue = heap_getattr(tuple,
									   primaryKeyColumn->number,
									   desc,
									   &isNULL);
		PGrnConvertFromData(primaryKeyValue,
							primaryKeyColumn->type,
							&(buffers->general));

		grn_expr_append_obj(ctx, expression,
							primaryKeyColumn->column, GRN_OP_PUSH, 1);
		grn_expr_append_op(ctx, expression, GRN_OP_GET_VALUE, 1);
		grn_expr_append_const(ctx, expression,
							  &(buffers->general), GRN_OP_PUSH, 1);
		grn_expr_append_op(ctx, expression, GRN_OP_EQUAL, 2);

		if (nPrimaryKeyColumns > 0)
			grn_expr_append_op(ctx, expression, GRN_OP_AND, 2);
		nPrimaryKeyColumns++;
	}
	grn_table_select(ctx,
					 so->sourcesTable,
					 expression,
					 so->scoreTargetRecords,
					 GRN_OP_OR);
	grn_obj_close(ctx, expression);

	{
		grn_table_cursor *tableCursor;

		tableCursor = grn_table_cursor_open(ctx,
											so->scoreTargetRecords,
											NULL, 0, NULL, 0,
											0, -1, GRN_CURSOR_ASCENDING);
		while (grn_table_cursor_next(ctx, tableCursor) != GRN_ID_NIL)
		{
			void *key;
			grn_id recordID;

			grn_table_cursor_get_key(ctx, tableCursor, &key);
			recordID = *((grn_id *) key);
			grn_table_cursor_delete(ctx, tableCursor);

			score += PGrnCollectScoreGetScore(table, so, recordID);
		}
		grn_obj_unlink(ctx, tableCursor);
	}

	return score;
}

static bool
PGrnCollectScoreIsTarget(PGrnScanOpaque so, Oid tableOid)
{
	NameData soTableName;

	if (so->dataTableID != tableOid)
	{
		NameData tableName;
		GRN_LOG(ctx,
				GRN_LOG_DEBUG,
				"pgroonga: [score][target][no] different table: "
				"<%s>(%u) != <%s>(%u)",
				PGrnPGGetRelationNameByID(so->dataTableID, soTableName.data),
				so->dataTableID,
				PGrnPGGetRelationNameByID(tableOid, tableName.data),
				tableOid);
		return false;
	}

	if (!so->scoreAccessor)
	{
		GRN_LOG(ctx,
				GRN_LOG_DEBUG,
				"pgroonga: [score][target][no] no score accessor: <%s>(%u)",
				PGrnPGGetRelationNameByID(so->dataTableID, soTableName.data),
				so->dataTableID);
		return false;
	}

	GRN_LOG(ctx,
			GRN_LOG_DEBUG,
			"pgroonga: [score][target][yes] <%s>(%u)",
			PGrnPGGetRelationNameByID(so->dataTableID, soTableName.data),
			so->dataTableID);

	return true;
}

static double
PGrnCollectScore(Relation table, HeapTuple tuple)
{
	double score = 0.0;
	dlist_iter iter;

	dlist_foreach(iter, &PGrnScanOpaques)
	{
		PGrnScanOpaque so = dlist_container(PGrnScanOpaqueData, node, iter.cur);

		if (!PGrnCollectScoreIsTarget(so, tuple->t_tableOid))
			continue;

		if (slist_is_empty(&(so->primaryKeyColumns)))
			continue;

		if (so->primaryKeyColumns.head.next->next)
		{
			score += PGrnCollectScoreMultiColumnPrimaryKey(table, tuple, so);
		}
		else
		{
			score += PGrnCollectScoreOneColumnPrimaryKey(table, tuple, so);
		}
	}

	return score;
}

/**
 * pgroonga_score(row record) : float8
 *
 * It's deprecated since 2.0.4. Just for backward compatibility.
 */
Datum
pgroonga_score(PG_FUNCTION_ARGS)
{
	return pgroonga_score_row(fcinfo);
}

/**
 * pgroonga_score(row record) : float8
 */
Datum
pgroonga_score_row(PG_FUNCTION_ARGS)
{
	HeapTupleHeader header = PG_GETARG_HEAPTUPLEHEADER(0);
	Oid	type;
	int32 recordType;
	TupleDesc desc;
	double score = 0.0;

	type = HeapTupleHeaderGetTypeId(header);
	recordType = HeapTupleHeaderGetTypMod(header);
	desc = lookup_rowtype_tupdesc(type, recordType);

	if (desc->natts > 0 && !dlist_is_empty(&PGrnScanOpaques))
	{
		HeapTupleData tupleData;
		HeapTuple tuple;
		Relation table;

		tupleData.t_len = HeapTupleHeaderGetDatumLength(header);
		tupleData.t_tableOid = TupleDescAttr(desc, 0)->attrelid;
		tupleData.t_data = header;
		ItemPointerSetInvalid(&(tupleData.t_self));
		tuple = &tupleData;

		table = RelationIdGetRelation(tuple->t_tableOid);

		score = PGrnCollectScore(table, tuple);

		RelationClose(table);
	}

	ReleaseTupleDesc(desc);

	PG_RETURN_FLOAT8(score);
}

static void
PGrnScanOpaqueCreateCtidResolveTable(PGrnScanOpaque so)
{
	const char *tag = "pgroonga: [ctid-resolve-table][create]";
	Relation table;
	grn_obj *sourceRecord;
	grn_column_cache *sourcesCtidColumnCache = NULL;

	table = RelationIdGetRelation(so->dataTableID);

	GRN_LOG(ctx,
			GRN_LOG_DEBUG,
			"%s[start] <%s>(%u)",
			tag,
			table->rd_rel->relname.data,
			so->dataTableID);

	sourceRecord = &(buffers->general);
	grn_obj_reinit(ctx, sourceRecord, grn_obj_id(ctx, so->sourcesTable), 0);

	if (so->sourcesCtidColumn)
		sourcesCtidColumnCache =
			grn_column_cache_open(ctx, so->sourcesCtidColumn);

	so->ctidResolveTable = grn_table_create(ctx,
											NULL, 0,
											NULL,
											GRN_OBJ_TABLE_HASH_KEY,
											grn_ctx_at(ctx, GRN_DB_UINT64),
											so->sourcesTable);
	GRN_TABLE_EACH_BEGIN(ctx, so->searched, cursor, id)
	{
		void *key;
		grn_id sourceID;
		uint64 packedCtid = 0;
		ItemPointerData ctid;
		ItemPointerData resolvedCtid;
		uint64 resolvedPackedCtid;
		grn_id resolvedID;

		grn_table_cursor_get_key(ctx, cursor, &key);
		sourceID = *((grn_id *) key);

		if (sourcesCtidColumnCache)
		{
			void *ctidColumnValue;
			size_t ctidColumnValueSize;
			ctidColumnValue = grn_column_cache_ref(ctx,
												   sourcesCtidColumnCache,
												   sourceID,
												   &ctidColumnValueSize);
			if (ctidColumnValueSize != sizeof(uint64))
			{
				GRN_LOG(ctx,
						GRN_LOG_DEBUG,
						"%s[ignore] <%s>(%u): <%u>: "
						"<%" PGRN_PRIuSIZE "> != <%" PGRN_PRIuSIZE ">",
						tag,
						table->rd_rel->relname.data,
						so->dataTableID,
						sourceID,
						ctidColumnValueSize,
						sizeof(uint64));
				continue;
			}
			packedCtid = *((uint64 *) ctidColumnValue);
		}
		else
		{
			int keySize;

			keySize = grn_table_get_key(ctx,
										so->sourcesTable,
										sourceID,
										&packedCtid,
										sizeof(uint64));
			if (keySize != sizeof(uint64))
			{
				GRN_LOG(ctx,
						GRN_LOG_DEBUG,
						"%s[ignore] <%s>(%u): <%u>: "
						"<%d> != <%" PGRN_PRIuSIZE ">",
						tag,
						table->rd_rel->relname.data,
						so->dataTableID,
						sourceID,
						keySize,
						sizeof(uint64));
				continue;
			}
		}
		ctid = PGrnCtidUnpack(packedCtid);
		resolvedCtid = ctid;
		if (!PGrnCtidIsAlive(table, &resolvedCtid))
		{
			GRN_LOG(ctx,
					GRN_LOG_DEBUG,
					"%s[ignore][dead] <%s>(%u): <%u>",
					tag,
					table->rd_rel->relname.data,
					so->dataTableID,
					sourceID);
			continue;
		}

		if (!sourcesCtidColumnCache &&
			ItemPointerEquals(&ctid, &resolvedCtid))
		{
			GRN_LOG(ctx,
					GRN_LOG_DEBUG,
					"%s[ignore][not-hot] <%s>(%u): <%u>: <(%u,%u),%u>",
					tag,
					table->rd_rel->relname.data,
					so->dataTableID,
					sourceID,
					ctid.ip_blkid.bi_hi,
					ctid.ip_blkid.bi_lo,
					ctid.ip_posid);
			continue;
		}

		resolvedPackedCtid = PGrnCtidPack(&resolvedCtid);
		resolvedID = grn_table_add(ctx,
								   so->ctidResolveTable,
								   &resolvedPackedCtid,
								   sizeof(uint64),
								   NULL);
		GRN_RECORD_SET(ctx, sourceRecord, sourceID);
		grn_obj_set_value(ctx,
						  so->ctidResolveTable,
						  resolvedID,
						  sourceRecord,
						  GRN_OBJ_SET);
		GRN_LOG(ctx,
				GRN_LOG_DEBUG,
				"%s[add] <%s>(%u): <%u>: <(%u,%u),%u> -> <(%u,%u),%u>",
				tag,
				table->rd_rel->relname.data,
				so->dataTableID,
				sourceID,
				ctid.ip_blkid.bi_hi,
				ctid.ip_blkid.bi_lo,
				ctid.ip_posid,
				resolvedCtid.ip_blkid.bi_hi,
				resolvedCtid.ip_blkid.bi_lo,
				resolvedCtid.ip_posid);
	}
	GRN_TABLE_EACH_END(ctx, cursor);

	if (sourcesCtidColumnCache)
		grn_column_cache_close(ctx, sourcesCtidColumnCache);

	GRN_LOG(ctx,
			GRN_LOG_DEBUG,
			"%s[end] <%s>(%u)",
			tag,
			table->rd_rel->relname.data,
			so->dataTableID);

	RelationClose(table);
}

static double
PGrnCollectScoreCtid(PGrnScanOpaque so, ItemPointer ctid)
{
	const char *tag = "pgroonga: [score][ctid][collect]";
	const uint64 packedCtid = PGrnCtidPack(ctid);
	double score = 0.0;
	grn_id resolveID;
	grn_id sourceID;
	grn_id searchedID;

	if (!so->ctidResolveTable)
		PGrnScanOpaqueCreateCtidResolveTable(so);

	resolveID = grn_table_get(ctx,
							  so->ctidResolveTable,
							  &packedCtid,
							  sizeof(uint64));
	if (resolveID != GRN_ID_NIL)
	{
		{
			NameData soTableName;
			GRN_LOG(ctx,
					GRN_LOG_DEBUG,
					"%s[hot-resolved] <%s>(%u):<(%u,%u),%u>:<%u>",
					tag,
					PGrnPGGetRelationNameByID(so->dataTableID, soTableName.data),
					so->dataTableID,
					ctid->ip_blkid.bi_hi,
					ctid->ip_blkid.bi_lo,
					ctid->ip_posid,
					resolveID);
		}
		GRN_BULK_REWIND(&(buffers->general));
		grn_obj_get_value(ctx,
						  so->ctidResolveTable,
						  resolveID,
						  &(buffers->general));
		sourceID = GRN_RECORD_VALUE(&(buffers->general));
	}
	else if (so->sourcesTable->header.type != GRN_TABLE_NO_KEY)
	{
		sourceID = grn_table_get(ctx,
								 so->sourcesTable,
								 &packedCtid,
								 sizeof(uint64));
	}

	if (sourceID == GRN_ID_NIL)
	{
		NameData soTableName;
		GRN_LOG(ctx,
				GRN_LOG_DEBUG,
				"%s[no-record] <%s>(%u):<(%u,%u),%u>",
				tag,
				PGrnPGGetRelationNameByID(so->dataTableID, soTableName.data),
				so->dataTableID,
				ctid->ip_blkid.bi_hi,
				ctid->ip_blkid.bi_lo,
				ctid->ip_posid);
		return 0.0;
	}

	searchedID = grn_table_get(ctx, so->searched, &sourceID, sizeof(grn_id));
	if (searchedID == GRN_ID_NIL)
	{
		NameData soTableName;
		GRN_LOG(ctx,
				GRN_LOG_DEBUG,
				"%s[not-match] <%s>(%u):<(%u,%u),%u>:<%u>",
				tag,
				PGrnPGGetRelationNameByID(so->dataTableID, soTableName.data),
				so->dataTableID,
				ctid->ip_blkid.bi_hi,
				ctid->ip_blkid.bi_lo,
				ctid->ip_posid,
				sourceID);
		return 0.0;
	}

	GRN_BULK_REWIND(&(buffers->score));
	grn_obj_get_value(ctx, so->scoreAccessor, searchedID, &(buffers->score));
	if (buffers->score.header.domain == GRN_DB_FLOAT)
	{
		score = GRN_FLOAT_VALUE(&(buffers->score));
	}
	else
	{
		score = GRN_INT32_VALUE(&(buffers->score));
	}

	{
		NameData soTableName;
		GRN_LOG(ctx,
				GRN_LOG_DEBUG,
				"%s[found] <%s>(%u):<(%u,%u),%u>: <%u> -> <%u>: <%f>",
				tag,
				PGrnPGGetRelationNameByID(so->dataTableID, soTableName.data),
				so->dataTableID,
				ctid->ip_blkid.bi_hi,
				ctid->ip_blkid.bi_lo,
				ctid->ip_posid,
				sourceID,
				searchedID,
				score);
	}

	return score;
}

/**
 * pgroonga_score_ctid(tableoid oid, ctid tid) : float8
 */
Datum
pgroonga_score_ctid(PG_FUNCTION_ARGS)
{
	Oid tableOid = PG_GETARG_OID(0);
	ItemPointer ctid = (ItemPointer) PG_GETARG_POINTER(1);
	double score = 0.0;
	dlist_iter iter;

	dlist_foreach(iter, &PGrnScanOpaques)
	{
		PGrnScanOpaque so;

		so = dlist_container(PGrnScanOpaqueData, node, iter.cur);
		if (!PGrnCollectScoreIsTarget(so, tableOid))
			continue;

		score += PGrnCollectScoreCtid(so, ctid);
	}

	{
		NameData tableName;
		GRN_LOG(ctx,
				GRN_LOG_DEBUG,
				"pgroonga: [score][ctid] <%s>(%u):<(%u,%u),%u>: <%f>",
				PGrnPGGetRelationNameByID(tableOid, tableName.data),
				tableOid,
				ctid->ip_blkid.bi_hi,
				ctid->ip_blkid.bi_lo,
				ctid->ip_posid,
				score);
	}

	PG_RETURN_FLOAT8(score);
}

/**
 * pgroonga.table_name(indexName cstring) : text
 */
Datum
pgroonga_table_name(PG_FUNCTION_ARGS)
{
	const char *indexName = PG_GETARG_CSTRING(0);
	char tableNameBuffer[GRN_TABLE_MAX_KEY_SIZE];
	text *tableName;

	PGrnFormatSourcesTableName(indexName, tableNameBuffer);
	tableName = cstring_to_text(tableNameBuffer);
	PG_RETURN_TEXT_P(tableName);
}

static void
PGrnCommandReceive(grn_ctx *ctx,
				   int flags,
				   void *user_data)
{
	grn_rc rc = ctx->rc;
	char *chunk;
	unsigned int chunkSize;
	int recv_flags;
	grn_ctx_recv(ctx, &chunk, &chunkSize, &recv_flags);
	GRN_TEXT_PUT(ctx, &(buffers->body), chunk, chunkSize);

	if (!(flags & GRN_CTX_TAIL)) {
		return;
	}

	grn_output_envelope(ctx,
						rc,
						&(buffers->head),
						&(buffers->body),
						&(buffers->foot),
						NULL,
						0);
}

/**
 * pgroonga.command(groongaCommand text) : text
 * pgroonga.command(groongaCommandName text, arguments text[]) : text
 */
Datum
pgroonga_command(PG_FUNCTION_ARGS)
{
	text *groongaCommand = PG_GETARG_TEXT_PP(0);
	text *result;

	GRN_BULK_REWIND(&(buffers->head));
	GRN_BULK_REWIND(&(buffers->body));
	GRN_BULK_REWIND(&(buffers->foot));

	if (PG_NARGS() == 2)
	{
		grn_obj *command = &(buffers->general);
		ArrayType *arguments = PG_GETARG_ARRAYTYPE_P(1);
		int i, n;

		if (ARR_NDIM(arguments) == 0)
			n = 0;
		else
			n = ARR_DIMS(arguments)[0];

		grn_obj_reinit(ctx, command, GRN_DB_TEXT, 0);
		GRN_TEXT_PUT(ctx,
					 command,
					 VARDATA_ANY(groongaCommand),
					 VARSIZE_ANY_EXHDR(groongaCommand));
		for (i = 1; i <= n; i += 2)
		{
			int nameIndex = i;
			Datum nameDatum;
			text *name;
			int valueIndex = i + 1;
			Datum valueDatum;
			text *value;
			bool isNULL;

			nameDatum = array_ref(arguments, 1, &nameIndex, -1, -1, false,
								  'i', &isNULL);
			if (isNULL)
				continue;
			valueDatum = array_ref(arguments, 1, &valueIndex, -1, -1, false,
								   'i', &isNULL);
			if (isNULL)
				continue;

			name = DatumGetTextPP(nameDatum);
			value = DatumGetTextPP(valueDatum);

			GRN_TEXT_PUTS(ctx, command, " --");
			GRN_TEXT_PUT(ctx,
						 command,
						 VARDATA_ANY(name),
						 VARSIZE_ANY_EXHDR(name));
			GRN_TEXT_PUTC(ctx, command, ' ');
			PGrnCommandEscapeValue(VARDATA_ANY(value),
								   VARSIZE_ANY_EXHDR(value),
								   command);
		}
		grn_ctx_recv_handler_set(ctx, PGrnCommandReceive, NULL);
		grn_ctx_send(ctx,
					 GRN_TEXT_VALUE(command),
					 GRN_TEXT_LEN(command),
					 0);
	}
	else
	{
		grn_ctx_recv_handler_set(ctx, PGrnCommandReceive, NULL);
		grn_ctx_send(ctx,
					 VARDATA_ANY(groongaCommand),
					 VARSIZE_ANY_EXHDR(groongaCommand),
					 0);
	}
	grn_ctx_recv_handler_set(ctx, NULL, NULL);

	grn_obj_reinit(ctx, &(buffers->general), GRN_DB_TEXT, 0);
	GRN_TEXT_PUT(ctx, &(buffers->general),
				 GRN_TEXT_VALUE(&(buffers->head)),
				 GRN_TEXT_LEN(&(buffers->head)));
	GRN_TEXT_PUT(ctx, &(buffers->general),
				 GRN_TEXT_VALUE(&(buffers->body)),
				 GRN_TEXT_LEN(&(buffers->body)));
	GRN_TEXT_PUT(ctx, &(buffers->general),
				 GRN_TEXT_VALUE(&(buffers->foot)),
				 GRN_TEXT_LEN(&(buffers->foot)));
	result = cstring_to_text_with_len(GRN_TEXT_VALUE(&(buffers->general)),
									  GRN_TEXT_LEN(&(buffers->general)));
	PG_RETURN_TEXT_P(result);
}

typedef bool (*PGrnBinaryOperatorStringFunction)(const char *operand1,
												 unsigned int operandSize1,
												 const char *operand2,
												 unsigned int operandSize2,
												 const char *indexName,
												 unsigned int indexNameSize);

static bool
pgroonga_execute_binary_operator_string_array(ArrayType *operands1,
											  const char *operand2,
											  unsigned int operandSize2,
											  const char *indexName,
											  unsigned int indexNameSize,
											  PGrnBinaryOperatorStringFunction operator,
											  grn_obj *isTargets)
{
	bool matched = false;
	ArrayIterator iterator;
	int i;
	int nTargets = 0;
	Datum operandDatum1;
	bool isNULL;

	if (ARR_NDIM(operands1) == 0)
		return false;

	iterator = pgrn_array_create_iterator(operands1, 0);
	if (isTargets)
		nTargets = GRN_BULK_VSIZE(isTargets) / sizeof(grn_bool);
	for (i = 0; array_iterate(iterator, &operandDatum1, &isNULL); i++)
	{
		const char *operand1 = NULL;
		unsigned int operandSize1 = 0;

		if (nTargets > i && !GRN_BOOL_VALUE_AT(isTargets, i))
			continue;

		if (isNULL)
			continue;

		PGrnPGDatumExtractString(operandDatum1,
								 ARR_ELEMTYPE(operands1),
								 &operand1,
								 &operandSize1);
		if (!operand1)
			continue;

		if (operator(operand1, operandSize1,
					 operand2, operandSize2,
					 indexName, indexNameSize))
		{
			matched = true;
			break;
		}
	}
	array_free_iterator(iterator);

	return matched;
}

static bool
pgroonga_execute_binary_operator_in_string(const char *operand1,
										   unsigned int operandSize1,
										   ArrayType *operands2,
										   const char *indexName,
										   unsigned int indexNameSize,
										   PGrnBinaryOperatorStringFunction operator)
{
	int i, n;

	if (ARR_NDIM(operands2) == 0)
		return false;

	n = ARR_DIMS(operands2)[0];
	for (i = 1; i <= n; i++)
	{
		Datum operandDatum2;
		const char *operand2 = NULL;
		unsigned int operandSize2 = 0;
		bool isNULL;

		operandDatum2 = array_ref(operands2, 1, &i, -1, -1, false, 'i', &isNULL);
		if (isNULL)
			continue;

		PGrnPGDatumExtractString(operandDatum2,
								 ARR_ELEMTYPE(operands2),
								 &operand2,
								 &operandSize2);
		if (!operand2)
			continue;

		if (operator(operand1, operandSize1,
					 operand2, operandSize2,
					 indexName, indexNameSize))
			return true;
	}

	return false;
}

static bool
pgroonga_execute_binary_operator_in_string_array(ArrayType *operands1,
												 ArrayType *operands2,
												 const char *indexName,
												 unsigned int indexNameSize,
												 PGrnBinaryOperatorStringFunction operator)
{
	int i, n;

	if (ARR_NDIM(operands1) == 0)
		return false;

	n = ARR_DIMS(operands1)[0];
	for (i = 1; i <= n; i++)
	{
		Datum operandDatum1;
		const char *operand1 = NULL;
		unsigned int operandSize1 = 0;
		bool isNULL;

		operandDatum1 = array_ref(operands1, 1, &i, -1, -1, false, 'i', &isNULL);
		if (isNULL)
			continue;

		PGrnPGDatumExtractString(operandDatum1,
								 ARR_ELEMTYPE(operands1),
								 &operand1,
								 &operandSize1);
		if (!operand1)
			continue;

		if (pgroonga_execute_binary_operator_in_string(operand1,
													   operandSize1,
													   operands2,
													   indexName,
													   indexNameSize,
													   operator))
			return true;
	}

	return false;
}

static bool
pgroonga_match_term_raw(const char *target, unsigned int targetSize,
						const char *term, unsigned int termSize,
						const char *indexName, unsigned int indexNameSize)
{
	if (indexNameSize > 0 && PGrnIsTemporaryIndexSearchAvailable)
	{
		PGrnSequentialSearchDataPrepareText(&sequentialSearchData,
											target, targetSize,
											indexName, indexNameSize);
		PGrnSequentialSearchDataSetMatchTerm(&sequentialSearchData,
											 term, termSize);
		return PGrnSequentialSearchDataExecute(&sequentialSearchData);
	}
	else
	{
		grn_bool matched;
		grn_obj targetBuffer;
		grn_obj termBuffer;

		GRN_TEXT_INIT(&targetBuffer, GRN_OBJ_DO_SHALLOW_COPY);
		GRN_TEXT_SET(ctx, &targetBuffer, target, targetSize);

		GRN_TEXT_INIT(&termBuffer, GRN_OBJ_DO_SHALLOW_COPY);
		GRN_TEXT_SET(ctx, &termBuffer, term, termSize);

		matched = grn_operator_exec_match(ctx, &targetBuffer, &termBuffer);

		GRN_OBJ_FIN(ctx, &targetBuffer);
		GRN_OBJ_FIN(ctx, &termBuffer);

		return matched;
	}
}

/**
 * pgroonga_match_term(target text, term text) : bool
 */
Datum
pgroonga_match_term_text(PG_FUNCTION_ARGS)
{
	text *target = PG_GETARG_TEXT_PP(0);
	text *term = PG_GETARG_TEXT_PP(1);
	bool matched;

	matched = pgroonga_match_term_raw(VARDATA_ANY(target),
									  VARSIZE_ANY_EXHDR(target),
									  VARDATA_ANY(term),
									  VARSIZE_ANY_EXHDR(term),
									  NULL,
									  0);

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_match_term(target text[], term text) : bool
 */
Datum
pgroonga_match_term_text_array(PG_FUNCTION_ARGS)
{
	ArrayType *targets = PG_GETARG_ARRAYTYPE_P(0);
	text *term = PG_GETARG_TEXT_PP(1);
	bool matched;

	matched =
		pgroonga_execute_binary_operator_string_array(
			targets,
			VARDATA_ANY(term),
			VARSIZE_ANY_EXHDR(term),
			NULL,
			0,
			pgroonga_match_term_raw,
			NULL);

	PG_RETURN_BOOL(matched);
}

static bool
pgroonga_equal_raw(const char *text1, unsigned int text1Size,
				   const char *text2, unsigned int text2Size,
				   const char *indexName, unsigned int indexNameSize)
{
	grn_bool matched;
	grn_obj text1Buffer;
	grn_obj text2Buffer;

	GRN_TEXT_INIT(&text1Buffer, GRN_OBJ_DO_SHALLOW_COPY);
	GRN_TEXT_SET(ctx, &text1Buffer, text1, text1Size);

	GRN_TEXT_INIT(&text2Buffer, GRN_OBJ_DO_SHALLOW_COPY);
	GRN_TEXT_SET(ctx, &text2Buffer, text2, text2Size);

	/* TODO: Use indexName */
	matched = grn_operator_exec_equal(ctx, &text1Buffer, &text2Buffer);

	GRN_OBJ_FIN(ctx, &text1Buffer);
	GRN_OBJ_FIN(ctx, &text2Buffer);

	return matched;
}

/**
 * pgroonga_match_term(target varchar, term varchar) : bool
 */
Datum
pgroonga_match_term_varchar(PG_FUNCTION_ARGS)
{
	VarChar *target = PG_GETARG_VARCHAR_PP(0);
	VarChar *term = PG_GETARG_VARCHAR_PP(1);
	bool matched;

	matched = pgroonga_match_term_raw(VARDATA_ANY(target),
									  VARSIZE_ANY_EXHDR(target),
									  VARDATA_ANY(term),
									  VARSIZE_ANY_EXHDR(term),
									  NULL,
									  0);

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_match_term(target varchar[], term varchar) : bool
 */
Datum
pgroonga_match_term_varchar_array(PG_FUNCTION_ARGS)
{
	ArrayType *targets = PG_GETARG_ARRAYTYPE_P(0);
	VarChar *term = PG_GETARG_VARCHAR_PP(1);
	bool matched;

	matched =
		pgroonga_execute_binary_operator_string_array(
			targets,
			VARDATA_ANY(term),
			VARSIZE_ANY_EXHDR(term),
			NULL,
			0,
			pgroonga_match_term_raw,
			NULL);

	PG_RETURN_BOOL(matched);
}

static bool
pgroonga_match_query_raw(const char *target, unsigned int targetSize,
						 const char *query, unsigned int querySize,
						 const char *indexName, unsigned int indexNameSize)
{
	PGrnSequentialSearchDataPrepareText(&sequentialSearchData,
										target, targetSize,
										indexName, indexNameSize);
	PGrnSequentialSearchDataSetQuery(&sequentialSearchData,
									 query, querySize);
	return PGrnSequentialSearchDataExecute(&sequentialSearchData);
}

static bool
pgroonga_match_query_string_array_raw(ArrayType *targets,
									  grn_obj *isTargets,
									  const char *query,
									  unsigned int querySize,
									  const char *indexName,
									  unsigned int indexNameSize)
{
	if (ARR_NDIM(targets) == 0)
		return false;

	PGrnSequentialSearchDataPrepareTexts(&sequentialSearchData,
										 targets,
										 isTargets,
										 indexName,
										 indexNameSize);
	PGrnSequentialSearchDataSetQuery(&sequentialSearchData,
									 query, querySize);
	return PGrnSequentialSearchDataExecute(&sequentialSearchData);
}

/**
 * pgroonga_match_query(target text, query text) : bool
 */
Datum
pgroonga_match_query_text(PG_FUNCTION_ARGS)
{
	text *target = PG_GETARG_TEXT_PP(0);
	text *query = PG_GETARG_TEXT_PP(1);
	bool matched;

	matched = pgroonga_match_query_raw(VARDATA_ANY(target),
									   VARSIZE_ANY_EXHDR(target),
									   VARDATA_ANY(query),
									   VARSIZE_ANY_EXHDR(query),
									   NULL,
									   0);

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_match_query(targets text[], query text) : bool
 */
Datum
pgroonga_match_query_text_array(PG_FUNCTION_ARGS)
{
	ArrayType *targets = PG_GETARG_ARRAYTYPE_P(0);
	text *query = PG_GETARG_TEXT_PP(1);
	bool matched;

	matched = pgroonga_match_query_string_array_raw(targets,
													NULL,
													VARDATA_ANY(query),
													VARSIZE_ANY_EXHDR(query),
													NULL,
													0);

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_match_query(target varchar, query varchar) : bool
 */
Datum
pgroonga_match_query_varchar(PG_FUNCTION_ARGS)
{
	VarChar *target = PG_GETARG_VARCHAR_PP(0);
	VarChar *query = PG_GETARG_VARCHAR_PP(1);
	bool matched;

	matched = pgroonga_match_query_raw(VARDATA_ANY(target),
									   VARSIZE_ANY_EXHDR(target),
									   VARDATA_ANY(query),
									   VARSIZE_ANY_EXHDR(query),
									   NULL,
									   0);

	PG_RETURN_BOOL(matched);
}

static bool
pgroonga_match_regexp_raw(const char *text, unsigned int textSize,
						  const char *pattern, unsigned int patternSize,
						  const char *indexName, unsigned int indexNameSize)
{
	grn_bool matched;
	grn_obj targetBuffer;
	grn_obj patternBuffer;

	GRN_TEXT_INIT(&targetBuffer, GRN_OBJ_DO_SHALLOW_COPY);
	GRN_TEXT_SET(ctx, &targetBuffer, text, textSize);

	GRN_TEXT_INIT(&patternBuffer, GRN_OBJ_DO_SHALLOW_COPY);
	GRN_TEXT_SET(ctx, &patternBuffer, pattern, patternSize);

	/* TODO: Use indexName */
	matched = grn_operator_exec_regexp(ctx, &targetBuffer, &patternBuffer);

	GRN_OBJ_FIN(ctx, &targetBuffer);
	GRN_OBJ_FIN(ctx, &patternBuffer);

	return matched;
}

/**
 * pgroonga_match_regexp(target text, pattern text) : bool
 */
Datum
pgroonga_match_regexp_text(PG_FUNCTION_ARGS)
{
	text *target = PG_GETARG_TEXT_PP(0);
	text *pattern = PG_GETARG_TEXT_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_match_regexp_raw(VARDATA_ANY(target),
											VARSIZE_ANY_EXHDR(target),
											VARDATA_ANY(pattern),
											VARSIZE_ANY_EXHDR(pattern),
											NULL,
											0);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_match_regexp_raw(VARDATA_ANY(target),
											VARSIZE_ANY_EXHDR(target),
											VARDATA_ANY(pattern),
											VARSIZE_ANY_EXHDR(pattern),
											NULL,
											0);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_match_regexp_varchar(target varchar, pattern varchar) : bool
 */
Datum
pgroonga_match_regexp_varchar(PG_FUNCTION_ARGS)
{
	VarChar *target = PG_GETARG_VARCHAR_PP(0);
	VarChar *pattern = PG_GETARG_VARCHAR_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_match_regexp_raw(VARDATA_ANY(target),
											VARSIZE_ANY_EXHDR(target),
											VARDATA_ANY(pattern),
											VARSIZE_ANY_EXHDR(pattern),
											NULL,
											0);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_match_regexp_raw(VARDATA_ANY(target),
											VARSIZE_ANY_EXHDR(target),
											VARDATA_ANY(pattern),
											VARSIZE_ANY_EXHDR(pattern),
											NULL,
											0);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/* v2 */
/**
 * pgroonga_match_text(target text, term text) : bool
 */
Datum
pgroonga_match_text(PG_FUNCTION_ARGS)
{
	text *target = PG_GETARG_TEXT_PP(0);
	text *term = PG_GETARG_TEXT_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_match_term_raw(VARDATA_ANY(target),
										  VARSIZE_ANY_EXHDR(target),
										  VARDATA_ANY(term),
										  VARSIZE_ANY_EXHDR(term),
										  NULL,
										  0);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_match_term_raw(VARDATA_ANY(target),
										  VARSIZE_ANY_EXHDR(target),
										  VARDATA_ANY(term),
										  VARSIZE_ANY_EXHDR(term),
										  NULL,
										  0);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

static bool
pgroonga_match_condition_raw(const char *target,
							 unsigned int targetSize,
							 HeapTupleHeader header,
							 bool withScorers)
{
	grn_obj *isTargets = &(buffers->isTargets);
	text *term;
	text *indexName;
	const char *indexNameData = NULL;
	unsigned int indexNameSize = 0;

	GRN_BULK_REWIND(isTargets);

	if (withScorers)
	{
		PGrnFullTextSearchConditionWithScorersDeconstruct(header,
														  &term,
														  NULL,
														  NULL,
														  &indexName,
														  isTargets);
	}
	else
	{
		PGrnFullTextSearchConditionDeconstruct(header,
											   &term,
											   NULL,
											   &indexName,
											   isTargets);
	}

	if (!term)
		return false;

	if (GRN_BULK_VSIZE(isTargets) > 0 && !GRN_BOOL_VALUE_AT(isTargets, 0))
		return false;

	if (indexName)
	{
		indexNameData = VARDATA_ANY(indexName);
		indexNameSize = VARSIZE_ANY_EXHDR(indexName);
	}

	return pgroonga_match_term_raw(target,
								   targetSize,
								   VARDATA_ANY(term),
								   VARSIZE_ANY_EXHDR(term),
								   indexNameData,
								   indexNameSize);
}

/**
 * pgroonga_match_text_condition(target text,
 *                               condition pgroonga_match_condition) : bool
 */
Datum
pgroonga_match_text_condition(PG_FUNCTION_ARGS)
{
	text *target = PG_GETARG_TEXT_PP(0);
	HeapTupleHeader header = PG_GETARG_HEAPTUPLEHEADER(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_match_condition_raw(VARDATA_ANY(target),
											   VARSIZE_ANY_EXHDR(target),
											   header,
											   false);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_match_condition_raw(VARDATA_ANY(target),
											   VARSIZE_ANY_EXHDR(target),
											   header,
											   false);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_match_text_condition_with_scorers(
 *   target text,
 *   condition pgroonga_match_condition_with_scorers) : bool
 */
Datum
pgroonga_match_text_condition_with_scorers(PG_FUNCTION_ARGS)
{
	text *target = PG_GETARG_TEXT_PP(0);
	HeapTupleHeader header = PG_GETARG_HEAPTUPLEHEADER(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_match_condition_raw(VARDATA_ANY(target),
											   VARSIZE_ANY_EXHDR(target),
											   header,
											   true);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_match_condition_raw(VARDATA_ANY(target),
											   VARSIZE_ANY_EXHDR(target),
											   header,
											   true);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_match_text_array(targets text[], term text) : bool
 */
Datum
pgroonga_match_text_array(PG_FUNCTION_ARGS)
{
	ArrayType *targets = PG_GETARG_ARRAYTYPE_P(0);
	text *term = PG_GETARG_TEXT_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_string_array(
				targets,
				VARDATA_ANY(term),
				VARSIZE_ANY_EXHDR(term),
				NULL,
				0,
				pgroonga_match_term_raw,
				NULL);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_string_array(
				targets,
				VARDATA_ANY(term),
				VARSIZE_ANY_EXHDR(term),
				NULL,
				0,
				pgroonga_match_term_raw,
				NULL);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

static bool
pgroonga_match_text_array_condition_raw(ArrayType *targets,
										HeapTupleHeader header,
										bool withScorers)
{
	grn_obj *isTargets = &(buffers->isTargets);
	text *term;
	text *indexName;
	const char *indexNameData = NULL;
	unsigned int indexNameSize = 0;

	GRN_BULK_REWIND(isTargets);

	if (withScorers)
	{
		PGrnFullTextSearchConditionWithScorersDeconstruct(header,
														  &term,
														  NULL,
														  NULL,
														  &indexName,
														  isTargets);
	}
	else
	{
		PGrnFullTextSearchConditionDeconstruct(header,
											   &term,
											   NULL,
											   &indexName,
											   isTargets);
	}

	if (!term)
		return false;

	if (indexName)
	{
		indexNameData = VARDATA_ANY(indexName);
		indexNameSize = VARSIZE_ANY_EXHDR(indexName);
	}

	return pgroonga_execute_binary_operator_string_array(targets,
														 VARDATA_ANY(term),
														 VARSIZE_ANY_EXHDR(term),
														 indexNameData,
														 indexNameSize,
														 pgroonga_match_term_raw,
														 isTargets);
}

/**
 * pgroonga_match_text_array_condition(targets text[],
 *                                     condition pgroonga_match_condition) : bool
 */
Datum
pgroonga_match_text_array_condition(PG_FUNCTION_ARGS)
{
	ArrayType *targets = PG_GETARG_ARRAYTYPE_P(0);
	HeapTupleHeader header = PG_GETARG_HEAPTUPLEHEADER(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_match_text_array_condition_raw(targets, header, false);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_match_text_array_condition_raw(targets, header, false);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_match_text_array_condition_with_scorers(
 *   targets text[],
 *   condition pgroonga_match_condition_with_scorers) : bool
 */
Datum
pgroonga_match_text_array_condition_with_scorers(PG_FUNCTION_ARGS)
{
	ArrayType *targets = PG_GETARG_ARRAYTYPE_P(0);
	HeapTupleHeader header = PG_GETARG_HEAPTUPLEHEADER(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_match_text_array_condition_raw(targets, header, true);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_match_text_array_condition_raw(targets, header, true);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_match_varchar(target varchar, term varchar) : bool
 */
Datum
pgroonga_match_varchar(PG_FUNCTION_ARGS)
{
	VarChar *target = PG_GETARG_VARCHAR_PP(0);
	VarChar *term = PG_GETARG_VARCHAR_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_match_term_raw(VARDATA_ANY(target),
										  VARSIZE_ANY_EXHDR(target),
										  VARDATA_ANY(term),
										  VARSIZE_ANY_EXHDR(term),
										  NULL,
										  0);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_match_term_raw(VARDATA_ANY(target),
										  VARSIZE_ANY_EXHDR(target),
										  VARDATA_ANY(term),
										  VARSIZE_ANY_EXHDR(term),
										  NULL,
										  0);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_match_varchar_condition(target varchar,
 *                                  condition pgroonga_full_text_search_condition) : bool
 */
Datum
pgroonga_match_varchar_condition(PG_FUNCTION_ARGS)
{
	VarChar *target = PG_GETARG_VARCHAR_PP(0);
	HeapTupleHeader header = PG_GETARG_HEAPTUPLEHEADER(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_match_condition_raw(VARDATA_ANY(target),
											   VARSIZE_ANY_EXHDR(target),
											   header,
											   false);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_match_condition_raw(VARDATA_ANY(target),
											   VARSIZE_ANY_EXHDR(target),
											   header,
											   false);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_match_varchar_condition_with_scorers(
 *   target varchar,
 *   condition pgroonga_full_text_search_condition_with_scorers) : bool
 */
Datum
pgroonga_match_varchar_condition_with_scorers(PG_FUNCTION_ARGS)
{
	VarChar *target = PG_GETARG_VARCHAR_PP(0);
	HeapTupleHeader header = PG_GETARG_HEAPTUPLEHEADER(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_match_condition_raw(VARDATA_ANY(target),
											   VARSIZE_ANY_EXHDR(target),
											   header,
											   true);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_match_condition_raw(VARDATA_ANY(target),
											   VARSIZE_ANY_EXHDR(target),
											   header,
											   true);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_contain_varchar_array(target varchar[], term varchar) : bool
 */
Datum
pgroonga_contain_varchar_array(PG_FUNCTION_ARGS)
{
	ArrayType *targets = PG_GETARG_ARRAYTYPE_P(0);
	VarChar *term = PG_GETARG_VARCHAR_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_string_array(
				targets,
				VARDATA_ANY(term),
				VARSIZE_ANY_EXHDR(term),
				NULL,
				0,
				pgroonga_equal_raw,
				NULL);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_string_array(
				targets,
				VARDATA_ANY(term),
				VARSIZE_ANY_EXHDR(term),
				NULL,
				0,
				pgroonga_equal_raw,
				NULL);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_query_text(target text, query text) : bool
 */
Datum
pgroonga_query_text(PG_FUNCTION_ARGS)
{
	text *target = PG_GETARG_TEXT_PP(0);
	text *query = PG_GETARG_TEXT_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_match_query_raw(VARDATA_ANY(target),
										   VARSIZE_ANY_EXHDR(target),
										   VARDATA_ANY(query),
										   VARSIZE_ANY_EXHDR(query),
										   NULL,
										   0);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_match_query_raw(VARDATA_ANY(target),
										   VARSIZE_ANY_EXHDR(target),
										   VARDATA_ANY(query),
										   VARSIZE_ANY_EXHDR(query),
										   NULL,
										   0);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

static bool
pgroonga_query_condition_raw(const char *target,
							 unsigned int targetSize,
							 HeapTupleHeader header,
							 bool withScorers)
{
	grn_obj *isTargets = &(buffers->isTargets);
	text *query;
	text *indexName;
	const char *indexNameData = NULL;
	unsigned int indexNameSize = 0;

	GRN_BULK_REWIND(isTargets);

	if (withScorers)
	{
		PGrnFullTextSearchConditionWithScorersDeconstruct(header,
														  &query,
														  NULL,
														  NULL,
														  &indexName,
														  isTargets);
	}
	else
	{
		PGrnFullTextSearchConditionDeconstruct(header,
											   &query,
											   NULL,
											   &indexName,
											   isTargets);
	}

	if (!query)
		return false;

	if (GRN_BULK_VSIZE(isTargets) > 0 && !GRN_BOOL_VALUE_AT(isTargets, 0))
		return false;

	if (indexName)
	{
		indexNameData = VARDATA_ANY(indexName);
		indexNameSize = VARSIZE_ANY_EXHDR(indexName);
	}

	return pgroonga_match_query_raw(target,
									targetSize,
									VARDATA_ANY(query),
									VARSIZE_ANY_EXHDR(query),
									indexNameData,
									indexNameSize);
}

/**
 * pgroonga_query_text_condition(target text,
 *                               condition pgroonga_match_condition) : bool
 */
Datum
pgroonga_query_text_condition(PG_FUNCTION_ARGS)
{
	text *target = PG_GETARG_TEXT_PP(0);
	HeapTupleHeader header = PG_GETARG_HEAPTUPLEHEADER(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_query_condition_raw(VARDATA_ANY(target),
											   VARSIZE_ANY_EXHDR(target),
											   header,
											   false);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_query_condition_raw(VARDATA_ANY(target),
											   VARSIZE_ANY_EXHDR(target),
											   header,
											   false);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_query_text_condition_with_scorers(
 *   target text,
 *   condition pgroonga_match_condition_with_scorers) : bool
 */
Datum
pgroonga_query_text_condition_with_scorers(PG_FUNCTION_ARGS)
{
	text *target = PG_GETARG_TEXT_PP(0);
	HeapTupleHeader header = PG_GETARG_HEAPTUPLEHEADER(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_query_condition_raw(VARDATA_ANY(target),
											   VARSIZE_ANY_EXHDR(target),
											   header,
											   true);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_query_condition_raw(VARDATA_ANY(target),
											   VARSIZE_ANY_EXHDR(target),
											   header,
											   true);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_query_text_array(targets text[], query text) : bool
 */
Datum
pgroonga_query_text_array(PG_FUNCTION_ARGS)
{
	ArrayType *targets = PG_GETARG_ARRAYTYPE_P(0);
	text *query = PG_GETARG_TEXT_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_match_query_string_array_raw(targets,
														NULL,
														VARDATA_ANY(query),
														VARSIZE_ANY_EXHDR(query),
														NULL,
														0);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_match_query_string_array_raw(targets,
														NULL,
														VARDATA_ANY(query),
														VARSIZE_ANY_EXHDR(query),
														NULL,
														0);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

static bool
pgroonga_query_text_array_condition_raw(ArrayType *targets,
										HeapTupleHeader header,
										bool withScorers)
{
	grn_obj *isTargets = &(buffers->isTargets);
	text *query;
	text *indexName;
	const char *indexNameData = NULL;
	unsigned int indexNameSize = 0;

	GRN_BULK_REWIND(isTargets);

	if (withScorers) {
		PGrnFullTextSearchConditionWithScorersDeconstruct(header,
														  &query,
														  NULL,
														  NULL,
														  &indexName,
														  isTargets);
	}
	else
	{
		PGrnFullTextSearchConditionDeconstruct(header,
											   &query,
											   NULL,
											   &indexName,
											   isTargets);
	}

	if (!query)
		return false;

	if (indexName)
	{
		indexNameData = VARDATA_ANY(indexName);
		indexNameSize = VARSIZE_ANY_EXHDR(indexName);
	}

	return pgroonga_match_query_string_array_raw(targets,
												 isTargets,
												 VARDATA_ANY(query),
												 VARSIZE_ANY_EXHDR(query),
												 indexNameData,
												 indexNameSize);
}

/**
 * pgroonga_query_text_array_condition(targets text[],
 *                                     condition pgroonga_match_condition) : bool
 */
Datum
pgroonga_query_text_array_condition(PG_FUNCTION_ARGS)
{
	ArrayType *targets = PG_GETARG_ARRAYTYPE_P(0);
	HeapTupleHeader header = PG_GETARG_HEAPTUPLEHEADER(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_query_text_array_condition_raw(targets,
														  header,
														  false);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_query_text_array_condition_raw(targets,
														  header,
														  false);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_query_text_array_condition_with_scorers(
 *   targets text[],
 *   condition pgroonga_match_condition_with_scorers) : bool
 */
Datum
pgroonga_query_text_array_condition_with_scorers(PG_FUNCTION_ARGS)
{
	ArrayType *targets = PG_GETARG_ARRAYTYPE_P(0);
	HeapTupleHeader header = PG_GETARG_HEAPTUPLEHEADER(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_query_text_array_condition_raw(targets,
														  header,
														  true);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_query_text_array_condition_raw(targets,
														  header,
														  true);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_query_varchar(target varchar, term varchar) : bool
 */
Datum
pgroonga_query_varchar(PG_FUNCTION_ARGS)
{
	VarChar *target = PG_GETARG_VARCHAR_PP(0);
	VarChar *query = PG_GETARG_VARCHAR_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_match_query_raw(VARDATA_ANY(target),
										   VARSIZE_ANY_EXHDR(target),
										   VARDATA_ANY(query),
										   VARSIZE_ANY_EXHDR(query),
										   NULL,
										   0);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_match_query_raw(VARDATA_ANY(target),
										   VARSIZE_ANY_EXHDR(target),
										   VARDATA_ANY(query),
										   VARSIZE_ANY_EXHDR(query),
										   NULL,
										   0);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_query_varchar_condition(target varchar,
 *                                  condition pgroonga_full_text_search_condition) : bool
 */
Datum
pgroonga_query_varchar_condition(PG_FUNCTION_ARGS)
{
	VarChar *target = PG_GETARG_VARCHAR_PP(0);
	HeapTupleHeader header = PG_GETARG_HEAPTUPLEHEADER(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_query_condition_raw(VARDATA_ANY(target),
											   VARSIZE_ANY_EXHDR(target),
											   header,
											   false);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_query_condition_raw(VARDATA_ANY(target),
											   VARSIZE_ANY_EXHDR(target),
											   header,
											   false);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_query_varchar_condition_with_scorers(
 *   target varchar,
 *   condition pgroonga_full_text_search_condition_with_scorers) : bool
 */
Datum
pgroonga_query_varchar_condition_with_scorers(PG_FUNCTION_ARGS)
{
	VarChar *target = PG_GETARG_VARCHAR_PP(0);
	HeapTupleHeader header = PG_GETARG_HEAPTUPLEHEADER(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_query_condition_raw(VARDATA_ANY(target),
											   VARSIZE_ANY_EXHDR(target),
											   header,
											   true);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_query_condition_raw(VARDATA_ANY(target),
											   VARSIZE_ANY_EXHDR(target),
											   header,
											   true);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_similar_text(target text, document text) : bool
 */
Datum
pgroonga_similar_text(PG_FUNCTION_ARGS)
{
	const char *tag = "[similar][text]";
	PGrnCheckRC(GRN_FUNCTION_NOT_IMPLEMENTED,
				"%s similar search available only in index scan",
				tag);

	PG_RETURN_BOOL(false);
}

/**
 * pgroonga_similar_text_array(targets text[], document text) : bool
 */
Datum
pgroonga_similar_text_array(PG_FUNCTION_ARGS)
{
	const char *tag = "[similar][text-array]";
	PGrnCheckRC(GRN_FUNCTION_NOT_IMPLEMENTED,
				"%s similar search is available only in index scan",
				tag);

	PG_RETURN_BOOL(false);
}

/**
 * pgroonga_similar_varchar(target varchar, document varchar) : bool
 */
Datum
pgroonga_similar_varchar(PG_FUNCTION_ARGS)
{
	const char *tag = "[similar][varchar]";
	PGrnCheckRC(GRN_FUNCTION_NOT_IMPLEMENTED,
				"%s similar search is available only in index scan",
				tag);

	PG_RETURN_BOOL(false);
}

static bool
pgroonga_script_raw(const char *target, unsigned int targetSize,
					const char *script, unsigned int scriptSize,
					const char *indexName, unsigned int indexNameSize)
{
	PGrnSequentialSearchDataPrepareText(&sequentialSearchData,
										target, targetSize,
										indexName, indexNameSize);
	PGrnSequentialSearchDataSetScript(&sequentialSearchData,
									  script, scriptSize);
	return PGrnSequentialSearchDataExecute(&sequentialSearchData);
}

/**
 * pgroonga_script_text(target text, script text) : bool
 */
Datum
pgroonga_script_text(PG_FUNCTION_ARGS)
{
	text *target = PG_GETARG_TEXT_PP(0);
	text *script = PG_GETARG_TEXT_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_script_raw(VARDATA_ANY(target),
									  VARSIZE_ANY_EXHDR(target),
									  VARDATA_ANY(script),
									  VARSIZE_ANY_EXHDR(script),
									  NULL,
									  0);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_script_raw(VARDATA_ANY(target),
									  VARSIZE_ANY_EXHDR(target),
									  VARDATA_ANY(script),
									  VARSIZE_ANY_EXHDR(script),
									  NULL,
									  0);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_script_text_array(targets text[], script text) : bool
 */
Datum
pgroonga_script_text_array(PG_FUNCTION_ARGS)
{
	ArrayType *targets = PG_GETARG_ARRAYTYPE_P(0);
	text *script = PG_GETARG_TEXT_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_string_array(
				targets,
				VARDATA_ANY(script),
				VARSIZE_ANY_EXHDR(script),
				NULL,
				0,
				pgroonga_script_raw,
				NULL);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_string_array(
				targets,
				VARDATA_ANY(script),
				VARSIZE_ANY_EXHDR(script),
				NULL,
				0,
				pgroonga_script_raw,
				NULL);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_script_varchar(target varchar, script varchar) : bool
 */
Datum
pgroonga_script_varchar(PG_FUNCTION_ARGS)
{
	VarChar *target = PG_GETARG_VARCHAR_PP(0);
	VarChar *script = PG_GETARG_VARCHAR_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_script_raw(VARDATA_ANY(target),
									  VARSIZE_ANY_EXHDR(target),
									  VARDATA_ANY(script),
									  VARSIZE_ANY_EXHDR(script),
									  NULL,
									  0);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_script_raw(VARDATA_ANY(target),
									  VARSIZE_ANY_EXHDR(target),
									  VARDATA_ANY(script),
									  VARSIZE_ANY_EXHDR(script),
									  NULL,
									  0);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

static bool
pgroonga_prefix_raw(const char *text, unsigned int textSize,
					const char *prefix, unsigned int prefixSize,
					const char *indexName, unsigned int indexNameSize)
{
	grn_bool matched;
	grn_obj targetBuffer;
	grn_obj prefixBuffer;

	GRN_TEXT_INIT(&targetBuffer, GRN_OBJ_DO_SHALLOW_COPY);
	GRN_TEXT_SET(ctx, &targetBuffer, text, textSize);

	GRN_TEXT_INIT(&prefixBuffer, GRN_OBJ_DO_SHALLOW_COPY);
	GRN_TEXT_SET(ctx, &prefixBuffer, prefix, prefixSize);

	/* TODO: Use indexName */
	matched = grn_operator_exec_prefix(ctx, &targetBuffer, &prefixBuffer);

	GRN_OBJ_FIN(ctx, &targetBuffer);
	GRN_OBJ_FIN(ctx, &prefixBuffer);

	return matched;
}

/**
 * pgroonga_prefix_text(target text, prefix text) : bool
 */
Datum
pgroonga_prefix_text(PG_FUNCTION_ARGS)
{
	text *target = PG_GETARG_TEXT_PP(0);
	text *prefix = PG_GETARG_TEXT_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_prefix_raw(VARDATA_ANY(target),
									  VARSIZE_ANY_EXHDR(target),
									  VARDATA_ANY(prefix),
									  VARSIZE_ANY_EXHDR(prefix),
									  NULL,
									  0);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_prefix_raw(VARDATA_ANY(target),
									  VARSIZE_ANY_EXHDR(target),
									  VARDATA_ANY(prefix),
									  VARSIZE_ANY_EXHDR(prefix),
									  NULL,
									  0);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_prefix_text_array(targets text[], prefix text) : bool
 */
Datum
pgroonga_prefix_text_array(PG_FUNCTION_ARGS)
{
	ArrayType *targets = PG_GETARG_ARRAYTYPE_P(0);
	text *prefix = PG_GETARG_TEXT_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_string_array(
				targets,
				VARDATA_ANY(prefix),
				VARSIZE_ANY_EXHDR(prefix),
				NULL,
				0,
				pgroonga_prefix_raw,
				NULL);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_string_array(
				targets,
				VARDATA_ANY(prefix),
				VARSIZE_ANY_EXHDR(prefix),
				NULL,
				0,
				pgroonga_prefix_raw,
				NULL);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_prefix_varchar(target varchar, prefix varchar) : bool
 */
Datum
pgroonga_prefix_varchar(PG_FUNCTION_ARGS)
{
	VarChar *target = PG_GETARG_VARCHAR_PP(0);
	VarChar *prefix = PG_GETARG_VARCHAR_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_prefix_raw(VARDATA_ANY(target),
									  VARSIZE_ANY_EXHDR(target),
									  VARDATA_ANY(prefix),
									  VARSIZE_ANY_EXHDR(prefix),
									  NULL,
									  0);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_prefix_raw(VARDATA_ANY(target),
									  VARSIZE_ANY_EXHDR(target),
									  VARDATA_ANY(prefix),
									  VARSIZE_ANY_EXHDR(prefix),
									  NULL,
									  0);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_prefix_varchar_array(targets varchar[], prefix varchar) : bool
 */
Datum
pgroonga_prefix_varchar_array(PG_FUNCTION_ARGS)
{
	ArrayType *targets = PG_GETARG_ARRAYTYPE_P(0);
	VarChar *prefix = PG_GETARG_VARCHAR_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_string_array(
				targets,
				VARDATA_ANY(prefix),
				VARSIZE_ANY_EXHDR(prefix),
				NULL,
				0,
				pgroonga_prefix_raw,
				NULL);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_string_array(
				targets,
				VARDATA_ANY(prefix),
				VARSIZE_ANY_EXHDR(prefix),
				NULL,
				0,
				pgroonga_prefix_raw,
				NULL);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_prefix_contain_text_array(targets text[], prefix text) : bool
 *
 * It's deprecated since 1.2.1. Just for backward compatibility.
 */
Datum
pgroonga_prefix_contain_text_array(PG_FUNCTION_ARGS)
{
	return pgroonga_prefix_text_array(fcinfo);
}

static bool
pgroonga_prefix_rk_raw(const char *text, unsigned int textSize,
					   const char *prefix, unsigned int prefixSize,
					   const char *indexName, unsigned int indexNameSize)
{
	const char *tag = "[prefix-rk]";
	grn_obj *expression;
	grn_obj *variable;
	bool matched;
	grn_id id;

	/* TODO: Use indexName */

	GRN_EXPR_CREATE_FOR_QUERY(ctx,
							  prefixRKSequentialSearchData.table,
							  expression,
							  variable);
	if (!expression)
	{
		PGrnCheckRC(GRN_NO_MEMORY_AVAILABLE,
					"%s failed to create expression",
					tag);
	}

	grn_expr_append_obj(ctx, expression,
						grn_ctx_get(ctx, "prefix_rk_search", -1),
						GRN_OP_PUSH, 1);
	grn_expr_append_obj(ctx, expression,
						prefixRKSequentialSearchData.key,
						GRN_OP_GET_VALUE, 1);
	grn_expr_append_const_str(ctx, expression,
							  prefix, prefixSize,
							  GRN_OP_PUSH, 1);
	grn_expr_append_op(ctx, expression, GRN_OP_CALL, 2);

	id = grn_table_add(ctx,
					   prefixRKSequentialSearchData.table,
					   text, textSize, NULL);
	grn_table_select(ctx,
					 prefixRKSequentialSearchData.table,
					 expression,
					 prefixRKSequentialSearchData.resultTable,
					 GRN_OP_OR);
	matched = grn_table_size(ctx, prefixRKSequentialSearchData.resultTable) > 0;
	grn_table_delete(ctx,
					 prefixRKSequentialSearchData.resultTable,
					 &id, sizeof(grn_id));
	grn_table_delete(ctx,
					 prefixRKSequentialSearchData.table,
					 text, textSize);

	grn_obj_close(ctx, expression);

	return matched;
}

/**
 * pgroonga_prefix_rk_text(target text, prefix text) : bool
 */
Datum
pgroonga_prefix_rk_text(PG_FUNCTION_ARGS)
{
	text *target = PG_GETARG_TEXT_PP(0);
	text *prefix = PG_GETARG_TEXT_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_prefix_rk_raw(VARDATA_ANY(target),
										 VARSIZE_ANY_EXHDR(target),
										 VARDATA_ANY(prefix),
										 VARSIZE_ANY_EXHDR(prefix),
										 NULL,
										 0);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_prefix_rk_raw(VARDATA_ANY(target),
										 VARSIZE_ANY_EXHDR(target),
										 VARDATA_ANY(prefix),
										 VARSIZE_ANY_EXHDR(prefix),
										 NULL,
										 0);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_prefix_rk_text_array(targets text[], prefix text) : bool
 */
Datum
pgroonga_prefix_rk_text_array(PG_FUNCTION_ARGS)
{
	ArrayType *targets = PG_GETARG_ARRAYTYPE_P(0);
	text *prefix = PG_GETARG_TEXT_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_string_array(
				targets,
				VARDATA_ANY(prefix),
				VARSIZE_ANY_EXHDR(prefix),
				NULL,
				0,
				pgroonga_prefix_rk_raw,
				NULL);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_string_array(
				targets,
				VARDATA_ANY(prefix),
				VARSIZE_ANY_EXHDR(prefix),
				NULL,
				0,
				pgroonga_prefix_rk_raw,
				NULL);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_prefix_rk_varchar(target varchar, prefix varchar) : bool
 */
Datum
pgroonga_prefix_rk_varchar(PG_FUNCTION_ARGS)
{
	VarChar *target = PG_GETARG_VARCHAR_PP(0);
	VarChar *prefix = PG_GETARG_VARCHAR_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_prefix_rk_raw(VARDATA_ANY(target),
										 VARSIZE_ANY_EXHDR(target),
										 VARDATA_ANY(prefix),
										 VARSIZE_ANY_EXHDR(prefix),
										 NULL,
										 0);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_prefix_rk_raw(VARDATA_ANY(target),
										 VARSIZE_ANY_EXHDR(target),
										 VARDATA_ANY(prefix),
										 VARSIZE_ANY_EXHDR(prefix),
										 NULL,
										 0);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_prefix_rk_varchar_array(targets varchar[], prefix varchar) : bool
 */
Datum
pgroonga_prefix_rk_varchar_array(PG_FUNCTION_ARGS)
{
	ArrayType *targets = PG_GETARG_ARRAYTYPE_P(0);
	VarChar *prefix = PG_GETARG_VARCHAR_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_string_array(
				targets,
				VARDATA_ANY(prefix),
				VARSIZE_ANY_EXHDR(prefix),
				NULL,
				0,
				pgroonga_prefix_rk_raw,
				NULL);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_string_array(
				targets,
				VARDATA_ANY(prefix),
				VARSIZE_ANY_EXHDR(prefix),
				NULL,
				0,
				pgroonga_prefix_rk_raw,
				NULL);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_prefix_rk_contain_text_array(targets text[], prefix text) : bool
 *
 * It's deprecated since 1.2.1. Just for backward compatibility.
 */
Datum
pgroonga_prefix_rk_contain_text_array(PG_FUNCTION_ARGS)
{
	return pgroonga_prefix_rk_text_array(fcinfo);
}

/**
 * pgroonga_match_in_text(target text, keywords text[]) : bool
 */
Datum
pgroonga_match_in_text(PG_FUNCTION_ARGS)
{
	text *target = PG_GETARG_TEXT_PP(0);
	ArrayType *keywords = PG_GETARG_ARRAYTYPE_P(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_in_string(VARDATA_ANY(target),
													   VARSIZE_ANY_EXHDR(target),
													   keywords,
													   NULL,
													   0,
													   pgroonga_match_term_raw);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_in_string(VARDATA_ANY(target),
													   VARSIZE_ANY_EXHDR(target),
													   keywords,
													   NULL,
													   0,
													   pgroonga_match_term_raw);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_match_contain_text(target text, keywords text[]) : bool
 *
 * It's deprecated since 1.2.1. Just for backward compatibility.
 */
Datum
pgroonga_match_contain_text(PG_FUNCTION_ARGS)
{
	return pgroonga_match_in_text(fcinfo);
}

/**
 * pgroonga_match_in_text_array(targets text[], keywords text[]) : bool
 */
Datum
pgroonga_match_in_text_array(PG_FUNCTION_ARGS)
{
	ArrayType *targets = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType *keywords = PG_GETARG_ARRAYTYPE_P(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_in_string_array
			(targets,
			 keywords,
			 NULL,
			 0,
			 pgroonga_match_term_raw);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_in_string_array
			(targets,
			 keywords,
			 NULL,
			 0,
			 pgroonga_match_term_raw);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_match_in_varchar(target varchar, keywords varchar[]) : bool
 */
Datum
pgroonga_match_in_varchar(PG_FUNCTION_ARGS)
{
	VarChar *target = PG_GETARG_VARCHAR_PP(0);
	ArrayType *keywords = PG_GETARG_ARRAYTYPE_P(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_in_string(VARDATA_ANY(target),
													   VARSIZE_ANY_EXHDR(target),
													   keywords,
													   NULL,
													   0,
													   pgroonga_match_term_raw);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_in_string(VARDATA_ANY(target),
													   VARSIZE_ANY_EXHDR(target),
													   keywords,
													   NULL,
													   0,
													   pgroonga_match_term_raw);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_query_in_text(target text, queries text[]) : bool
 */
Datum
pgroonga_query_in_text(PG_FUNCTION_ARGS)
{
	text *target = PG_GETARG_TEXT_PP(0);
	ArrayType *queries = PG_GETARG_ARRAYTYPE_P(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_in_string(VARDATA_ANY(target),
													   VARSIZE_ANY_EXHDR(target),
													   queries,
													   NULL,
													   0,
													   pgroonga_match_query_raw);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_in_string(VARDATA_ANY(target),
													   VARSIZE_ANY_EXHDR(target),
													   queries,
													   NULL,
													   0,
													   pgroonga_match_query_raw);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_query_in_text(target text, queries text[]) : bool
 *
 * It's deprecated since 1.2.1. Just for backward compatibility.
 */
Datum
pgroonga_query_contain_text(PG_FUNCTION_ARGS)
{
	return pgroonga_query_in_text(fcinfo);
}

/**
 * pgroonga_query_in_text_array(targets text[], queries text[]) : bool
 */
Datum
pgroonga_query_in_text_array(PG_FUNCTION_ARGS)
{
	ArrayType *targets = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType *queries = PG_GETARG_ARRAYTYPE_P(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_in_string_array(
				targets,
				queries,
				NULL,
				0,
				pgroonga_match_query_raw);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_in_string_array(
				targets,
				queries,
				NULL,
				0,
				pgroonga_match_query_raw);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_query_in_varchar(target varchar, queries varchar[]) : bool
 */
Datum
pgroonga_query_in_varchar(PG_FUNCTION_ARGS)
{
	VarChar *target = PG_GETARG_VARCHAR_PP(0);
	ArrayType *queries = PG_GETARG_ARRAYTYPE_P(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_in_string(VARDATA_ANY(target),
													   VARSIZE_ANY_EXHDR(target),
													   queries,
													   NULL,
													   0,
													   pgroonga_match_query_raw);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_in_string(VARDATA_ANY(target),
													   VARSIZE_ANY_EXHDR(target),
													   queries,
													   NULL,
													   0,
													   pgroonga_match_query_raw);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_prefix_in_text(target text, prefixes text[]) : bool
 */
Datum
pgroonga_prefix_in_text(PG_FUNCTION_ARGS)
{
	text *target = PG_GETARG_TEXT_PP(0);
	ArrayType *prefixes = PG_GETARG_ARRAYTYPE_P(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_in_string(VARDATA_ANY(target),
													   VARSIZE_ANY_EXHDR(target),
													   prefixes,
													   NULL,
													   0,
													   pgroonga_prefix_raw);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_in_string(VARDATA_ANY(target),
													   VARSIZE_ANY_EXHDR(target),
													   prefixes,
													   NULL,
													   0,
													   pgroonga_prefix_raw);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_not_prefix_in_text(target text, prefixes text[]) : bool
 */
Datum
pgroonga_not_prefix_in_text(PG_FUNCTION_ARGS)
{
	text *target = PG_GETARG_TEXT_PP(0);
	ArrayType *prefixes = PG_GETARG_ARRAYTYPE_P(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_in_string(VARDATA_ANY(target),
													   VARSIZE_ANY_EXHDR(target),
													   prefixes,
													   NULL,
													   0,
													   pgroonga_prefix_raw);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_in_string(VARDATA_ANY(target),
													   VARSIZE_ANY_EXHDR(target),
													   prefixes,
													   NULL,
													   0,
													   pgroonga_prefix_raw);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(!matched);
}

/**
 * pgroonga_prefix_in_text_array(targets text[], prefixes text[]) : bool
 */
Datum
pgroonga_prefix_in_text_array(PG_FUNCTION_ARGS)
{
	ArrayType *targets = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType *prefixes = PG_GETARG_ARRAYTYPE_P(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_in_string_array(
				targets,
				prefixes,
				NULL,
				0,
				pgroonga_prefix_raw);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_in_string_array(
				targets,
				prefixes,
				NULL,
				0,
				pgroonga_prefix_raw);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_prefix_in_varchar(target varchar, prefixes varchar[]) : bool
 */
Datum
pgroonga_prefix_in_varchar(PG_FUNCTION_ARGS)
{
	VarChar *target = PG_GETARG_VARCHAR_PP(0);
	ArrayType *prefixes = PG_GETARG_ARRAYTYPE_P(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_in_string(VARDATA_ANY(target),
													   VARSIZE_ANY_EXHDR(target),
													   prefixes,
													   NULL,
													   0,
													   pgroonga_prefix_raw);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_in_string(VARDATA_ANY(target),
													   VARSIZE_ANY_EXHDR(target),
													   prefixes,
													   NULL,
													   0,
													   pgroonga_prefix_raw);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_prefix_in_varchar_array(targets varchar[], prefixes varchar[]) : bool
 */
Datum
pgroonga_prefix_in_varchar_array(PG_FUNCTION_ARGS)
{
	ArrayType *targets = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType *prefixes = PG_GETARG_ARRAYTYPE_P(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_in_string_array(
				targets,
				prefixes,
				NULL,
				0,
				pgroonga_prefix_raw);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_in_string_array(
				targets,
				prefixes,
				NULL,
				0,
				pgroonga_prefix_raw);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_prefix_rk_in_text(target text, prefixes text[]) : bool
 */
Datum
pgroonga_prefix_rk_in_text(PG_FUNCTION_ARGS)
{
	text *target = PG_GETARG_TEXT_PP(0);
	ArrayType *prefixes = PG_GETARG_ARRAYTYPE_P(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_in_string(VARDATA_ANY(target),
													   VARSIZE_ANY_EXHDR(target),
													   prefixes,
													   NULL,
													   0,
													   pgroonga_prefix_rk_raw);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_in_string(VARDATA_ANY(target),
													   VARSIZE_ANY_EXHDR(target),
													   prefixes,
													   NULL,
													   0,
													   pgroonga_prefix_rk_raw);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_prefix_rk_in_text_array(targets text[], prefixes text[]) : bool
 */
Datum
pgroonga_prefix_rk_in_text_array(PG_FUNCTION_ARGS)
{
	ArrayType *targets = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType *prefixes = PG_GETARG_ARRAYTYPE_P(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_in_string_array(
				targets,
				prefixes,
				NULL,
				0,
				pgroonga_prefix_rk_raw);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_in_string_array(
				targets,
				prefixes,
				NULL,
				0,
				pgroonga_prefix_rk_raw);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_prefix_rk_in_varchar(target varchar, prefixes varchar[]) : bool
 */
Datum
pgroonga_prefix_rk_in_varchar(PG_FUNCTION_ARGS)
{
	VarChar *target = PG_GETARG_VARCHAR_PP(0);
	ArrayType *prefixes = PG_GETARG_ARRAYTYPE_P(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_in_string(VARDATA_ANY(target),
													   VARSIZE_ANY_EXHDR(target),
													   prefixes,
													   NULL,
													   0,
													   pgroonga_prefix_rk_raw);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_in_string(VARDATA_ANY(target),
													   VARSIZE_ANY_EXHDR(target),
													   prefixes,
													   NULL,
													   0,
													   pgroonga_prefix_rk_raw);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_prefix_rk_in_varchar_array(targets varchar[], prefixes varchar[]) : bool
 */
Datum
pgroonga_prefix_rk_in_varchar_array(PG_FUNCTION_ARGS)
{
	ArrayType *targets = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType *prefixes = PG_GETARG_ARRAYTYPE_P(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_in_string_array(
				targets,
				prefixes,
				NULL,
				0,
				pgroonga_prefix_rk_raw);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_in_string_array(
				targets,
				prefixes,
				NULL,
				0,
				pgroonga_prefix_rk_raw);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_regexp_text(target text, pattern text) : bool
 */
Datum
pgroonga_regexp_text(PG_FUNCTION_ARGS)
{
	text *target = PG_GETARG_TEXT_PP(0);
	text *pattern = PG_GETARG_TEXT_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_match_regexp_raw(VARDATA_ANY(target),
											VARSIZE_ANY_EXHDR(target),
											VARDATA_ANY(pattern),
											VARSIZE_ANY_EXHDR(pattern),
											NULL,
											0);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_match_regexp_raw(VARDATA_ANY(target),
											VARSIZE_ANY_EXHDR(target),
											VARDATA_ANY(pattern),
											VARSIZE_ANY_EXHDR(pattern),
											NULL,
											0);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_regexp_varchar(target varchar, pattern varchar) : bool
 */
Datum
pgroonga_regexp_varchar(PG_FUNCTION_ARGS)
{
	VarChar *target = PG_GETARG_VARCHAR_PP(0);
	VarChar *pattern = PG_GETARG_VARCHAR_PP(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_match_regexp_raw(VARDATA_ANY(target),
											VARSIZE_ANY_EXHDR(target),
											VARDATA_ANY(pattern),
											VARSIZE_ANY_EXHDR(pattern),
											NULL,
											0);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_match_regexp_raw(VARDATA_ANY(target),
											VARSIZE_ANY_EXHDR(target),
											VARDATA_ANY(pattern),
											VARSIZE_ANY_EXHDR(pattern),
											NULL,
											0);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_regexp_in_text(target text, patterns text[]) : bool
 */
Datum
pgroonga_regexp_in_text(PG_FUNCTION_ARGS)
{
	text *target = PG_GETARG_TEXT_PP(0);
	ArrayType *patterns = PG_GETARG_ARRAYTYPE_P(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched = pgroonga_execute_binary_operator_in_string(
			VARDATA_ANY(target),
			VARSIZE_ANY_EXHDR(target),
			patterns,
			NULL,
			0,
			pgroonga_match_regexp_raw);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched = pgroonga_execute_binary_operator_in_string(
			VARDATA_ANY(target),
			VARSIZE_ANY_EXHDR(target),
			patterns,
			NULL,
			0,
			pgroonga_match_regexp_raw);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}

/**
 * pgroonga_regexp_in_varchar(target varchar, patterns varchar[]) : bool
 */
Datum
pgroonga_regexp_in_varchar(PG_FUNCTION_ARGS)
{
	VarChar *target = PG_GETARG_VARCHAR_PP(0);
	ArrayType *patterns = PG_GETARG_ARRAYTYPE_P(1);
	bool matched = false;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabledSeqScan(fcinfo));
	{
		matched =
			pgroonga_execute_binary_operator_in_string(
				VARDATA_ANY(target),
				VARSIZE_ANY_EXHDR(target),
				patterns,
				NULL,
				0,
				pgroonga_match_regexp_raw);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		matched =
			pgroonga_execute_binary_operator_in_string(
				VARDATA_ANY(target),
				VARSIZE_ANY_EXHDR(target),
				patterns,
				NULL,
				0,
				pgroonga_match_regexp_raw);
	}
	PGRN_RLS_ENABLED_END();

	PG_RETURN_BOOL(matched);
}


static bool
PGrnNeedMaxRecordSizeUpdate(Relation index)
{
	TupleDesc desc = RelationGetDescr(index);
	unsigned int nVarCharColumns = 0;
	unsigned int i;

	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attribute;

		attribute = TupleDescAttr(desc, i);
		switch (attribute->atttypid)
		{
		case VARCHAROID:
			nVarCharColumns++;
			break;
		case TEXTOID:
		case VARCHARARRAYOID:
		case TEXTARRAYOID:
			return true;
			break;
		default:
			break;
		}
	}

	return nVarCharColumns >= 2;
}

#define PGRN_INDEX_ONLY_SCAN_THRESHOLD_SIZE (INDEX_SIZE_MASK * 0.9)

static void
PGrnUpdateMaxRecordSize(Relation index,
						uint32_t recordSize)
{
	uint32_t currentMaxRecordSize;

	if (recordSize < PGRN_INDEX_ONLY_SCAN_THRESHOLD_SIZE)
		return;

	currentMaxRecordSize = PGrnIndexStatusGetMaxRecordSize(index);
	if (recordSize < currentMaxRecordSize)
		return;

	PGrnIndexStatusSetMaxRecordSize(index, recordSize);
}

static uint32_t
PGrnComputeSize(grn_obj *buffer)
{
	switch (buffer->header.type)
	{
	case GRN_BULK:
		return GRN_BULK_VSIZE(buffer);
	case GRN_UVECTOR:
	{
		const unsigned int element_size = grn_uvector_element_size(ctx, buffer);
		const unsigned int n_elements = grn_uvector_size(ctx, buffer);
		return element_size * n_elements;
	}
	case GRN_VECTOR:
	{
		uint32_t size = 0;
		unsigned int i;
		const unsigned int n = grn_vector_size(ctx, buffer);
		for (i = 0; i < n; i++)
		{
			const char *value;
			size += grn_vector_get_element(ctx, buffer, i, &value, NULL, NULL);
		}
		return size;
	}
	default:
		return 0;
	}
}

static uint32_t
PGrnInsertColumn(Relation index,
				 grn_obj *sourcesTable,
				 Datum *values,
				 PGrnWALData *walData,
				 unsigned int i,
				 grn_id id,
				 const char *tag)
{
	TupleDesc desc = RelationGetDescr(index);
	Form_pg_attribute attribute = TupleDescAttr(desc, i);
	NameData *name = &(attribute->attname);
	grn_obj *dataColumn = PGrnLookupColumn(sourcesTable, name->data, ERROR);
	grn_obj *rawValue = &(buffers->general);
	grn_obj *value;
	grn_id rawDomain;
	unsigned char flags;
	grn_id domain;

	rawDomain = PGrnGetType(index, i, &flags);
	grn_obj_reinit(ctx, rawValue, rawDomain, flags);
	PGrnConvertFromData(values[i], attribute->atttypid, rawValue);
	domain = grn_obj_get_range(ctx, dataColumn);
	if (domain == rawDomain)
	{
		value = rawValue;
	}
	else
	{
		grn_rc rc = GRN_SUCCESS;
		grn_obj rawElement;
		grn_obj *element = &(buffers->castElement);
		GRN_VOID_INIT(&rawElement);
		value = &(buffers->cast);
		grn_obj_reinit(ctx, value, domain, flags);
		if (grn_obj_is_vector(ctx, rawValue))
		{
			uint32_t n = grn_vector_size(ctx, rawValue);
			uint32_t i;
			for (i = 0; i < n; i++)
			{
				const char *elementValue;
				float weight;
				grn_id elementDomain;
				uint32_t elementSize =
					grn_vector_get_element_float(ctx,
												 rawValue,
												 i,
												 &elementValue,
												 &weight,
												 &elementDomain);
				GRN_OBJ_FIN(ctx, &rawElement);
				GRN_VALUE_VAR_SIZE_INIT(&rawElement,
										GRN_OBJ_DO_SHALLOW_COPY,
										elementDomain);
				GRN_TEXT_SET(ctx,
							 &rawElement,
							 elementValue,
							 elementSize);
				grn_obj_reinit(ctx, element, domain, 0);
				rc = grn_obj_cast(ctx, &rawElement, element, true);
				if (rc != GRN_SUCCESS)
					break;
				grn_vector_add_element_float(ctx,
											 value,
											 GRN_BULK_HEAD(element),
											 GRN_BULK_VSIZE(element),
											 weight,
											 domain);
			}
		}
		else if (grn_obj_is_uvector(ctx, value))
		{
			uint32_t elementSize = grn_uvector_element_size(ctx, rawValue);
			uint32_t n = grn_uvector_size(ctx, rawValue);
			uint32_t i;
			GRN_OBJ_FIN(ctx, &rawElement);
			GRN_VALUE_FIX_SIZE_INIT(&rawElement,
									GRN_OBJ_DO_SHALLOW_COPY,
									rawDomain);
			grn_obj_reinit(ctx, element, domain, 0);
			for (i = 0; i < n; i++)
			{
				GRN_TEXT_SET(ctx,
							 &rawElement,
							 GRN_BULK_HEAD(rawValue) + (elementSize * i),
							 elementSize);
				GRN_BULK_REWIND(element);
				rc = grn_obj_cast(ctx, &rawElement, element, true);
				if (rc != GRN_SUCCESS)
					break;
				grn_bulk_write(ctx,
							   value,
							   GRN_BULK_HEAD(element),
							   GRN_BULK_VSIZE(element));
			}
		}
		else
		{
			rc = grn_obj_cast(ctx, rawValue, value, true);
		}
		GRN_OBJ_FIN(ctx, &rawElement);

		if (rc != GRN_SUCCESS)
		{
			grn_obj *inspected = &(buffers->inspect);
			GRN_BULK_REWIND(inspected);
			grn_inspect(ctx, inspected, rawValue);
			elog(WARNING,
				 "pgroonga: %s <%s.%s>: failed to cast: <%.*s>",
				 tag,
				 index->rd_rel->relname.data,
				 name->data,
				 (int)GRN_TEXT_LEN(inspected),
				 GRN_TEXT_VALUE(inspected));
			return 0;
		}
	}

	grn_obj_set_value(ctx, dataColumn, id, value, GRN_OBJ_SET);
	PGrnCheck("%s failed to set column value", tag);

	PGrnWALInsertColumn(walData, dataColumn, rawValue);

	return PGrnComputeSize(value);
}

static uint32_t
PGrnInsert(Relation index,
		   grn_obj *sourcesTable,
		   grn_obj *sourcesCtidColumn,
		   Datum *values,
		   bool *isnull,
		   ItemPointer ht_ctid)
{
	const char *tag = "[insert]";
	TupleDesc desc = RelationGetDescr(index);
	grn_id id;
	PGrnWALData *walData;
	uint64 packedCtid = PGrnCtidPack(ht_ctid);
	unsigned int i;
	uint32_t recordSize = 0;

	if (desc->natts == 1 &&
		PGrnAttributeIsJSONB(TupleDescAttr(desc, 0)->atttypid))
	{
		return PGrnJSONBInsert(index,
							   sourcesTable,
							   sourcesCtidColumn,
							   values,
							   isnull,
							   PGrnCtidPack(ht_ctid));
	}

	walData = PGrnWALStart(index);
	{
		size_t nValidAttributes = 1; /* ctid is always valid. */

		for (i = 0; i < desc->natts; i++)
		{
			if (!isnull[i])
				nValidAttributes++;
		}
		PGrnWALInsertStart(walData, sourcesTable, nValidAttributes);
	}

	PG_TRY();
	{
		if (sourcesTable->header.type == GRN_TABLE_NO_KEY)
		{
			id = grn_table_add(ctx, sourcesTable, NULL, 0, NULL);
			PGrnCheck("%s failed to add a record", tag);
			if (id == GRN_ID_NIL)
			{
				PGrnCheckRC(GRN_UNKNOWN_ERROR,
							"%s failed to add a record",
							tag);
			}
			GRN_UINT64_SET(ctx, &(buffers->ctid), packedCtid);
			grn_obj_set_value(ctx,
							  sourcesCtidColumn,
							  id,
							  &(buffers->ctid),
							  GRN_OBJ_SET);
			PGrnCheck("%s failed to set ctid value: <%u>: <%" PRIu64 ">",
					  tag,
					  id,
					  packedCtid);
			GRN_LOG(ctx,
					GRN_LOG_DEBUG,
					"pgroonga: "
					"%s[array] <%s>(%u): <%u>: <(%u,%u),%u>(%" PRIu64 ")",
					tag,
					index->rd_rel->relname.data,
					index->rd_id,
					id,
					ht_ctid->ip_blkid.bi_hi,
					ht_ctid->ip_blkid.bi_lo,
					ht_ctid->ip_posid,
					packedCtid);
			PGrnWALInsertColumn(walData, sourcesCtidColumn, &(buffers->ctid));
		}
		else
		{
			id = grn_table_add(ctx,
							   sourcesTable,
							   &packedCtid,
							   sizeof(uint64),
							   NULL);
			PGrnCheck("%s failed to add a record: <%" PRIu64 ">",
					  tag,
					  packedCtid);
			if (id == GRN_ID_NIL)
			{
				PGrnCheckRC(GRN_UNKNOWN_ERROR,
							"%s failed to add a record: <%" PRIu64 ">",
							tag,
							packedCtid);
			}
			PGrnWALInsertKeyRaw(walData, &packedCtid, sizeof(uint64));
			GRN_LOG(ctx,
					GRN_LOG_DEBUG,
					"pgroonga: %s <%s>(%u): <%u>: <(%u,%u),%u>(%" PRIu64 ")",
					tag,
					index->rd_rel->relname.data,
					index->rd_id,
					id,
					ht_ctid->ip_blkid.bi_hi,
					ht_ctid->ip_blkid.bi_lo,
					ht_ctid->ip_posid,
					packedCtid);
		}

		for (i = 0; i < desc->natts; i++)
		{
			if (isnull[i])
				continue;
			recordSize += PGrnInsertColumn(index,
										   sourcesTable,
										   values,
										   walData,
										   i,
										   id,
										   tag);
		}

		PGrnWALInsertFinish(walData);
		PGrnWALFinish(walData);
	}
	PG_CATCH();
	{
		PGrnWALAbort(walData);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return recordSize;
}

static bool
pgroonga_insert_raw(Relation index,
					Datum *values,
					bool *isnull,
					ItemPointer ctid,
					Relation heap,
					IndexUniqueCheck checkUnique,
#ifdef PGRN_AM_INSERT_HAVE_INDEX_UNCHANGED
					bool indexUnchanged,
#endif
					struct IndexInfo *indexInfo)
{
	const char *tag = "[insert]";
	grn_obj *sourcesTable;
	grn_obj *sourcesCtidColumn = NULL;
	uint32_t recordSize;

	if (!PGrnIsWritable())
	{
		ereport(ERROR,
				(errcode(ERRCODE_E_R_E_MODIFYING_SQL_DATA_NOT_PERMITTED),
				 errmsg("pgroonga: %s "
						"can't insert a record "
						"while pgroonga.writable is false",
						tag)));
	}

	PGrnWALApply(index);

	sourcesTable = PGrnLookupSourcesTable(index, ERROR);
	if (sourcesTable->header.type == GRN_TABLE_NO_KEY)
	{
		sourcesCtidColumn = PGrnLookupSourcesCtidColumn(index, ERROR);
	}
	recordSize = PGrnInsert(index,
							sourcesTable,
							sourcesCtidColumn,
							values,
							isnull,
							ctid);
	if (PGrnNeedMaxRecordSizeUpdate(index))
		PGrnUpdateMaxRecordSize(index, recordSize);
	grn_db_touch(ctx, grn_ctx_db(ctx));

	return false;
}

/**
 * pgroonga.insert() -- aminsert
 */
Datum
pgroonga_insert(PG_FUNCTION_ARGS)
{
	Relation index = (Relation) PG_GETARG_POINTER(0);
	Datum *values = (Datum *) PG_GETARG_POINTER(1);
	bool *isnull = (bool *) PG_GETARG_POINTER(2);
	ItemPointer ctid = (ItemPointer) PG_GETARG_POINTER(3);
	Relation heap = (Relation) PG_GETARG_POINTER(4);
	IndexUniqueCheck uniqueCheck = PG_GETARG_INT32(5);
	bool isUnique;

	isUnique = pgroonga_insert_raw(index,
								   values,
								   isnull,
								   ctid,
								   heap,
								   uniqueCheck,
#ifdef PGRN_AM_INSERT_HAVE_INDEX_UNCHANGED
								   false,
#endif
								   NULL);

	PG_RETURN_BOOL(isUnique);
}

static void
PGrnPrimaryKeyColumnsFin(slist_head *columns)
{
	while (!slist_is_empty(columns))
	{
		slist_node *current;
		PGrnPrimaryKeyColumn *column;

		current = slist_pop_head_node(columns);
		column = slist_container(PGrnPrimaryKeyColumn, node, current);
		free(column);
	}
}

static void
PGrnPrimaryKeyColumnsInit(slist_head *columns,
						  PGrnScanOpaque so)
{
	Relation table;
	List *indexOIDList;
	ListCell *cell;

	table = RelationIdGetRelation(so->dataTableID);
	indexOIDList = RelationGetIndexList(table);
	foreach(cell, indexOIDList)
	{
		const LOCKMODE lockMode = AccessShareLock;
		Oid indexOID = lfirst_oid(cell);
		Relation primaryKeyIndex;
		int i;

		primaryKeyIndex = index_open(indexOID, lockMode);
		if (!primaryKeyIndex->rd_index->indisprimary) {
			index_close(primaryKeyIndex, lockMode);
			continue;
		}

		for (i = 0; i < primaryKeyIndex->rd_index->indnatts; i++)
		{
			Oid primaryKeyNumber;
			int j;
			bool havePrimaryKey = false;

			primaryKeyNumber = primaryKeyIndex->rd_index->indkey.values[i];

			for (j = 0; j < so->index->rd_index->indnatts; j++)
			{
				TupleDesc desc;
				const char *columnName;
				PGrnPrimaryKeyColumn *primaryKeyColumn;

				if (so->index->rd_index->indkey.values[j] != primaryKeyNumber)
					continue;

				primaryKeyColumn =
					(PGrnPrimaryKeyColumn *) malloc(sizeof(PGrnPrimaryKeyColumn));

				desc = RelationGetDescr(table);
				columnName = TupleDescAttr(so->index->rd_att, j)->attname.data;

				primaryKeyColumn->number = primaryKeyNumber;
				primaryKeyColumn->type =
					TupleDescAttr(desc, primaryKeyNumber - 1)->atttypid;
				primaryKeyColumn->domain = PGrnGetType(primaryKeyIndex,
													   i,
													   &(primaryKeyColumn->flags));
				primaryKeyColumn->column = grn_obj_column(ctx,
														  so->sourcesTable,
														  columnName,
														  strlen(columnName));
				slist_push_head(columns, &(primaryKeyColumn->node));
				havePrimaryKey = true;
				break;
			}

			if (!havePrimaryKey)
			{
				PGrnPrimaryKeyColumnsFin(columns);
				break;
			}
		}

		index_close(primaryKeyIndex, lockMode);
		break;
	}
	list_free(indexOIDList);
	RelationClose(table);
}

static void
PGrnScanOpaqueInitPrimaryKeyColumns(PGrnScanOpaque so)
{
	slist_init(&(so->primaryKeyColumns));
	PGrnPrimaryKeyColumnsInit(&(so->primaryKeyColumns), so);
}

static void
PGrnScanOpaqueInit(PGrnScanOpaque so, Relation index)
{
	GRN_LOG(ctx, GRN_LOG_DEBUG,
			"pgroonga: [initialize][scan-opaque][start] %u",
			PGrnNScanOpaques);

	so->index = index;

	so->memoryContext =
		AllocSetContextCreate(CurrentMemoryContext,
							  "PGroonga scan opaque temporay context",
							  ALLOCSET_DEFAULT_SIZES);

	so->dataTableID = index->rd_index->indrelid;
	so->sourcesTable = PGrnLookupSourcesTable(index, ERROR);
	if (so->sourcesTable->header.type == GRN_TABLE_NO_KEY)
	{
		so->sourcesCtidColumn = PGrnLookupSourcesCtidColumn(index, ERROR);
	}
	else
	{
		so->sourcesCtidColumn = NULL;
	}
	so->ctidResolveTable = NULL;
	GRN_VOID_INIT(&(so->minBorderValue));
	GRN_VOID_INIT(&(so->maxBorderValue));
	so->searched = NULL;
	so->sorted = NULL;
	so->targetTable = NULL;
	so->indexCursor = NULL;
	so->tableCursor = NULL;
	so->ctidAccessor = NULL;
	so->scoreAccessor = NULL;
	so->currentID = GRN_ID_NIL;

	GRN_BOOL_INIT(&(so->canReturns), GRN_OBJ_VECTOR);

	dlist_push_head(&PGrnScanOpaques, &(so->node));
	PGrnNScanOpaques++;
	PGrnScanOpaqueInitPrimaryKeyColumns(so);
	so->scoreTargetRecords = NULL;

	GRN_LOG(ctx, GRN_LOG_DEBUG,
			"pgroonga: [initialize][scan-opaque][end] %u: <%p>",
			PGrnNScanOpaques,
			so);
}

static void
PGrnScanOpaqueReinit(PGrnScanOpaque so)
{
	so->currentID = GRN_ID_NIL;
	if (so->scoreAccessor)
	{
		grn_obj_unlink(ctx, so->scoreAccessor);
		so->scoreAccessor = NULL;
	}
	if (so->ctidAccessor)
	{
		grn_obj_unlink(ctx, so->ctidAccessor);
		so->ctidAccessor = NULL;
	}
	if (so->indexCursor)
	{
		grn_obj_close(ctx, so->indexCursor);
		so->indexCursor = NULL;
	}
	if (so->tableCursor)
	{
		grn_table_cursor_close(ctx, so->tableCursor);
		so->tableCursor = NULL;
	}
	GRN_BULK_REWIND(&(so->minBorderValue));
	GRN_BULK_REWIND(&(so->maxBorderValue));
	if (so->ctidResolveTable)
	{
		grn_obj_close(ctx, so->ctidResolveTable);
		so->ctidResolveTable = NULL;
	}
	if (so->sorted)
	{
		grn_obj_close(ctx, so->sorted);
		so->sorted = NULL;
	}
	if (so->searched)
	{
		grn_obj_close(ctx, so->searched);
		so->searched = NULL;
	}
	GRN_BULK_REWIND(&(so->canReturns));
}

static void
PGrnScanOpaqueFin(PGrnScanOpaque so)
{
	GRN_LOG(ctx, GRN_LOG_DEBUG,
			"pgroonga: [finalize][scan-opaque][start] %u: <%p>",
			PGrnNScanOpaques,
			so);

	dlist_delete(&(so->node));
	PGrnNScanOpaques--;

	PGrnPrimaryKeyColumnsFin(&(so->primaryKeyColumns));
	if (so->scoreTargetRecords)
	{
		grn_obj_close(ctx, so->scoreTargetRecords);
		so->scoreTargetRecords = NULL;
	}

	PGrnScanOpaqueReinit(so);

	GRN_OBJ_FIN(ctx, &(so->minBorderValue));
	GRN_OBJ_FIN(ctx, &(so->maxBorderValue));

	GRN_OBJ_FIN(ctx, &(so->canReturns));

	free(so);

	GRN_LOG(ctx, GRN_LOG_DEBUG,
			"pgroonga: [finalize][scan-opaque][end] %u: <%p>",
			PGrnNScanOpaques,
			so);
}

static IndexScanDesc
pgroonga_beginscan_raw(Relation index,
					   int nKeys,
					   int nOrderBys)
{
	IndexScanDesc scan;
	PGrnScanOpaque so;

	scan = RelationGetIndexScan(index, nKeys, nOrderBys);

	so = (PGrnScanOpaque) malloc(sizeof(PGrnScanOpaqueData));
	PGrnScanOpaqueInit(so, index);

	GRN_LOG(ctx, GRN_LOG_DEBUG,
			"pgroonga: [scan][begin] <%p>",
			so);

	scan->opaque = so;

	return scan;
}

/**
 * pgroonga.beginscan() -- ambeginscan
 */
Datum
pgroonga_beginscan(PG_FUNCTION_ARGS)
{
	Relation index = (Relation) PG_GETARG_POINTER(0);
	int nKeys = PG_GETARG_INT32(1);
	int nOrderBys = PG_GETARG_INT32(2);
	IndexScanDesc scan;

	scan = pgroonga_beginscan_raw(index,
								  nKeys,
								  nOrderBys);

	PG_RETURN_POINTER(scan);
}

static bool
PGrnSearchIsInCondition(ScanKey key)
{
	return (key->sk_flags & SK_SEARCHARRAY &&
			key->sk_strategy == PGrnEqualStrategyNumber);
}

static bool
PGrnSearchBuildConditionIn(PGrnSearchData *data,
						   ScanKey key,
						   grn_obj *targetColumn,
						   Form_pg_attribute attribute)
{
	const char *tag = "[build-condition][in]";
	const char *tag_any = "[build-condition][any]";
	ArrayType *values;
	int n_dimensions;
	grn_id domain;
	unsigned char flags = 0;
	int i, n;

	values = DatumGetArrayTypeP(key->sk_argument);
	n_dimensions = ARR_NDIM(values);
	switch (n_dimensions)
	{
	case 0 :
		grn_obj_reinit(ctx, &(buffers->general), GRN_DB_BOOL, 0);
		GRN_BOOL_SET(ctx, &(buffers->general), GRN_FALSE);
		grn_expr_append_const(ctx,
							  data->expression,
							  &(buffers->general),
							  GRN_OP_PUSH,
							  0);
		PGrnCheck("%s failed to push false value",
				  tag_any);
		return true;
		break;
	case 1 :
		/* OK */
		break;
	default :
		PGrnCheckRC(GRN_FUNCTION_NOT_IMPLEMENTED,
					"%s 2 or more dimensions array isn't supported yet: %d",
					tag,
					n_dimensions);
		break;
	}

	domain = PGrnPGTypeToGrnType(attribute->atttypid, &flags);
	grn_obj_reinit(ctx, &(buffers->general), domain, flags);
	n = ARR_DIMS(values)[0];

	grn_expr_append_obj(ctx, data->expression,
						PGrnLookup("in_values", ERROR),
						GRN_OP_PUSH,
						1);
	PGrnCheck("%s failed to push in_values()", tag);
	grn_expr_append_obj(ctx, data->expression,
						targetColumn,
						GRN_OP_PUSH,
						1);
	PGrnCheck("%s failed to push target column", tag);
	grn_expr_append_op(ctx, data->expression, GRN_OP_GET_VALUE, 1);
	PGrnCheck("%s failed to push GET_VALUE", tag);

	for (i = 1; i <= n; i++)
	{
		Datum valueDatum;
		bool isNULL;

		valueDatum = array_ref(values, 1, &i, -1,
							   attribute->attlen,
							   attribute->attbyval,
							   attribute->attalign,
							   &isNULL);
		if (isNULL)
			return false;

		PGrnConvertFromData(valueDatum,
							attribute->atttypid,
							&(buffers->general));
		grn_expr_append_const(ctx,
							  data->expression,
							  &(buffers->general),
							  GRN_OP_PUSH,
							  1);
		PGrnCheck("%s failed to push a value", tag);
	}

	grn_expr_append_op(ctx, data->expression, GRN_OP_CALL, 2 + (n - 1));
	PGrnCheck("%s failed to push CALL", tag);

	return true;
}

static bool
PGrnSearchBuildConditionPrepareConditionBuildMatchColumns(PGrnSearchData *data,
														  grn_obj *matchColumns,
														  grn_index_datum *indexData,
														  ArrayType *weights,
														  ArrayType *scorers,
														  const char *tag)
{
	char indexName[GRN_TABLE_MAX_KEY_SIZE];
	ArrayIterator weightsIterator = NULL;
	ArrayIterator scorersIterator = NULL;
	int section;
	int nMatchColumns = 0;
	grn_obj *substitutedScorer = &(buffers->general);

	{
		int nameSize;
		nameSize = grn_obj_name(ctx,
								indexData->index,
								indexName,
								GRN_TABLE_MAX_KEY_SIZE);
		indexName[nameSize] = '\0';
	}

	if (weights)
		weightsIterator = pgrn_array_create_iterator(weights, 0);
	if (scorers)
		scorersIterator = pgrn_array_create_iterator(scorers, 0);

	section = -1;
	grn_obj_reinit(ctx, substitutedScorer, GRN_DB_TEXT, 0);
	while (true)
	{
		Datum weightDatum;
		bool weightIsNULL = true;
		Datum scorerDatum;
		bool scorerIsNULL = true;
		int32 weight = 1;
		text *scorer = NULL;

		section++;

		if (weightsIterator)
		{
			if (!array_iterate(weightsIterator, &weightDatum, &weightIsNULL))
			{
				array_free_iterator(weightsIterator);
				weightsIterator = NULL;
			}
		}
		if (scorersIterator)
		{
			if (!array_iterate(scorersIterator, &scorerDatum, &scorerIsNULL))
			{
				array_free_iterator(scorersIterator);
				scorersIterator = NULL;
			}
		}

		if (!weightsIterator && !scorersIterator)
			break;

		if (!weightIsNULL)
			weight = DatumGetInt32(weightDatum);
		if (!scorerIsNULL)
			scorer = DatumGetTextPP(scorerDatum);

		if (weight == 0)
			continue;

		if (scorer)
		{
			GRN_BULK_REWIND(substitutedScorer);
			PGrnStringSubstituteIndex(VARDATA_ANY(scorer),
									  VARSIZE_ANY_EXHDR(scorer),
									  substitutedScorer,
									  indexName,
									  section);
			grn_expr_parse(ctx,
						   matchColumns,
						   GRN_TEXT_VALUE(substitutedScorer),
						   GRN_TEXT_LEN(substitutedScorer),
						   NULL,
						   GRN_OP_MATCH,
						   GRN_OP_AND,
						   GRN_EXPR_SYNTAX_SCRIPT);
			PGrnCheck("%s failed to parse scorer: <%s>[%d]: <%.*s>",
					  tag,
					  indexName, section,
					  (int)VARSIZE_ANY_EXHDR(scorer),
					  VARDATA_ANY(scorer));
		}
		else
		{
			grn_expr_append_obj(ctx,
								matchColumns,
								indexData->index,
								GRN_OP_PUSH,
								1);
			PGrnCheck("%s failed to push index: <%s>[%d]",
					  tag,
					  indexName, section);
			grn_expr_append_const_int(ctx,
									  matchColumns,
									  section,
									  GRN_OP_PUSH,
									  1);
			PGrnCheck("%s failed to push section of index: <%s>[%d]",
					  tag,
					  indexName, section);
			grn_expr_append_op(ctx, matchColumns, GRN_OP_GET_MEMBER, 2);
			PGrnCheck("%s failed to push get-member operator: <%s>[%d]",
					  tag,
					  indexName, section);
		}
		if (weight != 1)
		{
			grn_expr_append_const_int(ctx,
									  matchColumns,
									  weight,
									  GRN_OP_PUSH,
									  1);
			PGrnCheck("%s failed to push weight: <%s>[%d] * <%d>",
					  tag,
					  indexName, section, weight);
			grn_expr_append_op(ctx, matchColumns, GRN_OP_STAR, 2);
			PGrnCheck("%s failed to push star operator for weight: "
					  "<%s>[%d] * <%d>",
					  tag,
					  indexName, section, weight);
		}

		if (nMatchColumns > 0)
		{
			grn_expr_append_op(ctx, matchColumns, GRN_OP_OR, 2);
			PGrnCheck("%s failed to push OR operator: <%s>[%d]",
					  tag,
					  indexName, section);
		}

		nMatchColumns++;
	}
	if (weightsIterator)
		array_free_iterator(weightsIterator);
	if (scorersIterator)
		array_free_iterator(scorersIterator);

	return true;
}

static bool
PGrnSearchBuildConditionPrepareCondition(PGrnSearchData *data,
										 ScanKey key,
										 grn_obj *targetColumn,
										 Form_pg_attribute attribute,
										 text **query,
										 grn_obj **matchTarget,
										 const char *tag)
{
	grn_index_datum indexData;
	unsigned int nIndexData;
	HeapTupleHeader header;
	ArrayType *weights = NULL;
	ArrayType *scorers = NULL;

	nIndexData = grn_column_find_index_data(ctx,
											targetColumn,
											GRN_OP_MATCH,
											&indexData,
											1);
	if (nIndexData == 0)
	{
		PGrnCheckRC(GRN_OBJECT_CORRUPT,
					"%s index doesn't exist for target column: <%s>",
					tag,
					PGrnInspectName(targetColumn));
		return false;
	}

	header = DatumGetHeapTupleHeader(key->sk_argument);
	if (key->sk_strategy == PGrnQueryConditionStrategyV2Number ||
		key->sk_strategy == PGrnMatchConditionStrategyV2Number)
	{
		PGrnFullTextSearchConditionDeconstruct(header,
											   query,
											   &weights,
											   NULL,
											   NULL);
	}
	else
	{
		PGrnFullTextSearchConditionWithScorersDeconstruct(header,
														  query,
														  &weights,
														  &scorers,
														  NULL,
														  NULL);
	}

	if (!query)
	{
		PGrnCheckRC(GRN_INVALID_ARGUMENT,
					"%s query must not NULL",
					tag);
		return false;
	}

	if (!weights && !scorers)
	{
		*matchTarget = targetColumn;
		return true;
	}

	if (weights && ARR_NDIM(weights) == 0)
	{
		PGrnCheckRC(GRN_INVALID_ARGUMENT,
					"%s weights must not empty array",
					tag);
		return false;
	}

	{
		grn_obj *matchColumns, *matchColumnsVariable;

		GRN_EXPR_CREATE_FOR_QUERY(ctx,
								  data->sourcesTable,
								  matchColumns,
								  matchColumnsVariable);
		GRN_PTR_PUT(ctx, &(data->matchTargets), matchColumns);

		if (!PGrnSearchBuildConditionPrepareConditionBuildMatchColumns(
				data,
				matchColumns,
				&indexData,
				weights,
				scorers,
				tag))
			return false;

		*matchTarget = matchColumns;
		return true;
	}
}

static bool
PGrnSearchBuildConditionMatchCondition(PGrnSearchData *data,
									   ScanKey key,
									   grn_obj *targetColumn,
									   Form_pg_attribute attribute)
{
	const char *tag = "[build-condition][match-condition]";
	text *term;
	grn_obj *matchTarget;

	if (!PGrnSearchBuildConditionPrepareCondition(data,
												  key,
												  targetColumn,
												  attribute,
												  &term,
												  &matchTarget,
												  tag))
	{
		return false;
	}

	grn_expr_append_obj(ctx,
						data->expression,
						matchTarget,
						GRN_OP_PUSH,
						1);
	if (matchTarget == targetColumn)
	{
		PGrnCheck("%s failed to push column to be matched: <%s>",
				  tag,
				  PGrnInspectName(targetColumn));
	}
	else
	{
		PGrnCheck("%s failed to push match columns",
				  tag);
	}
	grn_expr_append_const_str(ctx,
							  data->expression,
							  VARDATA_ANY(term),
							  VARSIZE_ANY_EXHDR(term),
							  GRN_OP_PUSH,
							  1);
	PGrnCheck("%s failed to push query: <%.*s>",
			  tag,
			  (int) VARSIZE_ANY_EXHDR(term),
			  VARDATA_ANY(term));
	grn_expr_append_op(ctx,
					   data->expression,
					   GRN_OP_MATCH,
					   1);
	PGrnCheck("%s failed to push MATCH operator",
			  tag);

	return true;
}

static bool
PGrnSearchBuildConditionQueryCondition(PGrnSearchData *data,
									   ScanKey key,
									   grn_obj *targetColumn,
									   Form_pg_attribute attribute)
{
	const char *tag = "[build-condition][query-condition]";
	text *query;
	grn_obj *matchTarget;
	grn_expr_flags flags = PGRN_EXPR_QUERY_PARSE_FLAGS;

	if (!PGrnSearchBuildConditionPrepareCondition(data,
												  key,
												  targetColumn,
												  attribute,
												  &query,
												  &matchTarget,
												  tag))
	{
		return false;
	}

	flags |= PGrnOptionsGetExprParseFlags(data->index);
	grn_expr_parse(ctx,
				   data->expression,
				   VARDATA_ANY(query),
				   VARSIZE_ANY_EXHDR(query),
				   matchTarget,
				   GRN_OP_MATCH,
				   GRN_OP_AND,
				   flags);
	PGrnCheck("%s failed to parse query: <%.*s>",
			  tag,
			  (int) VARSIZE_ANY_EXHDR(query),
			  VARDATA_ANY(query));

	return true;
}

static void
PGrnSearchBuildConditionLikeMatchFlush(grn_obj *expression,
									   grn_obj *targetColumn,
									   grn_obj *keyword,
									   int *nKeywords)
{
	if (GRN_TEXT_LEN(keyword) == 0)
		return;

	grn_expr_append_obj(ctx, expression, targetColumn, GRN_OP_PUSH, 1);
	grn_expr_append_op(ctx, expression, GRN_OP_GET_VALUE, 1);
	grn_expr_append_const_str(ctx, expression,
							  GRN_TEXT_VALUE(keyword),
							  GRN_TEXT_LEN(keyword),
							  GRN_OP_PUSH, 1);
	grn_expr_append_op(ctx, expression, GRN_OP_MATCH, 2);
	if (*nKeywords > 0)
		grn_expr_append_op(ctx, expression, GRN_OP_OR, 2);
	(*nKeywords)++;

	GRN_BULK_REWIND(keyword);
}

static void
PGrnSearchBuildConditionLikeMatch(PGrnSearchData *data,
								  grn_obj *targetColumn,
								  grn_obj *query)
{
	grn_obj *expression;
	const char *queryRaw;
	size_t i, querySize;
	int nKeywords = 0;

	expression = data->expression;
	queryRaw = GRN_TEXT_VALUE(query);
	querySize = GRN_TEXT_LEN(query);

	if (querySize == 0)
	{
		data->isEmptyCondition = true;
		return;
	}

	GRN_BULK_REWIND(&(buffers->keyword));
	for (i = 0; i < querySize; i++)
	{
		switch (queryRaw[i])
		{
		case '\\':
			if (i == querySize)
			{
				GRN_TEXT_PUTC(ctx, &(buffers->keyword), '\\');
			}
			else
			{
				GRN_TEXT_PUTC(ctx, &(buffers->keyword), queryRaw[i + 1]);
				i++;
			}
			break;
		case '%':
		case '_':
			PGrnSearchBuildConditionLikeMatchFlush(expression,
												   targetColumn,
												   &(buffers->keyword),
												   &nKeywords);
			break;
		default:
			GRN_TEXT_PUTC(ctx, &(buffers->keyword), queryRaw[i]);
			break;
		}
	}

	PGrnSearchBuildConditionLikeMatchFlush(expression,
										   targetColumn,
										   &(buffers->keyword),
										   &nKeywords);
	if (nKeywords == 0)
	{
		grn_expr_append_obj(ctx, expression,
							grn_ctx_get(ctx, "all_records", -1),
							GRN_OP_PUSH, 1);
		grn_expr_append_op(ctx, expression, GRN_OP_CALL, 0);
	}
}

static void
PGrnSearchBuildConditionLikeRegexp(PGrnSearchData *data,
								   grn_obj *targetColumn,
								   grn_obj *query)
{
	const char *tag = "[build-condition][like-regexp]";
	grn_obj *expression;
	const char *queryRaw;
	const char *queryRawEnd;
	const char *queryRawCurrent;
	size_t querySize;
	int characterSize;
	bool escaping = false;
	bool lastIsPercent = false;

	expression = data->expression;
	queryRaw = GRN_TEXT_VALUE(query);
	querySize = GRN_TEXT_LEN(query);
	queryRawEnd = queryRaw + querySize;

	GRN_BULK_REWIND(&(buffers->pattern));
	if (queryRaw[0] != '%')
		GRN_TEXT_PUTS(ctx, &(buffers->pattern), "\\A");

	queryRawCurrent = queryRaw;
	while ((characterSize = grn_charlen(ctx, queryRawCurrent, queryRawEnd)) > 0)
	{
		const char *current = queryRawCurrent;
		bool needToAddCharacter = true;

		queryRawCurrent += characterSize;

		if (!escaping && characterSize == 1)
		{
			switch (current[0])
			{
			case '%':
				if (queryRaw == current)
				{
					/* do nothing */
				}
				else if (queryRawCurrent == queryRawEnd)
				{
					lastIsPercent = true;
				}
				else
				{
					GRN_TEXT_PUTS(ctx, &(buffers->pattern), ".*");
				}
				needToAddCharacter = false;
				break;
			case '_':
				GRN_TEXT_PUTC(ctx, &(buffers->pattern), '.');
				needToAddCharacter = false;
				break;
			case '\\':
				escaping = true;
				needToAddCharacter = false;
				break;
			default:
				break;
			}

			if (!needToAddCharacter)
				continue;
		}

		if (characterSize == 1)
		{
			switch (current[0])
			{
			case '\\':
			case '|':
			case '(':
			case ')':
			case '[':
			case ']':
			case '.':
			case '*':
			case '+':
			case '?':
			case '{':
			case '}':
			case '^':
			case '$':
				GRN_TEXT_PUTC(ctx, &(buffers->pattern), '\\');
				GRN_TEXT_PUTC(ctx, &(buffers->pattern), current[0]);
				break;
			default:
				GRN_TEXT_PUTC(ctx, &(buffers->pattern), current[0]);
				break;
			}
		}
		else
		{
			GRN_TEXT_PUT(ctx, &(buffers->pattern), current, characterSize);
		}
		escaping = false;
	}

	if (queryRawCurrent != queryRawEnd)
	{
		PGrnCheckRC(GRN_INVALID_ARGUMENT,
					"%s invalid encoding character exist: <%.*s>",
					tag,
					(int) querySize, queryRaw);
	}

	if (!lastIsPercent)
		GRN_TEXT_PUTS(ctx, &(buffers->pattern), "\\z");

	grn_expr_append_obj(ctx, expression, targetColumn, GRN_OP_PUSH, 1);
	grn_expr_append_op(ctx, expression, GRN_OP_GET_VALUE, 1);
	grn_expr_append_const_str(ctx, expression,
							  GRN_TEXT_VALUE(&(buffers->pattern)),
							  GRN_TEXT_LEN(&(buffers->pattern)),
							  GRN_OP_PUSH, 1);
	grn_expr_append_op(ctx, expression, GRN_OP_REGEXP, 2);
}

void
PGrnSearchBuildConditionQuery(PGrnSearchData *data,
							  grn_obj *targetColumn,
							  const char *query,
							  unsigned int querySize)
{
	const char *tag = "[build-condition][query]";
	grn_obj *matchTarget, *matchTargetVariable;
	grn_expr_flags flags = PGRN_EXPR_QUERY_PARSE_FLAGS;

	GRN_EXPR_CREATE_FOR_QUERY(ctx, data->sourcesTable,
							  matchTarget, matchTargetVariable);
	GRN_PTR_PUT(ctx, &(data->matchTargets), matchTarget);
	grn_expr_append_obj(ctx, matchTarget, targetColumn, GRN_OP_PUSH, 1);

	flags |= PGrnOptionsGetExprParseFlags(data->index);
	grn_expr_parse(ctx, data->expression,
				   query, querySize,
				   matchTarget, GRN_OP_MATCH, GRN_OP_AND,
				   flags);
	PGrnCheck("%s failed to parse expression: <%.*s>",
			  tag,
			  (int) querySize, query);
}

static void
PGrnSearchBuildConditionPrefixRK(PGrnSearchData *data,
								 grn_obj *targetColumn,
								 const char *prefix,
								 unsigned int prefixSize)
{
	grn_obj subFilterScript;

	GRN_TEXT_INIT(&subFilterScript, 0);
	GRN_TEXT_PUTS(ctx, &subFilterScript, "prefix_rk_search(_key, ");
	grn_text_esc(ctx, &subFilterScript, prefix, prefixSize);
	GRN_TEXT_PUTS(ctx, &subFilterScript, ")");

	grn_expr_append_obj(ctx, data->expression,
						grn_ctx_get(ctx, "sub_filter", -1),
						GRN_OP_PUSH, 1);
	grn_expr_append_obj(ctx, data->expression,
						targetColumn,
						GRN_OP_GET_VALUE, 1);
	grn_expr_append_const_str(ctx, data->expression,
							  GRN_TEXT_VALUE(&subFilterScript),
							  GRN_TEXT_LEN(&subFilterScript),
							  GRN_OP_PUSH, 1);
	grn_expr_append_op(ctx, data->expression, GRN_OP_CALL, 2);

	GRN_OBJ_FIN(ctx, &subFilterScript);
}

static void
PGrnSearchBuildConditionScript(PGrnSearchData *data,
							   grn_obj *targetColumn,
							   const char *script,
							   unsigned int scriptSize)
{
	const char *tag = "[build-condition][script]";
	grn_obj *matchTarget, *matchTargetVariable;
	grn_expr_flags flags = GRN_EXPR_SYNTAX_SCRIPT;

	GRN_EXPR_CREATE_FOR_QUERY(ctx, data->sourcesTable,
							  matchTarget, matchTargetVariable);
	GRN_PTR_PUT(ctx, &(data->matchTargets), matchTarget);
	grn_expr_append_obj(ctx, matchTarget, targetColumn, GRN_OP_PUSH, 1);

	grn_expr_parse(ctx, data->expression,
				   script, scriptSize,
				   matchTarget, GRN_OP_MATCH, GRN_OP_AND,
				   flags);
	PGrnCheck("%s failed to parse expression", tag);
}

void
PGrnSearchBuildConditionBinaryOperation(PGrnSearchData *data,
										grn_obj *targetColumn,
										grn_obj *value,
										grn_operator operator)
{
	grn_expr_append_obj(ctx, data->expression,
						targetColumn, GRN_OP_PUSH, 1);
	grn_expr_append_op(ctx, data->expression, GRN_OP_GET_VALUE, 1);
	grn_expr_append_const(ctx, data->expression,
						  value, GRN_OP_PUSH, 1);
	grn_expr_append_op(ctx, data->expression, operator, 2);
}

static bool
PGrnSearchBuildCondition(Relation index,
						 ScanKey key,
						 PGrnSearchData *data)
{
	const char *tag = "[build-condition]";
	TupleDesc desc;
	Form_pg_attribute attribute;
	const char *targetColumnName;
	grn_obj *targetColumn;
	grn_operator operator = GRN_OP_NOP;
	Oid valueTypeID;

	/* NULL key is not supported */
	if (key->sk_flags & SK_ISNULL)
		return false;

	desc = RelationGetDescr(index);
	attribute = TupleDescAttr(desc, key->sk_attno - 1);

	targetColumnName = attribute->attname.data;
	targetColumn = PGrnLookupColumn(data->sourcesTable, targetColumnName, ERROR);
	GRN_PTR_PUT(ctx, &(data->targetColumns), targetColumn);

	if (PGrnSearchIsInCondition(key))
		return PGrnSearchBuildConditionIn(data, key, targetColumn, attribute);

	if (key->sk_strategy == PGrnMatchConditionStrategyV2Number ||
		key->sk_strategy == PGrnMatchConditionWithScorersStrategyV2Number)
	{
		return PGrnSearchBuildConditionMatchCondition(data,
													  key,
													  targetColumn,
													  attribute);
	}

	if (key->sk_strategy == PGrnQueryConditionStrategyV2Number ||
		key->sk_strategy == PGrnQueryConditionWithScorersStrategyV2Number)
	{
		return PGrnSearchBuildConditionQueryCondition(data,
													  key,
													  targetColumn,
													  attribute);
	}

	if (PGrnAttributeIsJSONB(attribute->atttypid))
		return PGrnJSONBBuildSearchCondition(data, index, key, targetColumn);

	valueTypeID = attribute->atttypid;
	switch (key->sk_strategy)
	{
	case PGrnContainStrategyNumber:
	case PGrnPrefixInStrategyV2Number:
	case PGrnNotPrefixInStrategyV2Number:
	case PGrnPrefixRKInStrategyV2Number:
	case PGrnQueryInStrategyV2Number:
	case PGrnQueryInStrategyV2DeprecatedNumber:
	case PGrnQueryInStrategyV2Deprecated2Number:
	case PGrnMatchInStrategyV2Number:
	case PGrnMatchInStrategyV2DeprecatedNumber:
	case PGrnRegexpInStrategyV2Number:
		switch (valueTypeID)
		{
		case VARCHAROID:
		case VARCHARARRAYOID:
			valueTypeID = VARCHARARRAYOID;
			break;
		case TEXTOID:
		case TEXTARRAYOID:
			valueTypeID = TEXTARRAYOID;
			break;
		}
		break;
	default:
		switch (valueTypeID)
		{
		case VARCHARARRAYOID:
			valueTypeID = VARCHAROID;
			break;
		case TEXTARRAYOID:
			valueTypeID = TEXTOID;
			break;
		}
		break;
	}

	switch (key->sk_strategy)
	{
	case PGrnLessStrategyNumber:
		operator = GRN_OP_LESS;
		break;
	case PGrnLessEqualStrategyNumber:
		operator = GRN_OP_LESS_EQUAL;
		break;
	case PGrnEqualStrategyNumber:
		operator = GRN_OP_EQUAL;
		break;
	case PGrnGreaterEqualStrategyNumber:
		operator = GRN_OP_GREATER_EQUAL;
		break;
	case PGrnGreaterStrategyNumber:
		operator = GRN_OP_GREATER;
		break;
	case PGrnLikeStrategyNumber:
	case PGrnILikeStrategyNumber:
		break;
	case PGrnMatchStrategyNumber:
	case PGrnMatchStrategyV2Number:
		operator = GRN_OP_MATCH;
		break;
	case PGrnQueryStrategyNumber:
	case PGrnQueryStrategyV2Number:
	case PGrnQueryStrategyV2DeprecatedNumber:
		break;
	case PGrnContainStrategyNumber:
		operator = GRN_OP_MATCH;
		break;
	case PGrnSimilarStrategyV2Number:
	case PGrnSimilarStrategyV2DeprecatedNumber:
		operator = GRN_OP_SIMILAR;
		break;
	case PGrnScriptStrategyV2Number:
		break;
	case PGrnPrefixStrategyV2Number:
	case PGrnPrefixStrategyV2DeprecatedNumber:
	case PGrnPrefixInStrategyV2Number:
	case PGrnNotPrefixInStrategyV2Number:
		operator = GRN_OP_PREFIX;
		break;
	case PGrnPrefixRKStrategyV2Number:
	case PGrnPrefixRKStrategyV2DeprecatedNumber:
	case PGrnPrefixRKInStrategyV2Number:
		break;
	case PGrnRegexpStrategyNumber:
	case PGrnRegexpStrategyV2Number:
	case PGrnRegexpInStrategyV2Number:
		operator = GRN_OP_REGEXP;
		break;
	case PGrnQueryInStrategyV2Number:
	case PGrnQueryInStrategyV2DeprecatedNumber:
	case PGrnQueryInStrategyV2Deprecated2Number:
		break;
	case PGrnMatchInStrategyV2Number:
	case PGrnMatchInStrategyV2DeprecatedNumber:
		operator = GRN_OP_MATCH;
		break;
	case PGrnContainStrategyV2Number:
		operator = GRN_OP_MATCH;
		break;
	default:
		PGrnCheckRC(GRN_INVALID_ARGUMENT,
					"%s unexpected strategy number: %d",
					tag,
					key->sk_strategy);
		break;
	}

	{
		grn_id domain;
		unsigned char flags = 0;
		domain = PGrnPGTypeToGrnType(valueTypeID, &flags);
		grn_obj_reinit(ctx, &(buffers->general), domain, flags);
		PGrnConvertFromData(key->sk_argument, valueTypeID, &(buffers->general));
	}

	switch (key->sk_strategy)
	{
	case PGrnLikeStrategyNumber:
		if (PGrnIsForRegexpSearchIndex(index, key->sk_attno - 1))
			PGrnSearchBuildConditionLikeRegexp(data, targetColumn, &(buffers->general));
		else
			PGrnSearchBuildConditionLikeMatch(data, targetColumn, &(buffers->general));
		break;
	case PGrnILikeStrategyNumber:
		PGrnSearchBuildConditionLikeMatch(data, targetColumn, &(buffers->general));
		break;
	case PGrnQueryStrategyNumber:
	case PGrnQueryStrategyV2Number:
	case PGrnQueryStrategyV2DeprecatedNumber:
		PGrnSearchBuildConditionQuery(data,
									  targetColumn,
									  GRN_TEXT_VALUE(&(buffers->general)),
									  GRN_TEXT_LEN(&(buffers->general)));
		break;
	case PGrnContainStrategyNumber:
	{
		grn_obj *elements = &(buffers->general);
		grn_obj elementBuffer;
		unsigned int i, n;

		GRN_TEXT_INIT(&elementBuffer, GRN_OBJ_DO_SHALLOW_COPY);
		n = grn_vector_size(ctx, elements);
		for (i = 0; i < n; i++)
		{
			const char *element;
			unsigned int elementSize;

			elementSize = grn_vector_get_element(ctx, elements, i,
												 &element, NULL, NULL);
			GRN_TEXT_SET(ctx, &elementBuffer, element, elementSize);
			PGrnSearchBuildConditionBinaryOperation(data,
													targetColumn,
													&elementBuffer,
													operator);
			if (i > 0)
				grn_expr_append_op(ctx, data->expression, GRN_OP_AND, 2);
		}
		GRN_OBJ_FIN(ctx, &elementBuffer);
		break;
	}
	case PGrnScriptStrategyV2Number:
		PGrnSearchBuildConditionScript(data,
									   targetColumn,
									   GRN_TEXT_VALUE(&(buffers->general)),
									   GRN_TEXT_LEN(&(buffers->general)));
		break;
	case PGrnPrefixRKStrategyV2Number:
	case PGrnPrefixRKStrategyV2DeprecatedNumber:
		PGrnSearchBuildConditionPrefixRK(data,
										 targetColumn,
										 GRN_TEXT_VALUE(&(buffers->general)),
										 GRN_TEXT_LEN(&(buffers->general)));
		break;
	case PGrnPrefixRKInStrategyV2Number:
	{
		grn_obj *prefixes = &(buffers->general);
		unsigned int i, n;

		n = grn_vector_size(ctx, prefixes);
		for (i = 0; i < n; i++)
		{
			const char *prefix;
			unsigned int prefixSize;

			prefixSize = grn_vector_get_element(ctx, prefixes, i,
												&prefix, NULL, NULL);
			PGrnSearchBuildConditionPrefixRK(data,
											 targetColumn,
											 prefix,
											 prefixSize);
			if (i > 0)
				grn_expr_append_op(ctx, data->expression, GRN_OP_OR, 2);
		}
		break;
	}
	case PGrnQueryInStrategyV2Number:
	case PGrnQueryInStrategyV2DeprecatedNumber:
	case PGrnQueryInStrategyV2Deprecated2Number:
	{
		grn_obj *queries = &(buffers->general);
		unsigned int i, n;

		n = grn_vector_size(ctx, queries);
		for (i = 0; i < n; i++)
		{
			const char *query;
			unsigned int querySize;

			querySize = grn_vector_get_element(ctx, queries, i,
												&query, NULL, NULL);
			PGrnSearchBuildConditionQuery(data,
										  targetColumn,
										  query,
										  querySize);
			if (i > 0)
				grn_expr_append_op(ctx, data->expression, GRN_OP_OR, 2);
		}
		break;
	}
	case PGrnPrefixInStrategyV2Number:
	case PGrnMatchInStrategyV2Number:
	case PGrnMatchInStrategyV2DeprecatedNumber:
	case PGrnRegexpInStrategyV2Number:
	{
		grn_obj *keywords = &(buffers->general);
		grn_obj keywordBuffer;
		unsigned int i, n;

		GRN_TEXT_INIT(&keywordBuffer, GRN_OBJ_DO_SHALLOW_COPY);
		n = grn_vector_size(ctx, keywords);
		for (i = 0; i < n; i++)
		{
			const char *keyword;
			unsigned int keywordSize;

			keywordSize = grn_vector_get_element(ctx, keywords, i,
												 &keyword, NULL, NULL);
			GRN_TEXT_SET(ctx, &keywordBuffer, keyword, keywordSize);
			PGrnSearchBuildConditionBinaryOperation(data,
													targetColumn,
													&keywordBuffer,
													operator);
			if (i > 0)
				grn_expr_append_op(ctx, data->expression, GRN_OP_OR, 2);
		}
		GRN_OBJ_FIN(ctx, &keywordBuffer);
		break;
	}
	case PGrnNotPrefixInStrategyV2Number:
	{
		grn_obj *keywords = &(buffers->general);
		grn_obj keywordBuffer;
		unsigned int i, n;

		if (data->nExpressions == 0)
		{
			grn_expr_append_obj(ctx, data->expression,
								grn_ctx_get(ctx, "all_records", -1),
								GRN_OP_PUSH, 1);
			grn_expr_append_op(ctx, data->expression, GRN_OP_CALL, 0);
		}

		GRN_TEXT_INIT(&keywordBuffer, GRN_OBJ_DO_SHALLOW_COPY);
		n = grn_vector_size(ctx, keywords);
		for (i = 0; i < n; i++)
		{
			const char *keyword;
			unsigned int keywordSize;

			keywordSize = grn_vector_get_element(ctx, keywords, i,
												 &keyword, NULL, NULL);
			GRN_TEXT_SET(ctx, &keywordBuffer, keyword, keywordSize);
			PGrnSearchBuildConditionBinaryOperation(data,
													targetColumn,
													&keywordBuffer,
													operator);
			grn_expr_append_op(ctx, data->expression, GRN_OP_AND_NOT, 2);
		}
		GRN_OBJ_FIN(ctx, &keywordBuffer);
		break;
	}
	default:
		PGrnSearchBuildConditionBinaryOperation(data,
												targetColumn,
												&(buffers->general),
												operator);
		break;
	}

	return true;
}

static void
PGrnSearchBuildConditions(IndexScanDesc scan,
						  PGrnScanOpaque so,
						  PGrnSearchData *data)
{
	int i;

	for (i = 0; i < scan->numberOfKeys; i++)
	{
		Relation index = scan->indexRelation;
		ScanKey key = &(scan->keyData[i]);

		PGrnAutoCloseUseIndex(index);

		if (!PGrnSearchBuildCondition(index, key, data))
			continue;

		if (data->isEmptyCondition)
			return;

		switch (key->sk_strategy)
		{
		case PGrnNotPrefixInStrategyV2Number:
			break;
		default:
			if (data->nExpressions > 0)
				grn_expr_append_op(ctx, data->expression, GRN_OP_AND, 2);
			break;
		}
		data->nExpressions++;
	}
}

static void
PGrnSearchDataInit(PGrnSearchData *data, Relation index, grn_obj *sourcesTable)
{
	data->index = index;
	data->sourcesTable = sourcesTable;
	GRN_PTR_INIT(&(data->matchTargets), GRN_OBJ_VECTOR, GRN_ID_NIL);
	GRN_PTR_INIT(&(data->targetColumns), GRN_OBJ_VECTOR, GRN_ID_NIL);
	GRN_UINT32_INIT(&(data->sectionID), 0);

	GRN_EXPR_CREATE_FOR_QUERY(ctx, sourcesTable,
							  data->expression, data->expressionVariable);
	data->isEmptyCondition = false;
	data->nExpressions = 0;
}

static void
PGrnSearchDataFree(PGrnSearchData *data)
{
	unsigned int i;
	unsigned int nMatchTargets;
	unsigned int nTargetColumns;

	grn_obj_unlink(ctx, data->expression);

	nMatchTargets = GRN_BULK_VSIZE(&(data->matchTargets)) / sizeof(grn_obj *);
	for (i = 0; i < nMatchTargets; i++)
	{
		grn_obj *matchTarget = GRN_PTR_VALUE_AT(&(data->matchTargets), i);
		grn_obj_unlink(ctx, matchTarget);
	}
	GRN_OBJ_FIN(ctx, &(data->matchTargets));

	nTargetColumns = GRN_BULK_VSIZE(&(data->targetColumns)) / sizeof(grn_obj *);
	for (i = 0; i < nTargetColumns; i++)
	{
		grn_obj *targetColumn = GRN_PTR_VALUE_AT(&(data->targetColumns), i);
		grn_obj_unlink(ctx, targetColumn);
	}
	GRN_OBJ_FIN(ctx, &(data->targetColumns));

	GRN_OBJ_FIN(ctx, &(data->sectionID));
}

static void
PGrnSearch(IndexScanDesc scan)
{
	PGrnScanOpaque so = (PGrnScanOpaque) scan->opaque;
	PGrnSearchData data;

	if (scan->numberOfKeys == 0)
		return;

	PGrnSearchDataInit(&data, so->index, so->sourcesTable);
	PG_TRY();
	{
		PGrnSearchBuildConditions(scan, so, &data);
	}
	PG_CATCH();
	{
		PGrnSearchDataFree(&data);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* TODO: Add NULL check for so->searched. */
	so->searched = grn_table_create(ctx, NULL, 0, NULL,
									GRN_OBJ_TABLE_HASH_KEY | GRN_OBJ_WITH_SUBREC,
									so->sourcesTable, 0);
	if (!data.isEmptyCondition)
	{
		grn_table_select(ctx,
						 so->sourcesTable,
						 data.expression,
						 so->searched,
						 GRN_OP_OR);
	}
	PGrnSearchDataFree(&data);
}

static void
PGrnSort(IndexScanDesc scan)
{
	PGrnScanOpaque so = (PGrnScanOpaque) scan->opaque;
	ScanKey key;
	TupleDesc desc;
	Form_pg_attribute attribute;
	const char *targetColumnName;
	grn_table_sort_key sort_key;

	if (!so->searched)
		return;

	if (scan->numberOfKeys != 1)
		return;

	key = &(scan->keyData[0]);
	if (!PGrnSearchIsInCondition(key))
		return;

	so->sorted = grn_table_create(ctx, NULL, 0, NULL,
								  GRN_OBJ_TABLE_NO_KEY,
								  NULL, so->searched);

	desc = RelationGetDescr(scan->indexRelation);
	attribute = TupleDescAttr(desc, key->sk_attno - 1);
	targetColumnName = attribute->attname.data;
	sort_key.key = grn_obj_column(ctx, so->searched,
								  targetColumnName,
								  strlen(targetColumnName));

	sort_key.flags = GRN_TABLE_SORT_ASC;
	sort_key.offset = 0;
	grn_table_sort(ctx,
				   so->searched,
				   0,
				   -1,
				   so->sorted,
				   &sort_key,
				   1);
	grn_obj_close(ctx, sort_key.key);
}

static void
PGrnOpenTableCursor(IndexScanDesc scan, ScanDirection dir)
{
	const char *tag = "[cursor][open]";
	PGrnScanOpaque so = (PGrnScanOpaque) scan->opaque;
	grn_obj *table;
	int offset = 0;
	int limit = -1;
	int flags = 0;

	table = so->sorted;
	if (!table)
		table = so->searched;
	if (!table)
		table = so->sourcesTable;

	if (ScanDirectionIsBackward(dir))
		flags |= GRN_CURSOR_DESCENDING;
	else
		flags |= GRN_CURSOR_ASCENDING;

	if (!grn_obj_is_table(ctx, table))
	{
		GRN_LOG(ctx, GRN_LOG_DEBUG,
				"pgroonga: %s target table is invalid: "
				"<%p>: <%p>:<%p>:<%p>:<%p>",
				tag,
				so,
				table,
				so->sorted,
				so->searched,
				so->sourcesTable);
	}
	so->tableCursor = grn_table_cursor_open(ctx, table,
											NULL, 0, NULL, 0,
											offset, limit, flags);
	PGrnCheck("%s failed to open cursor", tag);
	if (so->sourcesTable->header.type == GRN_TABLE_NO_KEY)
	{
		so->ctidAccessor = grn_obj_column(ctx, table,
										  PGrnSourcesCtidColumnName,
										  PGrnSourcesCtidColumnNameLength);
	}
	else
	{
		so->ctidAccessor = grn_obj_column(ctx, table,
										  GRN_COLUMN_NAME_KEY,
										  GRN_COLUMN_NAME_KEY_LEN);
	}
	if (so->searched)
	{
		so->scoreAccessor = grn_obj_column(ctx, so->searched,
										   GRN_COLUMN_NAME_SCORE,
										   GRN_COLUMN_NAME_SCORE_LEN);
	}
}

static bool
PGrnIsMeaningfullMaxBorderValue(grn_obj *currentValue,
								grn_obj *newValue,
								int flags,
								StrategyNumber strategy)
{
	if (((flags & GRN_CURSOR_LT) == GRN_CURSOR_LT) &&
		strategy == PGrnLessEqualStrategyNumber)
	{
		return grn_operator_exec_greater_equal(ctx, currentValue, newValue);
	}
	else
	{
		return grn_operator_exec_greater(ctx, currentValue, newValue);
	}
}

static bool
PGrnIsMeaningfullMinBorderValue(grn_obj *currentValue,
								grn_obj *newValue,
								int flags,
								StrategyNumber strategy)
{
	if (((flags & GRN_CURSOR_GT) == GRN_CURSOR_GT) &&
		strategy == PGrnGreaterEqualStrategyNumber)
	{
		return grn_operator_exec_less_equal(ctx, currentValue, newValue);
	}
	else
	{
		return grn_operator_exec_less(ctx, currentValue, newValue);
	}
}

static void
PGrnFillBorder(IndexScanDesc scan,
			   void **min, unsigned int *minSize,
			   void **max, unsigned int *maxSize,
			   int *flags)
{
	const char *tag = "[range][fill-border]";
	Relation index = scan->indexRelation;
	TupleDesc desc;
	PGrnScanOpaque so = (PGrnScanOpaque) scan->opaque;
	grn_obj *minBorderValue;
	grn_obj *maxBorderValue;
	int i;

	desc = RelationGetDescr(index);

	minBorderValue = &(so->minBorderValue);
	maxBorderValue = &(so->maxBorderValue);
	for (i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey key = &(scan->keyData[i]);
		AttrNumber attrNumber;
		Form_pg_attribute attribute;
		grn_id domain;

		attrNumber = key->sk_attno - 1;
		attribute = TupleDescAttr(desc, attrNumber);

		domain = PGrnGetType(index, attrNumber, NULL);
		switch (key->sk_strategy)
		{
		case PGrnLessStrategyNumber:
		case PGrnLessEqualStrategyNumber:
			if (maxBorderValue->header.type != GRN_DB_VOID)
			{
				grn_obj_reinit(ctx, &(buffers->general), domain, 0);
				PGrnConvertFromData(key->sk_argument,
									attribute->atttypid,
									&(buffers->general));
				if (!PGrnIsMeaningfullMaxBorderValue(maxBorderValue,
													 &(buffers->general),
													 *flags,
													 key->sk_strategy))
				{
					continue;
				}
			}
			grn_obj_reinit(ctx, maxBorderValue, domain, 0);
			PGrnConvertFromData(key->sk_argument,
								attribute->atttypid,
								maxBorderValue);
			*max = GRN_BULK_HEAD(maxBorderValue);
			*maxSize = GRN_BULK_VSIZE(maxBorderValue);
			*flags &= ~(GRN_CURSOR_LT | GRN_CURSOR_LE);
			if (key->sk_strategy == PGrnLessStrategyNumber)
			{
				*flags |= GRN_CURSOR_LT;
			}
			else
			{
				*flags |= GRN_CURSOR_LE;
			}
			break;
		case PGrnGreaterEqualStrategyNumber:
		case PGrnGreaterStrategyNumber:
			if (minBorderValue->header.type != GRN_DB_VOID)
			{
				grn_obj_reinit(ctx, &(buffers->general), domain, 0);
				PGrnConvertFromData(key->sk_argument,
									attribute->atttypid,
									&(buffers->general));
				if (!PGrnIsMeaningfullMinBorderValue(minBorderValue,
													 &(buffers->general),
													 *flags,
													 key->sk_strategy))
				{
					continue;
				}
			}
			grn_obj_reinit(ctx, minBorderValue, domain, 0);
			PGrnConvertFromData(key->sk_argument,
								attribute->atttypid,
								minBorderValue);
			*min = GRN_BULK_HEAD(minBorderValue);
			*minSize = GRN_BULK_VSIZE(minBorderValue);
			*flags &= ~(GRN_CURSOR_GT | GRN_CURSOR_GE);
			if (key->sk_strategy == PGrnGreaterEqualStrategyNumber)
			{
				*flags |= GRN_CURSOR_GE;
			}
			else
			{
				*flags |= GRN_CURSOR_GT;
			}
			break;
		default:
			PGrnCheckRC(GRN_INVALID_ARGUMENT,
						"%s unexpected strategy number for range search: %d",
						tag,
						key->sk_strategy);
			break;
		}
	}
}

static void
PGrnRangeSearch(IndexScanDesc scan, ScanDirection dir)
{
	PGrnScanOpaque so = (PGrnScanOpaque) scan->opaque;
	void *min = NULL;
	unsigned int minSize = 0;
	void *max = NULL;
	unsigned int maxSize = 0;
	int offset = 0;
	int limit = -1;
	int flags = 0;
	grn_id indexCursorMin = GRN_ID_NIL;
	grn_id indexCursorMax = GRN_ID_MAX;
	int indexCursorFlags = 0;
	grn_obj *indexColumn;
	grn_obj *lexicon;
	int i;
	unsigned int nthAttribute = 0;

	PGrnFillBorder(scan, &min, &minSize, &max, &maxSize, &flags);

	if (ScanDirectionIsBackward(dir))
		flags |= GRN_CURSOR_DESCENDING;
	else
		flags |= GRN_CURSOR_ASCENDING;

	for (i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey key = &(scan->keyData[i]);
		nthAttribute = key->sk_attno - 1;
		break;
	}
	indexColumn = PGrnLookupIndexColumn(scan->indexRelation, nthAttribute,
										ERROR);
	lexicon = grn_column_table(ctx, indexColumn);

	so->tableCursor = grn_table_cursor_open(ctx, lexicon,
											min, minSize,
											max, maxSize,
											offset, limit, flags);
	so->indexCursor = grn_index_cursor_open(ctx,
											so->tableCursor, indexColumn,
											indexCursorMin,
											indexCursorMax,
											indexCursorFlags);
	if (so->sourcesTable->header.type == GRN_TABLE_NO_KEY)
	{
		so->ctidAccessor = grn_obj_column(ctx, so->sourcesTable,
										  PGrnSourcesCtidColumnName,
										  PGrnSourcesCtidColumnNameLength);
	}
	else
	{
		so->ctidAccessor = grn_obj_column(ctx, so->sourcesTable,
										  GRN_COLUMN_NAME_KEY,
										  GRN_COLUMN_NAME_KEY_LEN);
	}
}

static bool
PGrnIsRangeSearchable(IndexScanDesc scan)
{
	int i;
	AttrNumber previousAttrNumber = InvalidAttrNumber;

	if (scan->numberOfKeys == 0)
	{
		TupleDesc desc = RelationGetDescr(scan->indexRelation);
		grn_obj *indexColumn;
		grn_obj *lexicon;
		grn_obj *tokenizer;

		if (desc->natts == 1)
		{
			Form_pg_attribute attribute = TupleDescAttr(desc, 0);
			if (PGrnAttributeIsJSONB(attribute->atttypid))
				return false;
		}

		indexColumn = PGrnLookupIndexColumn(scan->indexRelation, 0, ERROR);
		lexicon = grn_column_table(ctx, indexColumn);
		tokenizer = grn_obj_get_info(ctx, lexicon, GRN_INFO_DEFAULT_TOKENIZER,
									 NULL);
		if (tokenizer)
		{
			return false;
		}
	}

	for (i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey key = &(scan->keyData[i]);

		if (previousAttrNumber == InvalidAttrNumber)
		{
			previousAttrNumber = key->sk_attno;
		}
		if (key->sk_attno != previousAttrNumber)
		{
			return false;
		}

		switch (key->sk_strategy)
		{
		case PGrnLessStrategyNumber:
		case PGrnLessEqualStrategyNumber:
		case PGrnGreaterStrategyNumber:
		case PGrnGreaterEqualStrategyNumber:
			break;
		default:
			return false;
			break;
		}
	}

	return true;
}

static void
PGrnEnsureCursorOpened(IndexScanDesc scan,
					   ScanDirection dir,
					   bool needSort)
{
	PGrnScanOpaque so = (PGrnScanOpaque) scan->opaque;

	scan->xs_recheck = false;

	{
		int i;
		for (i = 0; i < scan->numberOfKeys; i++)
		{
			ScanKey key = &(scan->keyData[i]);
			if (key->sk_strategy == PGrnLikeStrategyNumber ||
				key->sk_strategy == PGrnILikeStrategyNumber)
			{
				scan->xs_recheck = true;
				break;
			}
		}
	}

	if (so->indexCursor)
		return;
	if (so->tableCursor)
		return;

	if (PGrnIsRangeSearchable(scan))
	{
		PGrnRangeSearch(scan, dir);
	}
	else
	{
		PGrnSearch(scan);
		if (needSort)
			PGrnSort(scan);
		PGrnOpenTableCursor(scan, dir);
	}
}

static grn_id
PGrnScanOpaqueResolveID(PGrnScanOpaque so)
{
	grn_id recordID = so->currentID;

	if (so->sorted)
	{
		GRN_BULK_REWIND(&(buffers->general));
		grn_obj_get_value(ctx, so->sorted, recordID, &(buffers->general));
		recordID = GRN_RECORD_VALUE(&(buffers->general));
	}
	if (so->searched)
	{
		grn_table_get_key(ctx, so->searched, recordID,
						  &recordID, sizeof(grn_id));
	}

	return recordID;
}

static bool pgroonga_canreturn_raw(Relation index, int nthAttribute);

static void
PGrnGetTupleFillIndexTuple(PGrnScanOpaque so,
						   IndexScanDesc scan)
{
	MemoryContext oldMemoryContext;
	TupleDesc desc;
	Datum *values;
	bool *isNulls;
	grn_id recordID;
	unsigned int i;

	MemoryContextReset(so->memoryContext);
	oldMemoryContext = MemoryContextSwitchTo(so->memoryContext);

	desc = RelationGetDescr(so->index);
	scan->xs_itupdesc = desc;

	values = palloc(sizeof(Datum) * desc->natts);
	isNulls = palloc(sizeof(bool) * desc->natts);

	recordID = PGrnScanOpaqueResolveID(so);

	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attribute = TupleDescAttr(desc, i);
		NameData *name;
		grn_obj *dataColumn;

		if (GRN_BOOL_VECTOR_SIZE(&(so->canReturns)) <= i) {
			unsigned int j;
			for (j = GRN_BOOL_VECTOR_SIZE(&(so->canReturns));
				 j <= i;
				 j++)
			{
				bool can = pgroonga_canreturn_raw(so->index, j);
				GRN_BOOL_PUT(ctx, &(so->canReturns), can);
			}
		}

		if (!GRN_BOOL_VALUE_AT(&(so->canReturns), i))
		{
			values[i] = PointerGetDatum(NULL);
			isNulls[i] = true;
			continue;
		}

		name = &(attribute->attname);
		dataColumn = PGrnLookupColumn(so->sourcesTable, name->data, ERROR);
		GRN_BULK_REWIND(&(buffers->general));
		grn_obj_get_value(ctx, dataColumn, recordID, &(buffers->general));
		values[i] = PGrnConvertToDatum(&(buffers->general), attribute->atttypid);
		isNulls[i] = false;
		grn_obj_unlink(ctx, dataColumn);
	}

	scan->xs_itup = index_form_tuple(scan->xs_itupdesc,
									 values,
									 isNulls);

	MemoryContextSwitchTo(oldMemoryContext);
}

static bool
pgroonga_gettuple_internal(IndexScanDesc scan,
						   ScanDirection direction)
{
	const char *tag = "pgroonga: [get-tuple]";
	PGrnScanOpaque so = (PGrnScanOpaque) scan->opaque;
	bool found = false;

	if (scan->parallel_scan)
	{
		if (!PGrnParallelScanAcquire(scan))
			return false;
	}

	PGrnEnsureCursorOpened(scan, direction, true);

	if (scan->kill_prior_tuple &&
		so->currentID != GRN_ID_NIL &&
		PGrnIsWritable())
	{
		grn_id recordID;
		uint64 packedCtid;

		recordID = PGrnScanOpaqueResolveID(so);
		GRN_BULK_REWIND(&(buffers->ctid));
		grn_obj_get_value(ctx,
						  so->ctidAccessor,
						  so->currentID,
						  &(buffers->ctid));
		if (GRN_BULK_VSIZE(&(buffers->ctid)) == 0)
		{
			NameData tableName;
			GRN_LOG(ctx,
					GRN_LOG_DEBUG,
					"%s[delete][nonexistent] <%s>(%u): <%u> -> <%u>",
					tag,
					PGrnPGGetRelationNameByID(so->dataTableID, tableName.data),
					so->dataTableID,
					so->currentID,
					recordID);
		}
		else
		{
			packedCtid = GRN_UINT64_VALUE(&(buffers->ctid));
			if (grn_logger_pass(ctx, GRN_LOG_DEBUG))
			{
				NameData tableName;
				ItemPointerData ctid = PGrnCtidUnpack(packedCtid);
				GRN_LOG(ctx,
						GRN_LOG_DEBUG,
						"%s[delete] "
						"<%s>(%u): <%u> -> <%u>: <(%u,%u),%u>: <%" PRIu64 ">",
						tag,
						PGrnPGGetRelationNameByID(so->dataTableID, tableName.data),
						so->dataTableID,
						so->currentID,
						recordID,
						ctid.ip_blkid.bi_hi,
						ctid.ip_blkid.bi_lo,
						ctid.ip_posid,
						packedCtid);
			}
			grn_table_delete_by_id(ctx, so->sourcesTable, recordID);

			PGrnWALDelete(so->index,
						  so->sourcesTable,
						  (const char *) &packedCtid,
						  sizeof(uint64));
		}
	}

	while (!found)
	{
		if (so->indexCursor)
		{
			grn_posting *posting;
			grn_id termID;
			grn_id id = GRN_ID_NIL;
			posting = grn_index_cursor_next(ctx, so->indexCursor, &termID);
			if (posting)
				id = posting->rid;
			so->currentID = id;
		}
		else
		{
			so->currentID = grn_table_cursor_next(ctx, so->tableCursor);
		}

		if (so->currentID == GRN_ID_NIL)
			break;

		{
			uint64 packedCtid;
			ItemPointerData ctid;
			bool valid;

			GRN_BULK_REWIND(&(buffers->ctid));
			grn_obj_get_value(ctx,
							  so->ctidAccessor,
							  so->currentID,
							  &(buffers->ctid));
			if (GRN_BULK_VSIZE(&(buffers->ctid)) == 0)
			{
				NameData tableName;
				GRN_LOG(ctx,
						GRN_LOG_DEBUG,
						"%s[nonexistent] <%s>(%u): <%u> -> <%u>",
						tag,
						PGrnPGGetRelationNameByID(so->dataTableID, tableName.data),
						so->dataTableID,
						so->currentID,
						PGrnScanOpaqueResolveID(so));
				continue;
			}

			packedCtid = GRN_UINT64_VALUE(&(buffers->ctid));
			ctid = PGrnCtidUnpack(packedCtid);
			valid = ItemPointerIsValid(&ctid);

			{
				NameData tableName;
				GRN_LOG(ctx,
						GRN_LOG_DEBUG,
						"%s <%s>(%u): <%u> -> <%u>: "
						"<(%u,%u),%u>: <%" PRIu64 ">: <%s>",
						tag,
						PGrnPGGetRelationNameByID(so->dataTableID,
												  tableName.data),
						so->dataTableID,
						so->currentID,
						PGrnScanOpaqueResolveID(so),
						ctid.ip_blkid.bi_hi,
						ctid.ip_blkid.bi_lo,
						ctid.ip_posid,
						packedCtid,
						valid ? "true" : "false");
			}

			if (!valid)
				continue;

			PGRN_INDEX_SCAN_DESC_SET_FOUND_CTID(scan, ctid);
		}

		if (scan->xs_want_itup)
			PGrnGetTupleFillIndexTuple(so, scan);

		found = true;
	}

	return found;
}

static bool
pgroonga_gettuple_raw(IndexScanDesc scan,
					  ScanDirection direction)
{
	bool found = false;
	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabled(scan->heapRelation->rd_id));
	{
		found = pgroonga_gettuple_internal(scan, direction);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		found = pgroonga_gettuple_internal(scan, direction);
	}
	PGRN_RLS_ENABLED_END();
	return found;
}

/**
 * pgroonga.gettuple() -- amgettuple
 */
Datum
pgroonga_gettuple(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanDirection direction = (ScanDirection) PG_GETARG_INT32(1);
	bool found;

	found = pgroonga_gettuple_raw(scan, direction);

	PG_RETURN_BOOL(found);
}

static int64
pgroonga_getbitmap_internal(IndexScanDesc scan,
							TIDBitmap *tbm)
{
	const char *tag = "pgroonga: [get-bitmap]";
	PGrnScanOpaque so = (PGrnScanOpaque) scan->opaque;
	int64 nRecords = 0;

	if (scan->parallel_scan)
	{
		if (!PGrnParallelScanAcquire(scan))
			return 0;
	}

	PGrnEnsureCursorOpened(scan, ForwardScanDirection, false);

	if (so->indexCursor)
	{
		grn_posting *posting;
		grn_id termID;
		while ((posting = grn_index_cursor_next(ctx, so->indexCursor, &termID)))
		{
			uint64 packedCtid;
			ItemPointerData ctid;

			so->currentID = posting->rid;
			GRN_BULK_REWIND(&(buffers->ctid));
			grn_obj_get_value(ctx,
							  so->ctidAccessor,
							  so->currentID,
							  &(buffers->ctid));
			if (GRN_BULK_VSIZE(&(buffers->ctid)) == 0)
			{
				GRN_LOG(ctx,
						GRN_LOG_DEBUG,
						"%s[index-cursor][nonexistent] <%s>(%u): <%u>",
						tag,
						so->index->rd_rel->relname.data,
						so->index->rd_id,
						so->currentID);
				continue;
			}

			packedCtid = GRN_UINT64_VALUE(&(buffers->ctid));
			ctid = PGrnCtidUnpack(packedCtid);
			GRN_LOG(ctx, GRN_LOG_DEBUG,
					"%s[index-cursor] <%s>(%u): <%u>: <(%u,%u),%u>(%" PRIu64 ")",
					tag,
					so->index->rd_rel->relname.data,
					so->index->rd_id,
					so->currentID,
					ctid.ip_blkid.bi_hi,
					ctid.ip_blkid.bi_lo,
					ctid.ip_posid,
					packedCtid);
			if (!ItemPointerIsValid(&ctid))
				continue;
			tbm_add_tuples(tbm, &ctid, 1, scan->xs_recheck);
			nRecords++;
		}
	}
	else
	{
		while (true)
		{
			uint64 packedCtid;
			ItemPointerData ctid;

			so->currentID = grn_table_cursor_next(ctx, so->tableCursor);
			if (so->currentID == GRN_ID_NIL)
				break;

			GRN_BULK_REWIND(&(buffers->ctid));
			grn_obj_get_value(ctx,
							  so->ctidAccessor,
							  so->currentID,
							  &(buffers->ctid));
			if (GRN_BULK_VSIZE(&(buffers->ctid)) == 0)
			{
				GRN_LOG(ctx,
						GRN_LOG_DEBUG,
						"%s[nonexistent] <%s>(%u): <%u>",
						tag,
						so->index->rd_rel->relname.data,
						so->index->rd_id,
						PGrnScanOpaqueResolveID(so));
				continue;
			}

			packedCtid = GRN_UINT64_VALUE(&(buffers->ctid));
			ctid = PGrnCtidUnpack(packedCtid);
			GRN_LOG(ctx, GRN_LOG_DEBUG,
					"%s <%s>(%u): <%u>: <(%u,%u),%u>(%" PRIu64 ")",
					tag,
					so->index->rd_rel->relname.data,
					so->index->rd_id,
					PGrnScanOpaqueResolveID(so),
					ctid.ip_blkid.bi_hi,
					ctid.ip_blkid.bi_lo,
					ctid.ip_posid,
					packedCtid);
			if (!ItemPointerIsValid(&ctid))
				continue;
			tbm_add_tuples(tbm, &ctid, 1, scan->xs_recheck);
			nRecords++;
		}
	}

	return nRecords;
}

static int64
pgroonga_getbitmap_raw(IndexScanDesc scan,
					   TIDBitmap *tbm)
{
	int64 nRecords = 0;
	bool enabled = PGrnCheckRLSEnabled(scan->indexRelation->rd_index->indrelid);
	PGRN_RLS_ENABLED_IF(enabled);
	{
		nRecords = pgroonga_getbitmap_internal(scan, tbm);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		nRecords = pgroonga_getbitmap_internal(scan, tbm);
	}
	PGRN_RLS_ENABLED_END();
	return nRecords;
}

/**
 * pgroonga.getbitmap() -- amgetbitmap
 */
Datum
pgroonga_getbitmap(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	TIDBitmap *tbm = (TIDBitmap *) PG_GETARG_POINTER(1);
	int64 nRecords;

	nRecords = pgroonga_getbitmap_raw(scan, tbm);

	PG_RETURN_INT64(nRecords);
}

static void
pgroonga_rescan_raw(IndexScanDesc scan,
					ScanKey keys,
					int nKeys,
					ScanKey orderBys,
					int nOrderBys)
{
	PGrnScanOpaque so = (PGrnScanOpaque) scan->opaque;

	MemoryContextReset(so->memoryContext);
	PGrnScanOpaqueReinit(so);

	if (keys && scan->numberOfKeys > 0)
		memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
}

/**
 * pgroonga.rescan() -- amrescan
 */
Datum
pgroonga_rescan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanKey	keys = (ScanKey) PG_GETARG_POINTER(1);
	int nKeys = PG_GETARG_INT32(2);
	ScanKey	orderBys = (ScanKey) PG_GETARG_POINTER(3);
	int nOrderBys = PG_GETARG_INT32(4);

	pgroonga_rescan_raw(scan,
						keys, nKeys,
						orderBys, nOrderBys);

	PG_RETURN_VOID();
}

static void
pgroonga_endscan_raw(IndexScanDesc scan)
{
	PGrnScanOpaque so = (PGrnScanOpaque) scan->opaque;
	MemoryContext memoryContext = so->memoryContext;

	GRN_LOG(ctx, GRN_LOG_DEBUG,
			"pgroonga: [scan][end] <%p>",
			so);

	PGrnScanOpaqueFin(so);
	MemoryContextDelete(memoryContext);
}

/**
 * pgroonga.endscan() -- amendscan
 */
Datum
pgroonga_endscan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);

	pgroonga_endscan_raw(scan);

	PG_RETURN_VOID();
}

static void
PGrnBuildCallback(Relation index,
#ifdef PGRN_INDEX_BUILD_CALLBACK_USE_ITEM_POINTER
				  ItemPointer tid,
#else
				  HeapTuple htup,
#endif
				  Datum *values,
				  bool *isnull,
				  bool tupleIsAlive,
				  void *state)
{
	PGrnBuildState bs = (PGrnBuildState) state;
	MemoryContext oldMemoryContext;
	uint32_t recordSize;
#ifndef PGRN_INDEX_BUILD_CALLBACK_USE_ITEM_POINTER
	ItemPointer tid = &(htup->t_self);
#endif

	if (!tupleIsAlive)
		return;

	oldMemoryContext = MemoryContextSwitchTo(bs->memoryContext);

	recordSize = PGrnInsert(index,
							bs->sourcesTable,
							bs->sourcesCtidColumn,
							values,
							isnull,
							tid);
	if (bs->needMaxRecordSizeUpdate &&
		recordSize > bs->maxRecordSize)
	{
		bs->maxRecordSize = recordSize;
	}
	bs->nIndexedTuples++;

	MemoryContextSwitchTo(oldMemoryContext);
	MemoryContextReset(bs->memoryContext);
}

static IndexBuildResult *
pgroonga_build_raw(Relation heap,
				   Relation index,
				   IndexInfo *indexInfo)
{
	const char *tag = "[build]";
	IndexBuildResult *result;
	double nHeapTuples = 0.0;
	PGrnCreateData data;
	PGrnBuildStateData bs;
	grn_obj supplementaryTables;
	grn_obj lexicons;

	if (!PGrnIsWritable())
	{
		ereport(ERROR,
				(errcode(ERRCODE_E_R_E_MODIFYING_SQL_DATA_NOT_PERMITTED),
				 errmsg("pgroonga: %s "
						"can't create an index "
						"while pgroonga.writable is false",
						tag)));
	}

	if (indexInfo->ii_Unique)
		PGrnCheckRC(GRN_FUNCTION_NOT_IMPLEMENTED,
					"%s unique index isn't supported",
					tag);

	PGrnAutoCloseUseIndex(index);

	data.sourcesTable = NULL;

	bs.sourcesTable = NULL;
	bs.sourcesCtidColumn = NULL;
	bs.nIndexedTuples = 0.0;
	bs.needMaxRecordSizeUpdate = PGrnNeedMaxRecordSizeUpdate(index);
	bs.maxRecordSize = 0;
	bs.memoryContext =
		AllocSetContextCreate(CurrentMemoryContext,
							  "PGroonga index build temporay context",
							  ALLOCSET_DEFAULT_SIZES);

	GRN_PTR_INIT(&supplementaryTables, GRN_OBJ_VECTOR, GRN_ID_NIL);
	GRN_PTR_INIT(&lexicons, GRN_OBJ_VECTOR, GRN_ID_NIL);
	PG_TRY();
	{
		data.index = index;
		data.supplementaryTables = &supplementaryTables;
		data.lexicons = &lexicons;
		data.desc = RelationGetDescr(index);
		data.relNode = index->rd_node.relNode;
		PGrnCreate(&data);
		bs.sourcesTable = data.sourcesTable;
		bs.sourcesCtidColumn = data.sourcesCtidColumn;
		nHeapTuples = PGrnIndexBuildHeapScan(heap,
											 index,
											 indexInfo,
											 true,
											 PGrnBuildCallback,
											 &bs);
		PGrnSetSources(index, bs.sourcesTable);
		PGrnCreateSourcesTableFinish(&data);
	}
	PG_CATCH();
	{
		size_t i, n;

		n = GRN_BULK_VSIZE(&lexicons) / sizeof(grn_obj *);
		for (i = 0; i < n; i++)
		{
			grn_obj *lexicon;
			lexicon = GRN_PTR_VALUE_AT(&lexicons, i);
			grn_obj_remove(ctx, lexicon);
		}
		GRN_OBJ_FIN(ctx, &lexicons);

		n = GRN_BULK_VSIZE(&supplementaryTables) / sizeof(grn_obj *);
		for (i = 0; i < n; i++)
		{
			grn_obj *supplementaryTable;
			supplementaryTable = GRN_PTR_VALUE_AT(&supplementaryTables, i);
			grn_obj_remove(ctx, supplementaryTable);
		}
		GRN_OBJ_FIN(ctx, &supplementaryTables);

		if (data.sourcesTable)
			grn_obj_remove(ctx, data.sourcesTable);

		PG_RE_THROW();
	}
	PG_END_TRY();
	GRN_OBJ_FIN(ctx, &lexicons);
	GRN_OBJ_FIN(ctx, &supplementaryTables);

	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));
	result->heap_tuples = nHeapTuples;
	result->index_tuples = bs.nIndexedTuples;

	MemoryContextDelete(bs.memoryContext);

	if (bs.needMaxRecordSizeUpdate)
	{
		PGrnUpdateMaxRecordSize(index, bs.maxRecordSize);
	}

	return result;
}

/**
 * pgroonga.build() -- ambuild
 */
Datum
pgroonga_build(PG_FUNCTION_ARGS)
{
	Relation heap = (Relation) PG_GETARG_POINTER(0);
	Relation index = (Relation) PG_GETARG_POINTER(1);
	IndexInfo *indexInfo = (IndexInfo *) PG_GETARG_POINTER(2);
	IndexBuildResult *result;

	result = pgroonga_build_raw(heap, index, indexInfo);

	PG_RETURN_POINTER(result);
}

static void
pgroonga_buildempty_raw(Relation index)
{
	const char *tag = "[build-empty]";
	PGrnCreateData data;
	grn_obj supplementaryTables;
	grn_obj lexicons;

	if (!PGrnIsWritable())
	{
		ereport(ERROR,
				(errcode(ERRCODE_E_R_E_MODIFYING_SQL_DATA_NOT_PERMITTED),
				 errmsg("pgroonga: %s "
						"can't create an empty index "
						"while pgroonga.writable is false",
						tag)));
	}

	PGrnAutoCloseUseIndex(index);

	GRN_PTR_INIT(&supplementaryTables, GRN_OBJ_VECTOR, GRN_ID_NIL);
	GRN_PTR_INIT(&lexicons, GRN_OBJ_VECTOR, GRN_ID_NIL);
	PG_TRY();
	{
		data.index = index;
		data.sourcesTable = NULL;
		data.sourcesCtidColumn = NULL;
		data.supplementaryTables = &supplementaryTables;
		data.lexicons = &lexicons;
		data.desc = RelationGetDescr(index);
		data.relNode = index->rd_node.relNode;
		PGrnCreate(&data);
		PGrnSetSources(index, data.sourcesTable);
		PGrnCreateSourcesTableFinish(&data);
	}
	PG_CATCH();
	{
		size_t i, n;

		n = GRN_BULK_VSIZE(&lexicons) / sizeof(grn_obj *);
		for (i = 0; i < n; i++)
		{
			grn_obj *lexicon;
			lexicon = GRN_PTR_VALUE_AT(&lexicons, i);
			grn_obj_remove(ctx, lexicon);
		}
		GRN_OBJ_FIN(ctx, &lexicons);

		n = GRN_BULK_VSIZE(&supplementaryTables) / sizeof(grn_obj *);
		for (i = 0; i < n; i++)
		{
			grn_obj *supplementaryTable;
			supplementaryTable = GRN_PTR_VALUE_AT(&supplementaryTables, i);
			grn_obj_remove(ctx, supplementaryTable);
		}
		GRN_OBJ_FIN(ctx, &supplementaryTables);

		if (data.sourcesTable)
			grn_obj_remove(ctx, data.sourcesTable);

		PG_RE_THROW();
	}
	PG_END_TRY();
	GRN_OBJ_FIN(ctx, &lexicons);
	GRN_OBJ_FIN(ctx, &supplementaryTables);
}

/**
 * pgroonga.buildempty() -- ambuildempty
 */
Datum
pgroonga_buildempty(PG_FUNCTION_ARGS)
{
	Relation index = (Relation) PG_GETARG_POINTER(0);

	pgroonga_buildempty_raw(index);

	PG_RETURN_VOID();
}

static IndexBulkDeleteResult *
PGrnBulkDeleteResult(IndexVacuumInfo *info, grn_obj *sourcesTable)
{
	IndexBulkDeleteResult *stats;

	stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
	stats->num_pages = (BlockNumber) 1;	/* TODO: sizeof index / BLCKSZ */

	/* table might be NULL if index is corrupted */
	if (sourcesTable)
		stats->num_index_tuples = grn_table_size(ctx, sourcesTable);
	else
		stats->num_index_tuples = 0;

	return stats;
}

static IndexBulkDeleteResult *
pgroonga_bulkdelete_raw(IndexVacuumInfo *info,
						IndexBulkDeleteResult *stats,
						IndexBulkDeleteCallback callback,
						void *callbackState)
{
	const char *tag = "[bulk-delete]";
	Relation index = info->index;
	grn_obj	*sourcesTable;
	grn_table_cursor *cursor;
	double nRemovedTuples;

	if (!PGrnIsWritable())
	{
		ereport(ERROR,
				(errcode(ERRCODE_E_R_E_MODIFYING_SQL_DATA_NOT_PERMITTED),
				 errmsg("pgroonga: %s "
						"can't delete bulk records "
						"while pgroonga.writable is false",
						tag)));
	}

	sourcesTable = PGrnLookupSourcesTable(index, WARNING);

	if (!stats)
		stats = PGrnBulkDeleteResult(info, sourcesTable);

	if (!sourcesTable || !callback)
		return stats;

	nRemovedTuples = 0;

	cursor = grn_table_cursor_open(ctx, sourcesTable,
								   NULL, 0, NULL, 0,
								   0, -1, 0);
	PGrnCheck("%s failed to open cursor", tag);

	PG_TRY();
	{
		grn_id id;
		grn_obj *sourcesCtidColumn = NULL;
		PGrnJSONBBulkDeleteData jsonbData;

		if (sourcesTable->header.type == GRN_TABLE_NO_KEY)
		{
			sourcesCtidColumn = PGrnLookupSourcesCtidColumn(index, ERROR);
		}

		jsonbData.index = index;
		jsonbData.sourcesTable = sourcesTable;
		PGrnJSONBBulkDeleteInit(&jsonbData);

		while ((id = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL)
		{
			uint64 packedCtid;
			ItemPointerData	ctid;

			CHECK_FOR_INTERRUPTS();

			if (sourcesCtidColumn)
			{
				GRN_BULK_REWIND(&(buffers->ctid));
				grn_obj_get_value(ctx, sourcesCtidColumn, id, &(buffers->ctid));
				if (GRN_BULK_VSIZE(&(buffers->ctid)) == 0)
				{
					GRN_LOG(ctx,
							GRN_LOG_DEBUG,
							"pgroonga: %s[nonexistent] <%s>(%u): <%u>",
							tag,
							index->rd_rel->relname.data,
							index->rd_id,
							id);
					continue;
				}
				packedCtid = GRN_UINT64_VALUE(&(buffers->ctid));
			}
			else
			{
				void *key;
				int keySize;
				keySize = grn_table_cursor_get_key(ctx, cursor, &key);
				if (keySize == 0)
				{
					GRN_LOG(ctx,
							GRN_LOG_DEBUG,
							"pgroonga: %s[nonexistent] <%s>(%u): <%u>",
							tag,
							index->rd_rel->relname.data,
							index->rd_id,
							id);
					continue;
				}
				packedCtid = *((uint64 *) key);
			}
			ctid = PGrnCtidUnpack(packedCtid);
			if (callback(&ctid, callbackState))
			{
				GRN_LOG(ctx,
						GRN_LOG_DEBUG,
						"pgroonga: %s <%s>(%u): <%u>: <(%u,%u),%u>(%" PRIu64 ")",
						tag,
						index->rd_rel->relname.data,
						index->rd_id,
						id,
						ctid.ip_blkid.bi_hi,
						ctid.ip_blkid.bi_lo,
						ctid.ip_posid,
						packedCtid);

				jsonbData.id = id;
				PGrnJSONBBulkDeleteRecord(&jsonbData);

				grn_table_cursor_delete(ctx, cursor);
				PGrnWALDelete(index,
							  sourcesTable,
							  (const char *) &packedCtid,
							  sizeof(uint64));

				nRemovedTuples += 1;
			}
		}

		PGrnJSONBBulkDeleteFin(&jsonbData);

		grn_table_cursor_close(ctx, cursor);
	}
	PG_CATCH();
	{
		grn_table_cursor_close(ctx, cursor);
		PG_RE_THROW();
	}
	PG_END_TRY();

	stats->tuples_removed = nRemovedTuples;

	return stats;
}

/**
 * pgroonga.bulkdelete() -- ambulkdelete
 */
Datum
pgroonga_bulkdelete(PG_FUNCTION_ARGS)
{
	IndexVacuumInfo *info = (IndexVacuumInfo *) PG_GETARG_POINTER(0);
	IndexBulkDeleteResult *stats = (IndexBulkDeleteResult *) PG_GETARG_POINTER(1);
	IndexBulkDeleteCallback	callback = (IndexBulkDeleteCallback) PG_GETARG_POINTER(2);
	void *callbackState = (void *) PG_GETARG_POINTER(3);

	stats = pgroonga_bulkdelete_raw(info,
									stats,
									callback,
									callbackState);

	PG_RETURN_POINTER(stats);
}

static void
PGrnRemoveUnusedTable(Oid relationFileNodeID)
{
	unsigned int i;

	for (i = 0; true; i++)
	{
		char tableName[GRN_TABLE_MAX_KEY_SIZE];
		grn_obj *lexicon;

		snprintf(tableName, sizeof(tableName),
				 PGrnLexiconNameFormat, relationFileNodeID, i);
		lexicon = grn_ctx_get(ctx, tableName, -1);
		if (!lexicon)
			break;

		PGrnRemoveColumns(lexicon);
	}

	{
		char tableName[GRN_TABLE_MAX_KEY_SIZE];
		snprintf(tableName, sizeof(tableName),
				 PGrnSourcesTableNameFormat, relationFileNodeID);
		PGrnRemoveObject(tableName);
		PGrnAliasDeleteRaw(relationFileNodeID);
		PGrnIndexStatusDeleteRaw(relationFileNodeID);
	}

	for (i = 0; true; i++)
	{
		char tableName[GRN_TABLE_MAX_KEY_SIZE];
		grn_obj *lexicon;

		snprintf(tableName, sizeof(tableName),
				 PGrnLexiconNameFormat, relationFileNodeID, i);
		lexicon = grn_ctx_get(ctx, tableName, -1);
		if (!lexicon)
			break;

		PGrnRemoveObject(tableName);
	}

	PGrnJSONBRemoveUnusedTables(relationFileNodeID);
}

void
PGrnRemoveUnusedTables(void)
{
	grn_table_cursor *cursor;
	const char *min = PGrnSourcesTableNamePrefix;

	if (!PGrnIsWritable())
		return;

	cursor = grn_table_cursor_open(ctx, grn_ctx_db(ctx),
								   min, strlen(min),
								   NULL, 0,
								   0, -1, GRN_CURSOR_BY_KEY|GRN_CURSOR_PREFIX);
	while (grn_table_cursor_next(ctx, cursor) != GRN_ID_NIL)
	{
		void *key;
		char name[GRN_TABLE_MAX_KEY_SIZE];
		char *idEnd = NULL;
		int nameSize;
		Oid relationFileNodeID;

		nameSize = grn_table_cursor_get_key(ctx, cursor, &key);
		memcpy(name, key, nameSize);
		name[nameSize] = '\0';
		relationFileNodeID = strtol(name + strlen(min), &idEnd, 10);
		if (idEnd[0] == '.')
			continue;

		if (PGrnPGIsValidFileNodeID(relationFileNodeID))
			continue;

		PGrnRemoveUnusedTable(relationFileNodeID);
	}
	grn_table_cursor_close(ctx, cursor);
}

static IndexBulkDeleteResult *
pgroonga_vacuumcleanup_raw(IndexVacuumInfo *info,
						   IndexBulkDeleteResult *stats)
{
	if (!PGrnIsWritable())
		return stats;

	if (!stats)
	{
		grn_obj *sourcesTable;
		sourcesTable = PGrnLookupSourcesTable(info->index, WARNING);
		stats = PGrnBulkDeleteResult(info, sourcesTable);
	}

	PGrnRemoveUnusedTables();

	return stats;
}

/**
 * pgroonga.vacuumcleanup() -- amvacuumcleanup
 */
Datum
pgroonga_vacuumcleanup(PG_FUNCTION_ARGS)
{
	IndexVacuumInfo *info = (IndexVacuumInfo *) PG_GETARG_POINTER(0);
	IndexBulkDeleteResult *stats = (IndexBulkDeleteResult *) PG_GETARG_POINTER(1);

	stats = pgroonga_vacuumcleanup_raw(info, stats);

	PG_RETURN_POINTER(stats);
}

static bool
pgroonga_canreturn_raw(Relation index,
					   int nthAttribute)
{
	bool can_return = true;
	Relation table = RelationIdGetRelation(index->rd_index->indrelid);
	TupleDesc table_desc = RelationGetDescr(table);
	TupleDesc desc = RelationGetDescr(index);
	unsigned int i;

	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attribute = TupleDescAttr(desc, i);

		bool is_nullable = true;
		unsigned int j;
		for (j = 0; j < table_desc->natts; j++)
		{
			Form_pg_attribute table_attribute = TupleDescAttr(table_desc, j);
			if (strcmp(NameStr(table_attribute->attname),
					   NameStr(attribute->attname)) != 0)
				continue;
			if (table_attribute->atttypid != attribute->atttypid)
				continue;
			is_nullable = !table_attribute->attnotnull;
			break;
		}
		if (is_nullable)
		{
			can_return = false;
			break;
		}

		if (PGrnAttributeIsJSONB(attribute->atttypid))
		{
			can_return = false;
			break;
		}

		if (PGrnIsForPrefixSearchIndex(index, i))
		{
			can_return = false;
			break;
		}
	}
	RelationClose(table);

	if (!can_return)
	{
		return false;
	}

	return PGrnIndexStatusGetMaxRecordSize(index) <
		PGRN_INDEX_ONLY_SCAN_THRESHOLD_SIZE;
}

/**
 * pgroonga.canreturn() -- amcanreturn
 */
Datum
pgroonga_canreturn(PG_FUNCTION_ARGS)
{
	Relation index = (Relation) PG_GETARG_POINTER(0);
	int nthAttribute = PG_GETARG_INT32(1);
	bool can;

	can = pgroonga_canreturn_raw(index, nthAttribute);

	PG_RETURN_BOOL(can);
}

static void
PGrnCostEstimateUpdateSelectivityOne(PlannerInfo *root,
									 IndexPath *path,
									 Relation index,
									 grn_obj *sourcesTable,
									 RestrictInfo *info)
{
	IndexOptInfo *indexInfo = path->indexinfo;
	OpExpr *expr;
	int strategy;
	Oid leftType;
	Oid rightType;
	Node *leftNode;
	Node *rightNode;
	Node *estimatedRightNode;
	Var *var;
	int nthAttribute = InvalidAttrNumber;
	Oid opFamily = InvalidOid;
	ScanKeyData key;
	PGrnSearchData data;

	if (!IsA(info->clause, OpExpr))
		return;

	expr = (OpExpr *) info->clause;

	leftNode = get_leftop(info->clause);
	rightNode = get_rightop(info->clause);

	if (!IsA(leftNode, Var))
		return;

	estimatedRightNode = estimate_expression_value(root, rightNode);
	if (!IsA(estimatedRightNode, Const))
		return;

	var = (Var *) leftNode;
	{
		int i;

		for (i = 0; i < indexInfo->ncolumns; i++)
		{
			if (indexInfo->indexkeys[i] == var->varattno)
			{
				nthAttribute = i + 1;
				break;
			}
		}
	}
	if (!AttributeNumberIsValid(nthAttribute))
		return;

	opFamily = index->rd_opfamily[nthAttribute - 1];
	get_op_opfamily_properties(expr->opno,
							   opFamily,
							   false,
							   &strategy,
							   &leftType,
							   &rightType);

	key.sk_flags = 0;
	key.sk_attno = nthAttribute;
	key.sk_strategy = strategy;
	key.sk_argument = ((Const *) estimatedRightNode)->constvalue;
	PGrnSearchDataInit(&data, index, sourcesTable);
	if (PGrnSearchBuildCondition(index, &key, &data))
	{
		unsigned int estimatedSize;
		unsigned int nRecords;

		if (data.isEmptyCondition)
		{
			estimatedSize = 0;
		}
		else
		{
			estimatedSize = grn_expr_estimate_size(ctx, data.expression);
		}

		nRecords = grn_table_size(ctx, sourcesTable);
		if (estimatedSize > nRecords)
			estimatedSize = nRecords * 0.8;
		if (estimatedSize == nRecords)
		{
			/* TODO: estimatedSize == nRecords means
			 * estimation isn't support in Groonga. We should
			 * support it in Groonga. */
			info->norm_selec = 0.01;
		}
		else
		{
			info->norm_selec = (double) estimatedSize / (double) nRecords;
			/* path->path.rows = (double) estimatedSize; */
		}
	}
	else
	{
		info->norm_selec = 0.0;
	}
	PGrnSearchDataFree(&data);
}

static void
PGrnCostEstimateUpdateSelectivity(Relation index,
								  PlannerInfo *root,
								  IndexPath *path)
{
	grn_obj *sourcesTable;
	List *quals;
	ListCell *cell;

	PGrnWALApply(index);
	sourcesTable = PGrnLookupSourcesTable(index, ERROR);

#ifdef PGRN_SUPPORT_INDEX_CLAUSE
	quals = get_quals_from_indexclauses(path->indexclauses);
#else
	quals = path->indexquals;
#endif
	foreach(cell, quals)
	{
		Node *clause = (Node *) lfirst(cell);
		RestrictInfo *info;

		if (!IsA(clause, RestrictInfo))
			continue;

		info = (RestrictInfo *) clause;
		PGrnCostEstimateUpdateSelectivityOne(root,
											 path,
											 index,
											 sourcesTable,
											 info);
	}
}

static void
pgroonga_costestimate_internal(Relation index,
							   PlannerInfo *root,
							   IndexPath *path,
							   double loopCount,
							   Cost *indexStartupCost,
							   Cost *indexTotalCost,
							   Selectivity *indexSelectivity,
							   double *indexCorrelation,
							   double *indexPages)
{
	List *quals;
	PGrnCostEstimateUpdateSelectivity(index, root, path);
#ifdef PGRN_SUPPORT_INDEX_CLAUSE
	{
		List *indexQuals;
		indexQuals = get_quals_from_indexclauses(path->indexclauses);
		quals = add_predicate_to_index_quals(path->indexinfo, indexQuals);
	}
#else
	quals = path->indexquals;
#endif
	*indexSelectivity = clauselist_selectivity(root,
											   quals,
											   path->indexinfo->rel->relid,
											   JOIN_INNER,
											   NULL);

	*indexStartupCost = 0.0; /* TODO */
	*indexTotalCost = 0.0; /* TODO */
	*indexCorrelation = 0.0;
	*indexPages = 0.0; /* TODO */
}

static void
pgroonga_costestimate_raw(PlannerInfo *root,
						  IndexPath *path,
						  double loopCount,
						  Cost *indexStartupCost,
						  Cost *indexTotalCost,
						  Selectivity *indexSelectivity,
						  double *indexCorrelation,
						  double *indexPages)
{
	IndexOptInfo *indexInfo = path->indexinfo;
	Relation index = RelationIdGetRelation(indexInfo->indexoid);

	*indexSelectivity = 0.0;
	*indexStartupCost = 0.0;
	*indexTotalCost = 0.0;
	*indexCorrelation = 0.0;
	*indexPages = 0.0;

	PGRN_RLS_ENABLED_IF(PGrnCheckRLSEnabled(index->rd_index->indrelid));
	{
		pgroonga_costestimate_internal(index,
									   root,
									   path,
									   loopCount,
									   indexStartupCost,
									   indexTotalCost,
									   indexSelectivity,
									   indexCorrelation,
									   indexPages);
	}
	PGRN_RLS_ENABLED_ELSE();
	{
		pgroonga_costestimate_internal(index,
									   root,
									   path,
									   loopCount,
									   indexStartupCost,
									   indexTotalCost,
									   indexSelectivity,
									   indexCorrelation,
									   indexPages);
	}
	PGRN_RLS_ENABLED_END();
	RelationClose(index);
}

/**
 * pgroonga.costestimate() -- amcostestimate
 */
Datum
pgroonga_costestimate(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	IndexPath *path = (IndexPath *) PG_GETARG_POINTER(1);
	double loopCount = PG_GETARG_FLOAT8(2);
	Cost *indexStartupCost = (Cost *) PG_GETARG_POINTER(3);
	Cost *indexTotalCost = (Cost *) PG_GETARG_POINTER(4);
	Selectivity *indexSelectivity = (Selectivity *) PG_GETARG_POINTER(5);
	double *indexCorrelation = (double *) PG_GETARG_POINTER(6);
	double indexPages;

	pgroonga_costestimate_raw(root,
							  path,
							  loopCount,
							  indexStartupCost,
							  indexTotalCost,
							  indexSelectivity,
							  indexCorrelation,
							  &indexPages);

	PG_RETURN_VOID();
}

static bool
pgroonga_validate_raw(Oid opClassOid)
{
	return true;
}

static Size
pgroonga_estimateparallelscan_raw(void)
{
	return sizeof(PGrnParallelScanDescData);
}

static void
pgroonga_initparallelscan_raw(void *target)
{
	PGrnParallelScanDesc pgrnParallelScan = (PGrnParallelScanDesc) target;

	SpinLockInit(&(pgrnParallelScan->mutex));
	pgrnParallelScan->scanning = false;
}

static void
pgroonga_parallelrescan_raw(IndexScanDesc scan)
{
	ParallelIndexScanDesc parallelScan = scan->parallel_scan;
	PGrnParallelScanDesc pgrnParallelScan =
		OffsetToPointer((void *) (parallelScan),
						parallelScan->ps_offset);

	pgrnParallelScan->scanning = false;
}

static bool
PGrnParallelScanAcquire(IndexScanDesc scan)
{
	PGrnScanOpaque so = (PGrnScanOpaque) scan->opaque;
	ParallelIndexScanDesc parallelScan = scan->parallel_scan;
	PGrnParallelScanDesc pgrnParallelScan =
		OffsetToPointer((void *) (parallelScan),
						parallelScan->ps_offset);
	bool acquired = false;

	if (so->indexCursor)
		return true;
	if (so->tableCursor)
		return true;

	SpinLockAcquire(&(pgrnParallelScan->mutex));
	if (!pgrnParallelScan->scanning) {
		acquired = true;
		pgrnParallelScan->scanning = true;
	}
	SpinLockRelease(&(pgrnParallelScan->mutex));
	return acquired;
}

Datum
pgroonga_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *routine = makeNode(IndexAmRoutine);

	routine->amstrategies = PGRN_N_STRATEGIES;
	routine->amsupport    = 0;
	routine->amcanorder = true;
	routine->amcanorderbyop = true;
	routine->amcanbackward = true;
	routine->amcanunique = true;
	routine->amcanmulticol = true;
	routine->amoptionalkey = true;
	routine->amsearcharray = true;
	routine->amsearchnulls = false;
	routine->amstorage = false;
	routine->amclusterable = true;
	routine->ampredlocks = false;
	routine->amcanparallel = true;
#ifdef PGRN_INDEX_AM_ROUTINE_HAVE_AM_CAN_INCLUDE
	routine->amcaninclude = true;
#endif
#ifdef PGRN_INDEX_AM_ROUTINE_HAVE_AM_USE_MAINTENANCE_WORK_MEM
	routine->amusemaintenanceworkmem = false;
#endif
#ifdef PGRN_INDEX_AM_ROUTINE_HAVE_AM_PARALLEL_VACUUM_OPTIONS
	routine->amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL;
#endif
#ifdef PGRN_INDEX_AM_ROUTINE_HAVE_AM_HOT_BLOCKING
	routine->amhotblocking = true;
#endif
	routine->amkeytype = InvalidOid;

	routine->aminsert = pgroonga_insert_raw;
	routine->ambeginscan = pgroonga_beginscan_raw;
	routine->amgettuple = pgroonga_gettuple_raw;
	routine->amgetbitmap = pgroonga_getbitmap_raw;
	routine->amrescan = pgroonga_rescan_raw;
	routine->amendscan = pgroonga_endscan_raw;
	routine->ammarkpos = NULL;
	routine->amrestrpos = NULL;
	routine->ambuild = pgroonga_build_raw;
	routine->ambuildempty = pgroonga_buildempty_raw;
	routine->ambulkdelete = pgroonga_bulkdelete_raw;
	routine->amvacuumcleanup = pgroonga_vacuumcleanup_raw;
	routine->amcanreturn = pgroonga_canreturn_raw;
	routine->amcostestimate = pgroonga_costestimate_raw;
	routine->amoptions = pgroonga_options_raw;
	routine->amvalidate = pgroonga_validate_raw;
	routine->amestimateparallelscan = pgroonga_estimateparallelscan_raw;
	routine->aminitparallelscan = pgroonga_initparallelscan_raw;
	routine->amparallelrescan = pgroonga_parallelrescan_raw;

	PG_RETURN_POINTER(routine);
}

bool
PGrnIndexIsPGroonga(Relation index)
{
	if (!PGRN_RELATION_GET_RD_INDAM(index))
		return false;
	return PGRN_RELATION_GET_RD_INDAM(index)->aminsert == pgroonga_insert_raw;
}
