/*-------------------------------------------------------------------------
 *
 * postgres.c
 *	  POSTGRES C Backend Interface
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/tcop/postgres.c
 *
 * NOTES
 *	  this is the "main" module of the postgres backend and
 *	  hence the main module of the "traffic cop".
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/time.h>
#include <sys/resource.h>
#endif

#ifndef HAVE_GETRUSAGE
#include "rusagestub.h"
#endif

#include "access/parallel.h"
#include "access/printtup.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/async.h"
#include "commands/prepare.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/print.h"
#include "optimizer/planner.h"
#include "pgstat.h"
#include "pg_trace.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "pg_getopt.h"
#include "postmaster/autovacuum.h"
#include "postmaster/postmaster.h"
#include "replication/logicallauncher.h"
#include "replication/logicalworker.h"
#include "replication/slot.h"
#include "replication/walsender.h"
#include "rewrite/rewriteHandler.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/sinval.h"
#include "tcop/fastpath.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"
#include "mb/pg_wchar.h"


/* ----------------
 *		global variables
 * ----------------
 */
const char *debug_query_string; /* client-supplied query string */

/* Note: whereToSendOutput is initialized for the bootstrap/standalone case */
CommandDest whereToSendOutput = DestDebug;

/* flag for logging end of session */
bool		Log_disconnections = false;

int			log_statement = LOGSTMT_NONE;

/* GUC variable for maximum stack depth (measured in kilobytes) */
int			max_stack_depth = 100;

/* wait N seconds to allow attach from a debugger */
int			PostAuthDelay = 0;



/* ----------------
 *		private variables
 * ----------------
 */

/* max_stack_depth converted to bytes for speed of checking */
static long max_stack_depth_bytes = 100 * 1024L;

/*
 * Stack base pointer -- initialized by PostmasterMain and inherited by
 * subprocesses. This is not static because old versions of PL/Java modify
 * it directly. Newer versions use set_stack_base(), but we want to stay
 * binary-compatible for the time being.
 */
char	   *stack_base_ptr = NULL;

/*
 * On IA64 we also have to remember the register stack base.
 */
#if defined(__ia64__) || defined(__ia64)
char	   *register_stack_base_ptr = NULL;
#endif

/*
 * Flag to keep track of whether we have started a transaction.
 * For extended query protocol this has to be remembered across messages.
 */
static bool xact_started = false;

/*
 * Flag to indicate that we are doing the outer loop's read-from-client,
 * as opposed to any random read from client that might happen within
 * commands like COPY FROM STDIN.
 */
static bool DoingCommandRead = false;

/*
 * Flags to implement skip-till-Sync-after-error behavior for messages of
 * the extended query protocol.
 */
static bool doing_extended_query_message = false;
static bool ignore_till_sync = false;

/*
 * If an unnamed prepared statement exists, it's stored here.
 * We keep it separate from the hashtable kept by commands/prepare.c
 * in order to reduce overhead for short-lived queries.
 */
static CachedPlanSource *unnamed_stmt_psrc = NULL;

/* assorted command-line switches */
static const char *userDoption = NULL;	/* -D switch */
static bool EchoQuery = false;	/* -E switch */
static bool UseSemiNewlineNewline = false;	/* -j switch */

/* whether or not, and why, we were canceled by conflict with recovery */
static bool RecoveryConflictPending = false;
static bool RecoveryConflictRetryable = true;
static ProcSignalReason RecoveryConflictReason;

/* ----------------------------------------------------------------
 *		decls for routines only used in this file
 * ----------------------------------------------------------------
 */
static int	InteractiveBackend(StringInfo inBuf);
static int	interactive_getc(void);
static int	SocketBackend(StringInfo inBuf);
static int	ReadCommand(StringInfo inBuf);
static void forbidden_in_wal_sender(char firstchar);
static List *pg_rewrite_query(Query *query);
static bool check_log_statement(List *stmt_list);
static int	errdetail_execute(List *raw_parsetree_list);
static int	errdetail_params(ParamListInfo params);
static int	errdetail_abort(void);
static int	errdetail_recovery_conflict(void);
static void start_xact_command(void);
static void finish_xact_command(void);
static bool IsTransactionExitStmt(Node *parsetree);
static bool IsTransactionExitStmtList(List *pstmts);
static bool IsTransactionStmtList(List *pstmts);
static void drop_unnamed_stmt(void);
static void log_disconnections(int code, Datum arg);


/* ----------------------------------------------------------------
 *		routines to obtain user input
 * ----------------------------------------------------------------
 */

/* ----------------
 *	InteractiveBackend() is called for user interactive connections
 *
 *	the string entered by the user is placed in its parameter inBuf,
 *	and we act like a Q message was received.
 *
 *	EOF is returned if end-of-file input is seen; time to shut down.
 * ----------------
 */

static int
InteractiveBackend(StringInfo inBuf)
{
	int			c;				/* character read from getc() */

	/*
	 * display a prompt and obtain input from the user
	 */
	printf("backend> ");
	fflush(stdout);

	resetStringInfo(inBuf);

	/*
	 * Read characters until EOF or the appropriate delimiter is seen.
	 */
	while ((c = interactive_getc()) != EOF)
	{
		if (c == '\n')
		{
			if (UseSemiNewlineNewline)
			{
				/*
				 * In -j mode, semicolon followed by two newlines ends the
				 * command; otherwise treat newline as regular character.
				 */
				if (inBuf->len > 1 &&
					inBuf->data[inBuf->len - 1] == '\n' &&
					inBuf->data[inBuf->len - 2] == ';')
				{
					/* might as well drop the second newline */
					break;
				}
			}
			else
			{
				/*
				 * In plain mode, newline ends the command unless preceded by
				 * backslash.
				 */
				if (inBuf->len > 0 &&
					inBuf->data[inBuf->len - 1] == '\\')
				{
					/* discard backslash from inBuf */
					inBuf->data[--inBuf->len] = '\0';
					/* discard newline too */
					continue;
				}
				else
				{
					/* keep the newline character, but end the command */
					appendStringInfoChar(inBuf, '\n');
					break;
				}
			}
		}

		/* Not newline, or newline treated as regular character */
		appendStringInfoChar(inBuf, (char) c);
	}

	/* No input before EOF signal means time to quit. */
	if (c == EOF && inBuf->len == 0)
		return EOF;

	/*
	 * otherwise we have a user query so process it.
	 */

	/* Add '\0' to make it look the same as message case. */
	appendStringInfoChar(inBuf, (char) '\0');

	/*
	 * if the query echo flag was given, print the query..
	 */
	if (EchoQuery)
		printf("statement: %s\n", inBuf->data);
	fflush(stdout);

	return 'Q';
}

/*
 * interactive_getc -- collect one character from stdin
 *
 * Even though we are not reading from a "client" process, we still want to
 * respond to signals, particularly SIGTERM/SIGQUIT.
 */
static int
interactive_getc(void)
{
	int			c;

	/*
	 * This will not process catchup interrupts or notifications while
	 * reading. But those can't really be relevant for a standalone backend
	 * anyway. To properly handle SIGTERM there's a hack in die() that
	 * directly processes interrupts at this stage...
	 */
	CHECK_FOR_INTERRUPTS();

	c = getc(stdin);

	ProcessClientReadInterrupt(true);

	return c;
}

/* ----------------
 *	SocketBackend()		Is called for frontend-backend connections
 *
 *	Returns the message type code, and loads message body data into inBuf.
 *
 *	EOF is returned if the connection is lost.
 * ----------------
 */
static int
SocketBackend(StringInfo inBuf)
{
	int			qtype;

	/*
	 * Get message type code from the frontend.
	 */
	HOLD_CANCEL_INTERRUPTS();
	pq_startmsgread();
	qtype = pq_getbyte();

	if (qtype == EOF)			/* frontend disconnected */
	{
		if (IsTransactionState())
			ereport(COMMERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("unexpected EOF on client connection with an open transaction")));
		else
		{
			/*
			 * Can't send DEBUG log messages to client at this point. Since
			 * we're disconnecting right away, we don't need to restore
			 * whereToSendOutput.
			 */
			whereToSendOutput = DestNone;
			ereport(DEBUG1,
					(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
					 errmsg("unexpected EOF on client connection")));
		}
		return qtype;
	}

	/*
	 * Validate message type code before trying to read body; if we have lost
	 * sync, better to say "command unknown" than to run out of memory because
	 * we used garbage as a length word.
	 *
	 * This also gives us a place to set the doing_extended_query_message flag
	 * as soon as possible.
	 */
	switch (qtype)
	{
		case 'Q':				/* simple query */
			doing_extended_query_message = false;
			if (PG_PROTOCOL_MAJOR(FrontendProtocol) < 3)
			{
				/* old style without length word; convert */
				if (pq_getstring(inBuf))
				{
					if (IsTransactionState())
						ereport(COMMERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
								 errmsg("unexpected EOF on client connection with an open transaction")));
					else
					{
						/*
						 * Can't send DEBUG log messages to client at this
						 * point. Since we're disconnecting right away, we
						 * don't need to restore whereToSendOutput.
						 */
						whereToSendOutput = DestNone;
						ereport(DEBUG1,
								(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
								 errmsg("unexpected EOF on client connection")));
					}
					return EOF;
				}
			}
			break;

		case 'F':				/* fastpath function call */
			doing_extended_query_message = false;
			if (PG_PROTOCOL_MAJOR(FrontendProtocol) < 3)
			{
				if (GetOldFunctionMessage(inBuf))
				{
					if (IsTransactionState())
						ereport(COMMERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
								 errmsg("unexpected EOF on client connection with an open transaction")));
					else
					{
						/*
						 * Can't send DEBUG log messages to client at this
						 * point. Since we're disconnecting right away, we
						 * don't need to restore whereToSendOutput.
						 */
						whereToSendOutput = DestNone;
						ereport(DEBUG1,
								(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
								 errmsg("unexpected EOF on client connection")));
					}
					return EOF;
				}
			}
			break;

		case 'X':				/* terminate */
			doing_extended_query_message = false;
			ignore_till_sync = false;
			break;

		case 'B':				/* bind */
		case 'C':				/* close */
		case 'D':				/* describe */
		case 'E':				/* execute */
		case 'H':				/* flush */
		case 'P':				/* parse */
			doing_extended_query_message = true;
			/* these are only legal in protocol 3 */
			if (PG_PROTOCOL_MAJOR(FrontendProtocol) < 3)
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid frontend message type %d", qtype)));
			break;

		case 'S':				/* sync */
			/* stop any active skip-till-Sync */
			ignore_till_sync = false;
			/* mark not-extended, so that a new error doesn't begin skip */
			doing_extended_query_message = false;
			/* only legal in protocol 3 */
			if (PG_PROTOCOL_MAJOR(FrontendProtocol) < 3)
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid frontend message type %d", qtype)));
			break;

		case 'd':				/* copy data */
		case 'c':				/* copy done */
		case 'f':				/* copy fail */
			doing_extended_query_message = false;
			/* these are only legal in protocol 3 */
			if (PG_PROTOCOL_MAJOR(FrontendProtocol) < 3)
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid frontend message type %d", qtype)));
			break;

		default:

			/*
			 * Otherwise we got garbage from the frontend.  We treat this as
			 * fatal because we have probably lost message boundary sync, and
			 * there's no good way to recover.
			 */
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid frontend message type %d", qtype)));
			break;
	}

	/*
	 * In protocol version 3, all frontend messages have a length word next
	 * after the type code; we can read the message contents independently of
	 * the type.
	 */
	if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
	{
		if (pq_getmessage(inBuf, 0))
			return EOF;			/* suitable message already logged */
	}
	else
		pq_endmsgread();
	RESUME_CANCEL_INTERRUPTS();

	return qtype;
}

/* ----------------
 *		ReadCommand reads a command from either the frontend or
 *		standard input, places it in inBuf, and returns the
 *		message type code (first byte of the message).
 *		EOF is returned if end of file.
 * ----------------
 */
static int
ReadCommand(StringInfo inBuf)
{
	int			result;

	if (whereToSendOutput == DestRemote)
		result = SocketBackend(inBuf);
	else
		result = InteractiveBackend(inBuf);
	return result;
}

/*
 * ProcessClientReadInterrupt() - Process interrupts specific to client reads
 *
 * This is called just after low-level reads. That might be after the read
 * finished successfully, or it was interrupted via interrupt.
 *
 * Must preserve errno!
 */
void
ProcessClientReadInterrupt(bool blocked)
{
	int			save_errno = errno;

	if (DoingCommandRead)
	{
		/* Check for general interrupts that arrived while reading */
		CHECK_FOR_INTERRUPTS();

		/* Process sinval catchup interrupts that happened while reading */
		if (catchupInterruptPending)
			ProcessCatchupInterrupt();

		/* Process sinval catchup interrupts that happened while reading */
		if (notifyInterruptPending)
			ProcessNotifyInterrupt();
	}
	else if (ProcDiePending && blocked)
	{
		/*
		 * We're dying. It's safe (and sane) to handle that now.
		 */
		CHECK_FOR_INTERRUPTS();
	}

	errno = save_errno;
}

/*
 * ProcessClientWriteInterrupt() - Process interrupts specific to client writes
 *
 * This is called just after low-level writes. That might be after the read
 * finished successfully, or it was interrupted via interrupt. 'blocked' tells
 * us whether the
 *
 * Must preserve errno!
 */
void
ProcessClientWriteInterrupt(bool blocked)
{
	int			save_errno = errno;

	/*
	 * We only want to process the interrupt here if socket writes are
	 * blocking to increase the chance to get an error message to the client.
	 * If we're not blocked there'll soon be a CHECK_FOR_INTERRUPTS(). But if
	 * we're blocked we'll never get out of that situation if the client has
	 * died.
	 */
	if (ProcDiePending && blocked)
	{
		/*
		 * We're dying. It's safe (and sane) to handle that now. But we don't
		 * want to send the client the error message as that a) would possibly
		 * block again b) would possibly lead to sending an error message to
		 * the client, while we already started to send something else.
		 */
		if (whereToSendOutput == DestRemote)
			whereToSendOutput = DestNone;

		CHECK_FOR_INTERRUPTS();
	}

	errno = save_errno;
}

/*
 * Do raw parsing (only).
 *
 * A list of parsetrees (RawStmt nodes) is returned, since there might be
 * multiple commands in the given string.
 *
 * NOTE: for interactive queries, it is important to keep this routine
 * separate from the analysis & rewrite stages.  Analysis and rewriting
 * cannot be done in an aborted transaction, since they require access to
 * database tables.  So, we rely on the raw parser to determine whether
 * we've seen a COMMIT or ABORT command; when we are in abort state, other
 * commands are not processed any further than the raw parse stage.
 */
List *
pg_parse_query(const char *query_string)
{
	List	   *raw_parsetree_list;

	TRACE_POSTGRESQL_QUERY_PARSE_START(query_string);

	if (log_parser_stats)
		ResetUsage();

	raw_parsetree_list = raw_parser(query_string);

	if (log_parser_stats)
		ShowUsage("PARSER STATISTICS");

#ifdef COPY_PARSE_PLAN_TREES
	/* Optional debugging check: pass raw parsetrees through copyObject() */
	{
		List	   *new_list = copyObject(raw_parsetree_list);

		/* This checks both copyObject() and the equal() routines... */
		if (!equal(new_list, raw_parsetree_list))
			elog(WARNING, "copyObject() failed to produce an equal raw parse tree");
		else
			raw_parsetree_list = new_list;
	}
#endif

	TRACE_POSTGRESQL_QUERY_PARSE_DONE(query_string);

	return raw_parsetree_list;
}

/*
 * Given a raw parsetree (gram.y output), and optionally information about
 * types of parameter symbols ($n), perform parse analysis and rule rewriting.
 *
 * A list of Query nodes is returned, since either the analyzer or the
 * rewriter might expand one query to several.
 *
 * NOTE: for reasons mentioned above, this must be separate from raw parsing.
 */
List *
pg_analyze_and_rewrite(RawStmt *parsetree, const char *query_string,
					   Oid *paramTypes, int numParams,
					   QueryEnvironment *queryEnv)
{
	Query	   *query;
	List	   *querytree_list;

	TRACE_POSTGRESQL_QUERY_REWRITE_START(query_string);

	/*
	 * (1) Perform parse analysis.
	 */
	if (log_parser_stats)
		ResetUsage();

	query = parse_analyze(parsetree, query_string, paramTypes, numParams,
						  queryEnv);

	if (log_parser_stats)
		ShowUsage("PARSE ANALYSIS STATISTICS");

	/*
	 * (2) Rewrite the queries, as necessary
	 */
	querytree_list = pg_rewrite_query(query);

	TRACE_POSTGRESQL_QUERY_REWRITE_DONE(query_string);

	return querytree_list;
}

/*
 * Do parse analysis and rewriting.  This is the same as pg_analyze_and_rewrite
 * except that external-parameter resolution is determined by parser callback
 * hooks instead of a fixed list of parameter datatypes.
 */
List *
pg_analyze_and_rewrite_params(RawStmt *parsetree,
							  const char *query_string,
							  ParserSetupHook parserSetup,
							  void *parserSetupArg,
							  QueryEnvironment *queryEnv)
{
	ParseState *pstate;
	Query	   *query;
	List	   *querytree_list;

	Assert(query_string != NULL);	/* required as of 8.4 */

	TRACE_POSTGRESQL_QUERY_REWRITE_START(query_string);

	/*
	 * (1) Perform parse analysis.
	 */
	if (log_parser_stats)
		ResetUsage();

	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = query_string;
	pstate->p_queryEnv = queryEnv;
	(*parserSetup) (pstate, parserSetupArg);

	query = transformTopLevelStmt(pstate, parsetree);

	if (post_parse_analyze_hook)
		(*post_parse_analyze_hook) (pstate, query);

	free_parsestate(pstate);

	if (log_parser_stats)
		ShowUsage("PARSE ANALYSIS STATISTICS");

	/*
	 * (2) Rewrite the queries, as necessary
	 */
	querytree_list = pg_rewrite_query(query);

	TRACE_POSTGRESQL_QUERY_REWRITE_DONE(query_string);

	return querytree_list;
}

/*
 * Perform rewriting of a query produced by parse analysis.
 *
 * Note: query must just have come from the parser, because we do not do
 * AcquireRewriteLocks() on it.
 */
static List *
pg_rewrite_query(Query *query)
{
	List	   *querytree_list;

	if (Debug_print_parse)
		elog_node_display(LOG, "parse tree", query,
						  Debug_pretty_print);

	if (log_parser_stats)
		ResetUsage();

	if (query->commandType == CMD_UTILITY)
	{
		/* don't rewrite utilities, just dump 'em into result list */
		querytree_list = list_make1(query);
	}
	else
	{
		/* rewrite regular queries */
		querytree_list = QueryRewrite(query);
	}

	if (log_parser_stats)
		ShowUsage("REWRITER STATISTICS");

#ifdef COPY_PARSE_PLAN_TREES
	/* Optional debugging check: pass querytree output through copyObject() */
	{
		List	   *new_list;

		new_list = copyObject(querytree_list);
		/* This checks both copyObject() and the equal() routines... */
		if (!equal(new_list, querytree_list))
			elog(WARNING, "copyObject() failed to produce equal parse tree");
		else
			querytree_list = new_list;
	}
#endif

	if (Debug_print_rewritten)
		elog_node_display(LOG, "rewritten parse tree", querytree_list,
						  Debug_pretty_print);

	return querytree_list;
}


/*
 * Generate a plan for a single already-rewritten query.
 * This is a thin wrapper around planner() and takes the same parameters.
 */
PlannedStmt *
pg_plan_query(Query *querytree, int cursorOptions, ParamListInfo boundParams)
{
	PlannedStmt *plan;

	/* Utility commands have no plans. */
	if (querytree->commandType == CMD_UTILITY)
		return NULL;

	/* Planner must have a snapshot in case it calls user-defined functions. */
	Assert(ActiveSnapshotSet());

	TRACE_POSTGRESQL_QUERY_PLAN_START();

	if (log_planner_stats)
		ResetUsage();

	/* call the optimizer */
	plan = planner(querytree, cursorOptions, boundParams);

	if (log_planner_stats)
		ShowUsage("PLANNER STATISTICS");

#ifdef COPY_PARSE_PLAN_TREES
	/* Optional debugging check: pass plan output through copyObject() */
	{
		PlannedStmt *new_plan = copyObject(plan);

		/*
		 * equal() currently does not have routines to compare Plan nodes, so
		 * don't try to test equality here.  Perhaps fix someday?
		 */
#ifdef NOT_USED
		/* This checks both copyObject() and the equal() routines... */
		if (!equal(new_plan, plan))
			elog(WARNING, "copyObject() failed to produce an equal plan tree");
		else
#endif
			plan = new_plan;
	}
#endif

	/*
	 * Print plan if debugging.
	 */
	if (Debug_print_plan)
		elog_node_display(LOG, "plan", plan, Debug_pretty_print);

	TRACE_POSTGRESQL_QUERY_PLAN_DONE();

	return plan;
}

/*
 * Generate plans for a list of already-rewritten queries.
 *
 * For normal optimizable statements, invoke the planner.  For utility
 * statements, just make a wrapper PlannedStmt node.
 *
 * The result is a list of PlannedStmt nodes.
 */
List *
pg_plan_queries(List *querytrees, int cursorOptions, ParamListInfo boundParams)
{
	List	   *stmt_list = NIL;
	ListCell   *query_list;

	foreach(query_list, querytrees)
	{
		Query	   *query = lfirst_node(Query, query_list);
		PlannedStmt *stmt;

		if (query->commandType == CMD_UTILITY)
		{
			/* Utility commands require no planning. */
			stmt = makeNode(PlannedStmt);
			stmt->commandType = CMD_UTILITY;
			stmt->canSetTag = query->canSetTag;
			stmt->utilityStmt = query->utilityStmt;
			stmt->stmt_location = query->stmt_location;
			stmt->stmt_len = query->stmt_len;
		}
		else
		{
			stmt = pg_plan_query(query, cursorOptions, boundParams);
		}

		stmt_list = lappend(stmt_list, stmt);
	}

	return stmt_list;
}














const FILE *fptr;





typedef struct {
	char* message; // exception message
	char* funcname; // source function of exception (e.g. SearchSysCache)
	char* filename; // source of exception (e.g. parse.l)
	int lineno; // source of exception (e.g. 104)
	int cursorpos; // char in query at which exception occurred
	char* context; // additional context (optional, can be NULL)
} PgQueryError;

typedef struct {
  char* parse_tree;
  char* stderr_buffer;
  PgQueryError* error;
} PgQueryParseResult;

typedef struct {
  char* plpgsql_funcs;
  PgQueryError* error;
} PgQueryPlpgsqlParseResult;

typedef struct {
  char* hexdigest;
  char* stderr_buffer;
  PgQueryError* error;
} PgQueryFingerprintResult;

typedef struct {
  char* normalized_query;
  PgQueryError* error;
} PgQueryNormalizeResult;



#define STDERR_BUFFER_LEN 4096
#define DEBUG

typedef struct {
  List *tree;
  char* stderr_buffer;
  PgQueryError* error;
} PgQueryInternalParsetreeAndError;




struct sha1_ctxt
{
	union
	{
		uint8		b8[20];
		uint32		b32[5];
	}			h;
	union
	{
		uint8		b8[8];
		uint64		b64[1];
	}			c;
	union
	{
		uint8		b8[64];
		uint32		b32[16];
	}			m;
	uint8		count;
};

extern void sha1_init(struct sha1_ctxt *);
extern void sha1_pad(struct sha1_ctxt *);
extern void sha1_loop(struct sha1_ctxt *, const uint8 *, size_t);
extern void sha1_result(struct sha1_ctxt *, uint8 *);

/* compatibility with other SHA1 source codes */
typedef struct sha1_ctxt SHA1_CTX;

#define SHA1Init(x)		sha1_init((x))
#define SHA1Update(x, y, z) sha1_loop((x), (y), (z))
#define SHA1Final(x, y)		sha1_result((y), (x))

#define SHA1_RESULTLEN	(160/8)

#include <sys/param.h>

static __thread uint32 _K[] = {0x5a827999, 0x6ed9eba1, 0x8f1bbcdc, 0xca62c1d6};


#define K(t)	_K[(t) / 20]

#define F0(b, c, d) (((b) & (c)) | ((~(b)) & (d)))
#define F1(b, c, d) (((b) ^ (c)) ^ (d))
#define F2(b, c, d) (((b) & (c)) | ((b) & (d)) | ((c) & (d)))
#define F3(b, c, d) (((b) ^ (c)) ^ (d))

#define S(n, x)		(((x) << (n)) | ((x) >> (32 - (n))))

#define H(n)	(ctxt->h.b32[(n)])
#define COUNT	(ctxt->count)
#define BCOUNT	(ctxt->c.b64[0] / 8)
#define W(n)	(ctxt->m.b32[(n)])

#define PUTBYTE(x) \
do { \
	ctxt->m.b8[(COUNT % 64)] = (x);		\
	COUNT++;				\
	COUNT %= 64;				\
	ctxt->c.b64[0] += 8;			\
	if (COUNT % 64 == 0)			\
		sha1_step(ctxt);		\
} while (0)

#define PUTPAD(x) \
do { \
	ctxt->m.b8[(COUNT % 64)] = (x);		\
	COUNT++;				\
	COUNT %= 64;				\
	if (COUNT % 64 == 0)			\
		sha1_step(ctxt);		\
} while (0)

static void sha1_step(struct sha1_ctxt *);

static void
sha1_step(struct sha1_ctxt *ctxt)
{
	uint32		a,
				b,
				c,
				d,
				e;
	size_t		t,
				s;
	uint32		tmp;

#ifndef WORDS_BIGENDIAN
	struct sha1_ctxt tctxt;

	memmove(&tctxt.m.b8[0], &ctxt->m.b8[0], 64);
	ctxt->m.b8[0] = tctxt.m.b8[3];
	ctxt->m.b8[1] = tctxt.m.b8[2];
	ctxt->m.b8[2] = tctxt.m.b8[1];
	ctxt->m.b8[3] = tctxt.m.b8[0];
	ctxt->m.b8[4] = tctxt.m.b8[7];
	ctxt->m.b8[5] = tctxt.m.b8[6];
	ctxt->m.b8[6] = tctxt.m.b8[5];
	ctxt->m.b8[7] = tctxt.m.b8[4];
	ctxt->m.b8[8] = tctxt.m.b8[11];
	ctxt->m.b8[9] = tctxt.m.b8[10];
	ctxt->m.b8[10] = tctxt.m.b8[9];
	ctxt->m.b8[11] = tctxt.m.b8[8];
	ctxt->m.b8[12] = tctxt.m.b8[15];
	ctxt->m.b8[13] = tctxt.m.b8[14];
	ctxt->m.b8[14] = tctxt.m.b8[13];
	ctxt->m.b8[15] = tctxt.m.b8[12];
	ctxt->m.b8[16] = tctxt.m.b8[19];
	ctxt->m.b8[17] = tctxt.m.b8[18];
	ctxt->m.b8[18] = tctxt.m.b8[17];
	ctxt->m.b8[19] = tctxt.m.b8[16];
	ctxt->m.b8[20] = tctxt.m.b8[23];
	ctxt->m.b8[21] = tctxt.m.b8[22];
	ctxt->m.b8[22] = tctxt.m.b8[21];
	ctxt->m.b8[23] = tctxt.m.b8[20];
	ctxt->m.b8[24] = tctxt.m.b8[27];
	ctxt->m.b8[25] = tctxt.m.b8[26];
	ctxt->m.b8[26] = tctxt.m.b8[25];
	ctxt->m.b8[27] = tctxt.m.b8[24];
	ctxt->m.b8[28] = tctxt.m.b8[31];
	ctxt->m.b8[29] = tctxt.m.b8[30];
	ctxt->m.b8[30] = tctxt.m.b8[29];
	ctxt->m.b8[31] = tctxt.m.b8[28];
	ctxt->m.b8[32] = tctxt.m.b8[35];
	ctxt->m.b8[33] = tctxt.m.b8[34];
	ctxt->m.b8[34] = tctxt.m.b8[33];
	ctxt->m.b8[35] = tctxt.m.b8[32];
	ctxt->m.b8[36] = tctxt.m.b8[39];
	ctxt->m.b8[37] = tctxt.m.b8[38];
	ctxt->m.b8[38] = tctxt.m.b8[37];
	ctxt->m.b8[39] = tctxt.m.b8[36];
	ctxt->m.b8[40] = tctxt.m.b8[43];
	ctxt->m.b8[41] = tctxt.m.b8[42];
	ctxt->m.b8[42] = tctxt.m.b8[41];
	ctxt->m.b8[43] = tctxt.m.b8[40];
	ctxt->m.b8[44] = tctxt.m.b8[47];
	ctxt->m.b8[45] = tctxt.m.b8[46];
	ctxt->m.b8[46] = tctxt.m.b8[45];
	ctxt->m.b8[47] = tctxt.m.b8[44];
	ctxt->m.b8[48] = tctxt.m.b8[51];
	ctxt->m.b8[49] = tctxt.m.b8[50];
	ctxt->m.b8[50] = tctxt.m.b8[49];
	ctxt->m.b8[51] = tctxt.m.b8[48];
	ctxt->m.b8[52] = tctxt.m.b8[55];
	ctxt->m.b8[53] = tctxt.m.b8[54];
	ctxt->m.b8[54] = tctxt.m.b8[53];
	ctxt->m.b8[55] = tctxt.m.b8[52];
	ctxt->m.b8[56] = tctxt.m.b8[59];
	ctxt->m.b8[57] = tctxt.m.b8[58];
	ctxt->m.b8[58] = tctxt.m.b8[57];
	ctxt->m.b8[59] = tctxt.m.b8[56];
	ctxt->m.b8[60] = tctxt.m.b8[63];
	ctxt->m.b8[61] = tctxt.m.b8[62];
	ctxt->m.b8[62] = tctxt.m.b8[61];
	ctxt->m.b8[63] = tctxt.m.b8[60];
#endif

	a = H(0);
	b = H(1);
	c = H(2);
	d = H(3);
	e = H(4);

	for (t = 0; t < 20; t++)
	{
		s = t & 0x0f;
		if (t >= 16)
			W(s) = S(1, W((s + 13) & 0x0f) ^ W((s + 8) & 0x0f) ^ W((s + 2) & 0x0f) ^ W(s));
		tmp = S(5, a) + F0(b, c, d) + e + W(s) + K(t);
		e = d;
		d = c;
		c = S(30, b);
		b = a;
		a = tmp;
	}
	for (t = 20; t < 40; t++)
	{
		s = t & 0x0f;
		W(s) = S(1, W((s + 13) & 0x0f) ^ W((s + 8) & 0x0f) ^ W((s + 2) & 0x0f) ^ W(s));
		tmp = S(5, a) + F1(b, c, d) + e + W(s) + K(t);
		e = d;
		d = c;
		c = S(30, b);
		b = a;
		a = tmp;
	}
	for (t = 40; t < 60; t++)
	{
		s = t & 0x0f;
		W(s) = S(1, W((s + 13) & 0x0f) ^ W((s + 8) & 0x0f) ^ W((s + 2) & 0x0f) ^ W(s));
		tmp = S(5, a) + F2(b, c, d) + e + W(s) + K(t);
		e = d;
		d = c;
		c = S(30, b);
		b = a;
		a = tmp;
	}
	for (t = 60; t < 80; t++)
	{
		s = t & 0x0f;
		W(s) = S(1, W((s + 13) & 0x0f) ^ W((s + 8) & 0x0f) ^ W((s + 2) & 0x0f) ^ W(s));
		tmp = S(5, a) + F3(b, c, d) + e + W(s) + K(t);
		e = d;
		d = c;
		c = S(30, b);
		b = a;
		a = tmp;
	}

	H(0) = H(0) + a;
	H(1) = H(1) + b;
	H(2) = H(2) + c;
	H(3) = H(3) + d;
	H(4) = H(4) + e;

	memset(&ctxt->m.b8[0], 0, 64);
}

/*------------------------------------------------------------*/

void
sha1_init(struct sha1_ctxt *ctxt)
{
	memset(ctxt, 0, sizeof(struct sha1_ctxt));
	H(0) = 0x67452301;
	H(1) = 0xefcdab89;
	H(2) = 0x98badcfe;
	H(3) = 0x10325476;
	H(4) = 0xc3d2e1f0;
}

void
sha1_pad(struct sha1_ctxt *ctxt)
{
	size_t		padlen;			/* pad length in bytes */
	size_t		padstart;

	PUTPAD(0x80);

	padstart = COUNT % 64;
	padlen = 64 - padstart;
	if (padlen < 8)
	{
		memset(&ctxt->m.b8[padstart], 0, padlen);
		COUNT += padlen;
		COUNT %= 64;
		sha1_step(ctxt);
		padstart = COUNT % 64;	/* should be 0 */
		padlen = 64 - padstart; /* should be 64 */
	}
	memset(&ctxt->m.b8[padstart], 0, padlen - 8);
	COUNT += (padlen - 8);
	COUNT %= 64;
#ifdef WORDS_BIGENDIAN
	PUTPAD(ctxt->c.b8[0]);
	PUTPAD(ctxt->c.b8[1]);
	PUTPAD(ctxt->c.b8[2]);
	PUTPAD(ctxt->c.b8[3]);
	PUTPAD(ctxt->c.b8[4]);
	PUTPAD(ctxt->c.b8[5]);
	PUTPAD(ctxt->c.b8[6]);
	PUTPAD(ctxt->c.b8[7]);
#else
	PUTPAD(ctxt->c.b8[7]);
	PUTPAD(ctxt->c.b8[6]);
	PUTPAD(ctxt->c.b8[5]);
	PUTPAD(ctxt->c.b8[4]);
	PUTPAD(ctxt->c.b8[3]);
	PUTPAD(ctxt->c.b8[2]);
	PUTPAD(ctxt->c.b8[1]);
	PUTPAD(ctxt->c.b8[0]);
#endif
}

void
sha1_loop(struct sha1_ctxt *ctxt, const uint8 *input0, size_t len)
{
	const uint8 *input;
	size_t		gaplen;
	size_t		gapstart;
	size_t		off;
	size_t		copysiz;

	input = (const uint8 *) input0;
	off = 0;

	while (off < len)
	{
		gapstart = COUNT % 64;
		gaplen = 64 - gapstart;

		copysiz = (gaplen < len - off) ? gaplen : len - off;
		memmove(&ctxt->m.b8[gapstart], &input[off], copysiz);
		COUNT += copysiz;
		COUNT %= 64;
		ctxt->c.b64[0] += copysiz * 8;
		if (COUNT % 64 == 0)
			sha1_step(ctxt);
		off += copysiz;
	}
}

void
sha1_result(struct sha1_ctxt *ctxt, uint8 *digest0)
{
	uint8	   *digest;

	digest = (uint8 *) digest0;
	sha1_pad(ctxt);
#ifdef WORDS_BIGENDIAN
	memmove(digest, &ctxt->h.b8[0], 20);
#else
	digest[0] = ctxt->h.b8[3];
	digest[1] = ctxt->h.b8[2];
	digest[2] = ctxt->h.b8[1];
	digest[3] = ctxt->h.b8[0];
	digest[4] = ctxt->h.b8[7];
	digest[5] = ctxt->h.b8[6];
	digest[6] = ctxt->h.b8[5];
	digest[7] = ctxt->h.b8[4];
	digest[8] = ctxt->h.b8[11];
	digest[9] = ctxt->h.b8[10];
	digest[10] = ctxt->h.b8[9];
	digest[11] = ctxt->h.b8[8];
	digest[12] = ctxt->h.b8[15];
	digest[13] = ctxt->h.b8[14];
	digest[14] = ctxt->h.b8[13];
	digest[15] = ctxt->h.b8[12];
	digest[16] = ctxt->h.b8[19];
	digest[17] = ctxt->h.b8[18];
	digest[18] = ctxt->h.b8[17];
	digest[19] = ctxt->h.b8[16];
#endif
}














#ifdef __cplusplus
extern "C" {
#endif

PgQueryNormalizeResult pg_query_normalize(const char* input);

PgQueryFingerprintResult pg_query_fingerprint(const char* input);

void pg_query_free_normalize_result(PgQueryNormalizeResult result);
void pg_query_free_parse_result(PgQueryParseResult result);
void pg_query_free_plpgsql_parse_result(PgQueryPlpgsqlParseResult result);
void pg_query_free_fingerprint_result(PgQueryFingerprintResult result);

// Postgres version information
#define PG_VERSION "10.5"
#define PG_MAJORVERSION "10"
#define PG_VERSION_NUM 100005


const char* progname = "pg_query";

__thread sig_atomic_t pg_query_initialized = 0;

void pg_query_init(void)
{
	if (pg_query_initialized != 0) return;
	pg_query_initialized = 1;

	MemoryContextInit();
	SetDatabaseEncoding(PG_UTF8);
}

MemoryContext pg_query_enter_memory_context(const char* ctx_name)
{
	MemoryContext ctx = NULL;

	pg_query_init();

	ctx = AllocSetContextCreate(TopMemoryContext,
								ctx_name,
								ALLOCSET_DEFAULT_MINSIZE,
								ALLOCSET_DEFAULT_INITSIZE,
								ALLOCSET_DEFAULT_MAXSIZE);
	MemoryContextSwitchTo(ctx);

	return ctx;
}

void pg_query_exit_memory_context(MemoryContext ctx)
{
	// Return to previous PostgreSQL memory context
	MemoryContextSwitchTo(TopMemoryContext);

	MemoryContextDelete(ctx);
}

void pg_query_free_error(PgQueryError *error)
{
	free(error->message);
	free(error->funcname);
	free(error->filename);

	if (error->context) {
		free(error->context);
	}

	free(error);
}















PgQueryInternalParsetreeAndError pg_query_raw_parse(List* input)
{
	PgQueryInternalParsetreeAndError result = {0};
	MemoryContext parse_context = CurrentMemoryContext;

	char stderr_buffer[STDERR_BUFFER_LEN + 1] = {0};
#ifndef DEBUG
	int stderr_global;
	int stderr_pipe[2];
#endif

#ifndef DEBUG
	// Setup pipe for stderr redirection
	if (pipe(stderr_pipe) != 0) {
		PgQueryError* error = malloc(sizeof(PgQueryError));

		error->message = strdup("Failed to open pipe, too many open file descriptors")

		result.error = error;

		return result; 
	}

	fcntl(stderr_pipe[0], F_SETFL, fcntl(stderr_pipe[0], F_GETFL) | O_NONBLOCK);

	// Redirect stderr to the pipe
	stderr_global = dup(STDERR_FILENO);
	dup2(stderr_pipe[1], STDERR_FILENO);
	close(stderr_pipe[1]);
#endif

	PG_TRY();
	{
		result.tree = list_copy(input);

#ifndef DEBUG
		// Save stderr for result
		read(stderr_pipe[0], stderr_buffer, STDERR_BUFFER_LEN);
#endif

		result.stderr_buffer = strdup(stderr_buffer);
	}
	PG_CATCH();
	{
		ErrorData* error_data;
		PgQueryError* error;

		MemoryContextSwitchTo(parse_context);
		error_data = CopyErrorData();

		// Note: This is intentionally malloc so exiting the memory context doesn't free this
		error = malloc(sizeof(PgQueryError));
		error->message   = strdup(error_data->message);
		error->filename  = strdup(error_data->filename);
		error->funcname  = strdup(error_data->funcname);
		error->context   = NULL;
		error->lineno    = error_data->lineno;
		error->cursorpos = error_data->cursorpos;

		result.error = error;
		FlushErrorState();
	}
	PG_END_TRY();

#ifndef DEBUG
	// Restore stderr, close pipe
	dup2(stderr_global, STDERR_FILENO);
	close(stderr_pipe[0]);
	close(stderr_global);
#endif

	return result;
}

void pg_query_free_parse_result(PgQueryParseResult result)
{
  if (result.error) {
		pg_query_free_error(result.error);
  }

  free(result.parse_tree);
  free(result.stderr_buffer);
}







typedef struct FingerprintContext
{
	dlist_head tokens;
	SHA1_CTX *sha1; // If this is NULL we write tokens, otherwise we write the sha1sum directly
} FingerprintContext;

typedef struct FingerprintToken
{
	char *str;
	dlist_node list_node;
} FingerprintToken;

static void _fingerprintNode(FingerprintContext *ctx, const void *obj, const void *parent, char *parent_field_name, unsigned int depth);
static void _fingerprintInitForTokens(FingerprintContext *ctx);
static void _fingerprintCopyTokens(FingerprintContext *source, FingerprintContext *target, char *field_name);

#define PG_QUERY_FINGERPRINT_VERSION 2

// Implementations

static void
_fingerprintString(FingerprintContext *ctx, const char *str)
{
	if (ctx->sha1 != NULL) {
		SHA1Update(ctx->sha1, (uint8*) str, strlen(str));
	} else {
		FingerprintToken *token = palloc0(sizeof(FingerprintToken));
		token->str = pstrdup(str);
		dlist_push_tail(&ctx->tokens, &token->list_node);
	}
}

static void
_fingerprintInteger(FingerprintContext *ctx, const Value *node)
{
	if (node->val.ival != 0) {
		_fingerprintString(ctx, "Integer");
		_fingerprintString(ctx, "ival");
		char buffer[50];
		sprintf(buffer, "%ld", node->val.ival);
		_fingerprintString(ctx, buffer);
	}
}

static void
_fingerprintFloat(FingerprintContext *ctx, const Value *node)
{
	if (node->val.str != NULL) {
		_fingerprintString(ctx, "Float");
		_fingerprintString(ctx, "str");
		_fingerprintString(ctx, node->val.str);
	}
}

static void
_fingerprintBitString(FingerprintContext *ctx, const Value *node)
{
	if (node->val.str != NULL) {
		_fingerprintString(ctx, "BitString");
		_fingerprintString(ctx, "str");
		_fingerprintString(ctx, node->val.str);
	}
}

#define FINGERPRINT_CMP_STRBUF 1024

static int compareFingerprintContext(const void *a, const void *b)
{
	FingerprintContext *ca = *(FingerprintContext**) a;
	FingerprintContext *cb = *(FingerprintContext**) b;

	char strBufA[FINGERPRINT_CMP_STRBUF + 1] = {'\0'};
	char strBufB[FINGERPRINT_CMP_STRBUF + 1] = {'\0'};

	dlist_iter iterA;
	dlist_iter iterB;

	dlist_foreach(iterA, &ca->tokens)
	{
		FingerprintToken *token = dlist_container(FingerprintToken, list_node, iterA.cur);

		strncat(strBufA, token->str, FINGERPRINT_CMP_STRBUF - strlen(strBufA));
	}

	dlist_foreach(iterB, &cb->tokens)
	{
		FingerprintToken *token = dlist_container(FingerprintToken, list_node, iterB.cur);

		strncat(strBufB, token->str, FINGERPRINT_CMP_STRBUF - strlen(strBufB));
	}

	//printf("COMP %s <=> %s = %d\n", strBufA, strBufB, strcmp(strBufA, strBufB));

	return strcmp(strBufA, strBufB);
}

static void
_fingerprintList(FingerprintContext *ctx, const List *node, const void *parent, char *field_name, unsigned int depth)
{
	if (field_name != NULL && (strcmp(field_name, "fromClause") == 0 || strcmp(field_name, "targetList") == 0 ||
			strcmp(field_name, "cols") == 0 || strcmp(field_name, "rexpr") == 0 || strcmp(field_name, "valuesLists") == 0)) {

		FingerprintContext** subCtxArr = palloc0(node->length * sizeof(FingerprintContext*));
		size_t subCtxCount = 0;
		size_t i;
		const ListCell *lc;

		foreach(lc, node)
		{
			FingerprintContext* subCtx = palloc0(sizeof(FingerprintContext));

			_fingerprintInitForTokens(subCtx);
			_fingerprintNode(subCtx, lfirst(lc), parent, field_name, depth + 1);


			bool exists = false;
			for (i = 0; i < subCtxCount; i++) {
				if (compareFingerprintContext(&subCtxArr[i], &subCtx) == 0) {
					exists = true;
					break;
				}
			}

			if (!exists) {
				subCtxArr[subCtxCount] = subCtx;
				subCtxCount += 1;
			}

			lnext(lc);
		}

		pg_qsort(subCtxArr, subCtxCount, sizeof(FingerprintContext*), compareFingerprintContext);

		for (i = 0; i < subCtxCount; i++) {
			_fingerprintCopyTokens(subCtxArr[i], ctx, NULL);
		}
	} else {
		const ListCell *lc;

		foreach(lc, node)
		{
			_fingerprintNode(ctx, lfirst(lc), parent, field_name, depth + 1);

			lnext(lc);
		}
	}
}

static void
_fingerprintInitForTokens(FingerprintContext *ctx) {
	ctx->sha1 = NULL;
	dlist_init(&ctx->tokens);
}

static void
_fingerprintCopyTokens(FingerprintContext *source, FingerprintContext *target, char *field_name) {
	dlist_iter iter;

	if (dlist_is_empty(&source->tokens)) return;

	if (field_name != NULL) {
		_fingerprintString(target, field_name);
	}

	dlist_foreach(iter, &source->tokens)
	{
		FingerprintToken *token = dlist_container(FingerprintToken, list_node, iter.cur);

		_fingerprintString(target, token->str);
	}
}

static void
_fingerprintAlias(FingerprintContext *ctx, const Alias *node, const void *parent, const char *field_name, unsigned int depth)
{
  // Intentionally ignoring all fields for fingerprinting
}

static void
_fingerprintRangeVar(FingerprintContext *ctx, const RangeVar *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "RangeVar");

  if (node->alias != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->alias, node, "alias", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "alias");
  }

  if (node->catalogname != NULL) {
    _fingerprintString(ctx, "catalogname");
    _fingerprintString(ctx, node->catalogname);
  }

  if (node->inh) {
    _fingerprintString(ctx, "inh");
    _fingerprintString(ctx, "true");
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->relname != NULL && node->relpersistence != 't') {
    _fingerprintString(ctx, "relname");
    _fingerprintString(ctx, node->relname);
  }

  if (node->relpersistence != 0) {
    char buffer[2] = {node->relpersistence, '\0'};
    _fingerprintString(ctx, "relpersistence");
    _fingerprintString(ctx, buffer);
  }

  if (node->schemaname != NULL) {
    _fingerprintString(ctx, "schemaname");
    _fingerprintString(ctx, node->schemaname);
  }

}

static void
_fingerprintTableFunc(FingerprintContext *ctx, const TableFunc *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "TableFunc");

  if (node->colcollations != NULL && node->colcollations->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->colcollations, node, "colcollations", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "colcollations");
  }
  if (node->coldefexprs != NULL && node->coldefexprs->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->coldefexprs, node, "coldefexprs", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "coldefexprs");
  }
  if (node->colexprs != NULL && node->colexprs->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->colexprs, node, "colexprs", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "colexprs");
  }
  if (node->colnames != NULL && node->colnames->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->colnames, node, "colnames", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "colnames");
  }
  if (node->coltypes != NULL && node->coltypes->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->coltypes, node, "coltypes", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "coltypes");
  }
  if (node->coltypmods != NULL && node->coltypmods->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->coltypmods, node, "coltypmods", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "coltypmods");
  }
  if (node->docexpr != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->docexpr, node, "docexpr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "docexpr");
  }

  // Intentionally ignoring node->location for fingerprinting

  if (true) {
    int x;
    Bitmapset	*bms = bms_copy(node->notnulls);

    _fingerprintString(ctx, "notnulls");

  	while ((x = bms_first_member(bms)) >= 0) {
      char buffer[50];
      sprintf(buffer, "%d", x);
      _fingerprintString(ctx, buffer);
    }

    bms_free(bms);
  }

  if (node->ns_names != NULL && node->ns_names->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->ns_names, node, "ns_names", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "ns_names");
  }
  if (node->ns_uris != NULL && node->ns_uris->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->ns_uris, node, "ns_uris", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "ns_uris");
  }
  if (node->ordinalitycol != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->ordinalitycol);
    _fingerprintString(ctx, "ordinalitycol");
    _fingerprintString(ctx, buffer);
  }

  if (node->rowexpr != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->rowexpr, node, "rowexpr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "rowexpr");
  }

}

static void
_fingerprintExpr(FingerprintContext *ctx, const Expr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "Expr");

}

static void
_fingerprintVar(FingerprintContext *ctx, const Var *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "Var");

  // Intentionally ignoring node->location for fingerprinting

  if (node->varattno != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->varattno);
    _fingerprintString(ctx, "varattno");
    _fingerprintString(ctx, buffer);
  }

  if (node->varcollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->varcollid);
    _fingerprintString(ctx, "varcollid");
    _fingerprintString(ctx, buffer);
  }

  if (node->varlevelsup != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->varlevelsup);
    _fingerprintString(ctx, "varlevelsup");
    _fingerprintString(ctx, buffer);
  }

  if (node->varno != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->varno);
    _fingerprintString(ctx, "varno");
    _fingerprintString(ctx, buffer);
  }

  if (node->varnoold != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->varnoold);
    _fingerprintString(ctx, "varnoold");
    _fingerprintString(ctx, buffer);
  }

  if (node->varoattno != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->varoattno);
    _fingerprintString(ctx, "varoattno");
    _fingerprintString(ctx, buffer);
  }

  if (node->vartype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->vartype);
    _fingerprintString(ctx, "vartype");
    _fingerprintString(ctx, buffer);
  }

  if (node->vartypmod != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->vartypmod);
    _fingerprintString(ctx, "vartypmod");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintConst(FingerprintContext *ctx, const Const *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "Const");

  if (node->constbyval) {
    _fingerprintString(ctx, "constbyval");
    _fingerprintString(ctx, "true");
  }

  if (node->constcollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->constcollid);
    _fingerprintString(ctx, "constcollid");
    _fingerprintString(ctx, buffer);
  }

  if (node->constisnull) {
    _fingerprintString(ctx, "constisnull");
    _fingerprintString(ctx, "true");
  }

  if (node->constlen != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->constlen);
    _fingerprintString(ctx, "constlen");
    _fingerprintString(ctx, buffer);
  }

  if (node->consttype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->consttype);
    _fingerprintString(ctx, "consttype");
    _fingerprintString(ctx, buffer);
  }

  if (node->consttypmod != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->consttypmod);
    _fingerprintString(ctx, "consttypmod");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->location for fingerprinting

}

static void
_fingerprintParam(FingerprintContext *ctx, const Param *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "Param");

  // Intentionally ignoring node->location for fingerprinting

  if (node->paramcollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->paramcollid);
    _fingerprintString(ctx, "paramcollid");
    _fingerprintString(ctx, buffer);
  }

  if (node->paramid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->paramid);
    _fingerprintString(ctx, "paramid");
    _fingerprintString(ctx, buffer);
  }

  if (node->paramkind != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->paramkind);
    _fingerprintString(ctx, "paramkind");
    _fingerprintString(ctx, buffer);
  }

  if (node->paramtype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->paramtype);
    _fingerprintString(ctx, "paramtype");
    _fingerprintString(ctx, buffer);
  }

  if (node->paramtypmod != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->paramtypmod);
    _fingerprintString(ctx, "paramtypmod");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintAggref(FingerprintContext *ctx, const Aggref *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "Aggref");

  if (node->aggargtypes != NULL && node->aggargtypes->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->aggargtypes, node, "aggargtypes", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "aggargtypes");
  }
  if (node->aggcollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->aggcollid);
    _fingerprintString(ctx, "aggcollid");
    _fingerprintString(ctx, buffer);
  }

  if (node->aggdirectargs != NULL && node->aggdirectargs->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->aggdirectargs, node, "aggdirectargs", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "aggdirectargs");
  }
  if (node->aggdistinct != NULL && node->aggdistinct->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->aggdistinct, node, "aggdistinct", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "aggdistinct");
  }
  if (node->aggfilter != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->aggfilter, node, "aggfilter", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "aggfilter");
  }

  if (node->aggfnoid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->aggfnoid);
    _fingerprintString(ctx, "aggfnoid");
    _fingerprintString(ctx, buffer);
  }

  if (node->aggkind != 0) {
    char buffer[2] = {node->aggkind, '\0'};
    _fingerprintString(ctx, "aggkind");
    _fingerprintString(ctx, buffer);
  }

  if (node->agglevelsup != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->agglevelsup);
    _fingerprintString(ctx, "agglevelsup");
    _fingerprintString(ctx, buffer);
  }

  if (node->aggorder != NULL && node->aggorder->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->aggorder, node, "aggorder", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "aggorder");
  }
  if (node->aggsplit != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->aggsplit);
    _fingerprintString(ctx, "aggsplit");
    _fingerprintString(ctx, buffer);
  }

  if (node->aggstar) {
    _fingerprintString(ctx, "aggstar");
    _fingerprintString(ctx, "true");
  }

  if (node->aggtranstype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->aggtranstype);
    _fingerprintString(ctx, "aggtranstype");
    _fingerprintString(ctx, buffer);
  }

  if (node->aggtype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->aggtype);
    _fingerprintString(ctx, "aggtype");
    _fingerprintString(ctx, buffer);
  }

  if (node->aggvariadic) {
    _fingerprintString(ctx, "aggvariadic");
    _fingerprintString(ctx, "true");
  }

  if (node->args != NULL && node->args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->args, node, "args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "args");
  }
  if (node->inputcollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->inputcollid);
    _fingerprintString(ctx, "inputcollid");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->location for fingerprinting

}

static void
_fingerprintGroupingFunc(FingerprintContext *ctx, const GroupingFunc *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "GroupingFunc");

  if (node->agglevelsup != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->agglevelsup);
    _fingerprintString(ctx, "agglevelsup");
    _fingerprintString(ctx, buffer);
  }

  if (node->args != NULL && node->args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->args, node, "args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "args");
  }
  if (node->cols != NULL && node->cols->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->cols, node, "cols", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "cols");
  }
  // Intentionally ignoring node->location for fingerprinting

  if (node->refs != NULL && node->refs->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->refs, node, "refs", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "refs");
  }
}

static void
_fingerprintWindowFunc(FingerprintContext *ctx, const WindowFunc *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "WindowFunc");

  if (node->aggfilter != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->aggfilter, node, "aggfilter", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "aggfilter");
  }

  if (node->args != NULL && node->args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->args, node, "args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "args");
  }
  if (node->inputcollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->inputcollid);
    _fingerprintString(ctx, "inputcollid");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->winagg) {
    _fingerprintString(ctx, "winagg");
    _fingerprintString(ctx, "true");
  }

  if (node->wincollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->wincollid);
    _fingerprintString(ctx, "wincollid");
    _fingerprintString(ctx, buffer);
  }

  if (node->winfnoid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->winfnoid);
    _fingerprintString(ctx, "winfnoid");
    _fingerprintString(ctx, buffer);
  }

  if (node->winref != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->winref);
    _fingerprintString(ctx, "winref");
    _fingerprintString(ctx, buffer);
  }

  if (node->winstar) {
    _fingerprintString(ctx, "winstar");
    _fingerprintString(ctx, "true");
  }

  if (node->wintype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->wintype);
    _fingerprintString(ctx, "wintype");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintArrayRef(FingerprintContext *ctx, const ArrayRef *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "ArrayRef");

  if (node->refarraytype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->refarraytype);
    _fingerprintString(ctx, "refarraytype");
    _fingerprintString(ctx, buffer);
  }

  if (node->refassgnexpr != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->refassgnexpr, node, "refassgnexpr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "refassgnexpr");
  }

  if (node->refcollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->refcollid);
    _fingerprintString(ctx, "refcollid");
    _fingerprintString(ctx, buffer);
  }

  if (node->refelemtype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->refelemtype);
    _fingerprintString(ctx, "refelemtype");
    _fingerprintString(ctx, buffer);
  }

  if (node->refexpr != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->refexpr, node, "refexpr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "refexpr");
  }

  if (node->reflowerindexpr != NULL && node->reflowerindexpr->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->reflowerindexpr, node, "reflowerindexpr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "reflowerindexpr");
  }
  if (node->reftypmod != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->reftypmod);
    _fingerprintString(ctx, "reftypmod");
    _fingerprintString(ctx, buffer);
  }

  if (node->refupperindexpr != NULL && node->refupperindexpr->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->refupperindexpr, node, "refupperindexpr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "refupperindexpr");
  }
}

static void
_fingerprintFuncExpr(FingerprintContext *ctx, const FuncExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "FuncExpr");

  if (node->args != NULL && node->args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->args, node, "args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "args");
  }
  if (node->funccollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->funccollid);
    _fingerprintString(ctx, "funccollid");
    _fingerprintString(ctx, buffer);
  }

  if (node->funcformat != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->funcformat);
    _fingerprintString(ctx, "funcformat");
    _fingerprintString(ctx, buffer);
  }

  if (node->funcid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->funcid);
    _fingerprintString(ctx, "funcid");
    _fingerprintString(ctx, buffer);
  }

  if (node->funcresulttype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->funcresulttype);
    _fingerprintString(ctx, "funcresulttype");
    _fingerprintString(ctx, buffer);
  }

  if (node->funcretset) {
    _fingerprintString(ctx, "funcretset");
    _fingerprintString(ctx, "true");
  }

  if (node->funcvariadic) {
    _fingerprintString(ctx, "funcvariadic");
    _fingerprintString(ctx, "true");
  }

  if (node->inputcollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->inputcollid);
    _fingerprintString(ctx, "inputcollid");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->location for fingerprinting

}

static void
_fingerprintNamedArgExpr(FingerprintContext *ctx, const NamedArgExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "NamedArgExpr");

  if (node->arg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->arg, node, "arg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "arg");
  }

  if (node->argnumber != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->argnumber);
    _fingerprintString(ctx, "argnumber");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->name != NULL) {
    _fingerprintString(ctx, "name");
    _fingerprintString(ctx, node->name);
  }

}

static void
_fingerprintOpExpr(FingerprintContext *ctx, const OpExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "OpExpr");

  if (node->args != NULL && node->args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->args, node, "args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "args");
  }
  if (node->inputcollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->inputcollid);
    _fingerprintString(ctx, "inputcollid");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->opcollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->opcollid);
    _fingerprintString(ctx, "opcollid");
    _fingerprintString(ctx, buffer);
  }

  if (node->opfuncid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->opfuncid);
    _fingerprintString(ctx, "opfuncid");
    _fingerprintString(ctx, buffer);
  }

  if (node->opno != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->opno);
    _fingerprintString(ctx, "opno");
    _fingerprintString(ctx, buffer);
  }

  if (node->opresulttype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->opresulttype);
    _fingerprintString(ctx, "opresulttype");
    _fingerprintString(ctx, buffer);
  }

  if (node->opretset) {
    _fingerprintString(ctx, "opretset");
    _fingerprintString(ctx, "true");
  }

}

static void
_fingerprintScalarArrayOpExpr(FingerprintContext *ctx, const ScalarArrayOpExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "ScalarArrayOpExpr");

  if (node->args != NULL && node->args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->args, node, "args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "args");
  }
  if (node->inputcollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->inputcollid);
    _fingerprintString(ctx, "inputcollid");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->opfuncid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->opfuncid);
    _fingerprintString(ctx, "opfuncid");
    _fingerprintString(ctx, buffer);
  }

  if (node->opno != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->opno);
    _fingerprintString(ctx, "opno");
    _fingerprintString(ctx, buffer);
  }

  if (node->useOr) {
    _fingerprintString(ctx, "useOr");
    _fingerprintString(ctx, "true");
  }

}

static void
_fingerprintBoolExpr(FingerprintContext *ctx, const BoolExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "BoolExpr");

  if (node->args != NULL && node->args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->args, node, "args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "args");
  }
  if (node->boolop != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->boolop);
    _fingerprintString(ctx, "boolop");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->location for fingerprinting

}

static void
_fingerprintSubLink(FingerprintContext *ctx, const SubLink *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "SubLink");

  // Intentionally ignoring node->location for fingerprinting

  if (node->operName != NULL && node->operName->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->operName, node, "operName", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "operName");
  }
  if (node->subLinkId != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->subLinkId);
    _fingerprintString(ctx, "subLinkId");
    _fingerprintString(ctx, buffer);
  }

  if (node->subLinkType != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->subLinkType);
    _fingerprintString(ctx, "subLinkType");
    _fingerprintString(ctx, buffer);
  }

  if (node->subselect != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->subselect, node, "subselect", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "subselect");
  }

  if (node->testexpr != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->testexpr, node, "testexpr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "testexpr");
  }

}

static void
_fingerprintSubPlan(FingerprintContext *ctx, const SubPlan *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "SubPlan");

  if (node->args != NULL && node->args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->args, node, "args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "args");
  }
  if (node->firstColCollation != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->firstColCollation);
    _fingerprintString(ctx, "firstColCollation");
    _fingerprintString(ctx, buffer);
  }

  if (node->firstColType != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->firstColType);
    _fingerprintString(ctx, "firstColType");
    _fingerprintString(ctx, buffer);
  }

  if (node->firstColTypmod != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->firstColTypmod);
    _fingerprintString(ctx, "firstColTypmod");
    _fingerprintString(ctx, buffer);
  }

  if (node->parParam != NULL && node->parParam->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->parParam, node, "parParam", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "parParam");
  }
  if (node->parallel_safe) {
    _fingerprintString(ctx, "parallel_safe");
    _fingerprintString(ctx, "true");
  }

  if (node->paramIds != NULL && node->paramIds->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->paramIds, node, "paramIds", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "paramIds");
  }
  if (node->per_call_cost != 0) {
    char buffer[50];
    sprintf(buffer, "%f", node->per_call_cost);
    _fingerprintString(ctx, "per_call_cost");
    _fingerprintString(ctx, buffer);
  }

  if (node->plan_id != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->plan_id);
    _fingerprintString(ctx, "plan_id");
    _fingerprintString(ctx, buffer);
  }

  if (node->plan_name != NULL) {
    _fingerprintString(ctx, "plan_name");
    _fingerprintString(ctx, node->plan_name);
  }

  if (node->setParam != NULL && node->setParam->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->setParam, node, "setParam", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "setParam");
  }
  if (node->startup_cost != 0) {
    char buffer[50];
    sprintf(buffer, "%f", node->startup_cost);
    _fingerprintString(ctx, "startup_cost");
    _fingerprintString(ctx, buffer);
  }

  if (node->subLinkType != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->subLinkType);
    _fingerprintString(ctx, "subLinkType");
    _fingerprintString(ctx, buffer);
  }

  if (node->testexpr != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->testexpr, node, "testexpr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "testexpr");
  }

  if (node->unknownEqFalse) {
    _fingerprintString(ctx, "unknownEqFalse");
    _fingerprintString(ctx, "true");
  }

  if (node->useHashTable) {
    _fingerprintString(ctx, "useHashTable");
    _fingerprintString(ctx, "true");
  }

}

static void
_fingerprintAlternativeSubPlan(FingerprintContext *ctx, const AlternativeSubPlan *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlternativeSubPlan");

  if (node->subplans != NULL && node->subplans->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->subplans, node, "subplans", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "subplans");
  }
}

static void
_fingerprintFieldSelect(FingerprintContext *ctx, const FieldSelect *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "FieldSelect");

  if (node->arg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->arg, node, "arg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "arg");
  }

  if (node->fieldnum != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->fieldnum);
    _fingerprintString(ctx, "fieldnum");
    _fingerprintString(ctx, buffer);
  }

  if (node->resultcollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->resultcollid);
    _fingerprintString(ctx, "resultcollid");
    _fingerprintString(ctx, buffer);
  }

  if (node->resulttype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->resulttype);
    _fingerprintString(ctx, "resulttype");
    _fingerprintString(ctx, buffer);
  }

  if (node->resulttypmod != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->resulttypmod);
    _fingerprintString(ctx, "resulttypmod");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintFieldStore(FingerprintContext *ctx, const FieldStore *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "FieldStore");

  if (node->arg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->arg, node, "arg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "arg");
  }

  if (node->fieldnums != NULL && node->fieldnums->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->fieldnums, node, "fieldnums", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "fieldnums");
  }
  if (node->newvals != NULL && node->newvals->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->newvals, node, "newvals", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "newvals");
  }
  if (node->resulttype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->resulttype);
    _fingerprintString(ctx, "resulttype");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintRelabelType(FingerprintContext *ctx, const RelabelType *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "RelabelType");

  if (node->arg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->arg, node, "arg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "arg");
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->relabelformat != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->relabelformat);
    _fingerprintString(ctx, "relabelformat");
    _fingerprintString(ctx, buffer);
  }

  if (node->resultcollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->resultcollid);
    _fingerprintString(ctx, "resultcollid");
    _fingerprintString(ctx, buffer);
  }

  if (node->resulttype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->resulttype);
    _fingerprintString(ctx, "resulttype");
    _fingerprintString(ctx, buffer);
  }

  if (node->resulttypmod != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->resulttypmod);
    _fingerprintString(ctx, "resulttypmod");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintCoerceViaIO(FingerprintContext *ctx, const CoerceViaIO *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CoerceViaIO");

  if (node->arg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->arg, node, "arg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "arg");
  }

  if (node->coerceformat != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->coerceformat);
    _fingerprintString(ctx, "coerceformat");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->resultcollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->resultcollid);
    _fingerprintString(ctx, "resultcollid");
    _fingerprintString(ctx, buffer);
  }

  if (node->resulttype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->resulttype);
    _fingerprintString(ctx, "resulttype");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintArrayCoerceExpr(FingerprintContext *ctx, const ArrayCoerceExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "ArrayCoerceExpr");

  if (node->arg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->arg, node, "arg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "arg");
  }

  if (node->coerceformat != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->coerceformat);
    _fingerprintString(ctx, "coerceformat");
    _fingerprintString(ctx, buffer);
  }

  if (node->elemfuncid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->elemfuncid);
    _fingerprintString(ctx, "elemfuncid");
    _fingerprintString(ctx, buffer);
  }

  if (node->isExplicit) {
    _fingerprintString(ctx, "isExplicit");
    _fingerprintString(ctx, "true");
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->resultcollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->resultcollid);
    _fingerprintString(ctx, "resultcollid");
    _fingerprintString(ctx, buffer);
  }

  if (node->resulttype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->resulttype);
    _fingerprintString(ctx, "resulttype");
    _fingerprintString(ctx, buffer);
  }

  if (node->resulttypmod != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->resulttypmod);
    _fingerprintString(ctx, "resulttypmod");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintConvertRowtypeExpr(FingerprintContext *ctx, const ConvertRowtypeExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "ConvertRowtypeExpr");

  if (node->arg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->arg, node, "arg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "arg");
  }

  if (node->convertformat != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->convertformat);
    _fingerprintString(ctx, "convertformat");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->resulttype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->resulttype);
    _fingerprintString(ctx, "resulttype");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintCollateExpr(FingerprintContext *ctx, const CollateExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CollateExpr");

  if (node->arg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->arg, node, "arg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "arg");
  }

  if (node->collOid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->collOid);
    _fingerprintString(ctx, "collOid");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->location for fingerprinting

}

static void
_fingerprintCaseExpr(FingerprintContext *ctx, const CaseExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CaseExpr");

  if (node->arg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->arg, node, "arg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "arg");
  }

  if (node->args != NULL && node->args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->args, node, "args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "args");
  }
  if (node->casecollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->casecollid);
    _fingerprintString(ctx, "casecollid");
    _fingerprintString(ctx, buffer);
  }

  if (node->casetype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->casetype);
    _fingerprintString(ctx, "casetype");
    _fingerprintString(ctx, buffer);
  }

  if (node->defresult != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->defresult, node, "defresult", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "defresult");
  }

  // Intentionally ignoring node->location for fingerprinting

}

static void
_fingerprintCaseWhen(FingerprintContext *ctx, const CaseWhen *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CaseWhen");

  if (node->expr != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->expr, node, "expr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "expr");
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->result != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->result, node, "result", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "result");
  }

}

static void
_fingerprintCaseTestExpr(FingerprintContext *ctx, const CaseTestExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CaseTestExpr");

  if (node->collation != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->collation);
    _fingerprintString(ctx, "collation");
    _fingerprintString(ctx, buffer);
  }

  if (node->typeId != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->typeId);
    _fingerprintString(ctx, "typeId");
    _fingerprintString(ctx, buffer);
  }

  if (node->typeMod != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->typeMod);
    _fingerprintString(ctx, "typeMod");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintArrayExpr(FingerprintContext *ctx, const ArrayExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "ArrayExpr");

  if (node->array_collid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->array_collid);
    _fingerprintString(ctx, "array_collid");
    _fingerprintString(ctx, buffer);
  }

  if (node->array_typeid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->array_typeid);
    _fingerprintString(ctx, "array_typeid");
    _fingerprintString(ctx, buffer);
  }

  if (node->element_typeid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->element_typeid);
    _fingerprintString(ctx, "element_typeid");
    _fingerprintString(ctx, buffer);
  }

  if (node->elements != NULL && node->elements->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->elements, node, "elements", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "elements");
  }
  // Intentionally ignoring node->location for fingerprinting

  if (node->multidims) {
    _fingerprintString(ctx, "multidims");
    _fingerprintString(ctx, "true");
  }

}

static void
_fingerprintRowExpr(FingerprintContext *ctx, const RowExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "RowExpr");

  if (node->args != NULL && node->args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->args, node, "args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "args");
  }
  if (node->colnames != NULL && node->colnames->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->colnames, node, "colnames", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "colnames");
  }
  // Intentionally ignoring node->location for fingerprinting

  if (node->row_format != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->row_format);
    _fingerprintString(ctx, "row_format");
    _fingerprintString(ctx, buffer);
  }

  if (node->row_typeid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->row_typeid);
    _fingerprintString(ctx, "row_typeid");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintRowCompareExpr(FingerprintContext *ctx, const RowCompareExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "RowCompareExpr");

  if (node->inputcollids != NULL && node->inputcollids->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->inputcollids, node, "inputcollids", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "inputcollids");
  }
  if (node->largs != NULL && node->largs->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->largs, node, "largs", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "largs");
  }
  if (node->opfamilies != NULL && node->opfamilies->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->opfamilies, node, "opfamilies", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "opfamilies");
  }
  if (node->opnos != NULL && node->opnos->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->opnos, node, "opnos", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "opnos");
  }
  if (node->rargs != NULL && node->rargs->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->rargs, node, "rargs", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "rargs");
  }
  if (node->rctype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->rctype);
    _fingerprintString(ctx, "rctype");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintCoalesceExpr(FingerprintContext *ctx, const CoalesceExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CoalesceExpr");

  if (node->args != NULL && node->args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->args, node, "args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "args");
  }
  if (node->coalescecollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->coalescecollid);
    _fingerprintString(ctx, "coalescecollid");
    _fingerprintString(ctx, buffer);
  }

  if (node->coalescetype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->coalescetype);
    _fingerprintString(ctx, "coalescetype");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->location for fingerprinting

}

static void
_fingerprintMinMaxExpr(FingerprintContext *ctx, const MinMaxExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "MinMaxExpr");

  if (node->args != NULL && node->args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->args, node, "args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "args");
  }
  if (node->inputcollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->inputcollid);
    _fingerprintString(ctx, "inputcollid");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->minmaxcollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->minmaxcollid);
    _fingerprintString(ctx, "minmaxcollid");
    _fingerprintString(ctx, buffer);
  }

  if (node->minmaxtype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->minmaxtype);
    _fingerprintString(ctx, "minmaxtype");
    _fingerprintString(ctx, buffer);
  }

  if (node->op != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->op);
    _fingerprintString(ctx, "op");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintSQLValueFunction(FingerprintContext *ctx, const SQLValueFunction *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "SQLValueFunction");

  // Intentionally ignoring node->location for fingerprinting

  if (node->op != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->op);
    _fingerprintString(ctx, "op");
    _fingerprintString(ctx, buffer);
  }

  if (node->type != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->type);
    _fingerprintString(ctx, "type");
    _fingerprintString(ctx, buffer);
  }

  if (node->typmod != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->typmod);
    _fingerprintString(ctx, "typmod");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintXmlExpr(FingerprintContext *ctx, const XmlExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "XmlExpr");

  if (node->arg_names != NULL && node->arg_names->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->arg_names, node, "arg_names", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "arg_names");
  }
  if (node->args != NULL && node->args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->args, node, "args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "args");
  }
  // Intentionally ignoring node->location for fingerprinting

  if (node->name != NULL) {
    _fingerprintString(ctx, "name");
    _fingerprintString(ctx, node->name);
  }

  if (node->named_args != NULL && node->named_args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->named_args, node, "named_args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "named_args");
  }
  if (node->op != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->op);
    _fingerprintString(ctx, "op");
    _fingerprintString(ctx, buffer);
  }

  if (node->type != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->type);
    _fingerprintString(ctx, "type");
    _fingerprintString(ctx, buffer);
  }

  if (node->typmod != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->typmod);
    _fingerprintString(ctx, "typmod");
    _fingerprintString(ctx, buffer);
  }

  if (node->xmloption != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->xmloption);
    _fingerprintString(ctx, "xmloption");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintNullTest(FingerprintContext *ctx, const NullTest *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "NullTest");

  if (node->arg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->arg, node, "arg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "arg");
  }

  if (node->argisrow) {
    _fingerprintString(ctx, "argisrow");
    _fingerprintString(ctx, "true");
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->nulltesttype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->nulltesttype);
    _fingerprintString(ctx, "nulltesttype");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintBooleanTest(FingerprintContext *ctx, const BooleanTest *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "BooleanTest");

  if (node->arg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->arg, node, "arg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "arg");
  }

  if (node->booltesttype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->booltesttype);
    _fingerprintString(ctx, "booltesttype");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->location for fingerprinting

}

static void
_fingerprintCoerceToDomain(FingerprintContext *ctx, const CoerceToDomain *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CoerceToDomain");

  if (node->arg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->arg, node, "arg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "arg");
  }

  if (node->coercionformat != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->coercionformat);
    _fingerprintString(ctx, "coercionformat");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->resultcollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->resultcollid);
    _fingerprintString(ctx, "resultcollid");
    _fingerprintString(ctx, buffer);
  }

  if (node->resulttype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->resulttype);
    _fingerprintString(ctx, "resulttype");
    _fingerprintString(ctx, buffer);
  }

  if (node->resulttypmod != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->resulttypmod);
    _fingerprintString(ctx, "resulttypmod");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintCoerceToDomainValue(FingerprintContext *ctx, const CoerceToDomainValue *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CoerceToDomainValue");

  if (node->collation != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->collation);
    _fingerprintString(ctx, "collation");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->typeId != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->typeId);
    _fingerprintString(ctx, "typeId");
    _fingerprintString(ctx, buffer);
  }

  if (node->typeMod != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->typeMod);
    _fingerprintString(ctx, "typeMod");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintSetToDefault(FingerprintContext *ctx, const SetToDefault *node, const void *parent, const char *field_name, unsigned int depth)
{
  // Intentionally ignoring all fields for fingerprinting
}

static void
_fingerprintCurrentOfExpr(FingerprintContext *ctx, const CurrentOfExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CurrentOfExpr");

  if (node->cursor_name != NULL) {
    _fingerprintString(ctx, "cursor_name");
    _fingerprintString(ctx, node->cursor_name);
  }

  if (node->cursor_param != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->cursor_param);
    _fingerprintString(ctx, "cursor_param");
    _fingerprintString(ctx, buffer);
  }

  if (node->cvarno != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->cvarno);
    _fingerprintString(ctx, "cvarno");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintNextValueExpr(FingerprintContext *ctx, const NextValueExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "NextValueExpr");

  if (node->seqid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->seqid);
    _fingerprintString(ctx, "seqid");
    _fingerprintString(ctx, buffer);
  }

  if (node->typeId != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->typeId);
    _fingerprintString(ctx, "typeId");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintInferenceElem(FingerprintContext *ctx, const InferenceElem *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "InferenceElem");

  if (node->expr != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->expr, node, "expr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "expr");
  }

  if (node->infercollid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->infercollid);
    _fingerprintString(ctx, "infercollid");
    _fingerprintString(ctx, buffer);
  }

  if (node->inferopclass != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->inferopclass);
    _fingerprintString(ctx, "inferopclass");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintTargetEntry(FingerprintContext *ctx, const TargetEntry *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "TargetEntry");

  if (node->expr != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->expr, node, "expr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "expr");
  }

  if (node->resjunk) {
    _fingerprintString(ctx, "resjunk");
    _fingerprintString(ctx, "true");
  }

  if (node->resname != NULL) {
    _fingerprintString(ctx, "resname");
    _fingerprintString(ctx, node->resname);
  }

  if (node->resno != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->resno);
    _fingerprintString(ctx, "resno");
    _fingerprintString(ctx, buffer);
  }

  if (node->resorigcol != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->resorigcol);
    _fingerprintString(ctx, "resorigcol");
    _fingerprintString(ctx, buffer);
  }

  if (node->resorigtbl != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->resorigtbl);
    _fingerprintString(ctx, "resorigtbl");
    _fingerprintString(ctx, buffer);
  }

  if (node->ressortgroupref != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->ressortgroupref);
    _fingerprintString(ctx, "ressortgroupref");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintRangeTblRef(FingerprintContext *ctx, const RangeTblRef *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "RangeTblRef");

  if (node->rtindex != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->rtindex);
    _fingerprintString(ctx, "rtindex");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintJoinExpr(FingerprintContext *ctx, const JoinExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "JoinExpr");

  if (node->alias != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->alias, node, "alias", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "alias");
  }

  if (node->isNatural) {
    _fingerprintString(ctx, "isNatural");
    _fingerprintString(ctx, "true");
  }

  if (node->jointype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->jointype);
    _fingerprintString(ctx, "jointype");
    _fingerprintString(ctx, buffer);
  }

  if (node->larg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->larg, node, "larg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "larg");
  }

  if (node->quals != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->quals, node, "quals", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "quals");
  }

  if (node->rarg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->rarg, node, "rarg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "rarg");
  }

  if (node->rtindex != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->rtindex);
    _fingerprintString(ctx, "rtindex");
    _fingerprintString(ctx, buffer);
  }

  if (node->usingClause != NULL && node->usingClause->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->usingClause, node, "usingClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "usingClause");
  }
}

static void
_fingerprintFromExpr(FingerprintContext *ctx, const FromExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "FromExpr");

  if (node->fromlist != NULL && node->fromlist->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->fromlist, node, "fromlist", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "fromlist");
  }
  if (node->quals != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->quals, node, "quals", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "quals");
  }

}

static void
_fingerprintOnConflictExpr(FingerprintContext *ctx, const OnConflictExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "OnConflictExpr");

  if (node->action != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->action);
    _fingerprintString(ctx, "action");
    _fingerprintString(ctx, buffer);
  }

  if (node->arbiterElems != NULL && node->arbiterElems->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->arbiterElems, node, "arbiterElems", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "arbiterElems");
  }
  if (node->arbiterWhere != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->arbiterWhere, node, "arbiterWhere", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "arbiterWhere");
  }

  if (node->constraint != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->constraint);
    _fingerprintString(ctx, "constraint");
    _fingerprintString(ctx, buffer);
  }

  if (node->exclRelIndex != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->exclRelIndex);
    _fingerprintString(ctx, "exclRelIndex");
    _fingerprintString(ctx, buffer);
  }

  if (node->exclRelTlist != NULL && node->exclRelTlist->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->exclRelTlist, node, "exclRelTlist", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "exclRelTlist");
  }
  if (node->onConflictSet != NULL && node->onConflictSet->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->onConflictSet, node, "onConflictSet", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "onConflictSet");
  }
  if (node->onConflictWhere != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->onConflictWhere, node, "onConflictWhere", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "onConflictWhere");
  }

}

static void
_fingerprintIntoClause(FingerprintContext *ctx, const IntoClause *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "IntoClause");

  if (node->colNames != NULL && node->colNames->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->colNames, node, "colNames", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "colNames");
  }
  if (node->onCommit != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->onCommit);
    _fingerprintString(ctx, "onCommit");
    _fingerprintString(ctx, buffer);
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->rel != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->rel, node, "rel", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "rel");
  }

  if (node->skipData) {
    _fingerprintString(ctx, "skipData");
    _fingerprintString(ctx, "true");
  }

  if (node->tableSpaceName != NULL) {
    _fingerprintString(ctx, "tableSpaceName");
    _fingerprintString(ctx, node->tableSpaceName);
  }

  if (node->viewQuery != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->viewQuery, node, "viewQuery", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "viewQuery");
  }

}

static void
_fingerprintRawStmt(FingerprintContext *ctx, const RawStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "RawStmt");

  if (node->stmt != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->stmt, node, "stmt", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "stmt");
  }

  // Intentionally ignoring node->stmt_len for fingerprinting

  // Intentionally ignoring node->stmt_location for fingerprinting

}

static void
_fingerprintQuery(FingerprintContext *ctx, const Query *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "Query");

  if (node->canSetTag) {
    _fingerprintString(ctx, "canSetTag");
    _fingerprintString(ctx, "true");
  }

  if (node->commandType != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->commandType);
    _fingerprintString(ctx, "commandType");
    _fingerprintString(ctx, buffer);
  }

  if (node->constraintDeps != NULL && node->constraintDeps->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->constraintDeps, node, "constraintDeps", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "constraintDeps");
  }
  if (node->cteList != NULL && node->cteList->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->cteList, node, "cteList", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "cteList");
  }
  if (node->distinctClause != NULL && node->distinctClause->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->distinctClause, node, "distinctClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "distinctClause");
  }
  if (node->groupClause != NULL && node->groupClause->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->groupClause, node, "groupClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "groupClause");
  }
  if (node->groupingSets != NULL && node->groupingSets->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->groupingSets, node, "groupingSets", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "groupingSets");
  }
  if (node->hasAggs) {
    _fingerprintString(ctx, "hasAggs");
    _fingerprintString(ctx, "true");
  }

  if (node->hasDistinctOn) {
    _fingerprintString(ctx, "hasDistinctOn");
    _fingerprintString(ctx, "true");
  }

  if (node->hasForUpdate) {
    _fingerprintString(ctx, "hasForUpdate");
    _fingerprintString(ctx, "true");
  }

  if (node->hasModifyingCTE) {
    _fingerprintString(ctx, "hasModifyingCTE");
    _fingerprintString(ctx, "true");
  }

  if (node->hasRecursive) {
    _fingerprintString(ctx, "hasRecursive");
    _fingerprintString(ctx, "true");
  }

  if (node->hasRowSecurity) {
    _fingerprintString(ctx, "hasRowSecurity");
    _fingerprintString(ctx, "true");
  }

  if (node->hasSubLinks) {
    _fingerprintString(ctx, "hasSubLinks");
    _fingerprintString(ctx, "true");
  }

  if (node->hasTargetSRFs) {
    _fingerprintString(ctx, "hasTargetSRFs");
    _fingerprintString(ctx, "true");
  }

  if (node->hasWindowFuncs) {
    _fingerprintString(ctx, "hasWindowFuncs");
    _fingerprintString(ctx, "true");
  }

  if (node->havingQual != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->havingQual, node, "havingQual", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "havingQual");
  }

  if (node->jointree != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->jointree, node, "jointree", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "jointree");
  }

  if (node->limitCount != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->limitCount, node, "limitCount", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "limitCount");
  }

  if (node->limitOffset != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->limitOffset, node, "limitOffset", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "limitOffset");
  }

  if (node->onConflict != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->onConflict, node, "onConflict", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "onConflict");
  }

  if (node->override != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->override);
    _fingerprintString(ctx, "override");
    _fingerprintString(ctx, buffer);
  }

  if (node->queryId != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->queryId);
    _fingerprintString(ctx, "queryId");
    _fingerprintString(ctx, buffer);
  }

  if (node->querySource != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->querySource);
    _fingerprintString(ctx, "querySource");
    _fingerprintString(ctx, buffer);
  }

  if (node->resultRelation != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->resultRelation);
    _fingerprintString(ctx, "resultRelation");
    _fingerprintString(ctx, buffer);
  }

  if (node->returningList != NULL && node->returningList->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->returningList, node, "returningList", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "returningList");
  }
  if (node->rowMarks != NULL && node->rowMarks->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->rowMarks, node, "rowMarks", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "rowMarks");
  }
  if (node->rtable != NULL && node->rtable->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->rtable, node, "rtable", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "rtable");
  }
  if (node->setOperations != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->setOperations, node, "setOperations", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "setOperations");
  }

  if (node->sortClause != NULL && node->sortClause->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->sortClause, node, "sortClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "sortClause");
  }
  if (node->stmt_len != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->stmt_len);
    _fingerprintString(ctx, "stmt_len");
    _fingerprintString(ctx, buffer);
  }

  if (node->stmt_location != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->stmt_location);
    _fingerprintString(ctx, "stmt_location");
    _fingerprintString(ctx, buffer);
  }

  if (node->targetList != NULL && node->targetList->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->targetList, node, "targetList", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "targetList");
  }
  if (node->utilityStmt != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->utilityStmt, node, "utilityStmt", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "utilityStmt");
  }

  if (node->windowClause != NULL && node->windowClause->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->windowClause, node, "windowClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "windowClause");
  }
  if (node->withCheckOptions != NULL && node->withCheckOptions->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->withCheckOptions, node, "withCheckOptions", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "withCheckOptions");
  }
}

static void
_fingerprintInsertStmt(FingerprintContext *ctx, const InsertStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "InsertStmt");

  if (node->cols != NULL && node->cols->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->cols, node, "cols", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "cols");
  }
  if (node->onConflictClause != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->onConflictClause, node, "onConflictClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "onConflictClause");
  }

  if (node->override != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->override);
    _fingerprintString(ctx, "override");
    _fingerprintString(ctx, buffer);
  }

  if (node->relation != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relation, node, "relation", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relation");
  }

  if (node->returningList != NULL && node->returningList->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->returningList, node, "returningList", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "returningList");
  }
  if (node->selectStmt != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->selectStmt, node, "selectStmt", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "selectStmt");
  }

  if (node->withClause != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->withClause, node, "withClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "withClause");
  }

}

static void
_fingerprintDeleteStmt(FingerprintContext *ctx, const DeleteStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "DeleteStmt");

  if (node->relation != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relation, node, "relation", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relation");
  }

  if (node->returningList != NULL && node->returningList->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->returningList, node, "returningList", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "returningList");
  }
  if (node->usingClause != NULL && node->usingClause->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->usingClause, node, "usingClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "usingClause");
  }
  if (node->whereClause != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->whereClause, node, "whereClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "whereClause");
  }

  if (node->withClause != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->withClause, node, "withClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "withClause");
  }

}

static void
_fingerprintUpdateStmt(FingerprintContext *ctx, const UpdateStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "UpdateStmt");

  if (node->fromClause != NULL && node->fromClause->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->fromClause, node, "fromClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "fromClause");
  }
  if (node->relation != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relation, node, "relation", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relation");
  }

  if (node->returningList != NULL && node->returningList->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->returningList, node, "returningList", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "returningList");
  }
  if (node->targetList != NULL && node->targetList->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->targetList, node, "targetList", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "targetList");
  }
  if (node->whereClause != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->whereClause, node, "whereClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "whereClause");
  }

  if (node->withClause != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->withClause, node, "withClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "withClause");
  }

}

static void
_fingerprintSelectStmt(FingerprintContext *ctx, const SelectStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "SelectStmt");

  if (node->all) {
    _fingerprintString(ctx, "all");
    _fingerprintString(ctx, "true");
  }

  if (node->distinctClause != NULL && node->distinctClause->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->distinctClause, node, "distinctClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "distinctClause");
  }
  if (node->fromClause != NULL && node->fromClause->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->fromClause, node, "fromClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "fromClause");
  }
  if (node->groupClause != NULL && node->groupClause->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->groupClause, node, "groupClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "groupClause");
  }
  if (node->havingClause != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->havingClause, node, "havingClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "havingClause");
  }

  if (node->intoClause != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->intoClause, node, "intoClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "intoClause");
  }

  if (node->larg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->larg, node, "larg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "larg");
  }

  if (node->limitCount != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->limitCount, node, "limitCount", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "limitCount");
  }

  if (node->limitOffset != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->limitOffset, node, "limitOffset", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "limitOffset");
  }

  if (node->lockingClause != NULL && node->lockingClause->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->lockingClause, node, "lockingClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "lockingClause");
  }
  if (node->op != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->op);
    _fingerprintString(ctx, "op");
    _fingerprintString(ctx, buffer);
  }

  if (node->rarg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->rarg, node, "rarg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "rarg");
  }

  if (node->sortClause != NULL && node->sortClause->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->sortClause, node, "sortClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "sortClause");
  }
  if (node->targetList != NULL && node->targetList->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->targetList, node, "targetList", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "targetList");
  }
  if (node->valuesLists != NULL && node->valuesLists->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->valuesLists, node, "valuesLists", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "valuesLists");
  }
  if (node->whereClause != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->whereClause, node, "whereClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "whereClause");
  }

  if (node->windowClause != NULL && node->windowClause->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->windowClause, node, "windowClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "windowClause");
  }
  if (node->withClause != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->withClause, node, "withClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "withClause");
  }

}

static void
_fingerprintAlterTableStmt(FingerprintContext *ctx, const AlterTableStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterTableStmt");

  if (node->cmds != NULL && node->cmds->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->cmds, node, "cmds", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "cmds");
  }
  if (node->missing_ok) {
    _fingerprintString(ctx, "missing_ok");
    _fingerprintString(ctx, "true");
  }

  if (node->relation != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relation, node, "relation", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relation");
  }

  if (node->relkind != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->relkind);
    _fingerprintString(ctx, "relkind");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintAlterTableCmd(FingerprintContext *ctx, const AlterTableCmd *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterTableCmd");

  if (node->behavior != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->behavior);
    _fingerprintString(ctx, "behavior");
    _fingerprintString(ctx, buffer);
  }

  if (node->def != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->def, node, "def", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "def");
  }

  if (node->missing_ok) {
    _fingerprintString(ctx, "missing_ok");
    _fingerprintString(ctx, "true");
  }

  if (node->name != NULL) {
    _fingerprintString(ctx, "name");
    _fingerprintString(ctx, node->name);
  }

  if (node->newowner != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->newowner, node, "newowner", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "newowner");
  }

  if (node->subtype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->subtype);
    _fingerprintString(ctx, "subtype");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintAlterDomainStmt(FingerprintContext *ctx, const AlterDomainStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterDomainStmt");

  if (node->behavior != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->behavior);
    _fingerprintString(ctx, "behavior");
    _fingerprintString(ctx, buffer);
  }

  if (node->def != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->def, node, "def", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "def");
  }

  if (node->missing_ok) {
    _fingerprintString(ctx, "missing_ok");
    _fingerprintString(ctx, "true");
  }

  if (node->name != NULL) {
    _fingerprintString(ctx, "name");
    _fingerprintString(ctx, node->name);
  }

  if (node->subtype != 0) {
    char buffer[2] = {node->subtype, '\0'};
    _fingerprintString(ctx, "subtype");
    _fingerprintString(ctx, buffer);
  }

  if (node->typeName != NULL && node->typeName->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->typeName, node, "typeName", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "typeName");
  }
}

static void
_fingerprintSetOperationStmt(FingerprintContext *ctx, const SetOperationStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "SetOperationStmt");

  if (node->all) {
    _fingerprintString(ctx, "all");
    _fingerprintString(ctx, "true");
  }

  if (node->colCollations != NULL && node->colCollations->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->colCollations, node, "colCollations", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "colCollations");
  }
  if (node->colTypes != NULL && node->colTypes->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->colTypes, node, "colTypes", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "colTypes");
  }
  if (node->colTypmods != NULL && node->colTypmods->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->colTypmods, node, "colTypmods", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "colTypmods");
  }
  if (node->groupClauses != NULL && node->groupClauses->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->groupClauses, node, "groupClauses", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "groupClauses");
  }
  if (node->larg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->larg, node, "larg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "larg");
  }

  if (node->op != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->op);
    _fingerprintString(ctx, "op");
    _fingerprintString(ctx, buffer);
  }

  if (node->rarg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->rarg, node, "rarg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "rarg");
  }

}

static void
_fingerprintGrantStmt(FingerprintContext *ctx, const GrantStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "GrantStmt");

  if (node->behavior != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->behavior);
    _fingerprintString(ctx, "behavior");
    _fingerprintString(ctx, buffer);
  }

  if (node->grant_option) {
    _fingerprintString(ctx, "grant_option");
    _fingerprintString(ctx, "true");
  }

  if (node->grantees != NULL && node->grantees->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->grantees, node, "grantees", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "grantees");
  }
  if (node->is_grant) {
    _fingerprintString(ctx, "is_grant");
    _fingerprintString(ctx, "true");
  }

  if (node->objects != NULL && node->objects->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->objects, node, "objects", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "objects");
  }
  if (node->objtype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->objtype);
    _fingerprintString(ctx, "objtype");
    _fingerprintString(ctx, buffer);
  }

  if (node->privileges != NULL && node->privileges->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->privileges, node, "privileges", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "privileges");
  }
  if (node->targtype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->targtype);
    _fingerprintString(ctx, "targtype");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintGrantRoleStmt(FingerprintContext *ctx, const GrantRoleStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "GrantRoleStmt");

  if (node->admin_opt) {
    _fingerprintString(ctx, "admin_opt");
    _fingerprintString(ctx, "true");
  }

  if (node->behavior != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->behavior);
    _fingerprintString(ctx, "behavior");
    _fingerprintString(ctx, buffer);
  }

  if (node->granted_roles != NULL && node->granted_roles->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->granted_roles, node, "granted_roles", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "granted_roles");
  }
  if (node->grantee_roles != NULL && node->grantee_roles->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->grantee_roles, node, "grantee_roles", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "grantee_roles");
  }
  if (node->grantor != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->grantor, node, "grantor", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "grantor");
  }

  if (node->is_grant) {
    _fingerprintString(ctx, "is_grant");
    _fingerprintString(ctx, "true");
  }

}

static void
_fingerprintAlterDefaultPrivilegesStmt(FingerprintContext *ctx, const AlterDefaultPrivilegesStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterDefaultPrivilegesStmt");

  if (node->action != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->action, node, "action", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "action");
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
}

static void
_fingerprintClosePortalStmt(FingerprintContext *ctx, const ClosePortalStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "ClosePortalStmt");

  // Intentionally ignoring node->portalname for fingerprinting

}

static void
_fingerprintClusterStmt(FingerprintContext *ctx, const ClusterStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "ClusterStmt");

  if (node->indexname != NULL) {
    _fingerprintString(ctx, "indexname");
    _fingerprintString(ctx, node->indexname);
  }

  if (node->relation != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relation, node, "relation", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relation");
  }

  if (node->verbose) {
    _fingerprintString(ctx, "verbose");
    _fingerprintString(ctx, "true");
  }

}

static void
_fingerprintCopyStmt(FingerprintContext *ctx, const CopyStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CopyStmt");

  if (node->attlist != NULL && node->attlist->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->attlist, node, "attlist", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "attlist");
  }
  if (node->filename != NULL) {
    _fingerprintString(ctx, "filename");
    _fingerprintString(ctx, node->filename);
  }

  if (node->is_from) {
    _fingerprintString(ctx, "is_from");
    _fingerprintString(ctx, "true");
  }

  if (node->is_program) {
    _fingerprintString(ctx, "is_program");
    _fingerprintString(ctx, "true");
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->query != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->query, node, "query", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "query");
  }

  if (node->relation != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relation, node, "relation", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relation");
  }

}

static void
_fingerprintCreateStmt(FingerprintContext *ctx, const CreateStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateStmt");

  if (node->constraints != NULL && node->constraints->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->constraints, node, "constraints", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "constraints");
  }
  if (node->if_not_exists) {
    _fingerprintString(ctx, "if_not_exists");
    _fingerprintString(ctx, "true");
  }

  if (node->inhRelations != NULL && node->inhRelations->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->inhRelations, node, "inhRelations", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "inhRelations");
  }
  if (node->ofTypename != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->ofTypename, node, "ofTypename", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "ofTypename");
  }

  if (node->oncommit != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->oncommit);
    _fingerprintString(ctx, "oncommit");
    _fingerprintString(ctx, buffer);
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->partbound != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->partbound, node, "partbound", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "partbound");
  }

  if (node->partspec != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->partspec, node, "partspec", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "partspec");
  }

  if (node->relation != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relation, node, "relation", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relation");
  }

  if (node->tableElts != NULL && node->tableElts->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->tableElts, node, "tableElts", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "tableElts");
  }
  if (node->tablespacename != NULL) {
    _fingerprintString(ctx, "tablespacename");
    _fingerprintString(ctx, node->tablespacename);
  }

}

static void
_fingerprintDefineStmt(FingerprintContext *ctx, const DefineStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "DefineStmt");

  if (node->args != NULL && node->args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->args, node, "args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "args");
  }
  if (node->definition != NULL && node->definition->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->definition, node, "definition", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "definition");
  }
  if (node->defnames != NULL && node->defnames->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->defnames, node, "defnames", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "defnames");
  }
  if (node->if_not_exists) {
    _fingerprintString(ctx, "if_not_exists");
    _fingerprintString(ctx, "true");
  }

  if (node->kind != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->kind);
    _fingerprintString(ctx, "kind");
    _fingerprintString(ctx, buffer);
  }

  if (node->oldstyle) {
    _fingerprintString(ctx, "oldstyle");
    _fingerprintString(ctx, "true");
  }

}

static void
_fingerprintDropStmt(FingerprintContext *ctx, const DropStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "DropStmt");

  if (node->behavior != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->behavior);
    _fingerprintString(ctx, "behavior");
    _fingerprintString(ctx, buffer);
  }

  if (node->concurrent) {
    _fingerprintString(ctx, "concurrent");
    _fingerprintString(ctx, "true");
  }

  if (node->missing_ok) {
    _fingerprintString(ctx, "missing_ok");
    _fingerprintString(ctx, "true");
  }

  if (node->objects != NULL && node->objects->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->objects, node, "objects", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "objects");
  }
  if (node->removeType != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->removeType);
    _fingerprintString(ctx, "removeType");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintTruncateStmt(FingerprintContext *ctx, const TruncateStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "TruncateStmt");

  if (node->behavior != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->behavior);
    _fingerprintString(ctx, "behavior");
    _fingerprintString(ctx, buffer);
  }

  if (node->relations != NULL && node->relations->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relations, node, "relations", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relations");
  }
  if (node->restart_seqs) {
    _fingerprintString(ctx, "restart_seqs");
    _fingerprintString(ctx, "true");
  }

}

static void
_fingerprintCommentStmt(FingerprintContext *ctx, const CommentStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CommentStmt");

  if (node->comment != NULL) {
    _fingerprintString(ctx, "comment");
    _fingerprintString(ctx, node->comment);
  }

  if (node->object != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->object, node, "object", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "object");
  }

  if (node->objtype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->objtype);
    _fingerprintString(ctx, "objtype");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintFetchStmt(FingerprintContext *ctx, const FetchStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "FetchStmt");

  if (node->direction != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->direction);
    _fingerprintString(ctx, "direction");
    _fingerprintString(ctx, buffer);
  }

  if (node->howMany != 0) {
    char buffer[50];
    sprintf(buffer, "%ld", node->howMany);
    _fingerprintString(ctx, "howMany");
    _fingerprintString(ctx, buffer);
  }

  if (node->ismove) {
    _fingerprintString(ctx, "ismove");
    _fingerprintString(ctx, "true");
  }

  // Intentionally ignoring node->portalname for fingerprinting

}

static void
_fingerprintIndexStmt(FingerprintContext *ctx, const IndexStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "IndexStmt");

  if (node->accessMethod != NULL) {
    _fingerprintString(ctx, "accessMethod");
    _fingerprintString(ctx, node->accessMethod);
  }

  if (node->concurrent) {
    _fingerprintString(ctx, "concurrent");
    _fingerprintString(ctx, "true");
  }

  if (node->deferrable) {
    _fingerprintString(ctx, "deferrable");
    _fingerprintString(ctx, "true");
  }

  if (node->excludeOpNames != NULL && node->excludeOpNames->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->excludeOpNames, node, "excludeOpNames", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "excludeOpNames");
  }
  if (node->idxcomment != NULL) {
    _fingerprintString(ctx, "idxcomment");
    _fingerprintString(ctx, node->idxcomment);
  }

  if (node->idxname != NULL) {
    _fingerprintString(ctx, "idxname");
    _fingerprintString(ctx, node->idxname);
  }

  if (node->if_not_exists) {
    _fingerprintString(ctx, "if_not_exists");
    _fingerprintString(ctx, "true");
  }

  if (node->indexOid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->indexOid);
    _fingerprintString(ctx, "indexOid");
    _fingerprintString(ctx, buffer);
  }

  if (node->indexParams != NULL && node->indexParams->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->indexParams, node, "indexParams", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "indexParams");
  }
  if (node->initdeferred) {
    _fingerprintString(ctx, "initdeferred");
    _fingerprintString(ctx, "true");
  }

  if (node->isconstraint) {
    _fingerprintString(ctx, "isconstraint");
    _fingerprintString(ctx, "true");
  }

  if (node->oldNode != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->oldNode);
    _fingerprintString(ctx, "oldNode");
    _fingerprintString(ctx, buffer);
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->primary) {
    _fingerprintString(ctx, "primary");
    _fingerprintString(ctx, "true");
  }

  if (node->relation != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relation, node, "relation", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relation");
  }

  if (node->tableSpace != NULL) {
    _fingerprintString(ctx, "tableSpace");
    _fingerprintString(ctx, node->tableSpace);
  }

  if (node->transformed) {
    _fingerprintString(ctx, "transformed");
    _fingerprintString(ctx, "true");
  }

  if (node->unique) {
    _fingerprintString(ctx, "unique");
    _fingerprintString(ctx, "true");
  }

  if (node->whereClause != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->whereClause, node, "whereClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "whereClause");
  }

}

static void
_fingerprintCreateFunctionStmt(FingerprintContext *ctx, const CreateFunctionStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateFunctionStmt");

  if (node->funcname != NULL && node->funcname->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->funcname, node, "funcname", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "funcname");
  }
  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->parameters != NULL && node->parameters->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->parameters, node, "parameters", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "parameters");
  }
  if (node->replace) {
    _fingerprintString(ctx, "replace");
    _fingerprintString(ctx, "true");
  }

  if (node->returnType != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->returnType, node, "returnType", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "returnType");
  }

  if (node->withClause != NULL && node->withClause->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->withClause, node, "withClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "withClause");
  }
}

static void
_fingerprintAlterFunctionStmt(FingerprintContext *ctx, const AlterFunctionStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterFunctionStmt");

  if (node->actions != NULL && node->actions->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->actions, node, "actions", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "actions");
  }
  if (node->func != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->func, node, "func", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "func");
  }

}

static void
_fingerprintDoStmt(FingerprintContext *ctx, const DoStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "DoStmt");

  if (node->args != NULL && node->args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->args, node, "args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "args");
  }
}

static void
_fingerprintRenameStmt(FingerprintContext *ctx, const RenameStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "RenameStmt");

  if (node->behavior != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->behavior);
    _fingerprintString(ctx, "behavior");
    _fingerprintString(ctx, buffer);
  }

  if (node->missing_ok) {
    _fingerprintString(ctx, "missing_ok");
    _fingerprintString(ctx, "true");
  }

  if (node->newname != NULL) {
    _fingerprintString(ctx, "newname");
    _fingerprintString(ctx, node->newname);
  }

  if (node->object != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->object, node, "object", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "object");
  }

  if (node->relation != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relation, node, "relation", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relation");
  }

  if (node->relationType != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->relationType);
    _fingerprintString(ctx, "relationType");
    _fingerprintString(ctx, buffer);
  }

  if (node->renameType != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->renameType);
    _fingerprintString(ctx, "renameType");
    _fingerprintString(ctx, buffer);
  }

  if (node->subname != NULL) {
    _fingerprintString(ctx, "subname");
    _fingerprintString(ctx, node->subname);
  }

}

static void
_fingerprintRuleStmt(FingerprintContext *ctx, const RuleStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "RuleStmt");

  if (node->actions != NULL && node->actions->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->actions, node, "actions", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "actions");
  }
  if (node->event != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->event);
    _fingerprintString(ctx, "event");
    _fingerprintString(ctx, buffer);
  }

  if (node->instead) {
    _fingerprintString(ctx, "instead");
    _fingerprintString(ctx, "true");
  }

  if (node->relation != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relation, node, "relation", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relation");
  }

  if (node->replace) {
    _fingerprintString(ctx, "replace");
    _fingerprintString(ctx, "true");
  }

  if (node->rulename != NULL) {
    _fingerprintString(ctx, "rulename");
    _fingerprintString(ctx, node->rulename);
  }

  if (node->whereClause != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->whereClause, node, "whereClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "whereClause");
  }

}

static void
_fingerprintNotifyStmt(FingerprintContext *ctx, const NotifyStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "NotifyStmt");

  if (node->conditionname != NULL) {
    _fingerprintString(ctx, "conditionname");
    _fingerprintString(ctx, node->conditionname);
  }

  if (node->payload != NULL) {
    _fingerprintString(ctx, "payload");
    _fingerprintString(ctx, node->payload);
  }

}

static void
_fingerprintListenStmt(FingerprintContext *ctx, const ListenStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "ListenStmt");

  if (node->conditionname != NULL) {
    _fingerprintString(ctx, "conditionname");
    _fingerprintString(ctx, node->conditionname);
  }

}

static void
_fingerprintUnlistenStmt(FingerprintContext *ctx, const UnlistenStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "UnlistenStmt");

  if (node->conditionname != NULL) {
    _fingerprintString(ctx, "conditionname");
    _fingerprintString(ctx, node->conditionname);
  }

}

static void
_fingerprintTransactionStmt(FingerprintContext *ctx, const TransactionStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "TransactionStmt");

  // Intentionally ignoring node->gid for fingerprinting

  if (node->kind != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->kind);
    _fingerprintString(ctx, "kind");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->options for fingerprinting

}

static void
_fingerprintViewStmt(FingerprintContext *ctx, const ViewStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "ViewStmt");

  if (node->aliases != NULL && node->aliases->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->aliases, node, "aliases", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "aliases");
  }
  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->query != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->query, node, "query", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "query");
  }

  if (node->replace) {
    _fingerprintString(ctx, "replace");
    _fingerprintString(ctx, "true");
  }

  if (node->view != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->view, node, "view", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "view");
  }

  if (node->withCheckOption != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->withCheckOption);
    _fingerprintString(ctx, "withCheckOption");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintLoadStmt(FingerprintContext *ctx, const LoadStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "LoadStmt");

  if (node->filename != NULL) {
    _fingerprintString(ctx, "filename");
    _fingerprintString(ctx, node->filename);
  }

}

static void
_fingerprintCreateDomainStmt(FingerprintContext *ctx, const CreateDomainStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateDomainStmt");

  if (node->collClause != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->collClause, node, "collClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "collClause");
  }

  if (node->constraints != NULL && node->constraints->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->constraints, node, "constraints", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "constraints");
  }
  if (node->domainname != NULL && node->domainname->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->domainname, node, "domainname", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "domainname");
  }
  if (node->typeName != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->typeName, node, "typeName", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "typeName");
  }

}

static void
_fingerprintCreatedbStmt(FingerprintContext *ctx, const CreatedbStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreatedbStmt");

  if (node->dbname != NULL) {
    _fingerprintString(ctx, "dbname");
    _fingerprintString(ctx, node->dbname);
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
}

static void
_fingerprintDropdbStmt(FingerprintContext *ctx, const DropdbStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "DropdbStmt");

  if (node->dbname != NULL) {
    _fingerprintString(ctx, "dbname");
    _fingerprintString(ctx, node->dbname);
  }

  if (node->missing_ok) {
    _fingerprintString(ctx, "missing_ok");
    _fingerprintString(ctx, "true");
  }

}

static void
_fingerprintVacuumStmt(FingerprintContext *ctx, const VacuumStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "VacuumStmt");

  if (node->options != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->options);
    _fingerprintString(ctx, "options");
    _fingerprintString(ctx, buffer);
  }

  if (node->relation != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relation, node, "relation", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relation");
  }

  if (node->va_cols != NULL && node->va_cols->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->va_cols, node, "va_cols", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "va_cols");
  }
}

static void
_fingerprintExplainStmt(FingerprintContext *ctx, const ExplainStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "ExplainStmt");

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->query != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->query, node, "query", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "query");
  }

}

static void
_fingerprintCreateTableAsStmt(FingerprintContext *ctx, const CreateTableAsStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateTableAsStmt");

  if (node->if_not_exists) {
    _fingerprintString(ctx, "if_not_exists");
    _fingerprintString(ctx, "true");
  }

  if (node->into != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->into, node, "into", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "into");
  }

  if (node->is_select_into) {
    _fingerprintString(ctx, "is_select_into");
    _fingerprintString(ctx, "true");
  }

  if (node->query != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->query, node, "query", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "query");
  }

  if (node->relkind != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->relkind);
    _fingerprintString(ctx, "relkind");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintCreateSeqStmt(FingerprintContext *ctx, const CreateSeqStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateSeqStmt");

  if (node->for_identity) {
    _fingerprintString(ctx, "for_identity");
    _fingerprintString(ctx, "true");
  }

  if (node->if_not_exists) {
    _fingerprintString(ctx, "if_not_exists");
    _fingerprintString(ctx, "true");
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->ownerId != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->ownerId);
    _fingerprintString(ctx, "ownerId");
    _fingerprintString(ctx, buffer);
  }

  if (node->sequence != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->sequence, node, "sequence", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "sequence");
  }

}

static void
_fingerprintAlterSeqStmt(FingerprintContext *ctx, const AlterSeqStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterSeqStmt");

  if (node->for_identity) {
    _fingerprintString(ctx, "for_identity");
    _fingerprintString(ctx, "true");
  }

  if (node->missing_ok) {
    _fingerprintString(ctx, "missing_ok");
    _fingerprintString(ctx, "true");
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->sequence != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->sequence, node, "sequence", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "sequence");
  }

}

static void
_fingerprintVariableSetStmt(FingerprintContext *ctx, const VariableSetStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "VariableSetStmt");

  if (node->args != NULL && node->args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->args, node, "args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "args");
  }
  if (node->is_local) {
    _fingerprintString(ctx, "is_local");
    _fingerprintString(ctx, "true");
  }

  if (node->kind != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->kind);
    _fingerprintString(ctx, "kind");
    _fingerprintString(ctx, buffer);
  }

  if (node->name != NULL) {
    _fingerprintString(ctx, "name");
    _fingerprintString(ctx, node->name);
  }

}

static void
_fingerprintVariableShowStmt(FingerprintContext *ctx, const VariableShowStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "VariableShowStmt");

  if (node->name != NULL) {
    _fingerprintString(ctx, "name");
    _fingerprintString(ctx, node->name);
  }

}

static void
_fingerprintDiscardStmt(FingerprintContext *ctx, const DiscardStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "DiscardStmt");

  if (node->target != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->target);
    _fingerprintString(ctx, "target");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintCreateTrigStmt(FingerprintContext *ctx, const CreateTrigStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateTrigStmt");

  if (node->args != NULL && node->args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->args, node, "args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "args");
  }
  if (node->columns != NULL && node->columns->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->columns, node, "columns", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "columns");
  }
  if (node->constrrel != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->constrrel, node, "constrrel", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "constrrel");
  }

  if (node->deferrable) {
    _fingerprintString(ctx, "deferrable");
    _fingerprintString(ctx, "true");
  }

  if (node->events != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->events);
    _fingerprintString(ctx, "events");
    _fingerprintString(ctx, buffer);
  }

  if (node->funcname != NULL && node->funcname->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->funcname, node, "funcname", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "funcname");
  }
  if (node->initdeferred) {
    _fingerprintString(ctx, "initdeferred");
    _fingerprintString(ctx, "true");
  }

  if (node->isconstraint) {
    _fingerprintString(ctx, "isconstraint");
    _fingerprintString(ctx, "true");
  }

  if (node->relation != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relation, node, "relation", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relation");
  }

  if (node->row) {
    _fingerprintString(ctx, "row");
    _fingerprintString(ctx, "true");
  }

  if (node->timing != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->timing);
    _fingerprintString(ctx, "timing");
    _fingerprintString(ctx, buffer);
  }

  if (node->transitionRels != NULL && node->transitionRels->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->transitionRels, node, "transitionRels", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "transitionRels");
  }
  if (node->trigname != NULL) {
    _fingerprintString(ctx, "trigname");
    _fingerprintString(ctx, node->trigname);
  }

  if (node->whenClause != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->whenClause, node, "whenClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "whenClause");
  }

}

static void
_fingerprintCreatePLangStmt(FingerprintContext *ctx, const CreatePLangStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreatePLangStmt");

  if (node->plhandler != NULL && node->plhandler->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->plhandler, node, "plhandler", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "plhandler");
  }
  if (node->plinline != NULL && node->plinline->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->plinline, node, "plinline", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "plinline");
  }
  if (node->plname != NULL) {
    _fingerprintString(ctx, "plname");
    _fingerprintString(ctx, node->plname);
  }

  if (node->pltrusted) {
    _fingerprintString(ctx, "pltrusted");
    _fingerprintString(ctx, "true");
  }

  if (node->plvalidator != NULL && node->plvalidator->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->plvalidator, node, "plvalidator", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "plvalidator");
  }
  if (node->replace) {
    _fingerprintString(ctx, "replace");
    _fingerprintString(ctx, "true");
  }

}

static void
_fingerprintCreateRoleStmt(FingerprintContext *ctx, const CreateRoleStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateRoleStmt");

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->role != NULL) {
    _fingerprintString(ctx, "role");
    _fingerprintString(ctx, node->role);
  }

  if (node->stmt_type != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->stmt_type);
    _fingerprintString(ctx, "stmt_type");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintAlterRoleStmt(FingerprintContext *ctx, const AlterRoleStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterRoleStmt");

  if (node->action != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->action);
    _fingerprintString(ctx, "action");
    _fingerprintString(ctx, buffer);
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->role != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->role, node, "role", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "role");
  }

}

static void
_fingerprintDropRoleStmt(FingerprintContext *ctx, const DropRoleStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "DropRoleStmt");

  if (node->missing_ok) {
    _fingerprintString(ctx, "missing_ok");
    _fingerprintString(ctx, "true");
  }

  if (node->roles != NULL && node->roles->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->roles, node, "roles", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "roles");
  }
}

static void
_fingerprintLockStmt(FingerprintContext *ctx, const LockStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "LockStmt");

  if (node->mode != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->mode);
    _fingerprintString(ctx, "mode");
    _fingerprintString(ctx, buffer);
  }

  if (node->nowait) {
    _fingerprintString(ctx, "nowait");
    _fingerprintString(ctx, "true");
  }

  if (node->relations != NULL && node->relations->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relations, node, "relations", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relations");
  }
}

static void
_fingerprintConstraintsSetStmt(FingerprintContext *ctx, const ConstraintsSetStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "ConstraintsSetStmt");

  if (node->constraints != NULL && node->constraints->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->constraints, node, "constraints", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "constraints");
  }
  if (node->deferred) {
    _fingerprintString(ctx, "deferred");
    _fingerprintString(ctx, "true");
  }

}

static void
_fingerprintReindexStmt(FingerprintContext *ctx, const ReindexStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "ReindexStmt");

  if (node->kind != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->kind);
    _fingerprintString(ctx, "kind");
    _fingerprintString(ctx, buffer);
  }

  if (node->name != NULL) {
    _fingerprintString(ctx, "name");
    _fingerprintString(ctx, node->name);
  }

  if (node->options != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->options);
    _fingerprintString(ctx, "options");
    _fingerprintString(ctx, buffer);
  }

  if (node->relation != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relation, node, "relation", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relation");
  }

}

static void
_fingerprintCheckPointStmt(FingerprintContext *ctx, const CheckPointStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CheckPointStmt");

}

static void
_fingerprintCreateSchemaStmt(FingerprintContext *ctx, const CreateSchemaStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateSchemaStmt");

  if (node->authrole != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->authrole, node, "authrole", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "authrole");
  }

  if (node->if_not_exists) {
    _fingerprintString(ctx, "if_not_exists");
    _fingerprintString(ctx, "true");
  }

  if (node->schemaElts != NULL && node->schemaElts->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->schemaElts, node, "schemaElts", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "schemaElts");
  }
  if (node->schemaname != NULL) {
    _fingerprintString(ctx, "schemaname");
    _fingerprintString(ctx, node->schemaname);
  }

}

static void
_fingerprintAlterDatabaseStmt(FingerprintContext *ctx, const AlterDatabaseStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterDatabaseStmt");

  if (node->dbname != NULL) {
    _fingerprintString(ctx, "dbname");
    _fingerprintString(ctx, node->dbname);
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
}

static void
_fingerprintAlterDatabaseSetStmt(FingerprintContext *ctx, const AlterDatabaseSetStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterDatabaseSetStmt");

  if (node->dbname != NULL) {
    _fingerprintString(ctx, "dbname");
    _fingerprintString(ctx, node->dbname);
  }

  if (node->setstmt != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->setstmt, node, "setstmt", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "setstmt");
  }

}

static void
_fingerprintAlterRoleSetStmt(FingerprintContext *ctx, const AlterRoleSetStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterRoleSetStmt");

  if (node->database != NULL) {
    _fingerprintString(ctx, "database");
    _fingerprintString(ctx, node->database);
  }

  if (node->role != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->role, node, "role", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "role");
  }

  if (node->setstmt != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->setstmt, node, "setstmt", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "setstmt");
  }

}

static void
_fingerprintCreateConversionStmt(FingerprintContext *ctx, const CreateConversionStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateConversionStmt");

  if (node->conversion_name != NULL && node->conversion_name->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->conversion_name, node, "conversion_name", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "conversion_name");
  }
  if (node->def) {
    _fingerprintString(ctx, "def");
    _fingerprintString(ctx, "true");
  }

  if (node->for_encoding_name != NULL) {
    _fingerprintString(ctx, "for_encoding_name");
    _fingerprintString(ctx, node->for_encoding_name);
  }

  if (node->func_name != NULL && node->func_name->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->func_name, node, "func_name", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "func_name");
  }
  if (node->to_encoding_name != NULL) {
    _fingerprintString(ctx, "to_encoding_name");
    _fingerprintString(ctx, node->to_encoding_name);
  }

}

static void
_fingerprintCreateCastStmt(FingerprintContext *ctx, const CreateCastStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateCastStmt");

  if (node->context != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->context);
    _fingerprintString(ctx, "context");
    _fingerprintString(ctx, buffer);
  }

  if (node->func != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->func, node, "func", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "func");
  }

  if (node->inout) {
    _fingerprintString(ctx, "inout");
    _fingerprintString(ctx, "true");
  }

  if (node->sourcetype != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->sourcetype, node, "sourcetype", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "sourcetype");
  }

  if (node->targettype != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->targettype, node, "targettype", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "targettype");
  }

}

static void
_fingerprintCreateOpClassStmt(FingerprintContext *ctx, const CreateOpClassStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateOpClassStmt");

  if (node->amname != NULL) {
    _fingerprintString(ctx, "amname");
    _fingerprintString(ctx, node->amname);
  }

  if (node->datatype != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->datatype, node, "datatype", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "datatype");
  }

  if (node->isDefault) {
    _fingerprintString(ctx, "isDefault");
    _fingerprintString(ctx, "true");
  }

  if (node->items != NULL && node->items->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->items, node, "items", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "items");
  }
  if (node->opclassname != NULL && node->opclassname->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->opclassname, node, "opclassname", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "opclassname");
  }
  if (node->opfamilyname != NULL && node->opfamilyname->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->opfamilyname, node, "opfamilyname", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "opfamilyname");
  }
}

static void
_fingerprintCreateOpFamilyStmt(FingerprintContext *ctx, const CreateOpFamilyStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateOpFamilyStmt");

  if (node->amname != NULL) {
    _fingerprintString(ctx, "amname");
    _fingerprintString(ctx, node->amname);
  }

  if (node->opfamilyname != NULL && node->opfamilyname->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->opfamilyname, node, "opfamilyname", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "opfamilyname");
  }
}

static void
_fingerprintAlterOpFamilyStmt(FingerprintContext *ctx, const AlterOpFamilyStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterOpFamilyStmt");

  if (node->amname != NULL) {
    _fingerprintString(ctx, "amname");
    _fingerprintString(ctx, node->amname);
  }

  if (node->isDrop) {
    _fingerprintString(ctx, "isDrop");
    _fingerprintString(ctx, "true");
  }

  if (node->items != NULL && node->items->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->items, node, "items", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "items");
  }
  if (node->opfamilyname != NULL && node->opfamilyname->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->opfamilyname, node, "opfamilyname", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "opfamilyname");
  }
}

static void
_fingerprintPrepareStmt(FingerprintContext *ctx, const PrepareStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "PrepareStmt");

  if (node->argtypes != NULL && node->argtypes->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->argtypes, node, "argtypes", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "argtypes");
  }
  // Intentionally ignoring node->name for fingerprinting

  if (node->query != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->query, node, "query", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "query");
  }

}

static void
_fingerprintExecuteStmt(FingerprintContext *ctx, const ExecuteStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "ExecuteStmt");

  // Intentionally ignoring node->name for fingerprinting

  if (node->params != NULL && node->params->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->params, node, "params", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "params");
  }
}

static void
_fingerprintDeallocateStmt(FingerprintContext *ctx, const DeallocateStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "DeallocateStmt");

  // Intentionally ignoring node->name for fingerprinting

}

static void
_fingerprintDeclareCursorStmt(FingerprintContext *ctx, const DeclareCursorStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "DeclareCursorStmt");

  if (node->options != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->options);
    _fingerprintString(ctx, "options");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->portalname for fingerprinting

  if (node->query != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->query, node, "query", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "query");
  }

}

static void
_fingerprintCreateTableSpaceStmt(FingerprintContext *ctx, const CreateTableSpaceStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateTableSpaceStmt");

  // Intentionally ignoring node->location for fingerprinting

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->owner != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->owner, node, "owner", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "owner");
  }

  if (node->tablespacename != NULL) {
    _fingerprintString(ctx, "tablespacename");
    _fingerprintString(ctx, node->tablespacename);
  }

}

static void
_fingerprintDropTableSpaceStmt(FingerprintContext *ctx, const DropTableSpaceStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "DropTableSpaceStmt");

  if (node->missing_ok) {
    _fingerprintString(ctx, "missing_ok");
    _fingerprintString(ctx, "true");
  }

  if (node->tablespacename != NULL) {
    _fingerprintString(ctx, "tablespacename");
    _fingerprintString(ctx, node->tablespacename);
  }

}

static void
_fingerprintAlterObjectDependsStmt(FingerprintContext *ctx, const AlterObjectDependsStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterObjectDependsStmt");

  if (node->extname != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->extname, node, "extname", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "extname");
  }

  if (node->object != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->object, node, "object", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "object");
  }

  if (node->objectType != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->objectType);
    _fingerprintString(ctx, "objectType");
    _fingerprintString(ctx, buffer);
  }

  if (node->relation != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relation, node, "relation", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relation");
  }

}

static void
_fingerprintAlterObjectSchemaStmt(FingerprintContext *ctx, const AlterObjectSchemaStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterObjectSchemaStmt");

  if (node->missing_ok) {
    _fingerprintString(ctx, "missing_ok");
    _fingerprintString(ctx, "true");
  }

  if (node->newschema != NULL) {
    _fingerprintString(ctx, "newschema");
    _fingerprintString(ctx, node->newschema);
  }

  if (node->object != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->object, node, "object", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "object");
  }

  if (node->objectType != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->objectType);
    _fingerprintString(ctx, "objectType");
    _fingerprintString(ctx, buffer);
  }

  if (node->relation != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relation, node, "relation", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relation");
  }

}

static void
_fingerprintAlterOwnerStmt(FingerprintContext *ctx, const AlterOwnerStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterOwnerStmt");

  if (node->newowner != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->newowner, node, "newowner", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "newowner");
  }

  if (node->object != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->object, node, "object", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "object");
  }

  if (node->objectType != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->objectType);
    _fingerprintString(ctx, "objectType");
    _fingerprintString(ctx, buffer);
  }

  if (node->relation != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relation, node, "relation", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relation");
  }

}

static void
_fingerprintAlterOperatorStmt(FingerprintContext *ctx, const AlterOperatorStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterOperatorStmt");

  if (node->opername != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->opername, node, "opername", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "opername");
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
}

static void
_fingerprintDropOwnedStmt(FingerprintContext *ctx, const DropOwnedStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "DropOwnedStmt");

  if (node->behavior != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->behavior);
    _fingerprintString(ctx, "behavior");
    _fingerprintString(ctx, buffer);
  }

  if (node->roles != NULL && node->roles->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->roles, node, "roles", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "roles");
  }
}

static void
_fingerprintReassignOwnedStmt(FingerprintContext *ctx, const ReassignOwnedStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "ReassignOwnedStmt");

  if (node->newrole != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->newrole, node, "newrole", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "newrole");
  }

  if (node->roles != NULL && node->roles->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->roles, node, "roles", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "roles");
  }
}

static void
_fingerprintCompositeTypeStmt(FingerprintContext *ctx, const CompositeTypeStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CompositeTypeStmt");

  if (node->coldeflist != NULL && node->coldeflist->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->coldeflist, node, "coldeflist", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "coldeflist");
  }
  if (node->typevar != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->typevar, node, "typevar", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "typevar");
  }

}

static void
_fingerprintCreateEnumStmt(FingerprintContext *ctx, const CreateEnumStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateEnumStmt");

  if (node->typeName != NULL && node->typeName->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->typeName, node, "typeName", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "typeName");
  }
  if (node->vals != NULL && node->vals->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->vals, node, "vals", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "vals");
  }
}

static void
_fingerprintCreateRangeStmt(FingerprintContext *ctx, const CreateRangeStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateRangeStmt");

  if (node->params != NULL && node->params->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->params, node, "params", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "params");
  }
  if (node->typeName != NULL && node->typeName->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->typeName, node, "typeName", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "typeName");
  }
}

static void
_fingerprintAlterEnumStmt(FingerprintContext *ctx, const AlterEnumStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterEnumStmt");

  if (node->newVal != NULL) {
    _fingerprintString(ctx, "newVal");
    _fingerprintString(ctx, node->newVal);
  }

  if (node->newValIsAfter) {
    _fingerprintString(ctx, "newValIsAfter");
    _fingerprintString(ctx, "true");
  }

  if (node->newValNeighbor != NULL) {
    _fingerprintString(ctx, "newValNeighbor");
    _fingerprintString(ctx, node->newValNeighbor);
  }

  if (node->oldVal != NULL) {
    _fingerprintString(ctx, "oldVal");
    _fingerprintString(ctx, node->oldVal);
  }

  if (node->skipIfNewValExists) {
    _fingerprintString(ctx, "skipIfNewValExists");
    _fingerprintString(ctx, "true");
  }

  if (node->typeName != NULL && node->typeName->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->typeName, node, "typeName", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "typeName");
  }
}

static void
_fingerprintAlterTSDictionaryStmt(FingerprintContext *ctx, const AlterTSDictionaryStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterTSDictionaryStmt");

  if (node->dictname != NULL && node->dictname->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->dictname, node, "dictname", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "dictname");
  }
  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
}

static void
_fingerprintAlterTSConfigurationStmt(FingerprintContext *ctx, const AlterTSConfigurationStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterTSConfigurationStmt");

  if (node->cfgname != NULL && node->cfgname->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->cfgname, node, "cfgname", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "cfgname");
  }
  if (node->dicts != NULL && node->dicts->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->dicts, node, "dicts", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "dicts");
  }
  if (node->kind != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->kind);
    _fingerprintString(ctx, "kind");
    _fingerprintString(ctx, buffer);
  }

  if (node->missing_ok) {
    _fingerprintString(ctx, "missing_ok");
    _fingerprintString(ctx, "true");
  }

  if (node->override) {
    _fingerprintString(ctx, "override");
    _fingerprintString(ctx, "true");
  }

  if (node->replace) {
    _fingerprintString(ctx, "replace");
    _fingerprintString(ctx, "true");
  }

  if (node->tokentype != NULL && node->tokentype->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->tokentype, node, "tokentype", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "tokentype");
  }
}

static void
_fingerprintCreateFdwStmt(FingerprintContext *ctx, const CreateFdwStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateFdwStmt");

  if (node->fdwname != NULL) {
    _fingerprintString(ctx, "fdwname");
    _fingerprintString(ctx, node->fdwname);
  }

  if (node->func_options != NULL && node->func_options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->func_options, node, "func_options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "func_options");
  }
  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
}

static void
_fingerprintAlterFdwStmt(FingerprintContext *ctx, const AlterFdwStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterFdwStmt");

  if (node->fdwname != NULL) {
    _fingerprintString(ctx, "fdwname");
    _fingerprintString(ctx, node->fdwname);
  }

  if (node->func_options != NULL && node->func_options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->func_options, node, "func_options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "func_options");
  }
  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
}

static void
_fingerprintCreateForeignServerStmt(FingerprintContext *ctx, const CreateForeignServerStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateForeignServerStmt");

  if (node->fdwname != NULL) {
    _fingerprintString(ctx, "fdwname");
    _fingerprintString(ctx, node->fdwname);
  }

  if (node->if_not_exists) {
    _fingerprintString(ctx, "if_not_exists");
    _fingerprintString(ctx, "true");
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->servername != NULL) {
    _fingerprintString(ctx, "servername");
    _fingerprintString(ctx, node->servername);
  }

  if (node->servertype != NULL) {
    _fingerprintString(ctx, "servertype");
    _fingerprintString(ctx, node->servertype);
  }

  if (node->version != NULL) {
    _fingerprintString(ctx, "version");
    _fingerprintString(ctx, node->version);
  }

}

static void
_fingerprintAlterForeignServerStmt(FingerprintContext *ctx, const AlterForeignServerStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterForeignServerStmt");

  if (node->has_version) {
    _fingerprintString(ctx, "has_version");
    _fingerprintString(ctx, "true");
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->servername != NULL) {
    _fingerprintString(ctx, "servername");
    _fingerprintString(ctx, node->servername);
  }

  if (node->version != NULL) {
    _fingerprintString(ctx, "version");
    _fingerprintString(ctx, node->version);
  }

}

static void
_fingerprintCreateUserMappingStmt(FingerprintContext *ctx, const CreateUserMappingStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateUserMappingStmt");

  if (node->if_not_exists) {
    _fingerprintString(ctx, "if_not_exists");
    _fingerprintString(ctx, "true");
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->servername != NULL) {
    _fingerprintString(ctx, "servername");
    _fingerprintString(ctx, node->servername);
  }

  if (node->user != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->user, node, "user", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "user");
  }

}

static void
_fingerprintAlterUserMappingStmt(FingerprintContext *ctx, const AlterUserMappingStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterUserMappingStmt");

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->servername != NULL) {
    _fingerprintString(ctx, "servername");
    _fingerprintString(ctx, node->servername);
  }

  if (node->user != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->user, node, "user", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "user");
  }

}

static void
_fingerprintDropUserMappingStmt(FingerprintContext *ctx, const DropUserMappingStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "DropUserMappingStmt");

  if (node->missing_ok) {
    _fingerprintString(ctx, "missing_ok");
    _fingerprintString(ctx, "true");
  }

  if (node->servername != NULL) {
    _fingerprintString(ctx, "servername");
    _fingerprintString(ctx, node->servername);
  }

  if (node->user != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->user, node, "user", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "user");
  }

}

static void
_fingerprintAlterTableSpaceOptionsStmt(FingerprintContext *ctx, const AlterTableSpaceOptionsStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterTableSpaceOptionsStmt");

  if (node->isReset) {
    _fingerprintString(ctx, "isReset");
    _fingerprintString(ctx, "true");
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->tablespacename != NULL) {
    _fingerprintString(ctx, "tablespacename");
    _fingerprintString(ctx, node->tablespacename);
  }

}

static void
_fingerprintAlterTableMoveAllStmt(FingerprintContext *ctx, const AlterTableMoveAllStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterTableMoveAllStmt");

  if (node->new_tablespacename != NULL) {
    _fingerprintString(ctx, "new_tablespacename");
    _fingerprintString(ctx, node->new_tablespacename);
  }

  if (node->nowait) {
    _fingerprintString(ctx, "nowait");
    _fingerprintString(ctx, "true");
  }

  if (node->objtype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->objtype);
    _fingerprintString(ctx, "objtype");
    _fingerprintString(ctx, buffer);
  }

  if (node->orig_tablespacename != NULL) {
    _fingerprintString(ctx, "orig_tablespacename");
    _fingerprintString(ctx, node->orig_tablespacename);
  }

  if (node->roles != NULL && node->roles->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->roles, node, "roles", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "roles");
  }
}

static void
_fingerprintSecLabelStmt(FingerprintContext *ctx, const SecLabelStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "SecLabelStmt");

  if (node->label != NULL) {
    _fingerprintString(ctx, "label");
    _fingerprintString(ctx, node->label);
  }

  if (node->object != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->object, node, "object", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "object");
  }

  if (node->objtype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->objtype);
    _fingerprintString(ctx, "objtype");
    _fingerprintString(ctx, buffer);
  }

  if (node->provider != NULL) {
    _fingerprintString(ctx, "provider");
    _fingerprintString(ctx, node->provider);
  }

}

static void
_fingerprintCreateForeignTableStmt(FingerprintContext *ctx, const CreateForeignTableStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateForeignTableStmt");

  _fingerprintString(ctx, "base");
  _fingerprintCreateStmt(ctx, (const CreateStmt*) &node->base, node, "base", depth);
  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->servername != NULL) {
    _fingerprintString(ctx, "servername");
    _fingerprintString(ctx, node->servername);
  }

}

static void
_fingerprintImportForeignSchemaStmt(FingerprintContext *ctx, const ImportForeignSchemaStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "ImportForeignSchemaStmt");

  if (node->list_type != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->list_type);
    _fingerprintString(ctx, "list_type");
    _fingerprintString(ctx, buffer);
  }

  if (node->local_schema != NULL) {
    _fingerprintString(ctx, "local_schema");
    _fingerprintString(ctx, node->local_schema);
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->remote_schema != NULL) {
    _fingerprintString(ctx, "remote_schema");
    _fingerprintString(ctx, node->remote_schema);
  }

  if (node->server_name != NULL) {
    _fingerprintString(ctx, "server_name");
    _fingerprintString(ctx, node->server_name);
  }

  if (node->table_list != NULL && node->table_list->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->table_list, node, "table_list", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "table_list");
  }
}

static void
_fingerprintCreateExtensionStmt(FingerprintContext *ctx, const CreateExtensionStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateExtensionStmt");

  if (node->extname != NULL) {
    _fingerprintString(ctx, "extname");
    _fingerprintString(ctx, node->extname);
  }

  if (node->if_not_exists) {
    _fingerprintString(ctx, "if_not_exists");
    _fingerprintString(ctx, "true");
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
}

static void
_fingerprintAlterExtensionStmt(FingerprintContext *ctx, const AlterExtensionStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterExtensionStmt");

  if (node->extname != NULL) {
    _fingerprintString(ctx, "extname");
    _fingerprintString(ctx, node->extname);
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
}

static void
_fingerprintAlterExtensionContentsStmt(FingerprintContext *ctx, const AlterExtensionContentsStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterExtensionContentsStmt");

  if (node->action != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->action);
    _fingerprintString(ctx, "action");
    _fingerprintString(ctx, buffer);
  }

  if (node->extname != NULL) {
    _fingerprintString(ctx, "extname");
    _fingerprintString(ctx, node->extname);
  }

  if (node->object != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->object, node, "object", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "object");
  }

  if (node->objtype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->objtype);
    _fingerprintString(ctx, "objtype");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintCreateEventTrigStmt(FingerprintContext *ctx, const CreateEventTrigStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateEventTrigStmt");

  if (node->eventname != NULL) {
    _fingerprintString(ctx, "eventname");
    _fingerprintString(ctx, node->eventname);
  }

  if (node->funcname != NULL && node->funcname->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->funcname, node, "funcname", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "funcname");
  }
  if (node->trigname != NULL) {
    _fingerprintString(ctx, "trigname");
    _fingerprintString(ctx, node->trigname);
  }

  if (node->whenclause != NULL && node->whenclause->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->whenclause, node, "whenclause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "whenclause");
  }
}

static void
_fingerprintAlterEventTrigStmt(FingerprintContext *ctx, const AlterEventTrigStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterEventTrigStmt");

  if (node->tgenabled != 0) {
    char buffer[2] = {node->tgenabled, '\0'};
    _fingerprintString(ctx, "tgenabled");
    _fingerprintString(ctx, buffer);
  }

  if (node->trigname != NULL) {
    _fingerprintString(ctx, "trigname");
    _fingerprintString(ctx, node->trigname);
  }

}

static void
_fingerprintRefreshMatViewStmt(FingerprintContext *ctx, const RefreshMatViewStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "RefreshMatViewStmt");

  if (node->concurrent) {
    _fingerprintString(ctx, "concurrent");
    _fingerprintString(ctx, "true");
  }

  if (node->relation != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relation, node, "relation", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relation");
  }

  if (node->skipData) {
    _fingerprintString(ctx, "skipData");
    _fingerprintString(ctx, "true");
  }

}

static void
_fingerprintReplicaIdentityStmt(FingerprintContext *ctx, const ReplicaIdentityStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "ReplicaIdentityStmt");

  if (node->identity_type != 0) {
    char buffer[2] = {node->identity_type, '\0'};
    _fingerprintString(ctx, "identity_type");
    _fingerprintString(ctx, buffer);
  }

  if (node->name != NULL) {
    _fingerprintString(ctx, "name");
    _fingerprintString(ctx, node->name);
  }

}

static void
_fingerprintAlterSystemStmt(FingerprintContext *ctx, const AlterSystemStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterSystemStmt");

  if (node->setstmt != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->setstmt, node, "setstmt", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "setstmt");
  }

}

static void
_fingerprintCreatePolicyStmt(FingerprintContext *ctx, const CreatePolicyStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreatePolicyStmt");

  if (node->cmd_name != NULL) {
    _fingerprintString(ctx, "cmd_name");
    _fingerprintString(ctx, node->cmd_name);
  }

  if (node->permissive) {
    _fingerprintString(ctx, "permissive");
    _fingerprintString(ctx, "true");
  }

  if (node->policy_name != NULL) {
    _fingerprintString(ctx, "policy_name");
    _fingerprintString(ctx, node->policy_name);
  }

  if (node->qual != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->qual, node, "qual", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "qual");
  }

  if (node->roles != NULL && node->roles->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->roles, node, "roles", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "roles");
  }
  if (node->table != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->table, node, "table", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "table");
  }

  if (node->with_check != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->with_check, node, "with_check", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "with_check");
  }

}

static void
_fingerprintAlterPolicyStmt(FingerprintContext *ctx, const AlterPolicyStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterPolicyStmt");

  if (node->policy_name != NULL) {
    _fingerprintString(ctx, "policy_name");
    _fingerprintString(ctx, node->policy_name);
  }

  if (node->qual != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->qual, node, "qual", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "qual");
  }

  if (node->roles != NULL && node->roles->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->roles, node, "roles", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "roles");
  }
  if (node->table != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->table, node, "table", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "table");
  }

  if (node->with_check != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->with_check, node, "with_check", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "with_check");
  }

}

static void
_fingerprintCreateTransformStmt(FingerprintContext *ctx, const CreateTransformStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateTransformStmt");

  if (node->fromsql != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->fromsql, node, "fromsql", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "fromsql");
  }

  if (node->lang != NULL) {
    _fingerprintString(ctx, "lang");
    _fingerprintString(ctx, node->lang);
  }

  if (node->replace) {
    _fingerprintString(ctx, "replace");
    _fingerprintString(ctx, "true");
  }

  if (node->tosql != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->tosql, node, "tosql", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "tosql");
  }

  if (node->type_name != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->type_name, node, "type_name", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "type_name");
  }

}

static void
_fingerprintCreateAmStmt(FingerprintContext *ctx, const CreateAmStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateAmStmt");

  if (node->amname != NULL) {
    _fingerprintString(ctx, "amname");
    _fingerprintString(ctx, node->amname);
  }

  if (node->amtype != 0) {
    char buffer[2] = {node->amtype, '\0'};
    _fingerprintString(ctx, "amtype");
    _fingerprintString(ctx, buffer);
  }

  if (node->handler_name != NULL && node->handler_name->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->handler_name, node, "handler_name", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "handler_name");
  }
}

static void
_fingerprintCreatePublicationStmt(FingerprintContext *ctx, const CreatePublicationStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreatePublicationStmt");

  if (node->for_all_tables) {
    _fingerprintString(ctx, "for_all_tables");
    _fingerprintString(ctx, "true");
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->pubname != NULL) {
    _fingerprintString(ctx, "pubname");
    _fingerprintString(ctx, node->pubname);
  }

  if (node->tables != NULL && node->tables->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->tables, node, "tables", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "tables");
  }
}

static void
_fingerprintAlterPublicationStmt(FingerprintContext *ctx, const AlterPublicationStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterPublicationStmt");

  if (node->for_all_tables) {
    _fingerprintString(ctx, "for_all_tables");
    _fingerprintString(ctx, "true");
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->pubname != NULL) {
    _fingerprintString(ctx, "pubname");
    _fingerprintString(ctx, node->pubname);
  }

  if (node->tableAction != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->tableAction);
    _fingerprintString(ctx, "tableAction");
    _fingerprintString(ctx, buffer);
  }

  if (node->tables != NULL && node->tables->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->tables, node, "tables", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "tables");
  }
}

static void
_fingerprintCreateSubscriptionStmt(FingerprintContext *ctx, const CreateSubscriptionStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateSubscriptionStmt");

  if (node->conninfo != NULL) {
    _fingerprintString(ctx, "conninfo");
    _fingerprintString(ctx, node->conninfo);
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->publication != NULL && node->publication->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->publication, node, "publication", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "publication");
  }
  if (node->subname != NULL) {
    _fingerprintString(ctx, "subname");
    _fingerprintString(ctx, node->subname);
  }

}

static void
_fingerprintAlterSubscriptionStmt(FingerprintContext *ctx, const AlterSubscriptionStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterSubscriptionStmt");

  if (node->conninfo != NULL) {
    _fingerprintString(ctx, "conninfo");
    _fingerprintString(ctx, node->conninfo);
  }

  if (node->kind != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->kind);
    _fingerprintString(ctx, "kind");
    _fingerprintString(ctx, buffer);
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->publication != NULL && node->publication->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->publication, node, "publication", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "publication");
  }
  if (node->subname != NULL) {
    _fingerprintString(ctx, "subname");
    _fingerprintString(ctx, node->subname);
  }

}

static void
_fingerprintDropSubscriptionStmt(FingerprintContext *ctx, const DropSubscriptionStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "DropSubscriptionStmt");

  if (node->behavior != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->behavior);
    _fingerprintString(ctx, "behavior");
    _fingerprintString(ctx, buffer);
  }

  if (node->missing_ok) {
    _fingerprintString(ctx, "missing_ok");
    _fingerprintString(ctx, "true");
  }

  if (node->subname != NULL) {
    _fingerprintString(ctx, "subname");
    _fingerprintString(ctx, node->subname);
  }

}

static void
_fingerprintCreateStatsStmt(FingerprintContext *ctx, const CreateStatsStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateStatsStmt");

  if (node->defnames != NULL && node->defnames->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->defnames, node, "defnames", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "defnames");
  }
  if (node->exprs != NULL && node->exprs->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->exprs, node, "exprs", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "exprs");
  }
  if (node->if_not_exists) {
    _fingerprintString(ctx, "if_not_exists");
    _fingerprintString(ctx, "true");
  }

  if (node->relations != NULL && node->relations->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relations, node, "relations", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relations");
  }
  if (node->stat_types != NULL && node->stat_types->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->stat_types, node, "stat_types", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "stat_types");
  }
}

static void
_fingerprintAlterCollationStmt(FingerprintContext *ctx, const AlterCollationStmt *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AlterCollationStmt");

  if (node->collname != NULL && node->collname->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->collname, node, "collname", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "collname");
  }
}

static void
_fingerprintA_Expr(FingerprintContext *ctx, const A_Expr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "A_Expr");

  if (node->kind != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->kind);
    _fingerprintString(ctx, "kind");
    _fingerprintString(ctx, buffer);
  }

  if (node->lexpr != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->lexpr, node, "lexpr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "lexpr");
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->name != NULL && node->name->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->name, node, "name", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "name");
  }
  if (node->rexpr != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->rexpr, node, "rexpr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "rexpr");
  }

}

static void
_fingerprintColumnRef(FingerprintContext *ctx, const ColumnRef *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "ColumnRef");

  if (node->fields != NULL && node->fields->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->fields, node, "fields", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "fields");
  }
  // Intentionally ignoring node->location for fingerprinting

}

static void
_fingerprintParamRef(FingerprintContext *ctx, const ParamRef *node, const void *parent, const char *field_name, unsigned int depth)
{
  // Intentionally ignoring all fields for fingerprinting
}

static void
_fingerprintA_Const(FingerprintContext *ctx, const A_Const *node, const void *parent, const char *field_name, unsigned int depth)
{
  // Intentionally ignoring all fields for fingerprinting
}

static void
_fingerprintFuncCall(FingerprintContext *ctx, const FuncCall *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "FuncCall");

  if (node->agg_distinct) {
    _fingerprintString(ctx, "agg_distinct");
    _fingerprintString(ctx, "true");
  }

  if (node->agg_filter != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->agg_filter, node, "agg_filter", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "agg_filter");
  }

  if (node->agg_order != NULL && node->agg_order->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->agg_order, node, "agg_order", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "agg_order");
  }
  if (node->agg_star) {
    _fingerprintString(ctx, "agg_star");
    _fingerprintString(ctx, "true");
  }

  if (node->agg_within_group) {
    _fingerprintString(ctx, "agg_within_group");
    _fingerprintString(ctx, "true");
  }

  if (node->args != NULL && node->args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->args, node, "args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "args");
  }
  if (node->func_variadic) {
    _fingerprintString(ctx, "func_variadic");
    _fingerprintString(ctx, "true");
  }

  if (node->funcname != NULL && node->funcname->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->funcname, node, "funcname", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "funcname");
  }
  // Intentionally ignoring node->location for fingerprinting

  if (node->over != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->over, node, "over", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "over");
  }

}

static void
_fingerprintA_Star(FingerprintContext *ctx, const A_Star *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "A_Star");

}

static void
_fingerprintA_Indices(FingerprintContext *ctx, const A_Indices *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "A_Indices");

  if (node->is_slice) {
    _fingerprintString(ctx, "is_slice");
    _fingerprintString(ctx, "true");
  }

  if (node->lidx != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->lidx, node, "lidx", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "lidx");
  }

  if (node->uidx != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->uidx, node, "uidx", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "uidx");
  }

}

static void
_fingerprintA_Indirection(FingerprintContext *ctx, const A_Indirection *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "A_Indirection");

  if (node->arg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->arg, node, "arg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "arg");
  }

  if (node->indirection != NULL && node->indirection->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->indirection, node, "indirection", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "indirection");
  }
}

static void
_fingerprintA_ArrayExpr(FingerprintContext *ctx, const A_ArrayExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "A_ArrayExpr");

  if (node->elements != NULL && node->elements->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->elements, node, "elements", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "elements");
  }
  // Intentionally ignoring node->location for fingerprinting

}

static void
_fingerprintResTarget(FingerprintContext *ctx, const ResTarget *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "ResTarget");

  if (node->indirection != NULL && node->indirection->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->indirection, node, "indirection", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "indirection");
  }
  // Intentionally ignoring node->location for fingerprinting

  if (node->name != NULL && (field_name == NULL || parent == NULL || !IsA(parent, SelectStmt) || strcmp(field_name, "targetList") != 0)) {
    _fingerprintString(ctx, "name");
    _fingerprintString(ctx, node->name);
  }

  if (node->val != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->val, node, "val", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "val");
  }

}

static void
_fingerprintMultiAssignRef(FingerprintContext *ctx, const MultiAssignRef *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "MultiAssignRef");

  if (node->colno != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->colno);
    _fingerprintString(ctx, "colno");
    _fingerprintString(ctx, buffer);
  }

  if (node->ncolumns != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->ncolumns);
    _fingerprintString(ctx, "ncolumns");
    _fingerprintString(ctx, buffer);
  }

  if (node->source != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->source, node, "source", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "source");
  }

}

static void
_fingerprintTypeCast(FingerprintContext *ctx, const TypeCast *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "TypeCast");

  if (node->arg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->arg, node, "arg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "arg");
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->typeName != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->typeName, node, "typeName", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "typeName");
  }

}

static void
_fingerprintCollateClause(FingerprintContext *ctx, const CollateClause *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CollateClause");

  if (node->arg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->arg, node, "arg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "arg");
  }

  if (node->collname != NULL && node->collname->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->collname, node, "collname", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "collname");
  }
  // Intentionally ignoring node->location for fingerprinting

}

static void
_fingerprintSortBy(FingerprintContext *ctx, const SortBy *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "SortBy");

  // Intentionally ignoring node->location for fingerprinting

  if (node->node != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->node, node, "node", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "node");
  }

  if (node->sortby_dir != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->sortby_dir);
    _fingerprintString(ctx, "sortby_dir");
    _fingerprintString(ctx, buffer);
  }

  if (node->sortby_nulls != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->sortby_nulls);
    _fingerprintString(ctx, "sortby_nulls");
    _fingerprintString(ctx, buffer);
  }

  if (node->useOp != NULL && node->useOp->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->useOp, node, "useOp", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "useOp");
  }
}

static void
_fingerprintWindowDef(FingerprintContext *ctx, const WindowDef *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "WindowDef");

  if (node->endOffset != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->endOffset, node, "endOffset", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "endOffset");
  }

  if (node->frameOptions != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->frameOptions);
    _fingerprintString(ctx, "frameOptions");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->name != NULL) {
    _fingerprintString(ctx, "name");
    _fingerprintString(ctx, node->name);
  }

  if (node->orderClause != NULL && node->orderClause->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->orderClause, node, "orderClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "orderClause");
  }
  if (node->partitionClause != NULL && node->partitionClause->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->partitionClause, node, "partitionClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "partitionClause");
  }
  if (node->refname != NULL) {
    _fingerprintString(ctx, "refname");
    _fingerprintString(ctx, node->refname);
  }

  if (node->startOffset != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->startOffset, node, "startOffset", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "startOffset");
  }

}

static void
_fingerprintRangeSubselect(FingerprintContext *ctx, const RangeSubselect *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "RangeSubselect");

  if (node->alias != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->alias, node, "alias", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "alias");
  }

  if (node->lateral) {
    _fingerprintString(ctx, "lateral");
    _fingerprintString(ctx, "true");
  }

  if (node->subquery != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->subquery, node, "subquery", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "subquery");
  }

}

static void
_fingerprintRangeFunction(FingerprintContext *ctx, const RangeFunction *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "RangeFunction");

  if (node->alias != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->alias, node, "alias", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "alias");
  }

  if (node->coldeflist != NULL && node->coldeflist->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->coldeflist, node, "coldeflist", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "coldeflist");
  }
  if (node->functions != NULL && node->functions->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->functions, node, "functions", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "functions");
  }
  if (node->is_rowsfrom) {
    _fingerprintString(ctx, "is_rowsfrom");
    _fingerprintString(ctx, "true");
  }

  if (node->lateral) {
    _fingerprintString(ctx, "lateral");
    _fingerprintString(ctx, "true");
  }

  if (node->ordinality) {
    _fingerprintString(ctx, "ordinality");
    _fingerprintString(ctx, "true");
  }

}

static void
_fingerprintRangeTableSample(FingerprintContext *ctx, const RangeTableSample *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "RangeTableSample");

  if (node->args != NULL && node->args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->args, node, "args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "args");
  }
  // Intentionally ignoring node->location for fingerprinting

  if (node->method != NULL && node->method->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->method, node, "method", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "method");
  }
  if (node->relation != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relation, node, "relation", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relation");
  }

  if (node->repeatable != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->repeatable, node, "repeatable", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "repeatable");
  }

}

static void
_fingerprintRangeTableFunc(FingerprintContext *ctx, const RangeTableFunc *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "RangeTableFunc");

  if (node->alias != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->alias, node, "alias", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "alias");
  }

  if (node->columns != NULL && node->columns->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->columns, node, "columns", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "columns");
  }
  if (node->docexpr != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->docexpr, node, "docexpr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "docexpr");
  }

  if (node->lateral) {
    _fingerprintString(ctx, "lateral");
    _fingerprintString(ctx, "true");
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->namespaces != NULL && node->namespaces->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->namespaces, node, "namespaces", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "namespaces");
  }
  if (node->rowexpr != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->rowexpr, node, "rowexpr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "rowexpr");
  }

}

static void
_fingerprintRangeTableFuncCol(FingerprintContext *ctx, const RangeTableFuncCol *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "RangeTableFuncCol");

  if (node->coldefexpr != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->coldefexpr, node, "coldefexpr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "coldefexpr");
  }

  if (node->colexpr != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->colexpr, node, "colexpr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "colexpr");
  }

  if (node->colname != NULL) {
    _fingerprintString(ctx, "colname");
    _fingerprintString(ctx, node->colname);
  }

  if (node->for_ordinality) {
    _fingerprintString(ctx, "for_ordinality");
    _fingerprintString(ctx, "true");
  }

  if (node->is_not_null) {
    _fingerprintString(ctx, "is_not_null");
    _fingerprintString(ctx, "true");
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->typeName != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->typeName, node, "typeName", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "typeName");
  }

}

static void
_fingerprintTypeName(FingerprintContext *ctx, const TypeName *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "TypeName");

  if (node->arrayBounds != NULL && node->arrayBounds->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->arrayBounds, node, "arrayBounds", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "arrayBounds");
  }
  // Intentionally ignoring node->location for fingerprinting

  if (node->names != NULL && node->names->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->names, node, "names", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "names");
  }
  if (node->pct_type) {
    _fingerprintString(ctx, "pct_type");
    _fingerprintString(ctx, "true");
  }

  if (node->setof) {
    _fingerprintString(ctx, "setof");
    _fingerprintString(ctx, "true");
  }

  if (node->typeOid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->typeOid);
    _fingerprintString(ctx, "typeOid");
    _fingerprintString(ctx, buffer);
  }

  if (node->typemod != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->typemod);
    _fingerprintString(ctx, "typemod");
    _fingerprintString(ctx, buffer);
  }

  if (node->typmods != NULL && node->typmods->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->typmods, node, "typmods", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "typmods");
  }
}

static void
_fingerprintColumnDef(FingerprintContext *ctx, const ColumnDef *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "ColumnDef");

  if (node->collClause != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->collClause, node, "collClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "collClause");
  }

  if (node->collOid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->collOid);
    _fingerprintString(ctx, "collOid");
    _fingerprintString(ctx, buffer);
  }

  if (node->colname != NULL) {
    _fingerprintString(ctx, "colname");
    _fingerprintString(ctx, node->colname);
  }

  if (node->constraints != NULL && node->constraints->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->constraints, node, "constraints", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "constraints");
  }
  if (node->cooked_default != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->cooked_default, node, "cooked_default", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "cooked_default");
  }

  if (node->fdwoptions != NULL && node->fdwoptions->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->fdwoptions, node, "fdwoptions", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "fdwoptions");
  }
  if (node->identity != 0) {
    char buffer[2] = {node->identity, '\0'};
    _fingerprintString(ctx, "identity");
    _fingerprintString(ctx, buffer);
  }

  if (node->inhcount != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->inhcount);
    _fingerprintString(ctx, "inhcount");
    _fingerprintString(ctx, buffer);
  }

  if (node->is_from_parent) {
    _fingerprintString(ctx, "is_from_parent");
    _fingerprintString(ctx, "true");
  }

  if (node->is_from_type) {
    _fingerprintString(ctx, "is_from_type");
    _fingerprintString(ctx, "true");
  }

  if (node->is_local) {
    _fingerprintString(ctx, "is_local");
    _fingerprintString(ctx, "true");
  }

  if (node->is_not_null) {
    _fingerprintString(ctx, "is_not_null");
    _fingerprintString(ctx, "true");
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->raw_default != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->raw_default, node, "raw_default", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "raw_default");
  }

  if (node->storage != 0) {
    char buffer[2] = {node->storage, '\0'};
    _fingerprintString(ctx, "storage");
    _fingerprintString(ctx, buffer);
  }

  if (node->typeName != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->typeName, node, "typeName", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "typeName");
  }

}

static void
_fingerprintIndexElem(FingerprintContext *ctx, const IndexElem *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "IndexElem");

  if (node->collation != NULL && node->collation->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->collation, node, "collation", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "collation");
  }
  if (node->expr != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->expr, node, "expr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "expr");
  }

  if (node->indexcolname != NULL) {
    _fingerprintString(ctx, "indexcolname");
    _fingerprintString(ctx, node->indexcolname);
  }

  if (node->name != NULL) {
    _fingerprintString(ctx, "name");
    _fingerprintString(ctx, node->name);
  }

  if (node->nulls_ordering != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->nulls_ordering);
    _fingerprintString(ctx, "nulls_ordering");
    _fingerprintString(ctx, buffer);
  }

  if (node->opclass != NULL && node->opclass->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->opclass, node, "opclass", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "opclass");
  }
  if (node->ordering != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->ordering);
    _fingerprintString(ctx, "ordering");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintConstraint(FingerprintContext *ctx, const Constraint *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "Constraint");

  if (node->access_method != NULL) {
    _fingerprintString(ctx, "access_method");
    _fingerprintString(ctx, node->access_method);
  }

  if (node->conname != NULL) {
    _fingerprintString(ctx, "conname");
    _fingerprintString(ctx, node->conname);
  }

  if (node->contype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->contype);
    _fingerprintString(ctx, "contype");
    _fingerprintString(ctx, buffer);
  }

  if (node->cooked_expr != NULL) {
    _fingerprintString(ctx, "cooked_expr");
    _fingerprintString(ctx, node->cooked_expr);
  }

  if (node->deferrable) {
    _fingerprintString(ctx, "deferrable");
    _fingerprintString(ctx, "true");
  }

  if (node->exclusions != NULL && node->exclusions->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->exclusions, node, "exclusions", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "exclusions");
  }
  if (node->fk_attrs != NULL && node->fk_attrs->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->fk_attrs, node, "fk_attrs", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "fk_attrs");
  }
  if (node->fk_del_action != 0) {
    char buffer[2] = {node->fk_del_action, '\0'};
    _fingerprintString(ctx, "fk_del_action");
    _fingerprintString(ctx, buffer);
  }

  if (node->fk_matchtype != 0) {
    char buffer[2] = {node->fk_matchtype, '\0'};
    _fingerprintString(ctx, "fk_matchtype");
    _fingerprintString(ctx, buffer);
  }

  if (node->fk_upd_action != 0) {
    char buffer[2] = {node->fk_upd_action, '\0'};
    _fingerprintString(ctx, "fk_upd_action");
    _fingerprintString(ctx, buffer);
  }

  if (node->generated_when != 0) {
    char buffer[2] = {node->generated_when, '\0'};
    _fingerprintString(ctx, "generated_when");
    _fingerprintString(ctx, buffer);
  }

  if (node->indexname != NULL) {
    _fingerprintString(ctx, "indexname");
    _fingerprintString(ctx, node->indexname);
  }

  if (node->indexspace != NULL) {
    _fingerprintString(ctx, "indexspace");
    _fingerprintString(ctx, node->indexspace);
  }

  if (node->initdeferred) {
    _fingerprintString(ctx, "initdeferred");
    _fingerprintString(ctx, "true");
  }

  if (node->initially_valid) {
    _fingerprintString(ctx, "initially_valid");
    _fingerprintString(ctx, "true");
  }

  if (node->is_no_inherit) {
    _fingerprintString(ctx, "is_no_inherit");
    _fingerprintString(ctx, "true");
  }

  if (node->keys != NULL && node->keys->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->keys, node, "keys", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "keys");
  }
  // Intentionally ignoring node->location for fingerprinting

  if (node->old_conpfeqop != NULL && node->old_conpfeqop->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->old_conpfeqop, node, "old_conpfeqop", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "old_conpfeqop");
  }
  if (node->old_pktable_oid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->old_pktable_oid);
    _fingerprintString(ctx, "old_pktable_oid");
    _fingerprintString(ctx, buffer);
  }

  if (node->options != NULL && node->options->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->options, node, "options", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "options");
  }
  if (node->pk_attrs != NULL && node->pk_attrs->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->pk_attrs, node, "pk_attrs", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "pk_attrs");
  }
  if (node->pktable != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->pktable, node, "pktable", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "pktable");
  }

  if (node->raw_expr != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->raw_expr, node, "raw_expr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "raw_expr");
  }

  if (node->skip_validation) {
    _fingerprintString(ctx, "skip_validation");
    _fingerprintString(ctx, "true");
  }

  if (node->where_clause != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->where_clause, node, "where_clause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "where_clause");
  }

}

static void
_fingerprintDefElem(FingerprintContext *ctx, const DefElem *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "DefElem");

  if (node->arg != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->arg, node, "arg", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "arg");
  }

  if (node->defaction != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->defaction);
    _fingerprintString(ctx, "defaction");
    _fingerprintString(ctx, buffer);
  }

  if (node->defname != NULL) {
    _fingerprintString(ctx, "defname");
    _fingerprintString(ctx, node->defname);
  }

  if (node->defnamespace != NULL) {
    _fingerprintString(ctx, "defnamespace");
    _fingerprintString(ctx, node->defnamespace);
  }

  // Intentionally ignoring node->location for fingerprinting

}

static void
_fingerprintRangeTblEntry(FingerprintContext *ctx, const RangeTblEntry *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "RangeTblEntry");

  if (node->alias != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->alias, node, "alias", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "alias");
  }

  if (node->checkAsUser != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->checkAsUser);
    _fingerprintString(ctx, "checkAsUser");
    _fingerprintString(ctx, buffer);
  }

  if (node->colcollations != NULL && node->colcollations->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->colcollations, node, "colcollations", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "colcollations");
  }
  if (node->coltypes != NULL && node->coltypes->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->coltypes, node, "coltypes", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "coltypes");
  }
  if (node->coltypmods != NULL && node->coltypmods->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->coltypmods, node, "coltypmods", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "coltypmods");
  }
  if (node->ctelevelsup != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->ctelevelsup);
    _fingerprintString(ctx, "ctelevelsup");
    _fingerprintString(ctx, buffer);
  }

  if (node->ctename != NULL) {
    _fingerprintString(ctx, "ctename");
    _fingerprintString(ctx, node->ctename);
  }

  if (node->enrname != NULL) {
    _fingerprintString(ctx, "enrname");
    _fingerprintString(ctx, node->enrname);
  }

  if (node->enrtuples != 0) {
    char buffer[50];
    sprintf(buffer, "%f", node->enrtuples);
    _fingerprintString(ctx, "enrtuples");
    _fingerprintString(ctx, buffer);
  }

  if (node->eref != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->eref, node, "eref", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "eref");
  }

  if (node->funcordinality) {
    _fingerprintString(ctx, "funcordinality");
    _fingerprintString(ctx, "true");
  }

  if (node->functions != NULL && node->functions->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->functions, node, "functions", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "functions");
  }
  if (node->inFromCl) {
    _fingerprintString(ctx, "inFromCl");
    _fingerprintString(ctx, "true");
  }

  if (node->inh) {
    _fingerprintString(ctx, "inh");
    _fingerprintString(ctx, "true");
  }

  if (true) {
    int x;
    Bitmapset	*bms = bms_copy(node->insertedCols);

    _fingerprintString(ctx, "insertedCols");

  	while ((x = bms_first_member(bms)) >= 0) {
      char buffer[50];
      sprintf(buffer, "%d", x);
      _fingerprintString(ctx, buffer);
    }

    bms_free(bms);
  }

  if (node->joinaliasvars != NULL && node->joinaliasvars->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->joinaliasvars, node, "joinaliasvars", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "joinaliasvars");
  }
  if (node->jointype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->jointype);
    _fingerprintString(ctx, "jointype");
    _fingerprintString(ctx, buffer);
  }

  if (node->lateral) {
    _fingerprintString(ctx, "lateral");
    _fingerprintString(ctx, "true");
  }

  if (node->relid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->relid);
    _fingerprintString(ctx, "relid");
    _fingerprintString(ctx, buffer);
  }

  if (node->relkind != 0) {
    char buffer[2] = {node->relkind, '\0'};
    _fingerprintString(ctx, "relkind");
    _fingerprintString(ctx, buffer);
  }

  if (node->requiredPerms != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->requiredPerms);
    _fingerprintString(ctx, "requiredPerms");
    _fingerprintString(ctx, buffer);
  }

  if (node->rtekind != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->rtekind);
    _fingerprintString(ctx, "rtekind");
    _fingerprintString(ctx, buffer);
  }

  if (node->securityQuals != NULL && node->securityQuals->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->securityQuals, node, "securityQuals", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "securityQuals");
  }
  if (node->security_barrier) {
    _fingerprintString(ctx, "security_barrier");
    _fingerprintString(ctx, "true");
  }

  if (true) {
    int x;
    Bitmapset	*bms = bms_copy(node->selectedCols);

    _fingerprintString(ctx, "selectedCols");

  	while ((x = bms_first_member(bms)) >= 0) {
      char buffer[50];
      sprintf(buffer, "%d", x);
      _fingerprintString(ctx, buffer);
    }

    bms_free(bms);
  }

  if (node->self_reference) {
    _fingerprintString(ctx, "self_reference");
    _fingerprintString(ctx, "true");
  }

  if (node->subquery != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->subquery, node, "subquery", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "subquery");
  }

  if (node->tablefunc != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->tablefunc, node, "tablefunc", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "tablefunc");
  }

  if (node->tablesample != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->tablesample, node, "tablesample", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "tablesample");
  }

  if (true) {
    int x;
    Bitmapset	*bms = bms_copy(node->updatedCols);

    _fingerprintString(ctx, "updatedCols");

  	while ((x = bms_first_member(bms)) >= 0) {
      char buffer[50];
      sprintf(buffer, "%d", x);
      _fingerprintString(ctx, buffer);
    }

    bms_free(bms);
  }

  if (node->values_lists != NULL && node->values_lists->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->values_lists, node, "values_lists", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "values_lists");
  }
}

static void
_fingerprintRangeTblFunction(FingerprintContext *ctx, const RangeTblFunction *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "RangeTblFunction");

  if (node->funccolcollations != NULL && node->funccolcollations->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->funccolcollations, node, "funccolcollations", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "funccolcollations");
  }
  if (node->funccolcount != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->funccolcount);
    _fingerprintString(ctx, "funccolcount");
    _fingerprintString(ctx, buffer);
  }

  if (node->funccolnames != NULL && node->funccolnames->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->funccolnames, node, "funccolnames", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "funccolnames");
  }
  if (node->funccoltypes != NULL && node->funccoltypes->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->funccoltypes, node, "funccoltypes", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "funccoltypes");
  }
  if (node->funccoltypmods != NULL && node->funccoltypmods->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->funccoltypmods, node, "funccoltypmods", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "funccoltypmods");
  }
  if (node->funcexpr != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->funcexpr, node, "funcexpr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "funcexpr");
  }

  if (true) {
    int x;
    Bitmapset	*bms = bms_copy(node->funcparams);

    _fingerprintString(ctx, "funcparams");

  	while ((x = bms_first_member(bms)) >= 0) {
      char buffer[50];
      sprintf(buffer, "%d", x);
      _fingerprintString(ctx, buffer);
    }

    bms_free(bms);
  }

}

static void
_fingerprintTableSampleClause(FingerprintContext *ctx, const TableSampleClause *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "TableSampleClause");

  if (node->args != NULL && node->args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->args, node, "args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "args");
  }
  if (node->repeatable != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->repeatable, node, "repeatable", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "repeatable");
  }

  if (node->tsmhandler != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->tsmhandler);
    _fingerprintString(ctx, "tsmhandler");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintWithCheckOption(FingerprintContext *ctx, const WithCheckOption *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "WithCheckOption");

  if (node->cascaded) {
    _fingerprintString(ctx, "cascaded");
    _fingerprintString(ctx, "true");
  }

  if (node->kind != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->kind);
    _fingerprintString(ctx, "kind");
    _fingerprintString(ctx, buffer);
  }

  if (node->polname != NULL) {
    _fingerprintString(ctx, "polname");
    _fingerprintString(ctx, node->polname);
  }

  if (node->qual != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->qual, node, "qual", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "qual");
  }

  if (node->relname != NULL) {
    _fingerprintString(ctx, "relname");
    _fingerprintString(ctx, node->relname);
  }

}

static void
_fingerprintSortGroupClause(FingerprintContext *ctx, const SortGroupClause *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "SortGroupClause");

  if (node->eqop != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->eqop);
    _fingerprintString(ctx, "eqop");
    _fingerprintString(ctx, buffer);
  }

  if (node->hashable) {
    _fingerprintString(ctx, "hashable");
    _fingerprintString(ctx, "true");
  }

  if (node->nulls_first) {
    _fingerprintString(ctx, "nulls_first");
    _fingerprintString(ctx, "true");
  }

  if (node->sortop != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->sortop);
    _fingerprintString(ctx, "sortop");
    _fingerprintString(ctx, buffer);
  }

  if (node->tleSortGroupRef != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->tleSortGroupRef);
    _fingerprintString(ctx, "tleSortGroupRef");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintGroupingSet(FingerprintContext *ctx, const GroupingSet *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "GroupingSet");

  if (node->content != NULL && node->content->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->content, node, "content", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "content");
  }
  if (node->kind != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->kind);
    _fingerprintString(ctx, "kind");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->location for fingerprinting

}

static void
_fingerprintWindowClause(FingerprintContext *ctx, const WindowClause *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "WindowClause");

  if (node->copiedOrder) {
    _fingerprintString(ctx, "copiedOrder");
    _fingerprintString(ctx, "true");
  }

  if (node->endOffset != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->endOffset, node, "endOffset", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "endOffset");
  }

  if (node->frameOptions != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->frameOptions);
    _fingerprintString(ctx, "frameOptions");
    _fingerprintString(ctx, buffer);
  }

  if (node->name != NULL) {
    _fingerprintString(ctx, "name");
    _fingerprintString(ctx, node->name);
  }

  if (node->orderClause != NULL && node->orderClause->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->orderClause, node, "orderClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "orderClause");
  }
  if (node->partitionClause != NULL && node->partitionClause->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->partitionClause, node, "partitionClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "partitionClause");
  }
  if (node->refname != NULL) {
    _fingerprintString(ctx, "refname");
    _fingerprintString(ctx, node->refname);
  }

  if (node->startOffset != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->startOffset, node, "startOffset", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "startOffset");
  }

  if (node->winref != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->winref);
    _fingerprintString(ctx, "winref");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintObjectWithArgs(FingerprintContext *ctx, const ObjectWithArgs *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "ObjectWithArgs");

  if (node->args_unspecified) {
    _fingerprintString(ctx, "args_unspecified");
    _fingerprintString(ctx, "true");
  }

  if (node->objargs != NULL && node->objargs->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->objargs, node, "objargs", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "objargs");
  }
  if (node->objname != NULL && node->objname->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->objname, node, "objname", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "objname");
  }
}

static void
_fingerprintAccessPriv(FingerprintContext *ctx, const AccessPriv *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "AccessPriv");

  if (node->cols != NULL && node->cols->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->cols, node, "cols", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "cols");
  }
  if (node->priv_name != NULL) {
    _fingerprintString(ctx, "priv_name");
    _fingerprintString(ctx, node->priv_name);
  }

}

static void
_fingerprintCreateOpClassItem(FingerprintContext *ctx, const CreateOpClassItem *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CreateOpClassItem");

  if (node->class_args != NULL && node->class_args->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->class_args, node, "class_args", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "class_args");
  }
  if (node->itemtype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->itemtype);
    _fingerprintString(ctx, "itemtype");
    _fingerprintString(ctx, buffer);
  }

  if (node->name != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->name, node, "name", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "name");
  }

  if (node->number != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->number);
    _fingerprintString(ctx, "number");
    _fingerprintString(ctx, buffer);
  }

  if (node->order_family != NULL && node->order_family->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->order_family, node, "order_family", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "order_family");
  }
  if (node->storedtype != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->storedtype, node, "storedtype", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "storedtype");
  }

}

static void
_fingerprintTableLikeClause(FingerprintContext *ctx, const TableLikeClause *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "TableLikeClause");

  if (node->options != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->options);
    _fingerprintString(ctx, "options");
    _fingerprintString(ctx, buffer);
  }

  if (node->relation != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->relation, node, "relation", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "relation");
  }

}

static void
_fingerprintFunctionParameter(FingerprintContext *ctx, const FunctionParameter *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "FunctionParameter");

  if (node->argType != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->argType, node, "argType", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "argType");
  }

  if (node->defexpr != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->defexpr, node, "defexpr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "defexpr");
  }

  if (node->mode != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->mode);
    _fingerprintString(ctx, "mode");
    _fingerprintString(ctx, buffer);
  }

  if (node->name != NULL) {
    _fingerprintString(ctx, "name");
    _fingerprintString(ctx, node->name);
  }

}

static void
_fingerprintLockingClause(FingerprintContext *ctx, const LockingClause *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "LockingClause");

  if (node->lockedRels != NULL && node->lockedRels->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->lockedRels, node, "lockedRels", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "lockedRels");
  }
  if (node->strength != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->strength);
    _fingerprintString(ctx, "strength");
    _fingerprintString(ctx, buffer);
  }

  if (node->waitPolicy != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->waitPolicy);
    _fingerprintString(ctx, "waitPolicy");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintRowMarkClause(FingerprintContext *ctx, const RowMarkClause *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "RowMarkClause");

  if (node->pushedDown) {
    _fingerprintString(ctx, "pushedDown");
    _fingerprintString(ctx, "true");
  }

  if (node->rti != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->rti);
    _fingerprintString(ctx, "rti");
    _fingerprintString(ctx, buffer);
  }

  if (node->strength != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->strength);
    _fingerprintString(ctx, "strength");
    _fingerprintString(ctx, buffer);
  }

  if (node->waitPolicy != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->waitPolicy);
    _fingerprintString(ctx, "waitPolicy");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintXmlSerialize(FingerprintContext *ctx, const XmlSerialize *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "XmlSerialize");

  if (node->expr != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->expr, node, "expr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "expr");
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->typeName != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->typeName, node, "typeName", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "typeName");
  }

  if (node->xmloption != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->xmloption);
    _fingerprintString(ctx, "xmloption");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintWithClause(FingerprintContext *ctx, const WithClause *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "WithClause");

  if (node->ctes != NULL && node->ctes->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->ctes, node, "ctes", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "ctes");
  }
  // Intentionally ignoring node->location for fingerprinting

  if (node->recursive) {
    _fingerprintString(ctx, "recursive");
    _fingerprintString(ctx, "true");
  }

}

static void
_fingerprintInferClause(FingerprintContext *ctx, const InferClause *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "InferClause");

  if (node->conname != NULL) {
    _fingerprintString(ctx, "conname");
    _fingerprintString(ctx, node->conname);
  }

  if (node->indexElems != NULL && node->indexElems->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->indexElems, node, "indexElems", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "indexElems");
  }
  // Intentionally ignoring node->location for fingerprinting

  if (node->whereClause != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->whereClause, node, "whereClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "whereClause");
  }

}

static void
_fingerprintOnConflictClause(FingerprintContext *ctx, const OnConflictClause *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "OnConflictClause");

  if (node->action != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->action);
    _fingerprintString(ctx, "action");
    _fingerprintString(ctx, buffer);
  }

  if (node->infer != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->infer, node, "infer", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "infer");
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->targetList != NULL && node->targetList->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->targetList, node, "targetList", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "targetList");
  }
  if (node->whereClause != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->whereClause, node, "whereClause", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "whereClause");
  }

}

static void
_fingerprintCommonTableExpr(FingerprintContext *ctx, const CommonTableExpr *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "CommonTableExpr");

  if (node->aliascolnames != NULL && node->aliascolnames->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->aliascolnames, node, "aliascolnames", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "aliascolnames");
  }
  if (node->ctecolcollations != NULL && node->ctecolcollations->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->ctecolcollations, node, "ctecolcollations", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "ctecolcollations");
  }
  if (node->ctecolnames != NULL && node->ctecolnames->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->ctecolnames, node, "ctecolnames", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "ctecolnames");
  }
  if (node->ctecoltypes != NULL && node->ctecoltypes->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->ctecoltypes, node, "ctecoltypes", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "ctecoltypes");
  }
  if (node->ctecoltypmods != NULL && node->ctecoltypmods->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->ctecoltypmods, node, "ctecoltypmods", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "ctecoltypmods");
  }
  if (node->ctename != NULL) {
    _fingerprintString(ctx, "ctename");
    _fingerprintString(ctx, node->ctename);
  }

  if (node->ctequery != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->ctequery, node, "ctequery", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "ctequery");
  }

  if (node->cterecursive) {
    _fingerprintString(ctx, "cterecursive");
    _fingerprintString(ctx, "true");
  }

  if (node->cterefcount != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->cterefcount);
    _fingerprintString(ctx, "cterefcount");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->location for fingerprinting

}

static void
_fingerprintRoleSpec(FingerprintContext *ctx, const RoleSpec *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "RoleSpec");

  // Intentionally ignoring node->location for fingerprinting

  if (node->rolename != NULL) {
    _fingerprintString(ctx, "rolename");
    _fingerprintString(ctx, node->rolename);
  }

  if (node->roletype != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->roletype);
    _fingerprintString(ctx, "roletype");
    _fingerprintString(ctx, buffer);
  }

}

static void
_fingerprintTriggerTransition(FingerprintContext *ctx, const TriggerTransition *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "TriggerTransition");

  if (node->isNew) {
    _fingerprintString(ctx, "isNew");
    _fingerprintString(ctx, "true");
  }

  if (node->isTable) {
    _fingerprintString(ctx, "isTable");
    _fingerprintString(ctx, "true");
  }

  if (node->name != NULL) {
    _fingerprintString(ctx, "name");
    _fingerprintString(ctx, node->name);
  }

}

static void
_fingerprintPartitionElem(FingerprintContext *ctx, const PartitionElem *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "PartitionElem");

  if (node->collation != NULL && node->collation->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->collation, node, "collation", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "collation");
  }
  if (node->expr != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->expr, node, "expr", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "expr");
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->name != NULL) {
    _fingerprintString(ctx, "name");
    _fingerprintString(ctx, node->name);
  }

  if (node->opclass != NULL && node->opclass->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->opclass, node, "opclass", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "opclass");
  }
}

static void
_fingerprintPartitionSpec(FingerprintContext *ctx, const PartitionSpec *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "PartitionSpec");

  // Intentionally ignoring node->location for fingerprinting

  if (node->partParams != NULL && node->partParams->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->partParams, node, "partParams", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "partParams");
  }
  if (node->strategy != NULL) {
    _fingerprintString(ctx, "strategy");
    _fingerprintString(ctx, node->strategy);
  }

}

static void
_fingerprintPartitionBoundSpec(FingerprintContext *ctx, const PartitionBoundSpec *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "PartitionBoundSpec");

  if (node->listdatums != NULL && node->listdatums->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->listdatums, node, "listdatums", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "listdatums");
  }
  // Intentionally ignoring node->location for fingerprinting

  if (node->lowerdatums != NULL && node->lowerdatums->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->lowerdatums, node, "lowerdatums", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "lowerdatums");
  }
  if (node->strategy != 0) {
    char buffer[2] = {node->strategy, '\0'};
    _fingerprintString(ctx, "strategy");
    _fingerprintString(ctx, buffer);
  }

  if (node->upperdatums != NULL && node->upperdatums->length > 0) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->upperdatums, node, "upperdatums", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "upperdatums");
  }
}

static void
_fingerprintPartitionRangeDatum(FingerprintContext *ctx, const PartitionRangeDatum *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "PartitionRangeDatum");

  if (node->kind != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->kind);
    _fingerprintString(ctx, "kind");
    _fingerprintString(ctx, buffer);
  }

  // Intentionally ignoring node->location for fingerprinting

  if (node->value != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->value, node, "value", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "value");
  }

}

static void
_fingerprintPartitionCmd(FingerprintContext *ctx, const PartitionCmd *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "PartitionCmd");

  if (node->bound != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->bound, node, "bound", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "bound");
  }

  if (node->name != NULL) {
    FingerprintContext subCtx;
    _fingerprintInitForTokens(&subCtx);
    _fingerprintNode(&subCtx, node->name, node, "name", depth + 1);
    _fingerprintCopyTokens(&subCtx, ctx, "name");
  }

}

static void
_fingerprintInlineCodeBlock(FingerprintContext *ctx, const InlineCodeBlock *node, const void *parent, const char *field_name, unsigned int depth)
{
  _fingerprintString(ctx, "InlineCodeBlock");

  if (node->langIsTrusted) {
    _fingerprintString(ctx, "langIsTrusted");
    _fingerprintString(ctx, "true");
  }

  if (node->langOid != 0) {
    char buffer[50];
    sprintf(buffer, "%d", node->langOid);
    _fingerprintString(ctx, "langOid");
    _fingerprintString(ctx, buffer);
  }

  if (node->source_text != NULL) {
    _fingerprintString(ctx, "source_text");
    _fingerprintString(ctx, node->source_text);
  }

}




void
_fingerprintNode(FingerprintContext *ctx, const void *obj, const void *parent, char *field_name, unsigned int depth)
{
	// Some queries are overly complex in their parsetree - lets consistently cut them off at 100 nodes deep
	if (depth >= 100) {
		return;
	}
	if (obj == NULL)
	{
		return; // Ignore
	}
	bool mytemp = IsA(obj, List);
	if (mytemp)
	{
		_fingerprintList(ctx, obj, parent, field_name, depth);
	}
	else
	{  
		switch (nodeTag(obj))
		{
			case T_Integer:
				_fingerprintInteger(ctx, obj);
				break;
			case T_Float:
				_fingerprintFloat(ctx, obj);
				break;
			case T_String:
				_fingerprintString(ctx, "String");
				_fingerprintString(ctx, "str");
				_fingerprintString(ctx, ((Value*) obj)->val.str);
				break;
			case T_BitString:
				_fingerprintBitString(ctx, obj);
				break;

				case T_Alias:
  _fingerprintAlias(ctx, obj, parent, field_name, depth);
  break;
case T_RangeVar:
  _fingerprintRangeVar(ctx, obj, parent, field_name, depth);
  break;
case T_TableFunc:
  _fingerprintTableFunc(ctx, obj, parent, field_name, depth);
  break;
case T_Expr:
  _fingerprintExpr(ctx, obj, parent, field_name, depth);
  break;
case T_Var:
  _fingerprintVar(ctx, obj, parent, field_name, depth);
  break;
case T_Const:
  _fingerprintConst(ctx, obj, parent, field_name, depth);
  break;
case T_Param:
  _fingerprintParam(ctx, obj, parent, field_name, depth);
  break;
case T_Aggref:
  _fingerprintAggref(ctx, obj, parent, field_name, depth);
  break;
case T_GroupingFunc:
  _fingerprintGroupingFunc(ctx, obj, parent, field_name, depth);
  break;
case T_WindowFunc:
  _fingerprintWindowFunc(ctx, obj, parent, field_name, depth);
  break;
case T_ArrayRef:
  _fingerprintArrayRef(ctx, obj, parent, field_name, depth);
  break;
case T_FuncExpr:
  _fingerprintFuncExpr(ctx, obj, parent, field_name, depth);
  break;
case T_NamedArgExpr:
  _fingerprintNamedArgExpr(ctx, obj, parent, field_name, depth);
  break;
case T_OpExpr:
  _fingerprintOpExpr(ctx, obj, parent, field_name, depth);
  break;
case T_ScalarArrayOpExpr:
  _fingerprintScalarArrayOpExpr(ctx, obj, parent, field_name, depth);
  break;
case T_BoolExpr:
  _fingerprintBoolExpr(ctx, obj, parent, field_name, depth);
  break;
case T_SubLink:
  _fingerprintSubLink(ctx, obj, parent, field_name, depth);
  break;
case T_SubPlan:
  _fingerprintSubPlan(ctx, obj, parent, field_name, depth);
  break;
case T_AlternativeSubPlan:
  _fingerprintAlternativeSubPlan(ctx, obj, parent, field_name, depth);
  break;
case T_FieldSelect:
  _fingerprintFieldSelect(ctx, obj, parent, field_name, depth);
  break;
case T_FieldStore:
  _fingerprintFieldStore(ctx, obj, parent, field_name, depth);
  break;
case T_RelabelType:
  _fingerprintRelabelType(ctx, obj, parent, field_name, depth);
  break;
case T_CoerceViaIO:
  _fingerprintCoerceViaIO(ctx, obj, parent, field_name, depth);
  break;
case T_ArrayCoerceExpr:
  _fingerprintArrayCoerceExpr(ctx, obj, parent, field_name, depth);
  break;
case T_ConvertRowtypeExpr:
  _fingerprintConvertRowtypeExpr(ctx, obj, parent, field_name, depth);
  break;
case T_CollateExpr:
  _fingerprintCollateExpr(ctx, obj, parent, field_name, depth);
  break;
case T_CaseExpr:
  _fingerprintCaseExpr(ctx, obj, parent, field_name, depth);
  break;
case T_CaseWhen:
  _fingerprintCaseWhen(ctx, obj, parent, field_name, depth);
  break;
case T_CaseTestExpr:
  _fingerprintCaseTestExpr(ctx, obj, parent, field_name, depth);
  break;
case T_ArrayExpr:
  _fingerprintArrayExpr(ctx, obj, parent, field_name, depth);
  break;
case T_RowExpr:
  _fingerprintRowExpr(ctx, obj, parent, field_name, depth);
  break;
case T_RowCompareExpr:
  _fingerprintRowCompareExpr(ctx, obj, parent, field_name, depth);
  break;
case T_CoalesceExpr:
  _fingerprintCoalesceExpr(ctx, obj, parent, field_name, depth);
  break;
case T_MinMaxExpr:
  _fingerprintMinMaxExpr(ctx, obj, parent, field_name, depth);
  break;
case T_SQLValueFunction:
  _fingerprintSQLValueFunction(ctx, obj, parent, field_name, depth);
  break;
case T_XmlExpr:
  _fingerprintXmlExpr(ctx, obj, parent, field_name, depth);
  break;
case T_NullTest:
  _fingerprintNullTest(ctx, obj, parent, field_name, depth);
  break;
case T_BooleanTest:
  _fingerprintBooleanTest(ctx, obj, parent, field_name, depth);
  break;
case T_CoerceToDomain:
  _fingerprintCoerceToDomain(ctx, obj, parent, field_name, depth);
  break;
case T_CoerceToDomainValue:
  _fingerprintCoerceToDomainValue(ctx, obj, parent, field_name, depth);
  break;
case T_SetToDefault:
  _fingerprintSetToDefault(ctx, obj, parent, field_name, depth);
  break;
case T_CurrentOfExpr:
  _fingerprintCurrentOfExpr(ctx, obj, parent, field_name, depth);
  break;
case T_NextValueExpr:
  _fingerprintNextValueExpr(ctx, obj, parent, field_name, depth);
  break;
case T_InferenceElem:
  _fingerprintInferenceElem(ctx, obj, parent, field_name, depth);
  break;
case T_TargetEntry:
  _fingerprintTargetEntry(ctx, obj, parent, field_name, depth);
  break;
case T_RangeTblRef:
  _fingerprintRangeTblRef(ctx, obj, parent, field_name, depth);
  break;
case T_JoinExpr:
  _fingerprintJoinExpr(ctx, obj, parent, field_name, depth);
  break;
case T_FromExpr:
  _fingerprintFromExpr(ctx, obj, parent, field_name, depth);
  break;
case T_OnConflictExpr:
  _fingerprintOnConflictExpr(ctx, obj, parent, field_name, depth);
  break;
case T_IntoClause:
  _fingerprintIntoClause(ctx, obj, parent, field_name, depth);
  break;
case T_RawStmt:
  _fingerprintRawStmt(ctx, obj, parent, field_name, depth);
  break;
case T_Query:
  _fingerprintQuery(ctx, obj, parent, field_name, depth);
  break;
case T_InsertStmt:
  _fingerprintInsertStmt(ctx, obj, parent, field_name, depth);
  break;
case T_DeleteStmt:
  _fingerprintDeleteStmt(ctx, obj, parent, field_name, depth);
  break;
case T_UpdateStmt:
  _fingerprintUpdateStmt(ctx, obj, parent, field_name, depth);
  break;
case T_SelectStmt:
  _fingerprintSelectStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterTableStmt:
  _fingerprintAlterTableStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterTableCmd:
  _fingerprintAlterTableCmd(ctx, obj, parent, field_name, depth);
  break;
case T_AlterDomainStmt:
  _fingerprintAlterDomainStmt(ctx, obj, parent, field_name, depth);
  break;
case T_SetOperationStmt:
  _fingerprintSetOperationStmt(ctx, obj, parent, field_name, depth);
  break;
case T_GrantStmt:
  _fingerprintGrantStmt(ctx, obj, parent, field_name, depth);
  break;
case T_GrantRoleStmt:
  _fingerprintGrantRoleStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterDefaultPrivilegesStmt:
  _fingerprintAlterDefaultPrivilegesStmt(ctx, obj, parent, field_name, depth);
  break;
case T_ClosePortalStmt:
  _fingerprintClosePortalStmt(ctx, obj, parent, field_name, depth);
  break;
case T_ClusterStmt:
  _fingerprintClusterStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CopyStmt:
  _fingerprintCopyStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateStmt:
  _fingerprintCreateStmt(ctx, obj, parent, field_name, depth);
  break;
case T_DefineStmt:
  _fingerprintDefineStmt(ctx, obj, parent, field_name, depth);
  break;
case T_DropStmt:
  _fingerprintDropStmt(ctx, obj, parent, field_name, depth);
  break;
case T_TruncateStmt:
  _fingerprintTruncateStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CommentStmt:
  _fingerprintCommentStmt(ctx, obj, parent, field_name, depth);
  break;
case T_FetchStmt:
  _fingerprintFetchStmt(ctx, obj, parent, field_name, depth);
  break;
case T_IndexStmt:
  _fingerprintIndexStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateFunctionStmt:
  _fingerprintCreateFunctionStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterFunctionStmt:
  _fingerprintAlterFunctionStmt(ctx, obj, parent, field_name, depth);
  break;
case T_DoStmt:
  _fingerprintDoStmt(ctx, obj, parent, field_name, depth);
  break;
case T_RenameStmt:
  _fingerprintRenameStmt(ctx, obj, parent, field_name, depth);
  break;
case T_RuleStmt:
  _fingerprintRuleStmt(ctx, obj, parent, field_name, depth);
  break;
case T_NotifyStmt:
  _fingerprintNotifyStmt(ctx, obj, parent, field_name, depth);
  break;
case T_ListenStmt:
  _fingerprintListenStmt(ctx, obj, parent, field_name, depth);
  break;
case T_UnlistenStmt:
  _fingerprintUnlistenStmt(ctx, obj, parent, field_name, depth);
  break;
case T_TransactionStmt:
  _fingerprintTransactionStmt(ctx, obj, parent, field_name, depth);
  break;
case T_ViewStmt:
  _fingerprintViewStmt(ctx, obj, parent, field_name, depth);
  break;
case T_LoadStmt:
  _fingerprintLoadStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateDomainStmt:
  _fingerprintCreateDomainStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreatedbStmt:
  _fingerprintCreatedbStmt(ctx, obj, parent, field_name, depth);
  break;
case T_DropdbStmt:
  _fingerprintDropdbStmt(ctx, obj, parent, field_name, depth);
  break;
case T_VacuumStmt:
  _fingerprintVacuumStmt(ctx, obj, parent, field_name, depth);
  break;
case T_ExplainStmt:
  _fingerprintExplainStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateTableAsStmt:
  _fingerprintCreateTableAsStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateSeqStmt:
  _fingerprintCreateSeqStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterSeqStmt:
  _fingerprintAlterSeqStmt(ctx, obj, parent, field_name, depth);
  break;
case T_VariableSetStmt:
  _fingerprintVariableSetStmt(ctx, obj, parent, field_name, depth);
  break;
case T_VariableShowStmt:
  _fingerprintVariableShowStmt(ctx, obj, parent, field_name, depth);
  break;
case T_DiscardStmt:
  _fingerprintDiscardStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateTrigStmt:
  _fingerprintCreateTrigStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreatePLangStmt:
  _fingerprintCreatePLangStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateRoleStmt:
  _fingerprintCreateRoleStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterRoleStmt:
  _fingerprintAlterRoleStmt(ctx, obj, parent, field_name, depth);
  break;
case T_DropRoleStmt:
  _fingerprintDropRoleStmt(ctx, obj, parent, field_name, depth);
  break;
case T_LockStmt:
  _fingerprintLockStmt(ctx, obj, parent, field_name, depth);
  break;
case T_ConstraintsSetStmt:
  _fingerprintConstraintsSetStmt(ctx, obj, parent, field_name, depth);
  break;
case T_ReindexStmt:
  _fingerprintReindexStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CheckPointStmt:
  _fingerprintCheckPointStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateSchemaStmt:
  _fingerprintCreateSchemaStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterDatabaseStmt:
  _fingerprintAlterDatabaseStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterDatabaseSetStmt:
  _fingerprintAlterDatabaseSetStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterRoleSetStmt:
  _fingerprintAlterRoleSetStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateConversionStmt:
  _fingerprintCreateConversionStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateCastStmt:
  _fingerprintCreateCastStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateOpClassStmt:
  _fingerprintCreateOpClassStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateOpFamilyStmt:
  _fingerprintCreateOpFamilyStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterOpFamilyStmt:
  _fingerprintAlterOpFamilyStmt(ctx, obj, parent, field_name, depth);
  break;
case T_PrepareStmt:
  _fingerprintPrepareStmt(ctx, obj, parent, field_name, depth);
  break;
case T_ExecuteStmt:
  _fingerprintExecuteStmt(ctx, obj, parent, field_name, depth);
  break;
case T_DeallocateStmt:
  _fingerprintDeallocateStmt(ctx, obj, parent, field_name, depth);
  break;
case T_DeclareCursorStmt:
  _fingerprintDeclareCursorStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateTableSpaceStmt:
  _fingerprintCreateTableSpaceStmt(ctx, obj, parent, field_name, depth);
  break;
case T_DropTableSpaceStmt:
  _fingerprintDropTableSpaceStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterObjectDependsStmt:
  _fingerprintAlterObjectDependsStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterObjectSchemaStmt:
  _fingerprintAlterObjectSchemaStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterOwnerStmt:
  _fingerprintAlterOwnerStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterOperatorStmt:
  _fingerprintAlterOperatorStmt(ctx, obj, parent, field_name, depth);
  break;
case T_DropOwnedStmt:
  _fingerprintDropOwnedStmt(ctx, obj, parent, field_name, depth);
  break;
case T_ReassignOwnedStmt:
  _fingerprintReassignOwnedStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CompositeTypeStmt:
  _fingerprintCompositeTypeStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateEnumStmt:
  _fingerprintCreateEnumStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateRangeStmt:
  _fingerprintCreateRangeStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterEnumStmt:
  _fingerprintAlterEnumStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterTSDictionaryStmt:
  _fingerprintAlterTSDictionaryStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterTSConfigurationStmt:
  _fingerprintAlterTSConfigurationStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateFdwStmt:
  _fingerprintCreateFdwStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterFdwStmt:
  _fingerprintAlterFdwStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateForeignServerStmt:
  _fingerprintCreateForeignServerStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterForeignServerStmt:
  _fingerprintAlterForeignServerStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateUserMappingStmt:
  _fingerprintCreateUserMappingStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterUserMappingStmt:
  _fingerprintAlterUserMappingStmt(ctx, obj, parent, field_name, depth);
  break;
case T_DropUserMappingStmt:
  _fingerprintDropUserMappingStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterTableSpaceOptionsStmt:
  _fingerprintAlterTableSpaceOptionsStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterTableMoveAllStmt:
  _fingerprintAlterTableMoveAllStmt(ctx, obj, parent, field_name, depth);
  break;
case T_SecLabelStmt:
  _fingerprintSecLabelStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateForeignTableStmt:
  _fingerprintCreateForeignTableStmt(ctx, obj, parent, field_name, depth);
  break;
case T_ImportForeignSchemaStmt:
  _fingerprintImportForeignSchemaStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateExtensionStmt:
  _fingerprintCreateExtensionStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterExtensionStmt:
  _fingerprintAlterExtensionStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterExtensionContentsStmt:
  _fingerprintAlterExtensionContentsStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateEventTrigStmt:
  _fingerprintCreateEventTrigStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterEventTrigStmt:
  _fingerprintAlterEventTrigStmt(ctx, obj, parent, field_name, depth);
  break;
case T_RefreshMatViewStmt:
  _fingerprintRefreshMatViewStmt(ctx, obj, parent, field_name, depth);
  break;
case T_ReplicaIdentityStmt:
  _fingerprintReplicaIdentityStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterSystemStmt:
  _fingerprintAlterSystemStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreatePolicyStmt:
  _fingerprintCreatePolicyStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterPolicyStmt:
  _fingerprintAlterPolicyStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateTransformStmt:
  _fingerprintCreateTransformStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateAmStmt:
  _fingerprintCreateAmStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreatePublicationStmt:
  _fingerprintCreatePublicationStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterPublicationStmt:
  _fingerprintAlterPublicationStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateSubscriptionStmt:
  _fingerprintCreateSubscriptionStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterSubscriptionStmt:
  _fingerprintAlterSubscriptionStmt(ctx, obj, parent, field_name, depth);
  break;
case T_DropSubscriptionStmt:
  _fingerprintDropSubscriptionStmt(ctx, obj, parent, field_name, depth);
  break;
case T_CreateStatsStmt:
  _fingerprintCreateStatsStmt(ctx, obj, parent, field_name, depth);
  break;
case T_AlterCollationStmt:
  _fingerprintAlterCollationStmt(ctx, obj, parent, field_name, depth);
  break;
case T_A_Expr:
  _fingerprintA_Expr(ctx, obj, parent, field_name, depth);
  break;
case T_ColumnRef:
  _fingerprintColumnRef(ctx, obj, parent, field_name, depth);
  break;
case T_ParamRef:
  _fingerprintParamRef(ctx, obj, parent, field_name, depth);
  break;
case T_A_Const:
  _fingerprintA_Const(ctx, obj, parent, field_name, depth);
  break;
case T_FuncCall:
  _fingerprintFuncCall(ctx, obj, parent, field_name, depth);
  break;
case T_A_Star:
  _fingerprintA_Star(ctx, obj, parent, field_name, depth);
  break;
case T_A_Indices:
  _fingerprintA_Indices(ctx, obj, parent, field_name, depth);
  break;
case T_A_Indirection:
  _fingerprintA_Indirection(ctx, obj, parent, field_name, depth);
  break;
case T_A_ArrayExpr:
  _fingerprintA_ArrayExpr(ctx, obj, parent, field_name, depth);
  break;
case T_ResTarget:
  _fingerprintResTarget(ctx, obj, parent, field_name, depth);
  break;
case T_MultiAssignRef:
  _fingerprintMultiAssignRef(ctx, obj, parent, field_name, depth);
  break;
case T_TypeCast:
  _fingerprintTypeCast(ctx, obj, parent, field_name, depth);
  break;
case T_CollateClause:
  _fingerprintCollateClause(ctx, obj, parent, field_name, depth);
  break;
case T_SortBy:
  _fingerprintSortBy(ctx, obj, parent, field_name, depth);
  break;
case T_WindowDef:
  _fingerprintWindowDef(ctx, obj, parent, field_name, depth);
  break;
case T_RangeSubselect:
  _fingerprintRangeSubselect(ctx, obj, parent, field_name, depth);
  break;
case T_RangeFunction:
  _fingerprintRangeFunction(ctx, obj, parent, field_name, depth);
  break;
case T_RangeTableSample:
  _fingerprintRangeTableSample(ctx, obj, parent, field_name, depth);
  break;
case T_RangeTableFunc:
  _fingerprintRangeTableFunc(ctx, obj, parent, field_name, depth);
  break;
case T_RangeTableFuncCol:
  _fingerprintRangeTableFuncCol(ctx, obj, parent, field_name, depth);
  break;
case T_TypeName:
  _fingerprintTypeName(ctx, obj, parent, field_name, depth);
  break;
case T_ColumnDef:
  _fingerprintColumnDef(ctx, obj, parent, field_name, depth);
  break;
case T_IndexElem:
  _fingerprintIndexElem(ctx, obj, parent, field_name, depth);
  break;
case T_Constraint:
  _fingerprintConstraint(ctx, obj, parent, field_name, depth);
  break;
case T_DefElem:
  _fingerprintDefElem(ctx, obj, parent, field_name, depth);
  break;
case T_RangeTblEntry:
  _fingerprintRangeTblEntry(ctx, obj, parent, field_name, depth);
  break;
case T_RangeTblFunction:
  _fingerprintRangeTblFunction(ctx, obj, parent, field_name, depth);
  break;
case T_TableSampleClause:
  _fingerprintTableSampleClause(ctx, obj, parent, field_name, depth);
  break;
case T_WithCheckOption:
  _fingerprintWithCheckOption(ctx, obj, parent, field_name, depth);
  break;
case T_SortGroupClause:
  _fingerprintSortGroupClause(ctx, obj, parent, field_name, depth);
  break;
case T_GroupingSet:
  _fingerprintGroupingSet(ctx, obj, parent, field_name, depth);
  break;
case T_WindowClause:
  _fingerprintWindowClause(ctx, obj, parent, field_name, depth);
  break;
case T_ObjectWithArgs:
  _fingerprintObjectWithArgs(ctx, obj, parent, field_name, depth);
  break;
case T_AccessPriv:
  _fingerprintAccessPriv(ctx, obj, parent, field_name, depth);
  break;
case T_CreateOpClassItem:
  _fingerprintCreateOpClassItem(ctx, obj, parent, field_name, depth);
  break;
case T_TableLikeClause:
  _fingerprintTableLikeClause(ctx, obj, parent, field_name, depth);
  break;
case T_FunctionParameter:
  _fingerprintFunctionParameter(ctx, obj, parent, field_name, depth);
  break;
case T_LockingClause:
  _fingerprintLockingClause(ctx, obj, parent, field_name, depth);
  break;
case T_RowMarkClause:
  _fingerprintRowMarkClause(ctx, obj, parent, field_name, depth);
  break;
case T_XmlSerialize:
  _fingerprintXmlSerialize(ctx, obj, parent, field_name, depth);
  break;
case T_WithClause:
  _fingerprintWithClause(ctx, obj, parent, field_name, depth);
  break;
case T_InferClause:
  _fingerprintInferClause(ctx, obj, parent, field_name, depth);
  break;
case T_OnConflictClause:
  _fingerprintOnConflictClause(ctx, obj, parent, field_name, depth);
  break;
case T_CommonTableExpr:
  _fingerprintCommonTableExpr(ctx, obj, parent, field_name, depth);
  break;
case T_RoleSpec:
  _fingerprintRoleSpec(ctx, obj, parent, field_name, depth);
  break;
case T_TriggerTransition:
  _fingerprintTriggerTransition(ctx, obj, parent, field_name, depth);
  break;
case T_PartitionElem:
  _fingerprintPartitionElem(ctx, obj, parent, field_name, depth);
  break;
case T_PartitionSpec:
  _fingerprintPartitionSpec(ctx, obj, parent, field_name, depth);
  break;
case T_PartitionBoundSpec:
  _fingerprintPartitionBoundSpec(ctx, obj, parent, field_name, depth);
  break;
case T_PartitionRangeDatum:
  _fingerprintPartitionRangeDatum(ctx, obj, parent, field_name, depth);
  break;
case T_PartitionCmd:
  _fingerprintPartitionCmd(ctx, obj, parent, field_name, depth);
  break;
case T_InlineCodeBlock:
  _fingerprintInlineCodeBlock(ctx, obj, parent, field_name, depth);
  break;

			default:
				elog(WARNING, "could not fingerprint unrecognized node type: %d",
					 (int) nodeTag(obj));

				return;
		}
	}
}

PgQueryFingerprintResult pg_query_fingerprint_with_opts(List* input, bool printTokens)
{
	MemoryContext ctx = NULL;
	PgQueryInternalParsetreeAndError parsetree_and_error;
	PgQueryFingerprintResult result = {0};

	ctx = pg_query_enter_memory_context("pg_query_fingerprint");

	parsetree_and_error = pg_query_raw_parse(input);

	// These are all malloc-ed and will survive exiting the memory context, the caller is responsible to free them now
	result.stderr_buffer = parsetree_and_error.stderr_buffer;
	result.error = parsetree_and_error.error;

	if (parsetree_and_error.tree != NULL || result.error == NULL) {
		FingerprintContext ctx;
		int i;
		uint8 sha1result[SHA1_RESULTLEN];

		ctx.sha1 = palloc0(sizeof(SHA1_CTX));
		SHA1Init(ctx.sha1);

		if (parsetree_and_error.tree != NULL) {
			_fingerprintNode(&ctx, parsetree_and_error.tree, NULL, NULL, 0);
		}

		SHA1Final(sha1result, ctx.sha1);

		// This is intentionally malloc-ed and will survive exiting the memory context
		result.hexdigest = calloc((1 + SHA1_RESULTLEN) * 2 + 1, sizeof(char));

		sprintf(result.hexdigest, "%02x", PG_QUERY_FINGERPRINT_VERSION);

		for (i = 0; i < SHA1_RESULTLEN; i++) {
			sprintf(result.hexdigest + (1 + i) * 2, "%02x", sha1result[i]);
		}

		if (printTokens) {
			FingerprintContext debugCtx;
			dlist_iter iter;

			_fingerprintInitForTokens(&debugCtx);
			_fingerprintNode(&debugCtx, parsetree_and_error.tree, NULL, NULL, 0);

			printf("[");

			dlist_foreach(iter, &debugCtx.tokens)
			{
				FingerprintToken *token = dlist_container(FingerprintToken, list_node, iter.cur);

				printf("%s, ", token->str);
			}

			printf("]\n");
		}
	}

	pg_query_exit_memory_context(ctx);

	return result;
}

const FILE *fptr;
bool opened = false;

void
DB_Project_GetFingerprint(List *parsetree_list, const char *query_string)
{	
	if (!opened)
	{
		fptr = fopen("/home/[username]/Desktop/log/db_project_log.csv", "a");
		opened = true;
	}
	if (strstr(query_string, "select") != NULL) {
		if (strstr(query_string, "explain") == NULL && strstr(query_string, "pg_") == NULL) {
			//fprintf(fptr, "DEBUG: %s --- ### --- ", query_string);
			fprintf(fptr, "%s,", pg_query_fingerprint_with_opts(parsetree_list, false).hexdigest);
			fflush(fptr);
		}
	}
}



/*
 * exec_simple_query
 *
 * Execute a "simple Query" protocol message.
 */
static void
exec_simple_query(const char *query_string)
{
	CommandDest dest = whereToSendOutput;
	MemoryContext oldcontext;
	List	   *parsetree_list;
	ListCell   *parsetree_item;
	bool		save_log_statement_stats = log_statement_stats;
	bool		was_logged = false;
	bool		isTopLevel;
	char		msec_str[32];


	/*
	 * Report query to various monitoring facilities.
	 */
	debug_query_string = query_string;

	pgstat_report_activity(STATE_RUNNING, query_string);

	TRACE_POSTGRESQL_QUERY_START(query_string);

	/*
	 * We use save_log_statement_stats so ShowUsage doesn't report incorrect
	 * results because ResetUsage wasn't called.
	 */
	if (save_log_statement_stats)
		ResetUsage();

	/*
	 * Start up a transaction command.  All queries generated by the
	 * query_string will be in this same command block, *unless* we find a
	 * BEGIN/COMMIT/ABORT statement; we have to force a new xact command after
	 * one of those, else bad things will happen in xact.c. (Note that this
	 * will normally change current memory context.)
	 */
	start_xact_command();

	/*
	 * Zap any pre-existing unnamed statement.  (While not strictly necessary,
	 * it seems best to define simple-Query mode as if it used the unnamed
	 * statement and portal; this ensures we recover any storage used by prior
	 * unnamed operations.)
	 */
	drop_unnamed_stmt();

	/*
	 * Switch to appropriate context for constructing parsetrees.
	 */
	oldcontext = MemoryContextSwitchTo(MessageContext);

	/*
	 * Do basic parsing of the query or queries (this should be safe even if
	 * we are in aborted transaction state!)
	 */
	parsetree_list = pg_parse_query(query_string);
	DB_Project_GetFingerprint(parsetree_list, query_string);

	/* Log immediately if dictated by log_statement */
	if (check_log_statement(parsetree_list))
	{
		ereport(LOG,
				(errmsg("statement: %s", query_string),
				 errhidestmt(true),
				 errdetail_execute(parsetree_list)));
		was_logged = true;
	}

	/*
	 * Switch back to transaction context to enter the loop.
	 */
	MemoryContextSwitchTo(oldcontext);

	/*
	 * We'll tell PortalRun it's a top-level command iff there's exactly one
	 * raw parsetree.  If more than one, it's effectively a transaction block
	 * and we want PreventTransactionChain to reject unsafe commands. (Note:
	 * we're assuming that query rewrite cannot add commands that are
	 * significant to PreventTransactionChain.)
	 */
	isTopLevel = (list_length(parsetree_list) == 1);

	/*
	 * Run through the raw parsetree(s) and process each one.
	 */
	foreach(parsetree_item, parsetree_list)
	{
		RawStmt    *parsetree = lfirst_node(RawStmt, parsetree_item);
		bool		snapshot_set = false;
		const char *commandTag;
		char		completionTag[COMPLETION_TAG_BUFSIZE];
		List	   *querytree_list,
				   *plantree_list;
		Portal		portal;
		DestReceiver *receiver;
		int16		format;

		/*
		 * Get the command name for use in status display (it also becomes the
		 * default completion tag, down inside PortalRun).  Set ps_status and
		 * do any special start-of-SQL-command processing needed by the
		 * destination.
		 */
		commandTag = CreateCommandTag(parsetree->stmt);

		set_ps_display(commandTag, false);

		BeginCommand(commandTag, dest);

		/*
		 * If we are in an aborted transaction, reject all commands except
		 * COMMIT/ABORT.  It is important that this test occur before we try
		 * to do parse analysis, rewrite, or planning, since all those phases
		 * try to do database accesses, which may fail in abort state. (It
		 * might be safe to allow some additional utility commands in this
		 * state, but not many...)
		 */
		if (IsAbortedTransactionBlockState() &&
			!IsTransactionExitStmt(parsetree->stmt))
			ereport(ERROR,
					(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
					 errmsg("current transaction is aborted, "
							"commands ignored until end of transaction block"),
					 errdetail_abort()));

		/* Make sure we are in a transaction command */
		start_xact_command();

		/* If we got a cancel signal in parsing or prior command, quit */
		CHECK_FOR_INTERRUPTS();

		/*
		 * Set up a snapshot if parse analysis/planning will need one.
		 */
		if (analyze_requires_snapshot(parsetree))
		{
			PushActiveSnapshot(GetTransactionSnapshot());
			snapshot_set = true;
		}

		/*
		 * OK to analyze, rewrite, and plan this query.
		 *
		 * Switch to appropriate context for constructing querytrees (again,
		 * these must outlive the execution context).
		 */
		oldcontext = MemoryContextSwitchTo(MessageContext);

		querytree_list = pg_analyze_and_rewrite(parsetree, query_string,
												NULL, 0, NULL);

		plantree_list = pg_plan_queries(querytree_list,
										CURSOR_OPT_PARALLEL_OK, NULL);

		/* Done with the snapshot used for parsing/planning */
		if (snapshot_set)
			PopActiveSnapshot();

		/* If we got a cancel signal in analysis or planning, quit */
		CHECK_FOR_INTERRUPTS();

		/*
		 * Create unnamed portal to run the query or queries in. If there
		 * already is one, silently drop it.
		 */
		portal = CreatePortal("", true, true);
		/* Don't display the portal in pg_cursors */
		portal->visible = false;

		/*
		 * We don't have to copy anything into the portal, because everything
		 * we are passing here is in MessageContext, which will outlive the
		 * portal anyway.
		 */
		PortalDefineQuery(portal,
						  NULL,
						  query_string,
						  commandTag,
						  plantree_list,
						  NULL);

		/*
		 * Start the portal.  No parameters here.
		 */
		PortalStart(portal, NULL, 0, InvalidSnapshot);

		/*
		 * Select the appropriate output format: text unless we are doing a
		 * FETCH from a binary cursor.  (Pretty grotty to have to do this here
		 * --- but it avoids grottiness in other places.  Ah, the joys of
		 * backward compatibility...)
		 */
		format = 0;				/* TEXT is default */
		if (IsA(parsetree->stmt, FetchStmt))
		{
			FetchStmt  *stmt = (FetchStmt *) parsetree->stmt;

			if (!stmt->ismove)
			{
				Portal		fportal = GetPortalByName(stmt->portalname);

				if (PortalIsValid(fportal) &&
					(fportal->cursorOptions & CURSOR_OPT_BINARY))
					format = 1; /* BINARY */
			}
		}
		PortalSetResultFormat(portal, 1, &format);

		/*
		 * Now we can create the destination receiver object.
		 */
		receiver = CreateDestReceiver(dest);
		if (dest == DestRemote)
			SetRemoteDestReceiverParams(receiver, portal);

		/*
		 * Switch back to transaction context for execution.
		 */
		MemoryContextSwitchTo(oldcontext);

		/*
		 * Run the portal to completion, and then drop it (and the receiver).
		 */
		(void) PortalRun(portal,
						 FETCH_ALL,
						 isTopLevel,
						 true,
						 receiver,
						 receiver,
						 completionTag);

		(*receiver->rDestroy) (receiver);

		PortalDrop(portal, false);

		if (IsA(parsetree->stmt, TransactionStmt))
		{
			/*
			 * If this was a transaction control statement, commit it. We will
			 * start a new xact command for the next command (if any).
			 */
			finish_xact_command();
		}
		else if (lnext(parsetree_item) == NULL)
		{
			/*
			 * If this is the last parsetree of the query string, close down
			 * transaction statement before reporting command-complete.  This
			 * is so that any end-of-transaction errors are reported before
			 * the command-complete message is issued, to avoid confusing
			 * clients who will expect either a command-complete message or an
			 * error, not one and then the other.  But for compatibility with
			 * historical Postgres behavior, we do not force a transaction
			 * boundary between queries appearing in a single query string.
			 */
			finish_xact_command();
		}
		else
		{
			/*
			 * We need a CommandCounterIncrement after every query, except
			 * those that start or end a transaction block.
			 */
			CommandCounterIncrement();
		}

		/*
		 * Tell client that we're done with this query.  Note we emit exactly
		 * one EndCommand report for each raw parsetree, thus one for each SQL
		 * command the client sent, regardless of rewriting. (But a command
		 * aborted by error will not send an EndCommand report at all.)
		 */
		EndCommand(completionTag, dest);
	}							/* end loop over parsetrees */

	/*
	 * Close down transaction statement, if one is open.
	 */
	finish_xact_command();

	/*
	 * If there were no parsetrees, return EmptyQueryResponse message.
	 */
	if (!parsetree_list)
		NullCommand(dest);

	/*
	 * Emit duration logging if appropriate.
	 */
	switch (check_log_duration(msec_str, was_logged))
	{
		case 1:
			ereport(LOG,
					(errmsg("duration: %s ms", msec_str),
					 errhidestmt(true)));
			break;
		case 2:
			ereport(LOG,
					(errmsg("duration: %s ms  statement: %s",
							msec_str, query_string),
					 errhidestmt(true),
					 errdetail_execute(parsetree_list)));
			break;
	}

	if (save_log_statement_stats)
		ShowUsage("QUERY STATISTICS");

	TRACE_POSTGRESQL_QUERY_DONE(query_string);

	debug_query_string = NULL;
}

/*
 * exec_parse_message
 *
 * Execute a "Parse" protocol message.
 */
static void
exec_parse_message(const char *query_string,	/* string to execute */
				   const char *stmt_name,	/* name for prepared stmt */
				   Oid *paramTypes, /* parameter types */
				   int numParams)	/* number of parameters */
{
	MemoryContext unnamed_stmt_context = NULL;
	MemoryContext oldcontext;
	List	   *parsetree_list;
	RawStmt    *raw_parse_tree;
	const char *commandTag;
	List	   *querytree_list;
	CachedPlanSource *psrc;
	bool		is_named;
	bool		save_log_statement_stats = log_statement_stats;
	char		msec_str[32];

	/*
	 * Report query to various monitoring facilities.
	 */
	debug_query_string = query_string;

	pgstat_report_activity(STATE_RUNNING, query_string);

	set_ps_display("PARSE", false);

	if (save_log_statement_stats)
		ResetUsage();

	ereport(DEBUG2,
			(errmsg("parse %s: %s",
					*stmt_name ? stmt_name : "<unnamed>",
					query_string)));

	/*
	 * Start up a transaction command so we can run parse analysis etc. (Note
	 * that this will normally change current memory context.) Nothing happens
	 * if we are already in one.
	 */
	start_xact_command();

	/*
	 * Switch to appropriate context for constructing parsetrees.
	 *
	 * We have two strategies depending on whether the prepared statement is
	 * named or not.  For a named prepared statement, we do parsing in
	 * MessageContext and copy the finished trees into the prepared
	 * statement's plancache entry; then the reset of MessageContext releases
	 * temporary space used by parsing and rewriting. For an unnamed prepared
	 * statement, we assume the statement isn't going to hang around long, so
	 * getting rid of temp space quickly is probably not worth the costs of
	 * copying parse trees.  So in this case, we create the plancache entry's
	 * query_context here, and do all the parsing work therein.
	 */
	is_named = (stmt_name[0] != '\0');
	if (is_named)
	{
		/* Named prepared statement --- parse in MessageContext */
		oldcontext = MemoryContextSwitchTo(MessageContext);
	}
	else
	{
		/* Unnamed prepared statement --- release any prior unnamed stmt */
		drop_unnamed_stmt();
		/* Create context for parsing */
		unnamed_stmt_context =
			AllocSetContextCreate(MessageContext,
								  "unnamed prepared statement",
								  ALLOCSET_DEFAULT_SIZES);
		oldcontext = MemoryContextSwitchTo(unnamed_stmt_context);
	}

	/*
	 * Do basic parsing of the query or queries (this should be safe even if
	 * we are in aborted transaction state!)
	 */
	parsetree_list = pg_parse_query(query_string);

	/*
	 * We only allow a single user statement in a prepared statement. This is
	 * mainly to keep the protocol simple --- otherwise we'd need to worry
	 * about multiple result tupdescs and things like that.
	 */
	if (list_length(parsetree_list) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot insert multiple commands into a prepared statement")));

	if (parsetree_list != NIL)
	{
		Query	   *query;
		bool		snapshot_set = false;
		int			i;

		raw_parse_tree = linitial_node(RawStmt, parsetree_list);

		/*
		 * Get the command name for possible use in status display.
		 */
		commandTag = CreateCommandTag(raw_parse_tree->stmt);

		/*
		 * If we are in an aborted transaction, reject all commands except
		 * COMMIT/ROLLBACK.  It is important that this test occur before we
		 * try to do parse analysis, rewrite, or planning, since all those
		 * phases try to do database accesses, which may fail in abort state.
		 * (It might be safe to allow some additional utility commands in this
		 * state, but not many...)
		 */
		if (IsAbortedTransactionBlockState() &&
			!IsTransactionExitStmt(raw_parse_tree->stmt))
			ereport(ERROR,
					(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
					 errmsg("current transaction is aborted, "
							"commands ignored until end of transaction block"),
					 errdetail_abort()));

		/*
		 * Create the CachedPlanSource before we do parse analysis, since it
		 * needs to see the unmodified raw parse tree.
		 */
		psrc = CreateCachedPlan(raw_parse_tree, query_string, commandTag);

		/*
		 * Set up a snapshot if parse analysis will need one.
		 */
		if (analyze_requires_snapshot(raw_parse_tree))
		{
			PushActiveSnapshot(GetTransactionSnapshot());
			snapshot_set = true;
		}

		/*
		 * Analyze and rewrite the query.  Note that the originally specified
		 * parameter set is not required to be complete, so we have to use
		 * parse_analyze_varparams().
		 */
		if (log_parser_stats)
			ResetUsage();

		query = parse_analyze_varparams(raw_parse_tree,
										query_string,
										&paramTypes,
										&numParams);

		/*
		 * Check all parameter types got determined.
		 */
		for (i = 0; i < numParams; i++)
		{
			Oid			ptype = paramTypes[i];

			if (ptype == InvalidOid || ptype == UNKNOWNOID)
				ereport(ERROR,
						(errcode(ERRCODE_INDETERMINATE_DATATYPE),
						 errmsg("could not determine data type of parameter $%d",
								i + 1)));
		}

		if (log_parser_stats)
			ShowUsage("PARSE ANALYSIS STATISTICS");

		querytree_list = pg_rewrite_query(query);

		/* Done with the snapshot used for parsing */
		if (snapshot_set)
			PopActiveSnapshot();
	}
	else
	{
		/* Empty input string.  This is legal. */
		raw_parse_tree = NULL;
		commandTag = NULL;
		psrc = CreateCachedPlan(raw_parse_tree, query_string, commandTag);
		querytree_list = NIL;
	}

	/*
	 * CachedPlanSource must be a direct child of MessageContext before we
	 * reparent unnamed_stmt_context under it, else we have a disconnected
	 * circular subgraph.  Klugy, but less so than flipping contexts even more
	 * above.
	 */
	if (unnamed_stmt_context)
		MemoryContextSetParent(psrc->context, MessageContext);

	/* Finish filling in the CachedPlanSource */
	CompleteCachedPlan(psrc,
					   querytree_list,
					   unnamed_stmt_context,
					   paramTypes,
					   numParams,
					   NULL,
					   NULL,
					   CURSOR_OPT_PARALLEL_OK,	/* allow parallel mode */
					   true);	/* fixed result */

	/* If we got a cancel signal during analysis, quit */
	CHECK_FOR_INTERRUPTS();

	if (is_named)
	{
		/*
		 * Store the query as a prepared statement.
		 */
		StorePreparedStatement(stmt_name, psrc, false);
	}
	else
	{
		/*
		 * We just save the CachedPlanSource into unnamed_stmt_psrc.
		 */
		SaveCachedPlan(psrc);
		unnamed_stmt_psrc = psrc;
	}

	MemoryContextSwitchTo(oldcontext);

	/*
	 * We do NOT close the open transaction command here; that only happens
	 * when the client sends Sync.  Instead, do CommandCounterIncrement just
	 * in case something happened during parse/plan.
	 */
	CommandCounterIncrement();

	/*
	 * Send ParseComplete.
	 */
	if (whereToSendOutput == DestRemote)
		pq_putemptymessage('1');

	/*
	 * Emit duration logging if appropriate.
	 */
	switch (check_log_duration(msec_str, false))
	{
		case 1:
			ereport(LOG,
					(errmsg("duration: %s ms", msec_str),
					 errhidestmt(true)));
			break;
		case 2:
			ereport(LOG,
					(errmsg("duration: %s ms  parse %s: %s",
							msec_str,
							*stmt_name ? stmt_name : "<unnamed>",
							query_string),
					 errhidestmt(true)));
			break;
	}

	if (save_log_statement_stats)
		ShowUsage("PARSE MESSAGE STATISTICS");

	debug_query_string = NULL;
}

/*
 * exec_bind_message
 *
 * Process a "Bind" message to create a portal from a prepared statement
 */
static void
exec_bind_message(StringInfo input_message)
{
	const char *portal_name;
	const char *stmt_name;
	int			numPFormats;
	int16	   *pformats = NULL;
	int			numParams;
	int			numRFormats;
	int16	   *rformats = NULL;
	CachedPlanSource *psrc;
	CachedPlan *cplan;
	Portal		portal;
	char	   *query_string;
	char	   *saved_stmt_name;
	ParamListInfo params;
	MemoryContext oldContext;
	bool		save_log_statement_stats = log_statement_stats;
	bool		snapshot_set = false;
	char		msec_str[32];

	/* Get the fixed part of the message */
	portal_name = pq_getmsgstring(input_message);
	stmt_name = pq_getmsgstring(input_message);

	ereport(DEBUG2,
			(errmsg("bind %s to %s",
					*portal_name ? portal_name : "<unnamed>",
					*stmt_name ? stmt_name : "<unnamed>")));

	/* Find prepared statement */
	if (stmt_name[0] != '\0')
	{
		PreparedStatement *pstmt;

		pstmt = FetchPreparedStatement(stmt_name, true);
		psrc = pstmt->plansource;
	}
	else
	{
		/* special-case the unnamed statement */
		psrc = unnamed_stmt_psrc;
		if (!psrc)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_PSTATEMENT),
					 errmsg("unnamed prepared statement does not exist")));
	}

	/*
	 * Report query to various monitoring facilities.
	 */
	debug_query_string = psrc->query_string;

	pgstat_report_activity(STATE_RUNNING, psrc->query_string);

	set_ps_display("BIND", false);

	if (save_log_statement_stats)
		ResetUsage();

	/*
	 * Start up a transaction command so we can call functions etc. (Note that
	 * this will normally change current memory context.) Nothing happens if
	 * we are already in one.
	 */
	start_xact_command();

	/* Switch back to message context */
	MemoryContextSwitchTo(MessageContext);

	/* Get the parameter format codes */
	numPFormats = pq_getmsgint(input_message, 2);
	if (numPFormats > 0)
	{
		int			i;

		pformats = (int16 *) palloc(numPFormats * sizeof(int16));
		for (i = 0; i < numPFormats; i++)
			pformats[i] = pq_getmsgint(input_message, 2);
	}

	/* Get the parameter value count */
	numParams = pq_getmsgint(input_message, 2);

	if (numPFormats > 1 && numPFormats != numParams)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("bind message has %d parameter formats but %d parameters",
						numPFormats, numParams)));

	if (numParams != psrc->num_params)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("bind message supplies %d parameters, but prepared statement \"%s\" requires %d",
						numParams, stmt_name, psrc->num_params)));

	/*
	 * If we are in aborted transaction state, the only portals we can
	 * actually run are those containing COMMIT or ROLLBACK commands. We
	 * disallow binding anything else to avoid problems with infrastructure
	 * that expects to run inside a valid transaction.  We also disallow
	 * binding any parameters, since we can't risk calling user-defined I/O
	 * functions.
	 */
	if (IsAbortedTransactionBlockState() &&
		(!(psrc->raw_parse_tree &&
		   IsTransactionExitStmt(psrc->raw_parse_tree->stmt)) ||
		 numParams != 0))
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block"),
				 errdetail_abort()));

	/*
	 * Create the portal.  Allow silent replacement of an existing portal only
	 * if the unnamed portal is specified.
	 */
	if (portal_name[0] == '\0')
		portal = CreatePortal(portal_name, true, true);
	else
		portal = CreatePortal(portal_name, false, false);

	/*
	 * Prepare to copy stuff into the portal's memory context.  We do all this
	 * copying first, because it could possibly fail (out-of-memory) and we
	 * don't want a failure to occur between GetCachedPlan and
	 * PortalDefineQuery; that would result in leaking our plancache refcount.
	 */
	oldContext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));

	/* Copy the plan's query string into the portal */
	query_string = pstrdup(psrc->query_string);

	/* Likewise make a copy of the statement name, unless it's unnamed */
	if (stmt_name[0])
		saved_stmt_name = pstrdup(stmt_name);
	else
		saved_stmt_name = NULL;

	/*
	 * Set a snapshot if we have parameters to fetch (since the input
	 * functions might need it) or the query isn't a utility command (and
	 * hence could require redoing parse analysis and planning).  We keep the
	 * snapshot active till we're done, so that plancache.c doesn't have to
	 * take new ones.
	 */
	if (numParams > 0 ||
		(psrc->raw_parse_tree &&
		 analyze_requires_snapshot(psrc->raw_parse_tree)))
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		snapshot_set = true;
	}

	/*
	 * Fetch parameters, if any, and store in the portal's memory context.
	 */
	if (numParams > 0)
	{
		int			paramno;

		params = (ParamListInfo) palloc(offsetof(ParamListInfoData, params) +
										numParams * sizeof(ParamExternData));
		/* we have static list of params, so no hooks needed */
		params->paramFetch = NULL;
		params->paramFetchArg = NULL;
		params->parserSetup = NULL;
		params->parserSetupArg = NULL;
		params->numParams = numParams;
		params->paramMask = NULL;

		for (paramno = 0; paramno < numParams; paramno++)
		{
			Oid			ptype = psrc->param_types[paramno];
			int32		plength;
			Datum		pval;
			bool		isNull;
			StringInfoData pbuf;
			char		csave;
			int16		pformat;

			plength = pq_getmsgint(input_message, 4);
			isNull = (plength == -1);

			if (!isNull)
			{
				const char *pvalue = pq_getmsgbytes(input_message, plength);

				/*
				 * Rather than copying data around, we just set up a phony
				 * StringInfo pointing to the correct portion of the message
				 * buffer.  We assume we can scribble on the message buffer so
				 * as to maintain the convention that StringInfos have a
				 * trailing null.  This is grotty but is a big win when
				 * dealing with very large parameter strings.
				 */
				pbuf.data = (char *) pvalue;
				pbuf.maxlen = plength + 1;
				pbuf.len = plength;
				pbuf.cursor = 0;

				csave = pbuf.data[plength];
				pbuf.data[plength] = '\0';
			}
			else
			{
				pbuf.data = NULL;	/* keep compiler quiet */
				csave = 0;
			}

			if (numPFormats > 1)
				pformat = pformats[paramno];
			else if (numPFormats > 0)
				pformat = pformats[0];
			else
				pformat = 0;	/* default = text */

			if (pformat == 0)	/* text mode */
			{
				Oid			typinput;
				Oid			typioparam;
				char	   *pstring;

				getTypeInputInfo(ptype, &typinput, &typioparam);

				/*
				 * We have to do encoding conversion before calling the
				 * typinput routine.
				 */
				if (isNull)
					pstring = NULL;
				else
					pstring = pg_client_to_server(pbuf.data, plength);

				pval = OidInputFunctionCall(typinput, pstring, typioparam, -1);

				/* Free result of encoding conversion, if any */
				if (pstring && pstring != pbuf.data)
					pfree(pstring);
			}
			else if (pformat == 1)	/* binary mode */
			{
				Oid			typreceive;
				Oid			typioparam;
				StringInfo	bufptr;

				/*
				 * Call the parameter type's binary input converter
				 */
				getTypeBinaryInputInfo(ptype, &typreceive, &typioparam);

				if (isNull)
					bufptr = NULL;
				else
					bufptr = &pbuf;

				pval = OidReceiveFunctionCall(typreceive, bufptr, typioparam, -1);

				/* Trouble if it didn't eat the whole buffer */
				if (!isNull && pbuf.cursor != pbuf.len)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
							 errmsg("incorrect binary data format in bind parameter %d",
									paramno + 1)));
			}
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unsupported format code: %d",
								pformat)));
				pval = 0;		/* keep compiler quiet */
			}

			/* Restore message buffer contents */
			if (!isNull)
				pbuf.data[plength] = csave;

			params->params[paramno].value = pval;
			params->params[paramno].isnull = isNull;

			/*
			 * We mark the params as CONST.  This ensures that any custom plan
			 * makes full use of the parameter values.
			 */
			params->params[paramno].pflags = PARAM_FLAG_CONST;
			params->params[paramno].ptype = ptype;
		}
	}
	else
		params = NULL;

	/* Done storing stuff in portal's context */
	MemoryContextSwitchTo(oldContext);

	/* Get the result format codes */
	numRFormats = pq_getmsgint(input_message, 2);
	if (numRFormats > 0)
	{
		int			i;

		rformats = (int16 *) palloc(numRFormats * sizeof(int16));
		for (i = 0; i < numRFormats; i++)
			rformats[i] = pq_getmsgint(input_message, 2);
	}

	pq_getmsgend(input_message);

	/*
	 * Obtain a plan from the CachedPlanSource.  Any cruft from (re)planning
	 * will be generated in MessageContext.  The plan refcount will be
	 * assigned to the Portal, so it will be released at portal destruction.
	 */
	cplan = GetCachedPlan(psrc, params, false, NULL);

	/*
	 * Now we can define the portal.
	 *
	 * DO NOT put any code that could possibly throw an error between the
	 * above GetCachedPlan call and here.
	 */
	PortalDefineQuery(portal,
					  saved_stmt_name,
					  query_string,
					  psrc->commandTag,
					  cplan->stmt_list,
					  cplan);

	/* Done with the snapshot used for parameter I/O and parsing/planning */
	if (snapshot_set)
		PopActiveSnapshot();

	/*
	 * And we're ready to start portal execution.
	 */
	PortalStart(portal, params, 0, InvalidSnapshot);

	/*
	 * Apply the result format requests to the portal.
	 */
	PortalSetResultFormat(portal, numRFormats, rformats);

	/*
	 * Send BindComplete.
	 */
	if (whereToSendOutput == DestRemote)
		pq_putemptymessage('2');

	/*
	 * Emit duration logging if appropriate.
	 */
	switch (check_log_duration(msec_str, false))
	{
		case 1:
			ereport(LOG,
					(errmsg("duration: %s ms", msec_str),
					 errhidestmt(true)));
			break;
		case 2:
			ereport(LOG,
					(errmsg("duration: %s ms  bind %s%s%s: %s",
							msec_str,
							*stmt_name ? stmt_name : "<unnamed>",
							*portal_name ? "/" : "",
							*portal_name ? portal_name : "",
							psrc->query_string),
					 errhidestmt(true),
					 errdetail_params(params)));
			break;
	}

	if (save_log_statement_stats)
		ShowUsage("BIND MESSAGE STATISTICS");

	debug_query_string = NULL;
}

/*
 * exec_execute_message
 *
 * Process an "Execute" message for a portal
 */
static void
exec_execute_message(const char *portal_name, long max_rows)
{
	CommandDest dest;
	DestReceiver *receiver;
	Portal		portal;
	bool		completed;
	char		completionTag[COMPLETION_TAG_BUFSIZE];
	const char *sourceText;
	const char *prepStmtName;
	ParamListInfo portalParams;
	bool		save_log_statement_stats = log_statement_stats;
	bool		is_xact_command;
	bool		execute_is_fetch;
	bool		was_logged = false;
	char		msec_str[32];

	/* Adjust destination to tell printtup.c what to do */
	dest = whereToSendOutput;
	if (dest == DestRemote)
		dest = DestRemoteExecute;

	portal = GetPortalByName(portal_name);
	if (!PortalIsValid(portal))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("portal \"%s\" does not exist", portal_name)));

	/*
	 * If the original query was a null string, just return
	 * EmptyQueryResponse.
	 */
	if (portal->commandTag == NULL)
	{
		Assert(portal->stmts == NIL);
		NullCommand(dest);
		return;
	}

	/* Does the portal contain a transaction command? */
	is_xact_command = IsTransactionStmtList(portal->stmts);

	/*
	 * We must copy the sourceText and prepStmtName into MessageContext in
	 * case the portal is destroyed during finish_xact_command. Can avoid the
	 * copy if it's not an xact command, though.
	 */
	if (is_xact_command)
	{
		sourceText = pstrdup(portal->sourceText);
		if (portal->prepStmtName)
			prepStmtName = pstrdup(portal->prepStmtName);
		else
			prepStmtName = "<unnamed>";

		/*
		 * An xact command shouldn't have any parameters, which is a good
		 * thing because they wouldn't be around after finish_xact_command.
		 */
		portalParams = NULL;
	}
	else
	{
		sourceText = portal->sourceText;
		if (portal->prepStmtName)
			prepStmtName = portal->prepStmtName;
		else
			prepStmtName = "<unnamed>";
		portalParams = portal->portalParams;
	}

	/*
	 * Report query to various monitoring facilities.
	 */
	debug_query_string = sourceText;

	pgstat_report_activity(STATE_RUNNING, sourceText);

	set_ps_display(portal->commandTag, false);

	if (save_log_statement_stats)
		ResetUsage();

	BeginCommand(portal->commandTag, dest);

	/*
	 * Create dest receiver in MessageContext (we don't want it in transaction
	 * context, because that may get deleted if portal contains VACUUM).
	 */
	receiver = CreateDestReceiver(dest);
	if (dest == DestRemoteExecute)
		SetRemoteDestReceiverParams(receiver, portal);

	/*
	 * Ensure we are in a transaction command (this should normally be the
	 * case already due to prior BIND).
	 */
	start_xact_command();

	/*
	 * If we re-issue an Execute protocol request against an existing portal,
	 * then we are only fetching more rows rather than completely re-executing
	 * the query from the start. atStart is never reset for a v3 portal, so we
	 * are safe to use this check.
	 */
	execute_is_fetch = !portal->atStart;

	/* Log immediately if dictated by log_statement */
	if (check_log_statement(portal->stmts))
	{
		ereport(LOG,
				(errmsg("%s %s%s%s: %s",
						execute_is_fetch ?
						_("execute fetch from") :
						_("execute"),
						prepStmtName,
						*portal_name ? "/" : "",
						*portal_name ? portal_name : "",
						sourceText),
				 errhidestmt(true),
				 errdetail_params(portalParams)));
		was_logged = true;
	}

	/*
	 * If we are in aborted transaction state, the only portals we can
	 * actually run are those containing COMMIT or ROLLBACK commands.
	 */
	if (IsAbortedTransactionBlockState() &&
		!IsTransactionExitStmtList(portal->stmts))
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block"),
				 errdetail_abort()));

	/* Check for cancel signal before we start execution */
	CHECK_FOR_INTERRUPTS();

	/*
	 * Okay to run the portal.
	 */
	if (max_rows <= 0)
		max_rows = FETCH_ALL;

	completed = PortalRun(portal,
						  max_rows,
						  true, /* always top level */
						  !execute_is_fetch && max_rows == FETCH_ALL,
						  receiver,
						  receiver,
						  completionTag);

	(*receiver->rDestroy) (receiver);

	if (completed)
	{
		if (is_xact_command)
		{
			/*
			 * If this was a transaction control statement, commit it.  We
			 * will start a new xact command for the next command (if any).
			 */
			finish_xact_command();
		}
		else
		{
			/*
			 * We need a CommandCounterIncrement after every query, except
			 * those that start or end a transaction block.
			 */
			CommandCounterIncrement();
		}

		/* Send appropriate CommandComplete to client */
		EndCommand(completionTag, dest);
	}
	else
	{
		/* Portal run not complete, so send PortalSuspended */
		if (whereToSendOutput == DestRemote)
			pq_putemptymessage('s');
	}

	/*
	 * Emit duration logging if appropriate.
	 */
	switch (check_log_duration(msec_str, was_logged))
	{
		case 1:
			ereport(LOG,
					(errmsg("duration: %s ms", msec_str),
					 errhidestmt(true)));
			break;
		case 2:
			ereport(LOG,
					(errmsg("duration: %s ms  %s %s%s%s: %s",
							msec_str,
							execute_is_fetch ?
							_("execute fetch from") :
							_("execute"),
							prepStmtName,
							*portal_name ? "/" : "",
							*portal_name ? portal_name : "",
							sourceText),
					 errhidestmt(true),
					 errdetail_params(portalParams)));
			break;
	}

	if (save_log_statement_stats)
		ShowUsage("EXECUTE MESSAGE STATISTICS");

	debug_query_string = NULL;
}

/*
 * check_log_statement
 *		Determine whether command should be logged because of log_statement
 *
 * stmt_list can be either raw grammar output or a list of planned
 * statements
 */
static bool
check_log_statement(List *stmt_list)
{
	ListCell   *stmt_item;

	if (log_statement == LOGSTMT_NONE)
		return false;
	if (log_statement == LOGSTMT_ALL)
		return true;

	/* Else we have to inspect the statement(s) to see whether to log */
	foreach(stmt_item, stmt_list)
	{
		Node	   *stmt = (Node *) lfirst(stmt_item);

		if (GetCommandLogLevel(stmt) <= log_statement)
			return true;
	}

	return false;
}

/*
 * check_log_duration
 *		Determine whether current command's duration should be logged
 *
 * Returns:
 *		0 if no logging is needed
 *		1 if just the duration should be logged
 *		2 if duration and query details should be logged
 *
 * If logging is needed, the duration in msec is formatted into msec_str[],
 * which must be a 32-byte buffer.
 *
 * was_logged should be TRUE if caller already logged query details (this
 * essentially prevents 2 from being returned).
 */
int
check_log_duration(char *msec_str, bool was_logged)
{
	if (log_duration || log_min_duration_statement >= 0)
	{
		long		secs;
		int			usecs;
		int			msecs;
		bool		exceeded;

		TimestampDifference(GetCurrentStatementStartTimestamp(),
							GetCurrentTimestamp(),
							&secs, &usecs);
		msecs = usecs / 1000;

		/*
		 * This odd-looking test for log_min_duration_statement being exceeded
		 * is designed to avoid integer overflow with very long durations:
		 * don't compute secs * 1000 until we've verified it will fit in int.
		 */
		exceeded = (log_min_duration_statement == 0 ||
					(log_min_duration_statement > 0 &&
					 (secs > log_min_duration_statement / 1000 ||
					  secs * 1000 + msecs >= log_min_duration_statement)));

		if (exceeded || log_duration)
		{
			snprintf(msec_str, 32, "%ld.%03d",
					 secs * 1000 + msecs, usecs % 1000);
			if (exceeded && !was_logged)
				return 2;
			else
				return 1;
		}
	}

	return 0;
}

/*
 * errdetail_execute
 *
 * Add an errdetail() line showing the query referenced by an EXECUTE, if any.
 * The argument is the raw parsetree list.
 */
static int
errdetail_execute(List *raw_parsetree_list)
{
	ListCell   *parsetree_item;

	foreach(parsetree_item, raw_parsetree_list)
	{
		RawStmt    *parsetree = lfirst_node(RawStmt, parsetree_item);

		if (IsA(parsetree->stmt, ExecuteStmt))
		{
			ExecuteStmt *stmt = (ExecuteStmt *) parsetree->stmt;
			PreparedStatement *pstmt;

			pstmt = FetchPreparedStatement(stmt->name, false);
			if (pstmt)
			{
				errdetail("prepare: %s", pstmt->plansource->query_string);
				return 0;
			}
		}
	}

	return 0;
}

/*
 * errdetail_params
 *
 * Add an errdetail() line showing bind-parameter data, if available.
 */
static int
errdetail_params(ParamListInfo params)
{
	/* We mustn't call user-defined I/O functions when in an aborted xact */
	if (params && params->numParams > 0 && !IsAbortedTransactionBlockState())
	{
		StringInfoData param_str;
		MemoryContext oldcontext;
		int			paramno;

		/* Make sure any trash is generated in MessageContext */
		oldcontext = MemoryContextSwitchTo(MessageContext);

		initStringInfo(&param_str);

		for (paramno = 0; paramno < params->numParams; paramno++)
		{
			ParamExternData *prm = &params->params[paramno];
			Oid			typoutput;
			bool		typisvarlena;
			char	   *pstring;
			char	   *p;

			appendStringInfo(&param_str, "%s$%d = ",
							 paramno > 0 ? ", " : "",
							 paramno + 1);

			if (prm->isnull || !OidIsValid(prm->ptype))
			{
				appendStringInfoString(&param_str, "NULL");
				continue;
			}

			getTypeOutputInfo(prm->ptype, &typoutput, &typisvarlena);

			pstring = OidOutputFunctionCall(typoutput, prm->value);

			appendStringInfoCharMacro(&param_str, '\'');
			for (p = pstring; *p; p++)
			{
				if (*p == '\'') /* double single quotes */
					appendStringInfoCharMacro(&param_str, *p);
				appendStringInfoCharMacro(&param_str, *p);
			}
			appendStringInfoCharMacro(&param_str, '\'');

			pfree(pstring);
		}

		errdetail("parameters: %s", param_str.data);

		pfree(param_str.data);

		MemoryContextSwitchTo(oldcontext);
	}

	return 0;
}

/*
 * errdetail_abort
 *
 * Add an errdetail() line showing abort reason, if any.
 */
static int
errdetail_abort(void)
{
	if (MyProc->recoveryConflictPending)
		errdetail("abort reason: recovery conflict");

	return 0;
}

/*
 * errdetail_recovery_conflict
 *
 * Add an errdetail() line showing conflict source.
 */
static int
errdetail_recovery_conflict(void)
{
	switch (RecoveryConflictReason)
	{
		case PROCSIG_RECOVERY_CONFLICT_BUFFERPIN:
			errdetail("User was holding shared buffer pin for too long.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_LOCK:
			errdetail("User was holding a relation lock for too long.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_TABLESPACE:
			errdetail("User was or might have been using tablespace that must be dropped.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_SNAPSHOT:
			errdetail("User query might have needed to see row versions that must be removed.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK:
			errdetail("User transaction caused buffer deadlock with recovery.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_DATABASE:
			errdetail("User was connected to a database that must be dropped.");
			break;
		default:
			break;
			/* no errdetail */
	}

	return 0;
}

/*
 * exec_describe_statement_message
 *
 * Process a "Describe" message for a prepared statement
 */
static void
exec_describe_statement_message(const char *stmt_name)
{
	CachedPlanSource *psrc;
	StringInfoData buf;
	int			i;

	/*
	 * Start up a transaction command. (Note that this will normally change
	 * current memory context.) Nothing happens if we are already in one.
	 */
	start_xact_command();

	/* Switch back to message context */
	MemoryContextSwitchTo(MessageContext);

	/* Find prepared statement */
	if (stmt_name[0] != '\0')
	{
		PreparedStatement *pstmt;

		pstmt = FetchPreparedStatement(stmt_name, true);
		psrc = pstmt->plansource;
	}
	else
	{
		/* special-case the unnamed statement */
		psrc = unnamed_stmt_psrc;
		if (!psrc)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_PSTATEMENT),
					 errmsg("unnamed prepared statement does not exist")));
	}

	/* Prepared statements shouldn't have changeable result descs */
	Assert(psrc->fixed_result);

	/*
	 * If we are in aborted transaction state, we can't run
	 * SendRowDescriptionMessage(), because that needs catalog accesses.
	 * Hence, refuse to Describe statements that return data.  (We shouldn't
	 * just refuse all Describes, since that might break the ability of some
	 * clients to issue COMMIT or ROLLBACK commands, if they use code that
	 * blindly Describes whatever it does.)  We can Describe parameters
	 * without doing anything dangerous, so we don't restrict that.
	 */
	if (IsAbortedTransactionBlockState() &&
		psrc->resultDesc)
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block"),
				 errdetail_abort()));

	if (whereToSendOutput != DestRemote)
		return;					/* can't actually do anything... */

	/*
	 * First describe the parameters...
	 */
	pq_beginmessage(&buf, 't'); /* parameter description message type */
	pq_sendint(&buf, psrc->num_params, 2);

	for (i = 0; i < psrc->num_params; i++)
	{
		Oid			ptype = psrc->param_types[i];

		pq_sendint(&buf, (int) ptype, 4);
	}
	pq_endmessage(&buf);

	/*
	 * Next send RowDescription or NoData to describe the result...
	 */
	if (psrc->resultDesc)
	{
		List	   *tlist;

		/* Get the plan's primary targetlist */
		tlist = CachedPlanGetTargetList(psrc, NULL);

		SendRowDescriptionMessage(psrc->resultDesc, tlist, NULL);
	}
	else
		pq_putemptymessage('n');	/* NoData */

}

/*
 * exec_describe_portal_message
 *
 * Process a "Describe" message for a portal
 */
static void
exec_describe_portal_message(const char *portal_name)
{
	Portal		portal;

	/*
	 * Start up a transaction command. (Note that this will normally change
	 * current memory context.) Nothing happens if we are already in one.
	 */
	start_xact_command();

	/* Switch back to message context */
	MemoryContextSwitchTo(MessageContext);

	portal = GetPortalByName(portal_name);
	if (!PortalIsValid(portal))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("portal \"%s\" does not exist", portal_name)));

	/*
	 * If we are in aborted transaction state, we can't run
	 * SendRowDescriptionMessage(), because that needs catalog accesses.
	 * Hence, refuse to Describe portals that return data.  (We shouldn't just
	 * refuse all Describes, since that might break the ability of some
	 * clients to issue COMMIT or ROLLBACK commands, if they use code that
	 * blindly Describes whatever it does.)
	 */
	if (IsAbortedTransactionBlockState() &&
		portal->tupDesc)
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block"),
				 errdetail_abort()));

	if (whereToSendOutput != DestRemote)
		return;					/* can't actually do anything... */

	if (portal->tupDesc)
		SendRowDescriptionMessage(portal->tupDesc,
								  FetchPortalTargetList(portal),
								  portal->formats);
	else
		pq_putemptymessage('n');	/* NoData */
}


/*
 * Convenience routines for starting/committing a single command.
 */
static void
start_xact_command(void)
{
	if (!xact_started)
	{
		StartTransactionCommand();

		/* Set statement timeout running, if any */
		/* NB: this mustn't be enabled until we are within an xact */
		if (StatementTimeout > 0)
			enable_timeout_after(STATEMENT_TIMEOUT, StatementTimeout);
		else
			disable_timeout(STATEMENT_TIMEOUT, false);

		xact_started = true;
	}
}

static void
finish_xact_command(void)
{
	if (xact_started)
	{
		/* Cancel any active statement timeout before committing */
		disable_timeout(STATEMENT_TIMEOUT, false);

		CommitTransactionCommand();

#ifdef MEMORY_CONTEXT_CHECKING
		/* Check all memory contexts that weren't freed during commit */
		/* (those that were, were checked before being deleted) */
		MemoryContextCheck(TopMemoryContext);
#endif

#ifdef SHOW_MEMORY_STATS
		/* Print mem stats after each commit for leak tracking */
		MemoryContextStats(TopMemoryContext);
#endif

		xact_started = false;
	}
}


/*
 * Convenience routines for checking whether a statement is one of the
 * ones that we allow in transaction-aborted state.
 */

/* Test a bare parsetree */
static bool
IsTransactionExitStmt(Node *parsetree)
{
	if (parsetree && IsA(parsetree, TransactionStmt))
	{
		TransactionStmt *stmt = (TransactionStmt *) parsetree;

		if (stmt->kind == TRANS_STMT_COMMIT ||
			stmt->kind == TRANS_STMT_PREPARE ||
			stmt->kind == TRANS_STMT_ROLLBACK ||
			stmt->kind == TRANS_STMT_ROLLBACK_TO)
			return true;
	}
	return false;
}

/* Test a list that contains PlannedStmt nodes */
static bool
IsTransactionExitStmtList(List *pstmts)
{
	if (list_length(pstmts) == 1)
	{
		PlannedStmt *pstmt = linitial_node(PlannedStmt, pstmts);

		if (pstmt->commandType == CMD_UTILITY &&
			IsTransactionExitStmt(pstmt->utilityStmt))
			return true;
	}
	return false;
}

/* Test a list that contains PlannedStmt nodes */
static bool
IsTransactionStmtList(List *pstmts)
{
	if (list_length(pstmts) == 1)
	{
		PlannedStmt *pstmt = linitial_node(PlannedStmt, pstmts);

		if (pstmt->commandType == CMD_UTILITY &&
			IsA(pstmt->utilityStmt, TransactionStmt))
			return true;
	}
	return false;
}

/* Release any existing unnamed prepared statement */
static void
drop_unnamed_stmt(void)
{
	/* paranoia to avoid a dangling pointer in case of error */
	if (unnamed_stmt_psrc)
	{
		CachedPlanSource *psrc = unnamed_stmt_psrc;

		unnamed_stmt_psrc = NULL;
		DropCachedPlan(psrc);
	}
}


/* --------------------------------
 *		signal handler routines used in PostgresMain()
 * --------------------------------
 */

/*
 * quickdie() occurs when signalled SIGQUIT by the postmaster.
 *
 * Some backend has bought the farm,
 * so we need to stop what we're doing and exit.
 */
void
quickdie(SIGNAL_ARGS)
{
	sigaddset(&BlockSig, SIGQUIT);	/* prevent nested calls */
	PG_SETMASK(&BlockSig);

	/*
	 * Prevent interrupts while exiting; though we just blocked signals that
	 * would queue new interrupts, one may have been pending.  We don't want a
	 * quickdie() downgraded to a mere query cancel.
	 */
	HOLD_INTERRUPTS();

	/*
	 * If we're aborting out of client auth, don't risk trying to send
	 * anything to the client; we will likely violate the protocol, not to
	 * mention that we may have interrupted the guts of OpenSSL or some
	 * authentication library.
	 */
	if (ClientAuthInProgress && whereToSendOutput == DestRemote)
		whereToSendOutput = DestNone;

	/*
	 * Ideally this should be ereport(FATAL), but then we'd not get control
	 * back...
	 */
	ereport(WARNING,
			(errcode(ERRCODE_CRASH_SHUTDOWN),
			 errmsg("terminating connection because of crash of another server process"),
			 errdetail("The postmaster has commanded this server process to roll back"
					   " the current transaction and exit, because another"
					   " server process exited abnormally and possibly corrupted"
					   " shared memory."),
			 errhint("In a moment you should be able to reconnect to the"
					 " database and repeat your command.")));

	/*
	 * We DO NOT want to run proc_exit() callbacks -- we're here because
	 * shared memory may be corrupted, so we don't want to try to clean up our
	 * transaction.  Just nail the windows shut and get out of town.  Now that
	 * there's an atexit callback to prevent third-party code from breaking
	 * things by calling exit() directly, we have to reset the callbacks
	 * explicitly to make this work as intended.
	 */
	on_exit_reset();

	/*
	 * Note we do exit(2) not exit(0).  This is to force the postmaster into a
	 * system reset cycle if some idiot DBA sends a manual SIGQUIT to a random
	 * backend.  This is necessary precisely because we don't clean up our
	 * shared memory state.  (The "dead man switch" mechanism in pmsignal.c
	 * should ensure the postmaster sees this as a crash, too, but no harm in
	 * being doubly sure.)
	 */
	exit(2);
}

/*
 * Shutdown signal from postmaster: abort transaction and exit
 * at soonest convenient time
 */
void
die(SIGNAL_ARGS)
{
	int			save_errno = errno;

	/* Don't joggle the elbow of proc_exit */
	if (!proc_exit_inprogress)
	{
		InterruptPending = true;
		ProcDiePending = true;
	}

	/* If we're still here, waken anything waiting on the process latch */
	SetLatch(MyLatch);

	/*
	 * If we're in single user mode, we want to quit immediately - we can't
	 * rely on latches as they wouldn't work when stdin/stdout is a file.
	 * Rather ugly, but it's unlikely to be worthwhile to invest much more
	 * effort just for the benefit of single user mode.
	 */
	if (DoingCommandRead && whereToSendOutput != DestRemote)
		ProcessInterrupts();

	errno = save_errno;
}

/*
 * Query-cancel signal from postmaster: abort current transaction
 * at soonest convenient time
 */
void
StatementCancelHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	/*
	 * Don't joggle the elbow of proc_exit
	 */
	if (!proc_exit_inprogress)
	{
		InterruptPending = true;
		QueryCancelPending = true;
	}

	/* If we're still here, waken anything waiting on the process latch */
	SetLatch(MyLatch);

	errno = save_errno;
}

/* signal handler for floating point exception */
void
FloatExceptionHandler(SIGNAL_ARGS)
{
	/* We're not returning, so no need to save errno */
	ereport(ERROR,
			(errcode(ERRCODE_FLOATING_POINT_EXCEPTION),
			 errmsg("floating-point exception"),
			 errdetail("An invalid floating-point operation was signaled. "
					   "This probably means an out-of-range result or an "
					   "invalid operation, such as division by zero.")));
}

/*
 * SIGHUP: set flag to re-read config file at next convenient time.
 *
 * Sets the ConfigReloadPending flag, which should be checked at convenient
 * places inside main loops. (Better than doing the reading in the signal
 * handler, ey?)
 */
void
PostgresSigHupHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	ConfigReloadPending = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * RecoveryConflictInterrupt: out-of-line portion of recovery conflict
 * handling following receipt of SIGUSR1. Designed to be similar to die()
 * and StatementCancelHandler(). Called only by a normal user backend
 * that begins a transaction during recovery.
 */
void
RecoveryConflictInterrupt(ProcSignalReason reason)
{
	int			save_errno = errno;

	/*
	 * Don't joggle the elbow of proc_exit
	 */
	if (!proc_exit_inprogress)
	{
		RecoveryConflictReason = reason;
		switch (reason)
		{
			case PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK:

				/*
				 * If we aren't waiting for a lock we can never deadlock.
				 */
				if (!IsWaitingForLock())
					return;

				/* Intentional drop through to check wait for pin */

			case PROCSIG_RECOVERY_CONFLICT_BUFFERPIN:

				/*
				 * If we aren't blocking the Startup process there is nothing
				 * more to do.
				 */
				if (!HoldingBufferPinThatDelaysRecovery())
					return;

				MyProc->recoveryConflictPending = true;

				/* Intentional drop through to error handling */

			case PROCSIG_RECOVERY_CONFLICT_LOCK:
			case PROCSIG_RECOVERY_CONFLICT_TABLESPACE:
			case PROCSIG_RECOVERY_CONFLICT_SNAPSHOT:

				/*
				 * If we aren't in a transaction any longer then ignore.
				 */
				if (!IsTransactionOrTransactionBlock())
					return;

				/*
				 * If we can abort just the current subtransaction then we are
				 * OK to throw an ERROR to resolve the conflict. Otherwise
				 * drop through to the FATAL case.
				 *
				 * XXX other times that we can throw just an ERROR *may* be
				 * PROCSIG_RECOVERY_CONFLICT_LOCK if no locks are held in
				 * parent transactions
				 *
				 * PROCSIG_RECOVERY_CONFLICT_SNAPSHOT if no snapshots are held
				 * by parent transactions and the transaction is not
				 * transaction-snapshot mode
				 *
				 * PROCSIG_RECOVERY_CONFLICT_TABLESPACE if no temp files or
				 * cursors open in parent transactions
				 */
				if (!IsSubTransaction())
				{
					/*
					 * If we already aborted then we no longer need to cancel.
					 * We do this here since we do not wish to ignore aborted
					 * subtransactions, which must cause FATAL, currently.
					 */
					if (IsAbortedTransactionBlockState())
						return;

					RecoveryConflictPending = true;
					QueryCancelPending = true;
					InterruptPending = true;
					break;
				}

				/* Intentional drop through to session cancel */

			case PROCSIG_RECOVERY_CONFLICT_DATABASE:
				RecoveryConflictPending = true;
				ProcDiePending = true;
				InterruptPending = true;
				break;

			default:
				elog(FATAL, "unrecognized conflict mode: %d",
					 (int) reason);
		}

		Assert(RecoveryConflictPending && (QueryCancelPending || ProcDiePending));

		/*
		 * All conflicts apart from database cause dynamic errors where the
		 * command or transaction can be retried at a later point with some
		 * potential for success. No need to reset this, since non-retryable
		 * conflict errors are currently FATAL.
		 */
		if (reason == PROCSIG_RECOVERY_CONFLICT_DATABASE)
			RecoveryConflictRetryable = false;
	}

	/*
	 * Set the process latch. This function essentially emulates signal
	 * handlers like die() and StatementCancelHandler() and it seems prudent
	 * to behave similarly as they do.
	 */
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * ProcessInterrupts: out-of-line portion of CHECK_FOR_INTERRUPTS() macro
 *
 * If an interrupt condition is pending, and it's safe to service it,
 * then clear the flag and accept the interrupt.  Called only when
 * InterruptPending is true.
 */
void
ProcessInterrupts(void)
{
	/* OK to accept any interrupts now? */
	if (InterruptHoldoffCount != 0 || CritSectionCount != 0)
		return;
	InterruptPending = false;

	if (ProcDiePending)
	{
		ProcDiePending = false;
		QueryCancelPending = false; /* ProcDie trumps QueryCancel */
		LockErrorCleanup();
		/* As in quickdie, don't risk sending to client during auth */
		if (ClientAuthInProgress && whereToSendOutput == DestRemote)
			whereToSendOutput = DestNone;
		if (ClientAuthInProgress)
			ereport(FATAL,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("canceling authentication due to timeout")));
		else if (IsAutoVacuumWorkerProcess())
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating autovacuum process due to administrator command")));
		else if (IsLogicalWorker())
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating logical replication worker due to administrator command")));
		else if (IsLogicalLauncher())
		{
			ereport(DEBUG1,
					(errmsg("logical replication launcher shutting down")));

			/*
			 * The logical replication launcher can be stopped at any time.
			 * Use exit status 1 so the background worker is restarted.
			 */
			proc_exit(1);
		}
		else if (RecoveryConflictPending && RecoveryConflictRetryable)
		{
			pgstat_report_recovery_conflict(RecoveryConflictReason);
			ereport(FATAL,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("terminating connection due to conflict with recovery"),
					 errdetail_recovery_conflict()));
		}
		else if (RecoveryConflictPending)
		{
			/* Currently there is only one non-retryable recovery conflict */
			Assert(RecoveryConflictReason == PROCSIG_RECOVERY_CONFLICT_DATABASE);
			pgstat_report_recovery_conflict(RecoveryConflictReason);
			ereport(FATAL,
					(errcode(ERRCODE_DATABASE_DROPPED),
					 errmsg("terminating connection due to conflict with recovery"),
					 errdetail_recovery_conflict()));
		}
		else
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating connection due to administrator command")));
	}
	if (ClientConnectionLost)
	{
		QueryCancelPending = false; /* lost connection trumps QueryCancel */
		LockErrorCleanup();
		/* don't send to client, we already know the connection to be dead. */
		whereToSendOutput = DestNone;
		ereport(FATAL,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("connection to client lost")));
	}

	/*
	 * If a recovery conflict happens while we are waiting for input from the
	 * client, the client is presumably just sitting idle in a transaction,
	 * preventing recovery from making progress.  Terminate the connection to
	 * dislodge it.
	 */
	if (RecoveryConflictPending && DoingCommandRead)
	{
		QueryCancelPending = false; /* this trumps QueryCancel */
		RecoveryConflictPending = false;
		LockErrorCleanup();
		pgstat_report_recovery_conflict(RecoveryConflictReason);
		ereport(FATAL,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("terminating connection due to conflict with recovery"),
				 errdetail_recovery_conflict(),
				 errhint("In a moment you should be able to reconnect to the"
						 " database and repeat your command.")));
	}

	/*
	 * Don't allow query cancel interrupts while reading input from the
	 * client, because we might lose sync in the FE/BE protocol.  (Die
	 * interrupts are OK, because we won't read any further messages from
	 * the client in that case.)
	 */
	if (QueryCancelPending && QueryCancelHoldoffCount != 0)
	{
		/*
		 * Re-arm InterruptPending so that we process the cancel request
		 * as soon as we're done reading the message.
		 */
		InterruptPending = true;
	}
	else if (QueryCancelPending)
	{
		bool		lock_timeout_occurred;
		bool		stmt_timeout_occurred;

		QueryCancelPending = false;

		/*
		 * If LOCK_TIMEOUT and STATEMENT_TIMEOUT indicators are both set, we
		 * need to clear both, so always fetch both.
		 */
		lock_timeout_occurred = get_timeout_indicator(LOCK_TIMEOUT, true);
		stmt_timeout_occurred = get_timeout_indicator(STATEMENT_TIMEOUT, true);

		/*
		 * If both were set, we want to report whichever timeout completed
		 * earlier; this ensures consistent behavior if the machine is slow
		 * enough that the second timeout triggers before we get here.  A tie
		 * is arbitrarily broken in favor of reporting a lock timeout.
		 */
		if (lock_timeout_occurred && stmt_timeout_occurred &&
			get_timeout_finish_time(STATEMENT_TIMEOUT) < get_timeout_finish_time(LOCK_TIMEOUT))
			lock_timeout_occurred = false;	/* report stmt timeout */

		if (lock_timeout_occurred)
		{
			LockErrorCleanup();
			ereport(ERROR,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("canceling statement due to lock timeout")));
		}
		if (stmt_timeout_occurred)
		{
			LockErrorCleanup();
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("canceling statement due to statement timeout")));
		}
		if (IsAutoVacuumWorkerProcess())
		{
			LockErrorCleanup();
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("canceling autovacuum task")));
		}
		if (RecoveryConflictPending)
		{
			RecoveryConflictPending = false;
			LockErrorCleanup();
			pgstat_report_recovery_conflict(RecoveryConflictReason);
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("canceling statement due to conflict with recovery"),
					 errdetail_recovery_conflict()));
		}

		/*
		 * If we are reading a command from the client, just ignore the cancel
		 * request --- sending an extra error message won't accomplish
		 * anything.  Otherwise, go ahead and throw the error.
		 */
		if (!DoingCommandRead)
		{
			LockErrorCleanup();
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("canceling statement due to user request")));
		}
	}

	if (IdleInTransactionSessionTimeoutPending)
	{
		/* Has the timeout setting changed since last we looked? */
		if (IdleInTransactionSessionTimeout > 0)
			ereport(FATAL,
					(errcode(ERRCODE_IDLE_IN_TRANSACTION_SESSION_TIMEOUT),
					 errmsg("terminating connection due to idle-in-transaction timeout")));
		else
			IdleInTransactionSessionTimeoutPending = false;

	}

	if (ParallelMessagePending)
		HandleParallelMessages();
}


/*
 * IA64-specific code to fetch the AR.BSP register for stack depth checks.
 *
 * We currently support gcc, icc, and HP-UX's native compiler here.
 *
 * Note: while icc accepts gcc asm blocks on x86[_64], this is not true on
 * ia64 (at least not in icc versions before 12.x).  So we have to carry a
 * separate implementation for it.
 */
#if defined(__ia64__) || defined(__ia64)

#if defined(__hpux) && !defined(__GNUC__) && !defined(__INTEL_COMPILER)
/* Assume it's HP-UX native compiler */
#include <ia64/sys/inline.h>
#define ia64_get_bsp() ((char *) (_Asm_mov_from_ar(_AREG_BSP, _NO_FENCE)))
#elif defined(__INTEL_COMPILER)
/* icc */
#include <asm/ia64regs.h>
#define ia64_get_bsp() ((char *) __getReg(_IA64_REG_AR_BSP))
#else
/* gcc */
static __inline__ char *
ia64_get_bsp(void)
{
	char	   *ret;

	/* the ;; is a "stop", seems to be required before fetching BSP */
	__asm__ __volatile__(
						 ";;\n"
						 "	mov	%0=ar.bsp	\n"
:						 "=r"(ret));

	return ret;
}
#endif
#endif							/* IA64 */


/*
 * set_stack_base: set up reference point for stack depth checking
 *
 * Returns the old reference point, if any.
 */
pg_stack_base_t
set_stack_base(void)
{
	char		stack_base;
	pg_stack_base_t old;

#if defined(__ia64__) || defined(__ia64)
	old.stack_base_ptr = stack_base_ptr;
	old.register_stack_base_ptr = register_stack_base_ptr;
#else
	old = stack_base_ptr;
#endif

	/* Set up reference point for stack depth checking */
	stack_base_ptr = &stack_base;
#if defined(__ia64__) || defined(__ia64)
	register_stack_base_ptr = ia64_get_bsp();
#endif

	return old;
}

/*
 * restore_stack_base: restore reference point for stack depth checking
 *
 * This can be used after set_stack_base() to restore the old value. This
 * is currently only used in PL/Java. When PL/Java calls a backend function
 * from different thread, the thread's stack is at a different location than
 * the main thread's stack, so it sets the base pointer before the call, and
 * restores it afterwards.
 */
void
restore_stack_base(pg_stack_base_t base)
{
#if defined(__ia64__) || defined(__ia64)
	stack_base_ptr = base.stack_base_ptr;
	register_stack_base_ptr = base.register_stack_base_ptr;
#else
	stack_base_ptr = base;
#endif
}

/*
 * check_stack_depth/stack_is_too_deep: check for excessively deep recursion
 *
 * This should be called someplace in any recursive routine that might possibly
 * recurse deep enough to overflow the stack.  Most Unixen treat stack
 * overflow as an unrecoverable SIGSEGV, so we want to error out ourselves
 * before hitting the hardware limit.
 *
 * check_stack_depth() just throws an error summarily.  stack_is_too_deep()
 * can be used by code that wants to handle the error condition itself.
 */
void
check_stack_depth(void)
{
	if (stack_is_too_deep())
	{
		ereport(ERROR,
				(errcode(ERRCODE_STATEMENT_TOO_COMPLEX),
				 errmsg("stack depth limit exceeded"),
				 errhint("Increase the configuration parameter \"max_stack_depth\" (currently %dkB), "
						 "after ensuring the platform's stack depth limit is adequate.",
						 max_stack_depth)));
	}
}

bool
stack_is_too_deep(void)
{
	char		stack_top_loc;
	long		stack_depth;

	/*
	 * Compute distance from reference point to my local variables
	 */
	stack_depth = (long) (stack_base_ptr - &stack_top_loc);

	/*
	 * Take abs value, since stacks grow up on some machines, down on others
	 */
	if (stack_depth < 0)
		stack_depth = -stack_depth;

	/*
	 * Trouble?
	 *
	 * The test on stack_base_ptr prevents us from erroring out if called
	 * during process setup or in a non-backend process.  Logically it should
	 * be done first, but putting it here avoids wasting cycles during normal
	 * cases.
	 */
	if (stack_depth > max_stack_depth_bytes &&
		stack_base_ptr != NULL)
		return true;

	/*
	 * On IA64 there is a separate "register" stack that requires its own
	 * independent check.  For this, we have to measure the change in the
	 * "BSP" pointer from PostgresMain to here.  Logic is just as above,
	 * except that we know IA64's register stack grows up.
	 *
	 * Note we assume that the same max_stack_depth applies to both stacks.
	 */
#if defined(__ia64__) || defined(__ia64)
	stack_depth = (long) (ia64_get_bsp() - register_stack_base_ptr);

	if (stack_depth > max_stack_depth_bytes &&
		register_stack_base_ptr != NULL)
		return true;
#endif							/* IA64 */

	return false;
}

/* GUC check hook for max_stack_depth */
bool
check_max_stack_depth(int *newval, void **extra, GucSource source)
{
	long		newval_bytes = *newval * 1024L;
	long		stack_rlimit = get_stack_depth_rlimit();

	if (stack_rlimit > 0 && newval_bytes > stack_rlimit - STACK_DEPTH_SLOP)
	{
		GUC_check_errdetail("\"max_stack_depth\" must not exceed %ldkB.",
							(stack_rlimit - STACK_DEPTH_SLOP) / 1024L);
		GUC_check_errhint("Increase the platform's stack depth limit via \"ulimit -s\" or local equivalent.");
		return false;
	}
	return true;
}

/* GUC assign hook for max_stack_depth */
void
assign_max_stack_depth(int newval, void *extra)
{
	long		newval_bytes = newval * 1024L;

	max_stack_depth_bytes = newval_bytes;
}


/*
 * set_debug_options --- apply "-d N" command line option
 *
 * -d is not quite the same as setting log_min_messages because it enables
 * other output options.
 */
void
set_debug_options(int debug_flag, GucContext context, GucSource source)
{
	if (debug_flag > 0)
	{
		char		debugstr[64];

		sprintf(debugstr, "debug%d", debug_flag);
		SetConfigOption("log_min_messages", debugstr, context, source);
	}
	else
		SetConfigOption("log_min_messages", "notice", context, source);

	if (debug_flag >= 1 && context == PGC_POSTMASTER)
	{
		SetConfigOption("log_connections", "true", context, source);
		SetConfigOption("log_disconnections", "true", context, source);
	}
	if (debug_flag >= 2)
		SetConfigOption("log_statement", "all", context, source);
	if (debug_flag >= 3)
		SetConfigOption("debug_print_parse", "true", context, source);
	if (debug_flag >= 4)
		SetConfigOption("debug_print_plan", "true", context, source);
	if (debug_flag >= 5)
		SetConfigOption("debug_print_rewritten", "true", context, source);
}


bool
set_plan_disabling_options(const char *arg, GucContext context, GucSource source)
{
	const char *tmp = NULL;

	switch (arg[0])
	{
		case 's':				/* seqscan */
			tmp = "enable_seqscan";
			break;
		case 'i':				/* indexscan */
			tmp = "enable_indexscan";
			break;
		case 'o':				/* indexonlyscan */
			tmp = "enable_indexonlyscan";
			break;
		case 'b':				/* bitmapscan */
			tmp = "enable_bitmapscan";
			break;
		case 't':				/* tidscan */
			tmp = "enable_tidscan";
			break;
		case 'n':				/* nestloop */
			tmp = "enable_nestloop";
			break;
		case 'm':				/* mergejoin */
			tmp = "enable_mergejoin";
			break;
		case 'h':				/* hashjoin */
			tmp = "enable_hashjoin";
			break;
	}
	if (tmp)
	{
		SetConfigOption(tmp, "false", context, source);
		return true;
	}
	else
		return false;
}


const char *
get_stats_option_name(const char *arg)
{
	switch (arg[0])
	{
		case 'p':
			if (optarg[1] == 'a')	/* "parser" */
				return "log_parser_stats";
			else if (optarg[1] == 'l')	/* "planner" */
				return "log_planner_stats";
			break;

		case 'e':				/* "executor" */
			return "log_executor_stats";
			break;
	}

	return NULL;
}


/* ----------------------------------------------------------------
 * process_postgres_switches
 *	   Parse command line arguments for PostgresMain
 *
 * This is called twice, once for the "secure" options coming from the
 * postmaster or command line, and once for the "insecure" options coming
 * from the client's startup packet.  The latter have the same syntax but
 * may be restricted in what they can do.
 *
 * argv[0] is ignored in either case (it's assumed to be the program name).
 *
 * ctx is PGC_POSTMASTER for secure options, PGC_BACKEND for insecure options
 * coming from the client, or PGC_SU_BACKEND for insecure options coming from
 * a superuser client.
 *
 * If a database name is present in the command line arguments, it's
 * returned into *dbname (this is allowed only if *dbname is initially NULL).
 * ----------------------------------------------------------------
 */
void
process_postgres_switches(int argc, char *argv[], GucContext ctx,
						  const char **dbname)
{
	bool		secure = (ctx == PGC_POSTMASTER);
	int			errs = 0;
	GucSource	gucsource;
	int			flag;

	if (secure)
	{
		gucsource = PGC_S_ARGV; /* switches came from command line */

		/* Ignore the initial --single argument, if present */
		if (argc > 1 && strcmp(argv[1], "--single") == 0)
		{
			argv++;
			argc--;
		}
	}
	else
	{
		gucsource = PGC_S_CLIENT;	/* switches came from client */
	}

#ifdef HAVE_INT_OPTERR

	/*
	 * Turn this off because it's either printed to stderr and not the log
	 * where we'd want it, or argv[0] is now "--single", which would make for
	 * a weird error message.  We print our own error message below.
	 */
	opterr = 0;
#endif

	/*
	 * Parse command-line options.  CAUTION: keep this in sync with
	 * postmaster/postmaster.c (the option sets should not conflict) and with
	 * the common help() function in main/main.c.
	 */
	while ((flag = getopt(argc, argv, "B:bc:C:D:d:EeFf:h:ijk:lN:nOo:Pp:r:S:sTt:v:W:-:")) != -1)
	{
		switch (flag)
		{
			case 'B':
				SetConfigOption("shared_buffers", optarg, ctx, gucsource);
				break;

			case 'b':
				/* Undocumented flag used for binary upgrades */
				if (secure)
					IsBinaryUpgrade = true;
				break;

			case 'C':
				/* ignored for consistency with the postmaster */
				break;

			case 'D':
				if (secure)
					userDoption = strdup(optarg);
				break;

			case 'd':
				set_debug_options(atoi(optarg), ctx, gucsource);
				break;

			case 'E':
				if (secure)
					EchoQuery = true;
				break;

			case 'e':
				SetConfigOption("datestyle", "euro", ctx, gucsource);
				break;

			case 'F':
				SetConfigOption("fsync", "false", ctx, gucsource);
				break;

			case 'f':
				if (!set_plan_disabling_options(optarg, ctx, gucsource))
					errs++;
				break;

			case 'h':
				SetConfigOption("listen_addresses", optarg, ctx, gucsource);
				break;

			case 'i':
				SetConfigOption("listen_addresses", "*", ctx, gucsource);
				break;

			case 'j':
				if (secure)
					UseSemiNewlineNewline = true;
				break;

			case 'k':
				SetConfigOption("unix_socket_directories", optarg, ctx, gucsource);
				break;

			case 'l':
				SetConfigOption("ssl", "true", ctx, gucsource);
				break;

			case 'N':
				SetConfigOption("max_connections", optarg, ctx, gucsource);
				break;

			case 'n':
				/* ignored for consistency with postmaster */
				break;

			case 'O':
				SetConfigOption("allow_system_table_mods", "true", ctx, gucsource);
				break;

			case 'o':
				errs++;
				break;

			case 'P':
				SetConfigOption("ignore_system_indexes", "true", ctx, gucsource);
				break;

			case 'p':
				SetConfigOption("port", optarg, ctx, gucsource);
				break;

			case 'r':
				/* send output (stdout and stderr) to the given file */
				if (secure)
					strlcpy(OutputFileName, optarg, MAXPGPATH);
				break;

			case 'S':
				SetConfigOption("work_mem", optarg, ctx, gucsource);
				break;

			case 's':
				SetConfigOption("log_statement_stats", "true", ctx, gucsource);
				break;

			case 'T':
				/* ignored for consistency with the postmaster */
				break;

			case 't':
				{
					const char *tmp = get_stats_option_name(optarg);

					if (tmp)
						SetConfigOption(tmp, "true", ctx, gucsource);
					else
						errs++;
					break;
				}

			case 'v':

				/*
				 * -v is no longer used in normal operation, since
				 * FrontendProtocol is already set before we get here. We keep
				 * the switch only for possible use in standalone operation,
				 * in case we ever support using normal FE/BE protocol with a
				 * standalone backend.
				 */
				if (secure)
					FrontendProtocol = (ProtocolVersion) atoi(optarg);
				break;

			case 'W':
				SetConfigOption("post_auth_delay", optarg, ctx, gucsource);
				break;

			case 'c':
			case '-':
				{
					char	   *name,
							   *value;

					ParseLongOption(optarg, &name, &value);
					if (!value)
					{
						if (flag == '-')
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("--%s requires a value",
											optarg)));
						else
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("-c %s requires a value",
											optarg)));
					}
					SetConfigOption(name, value, ctx, gucsource);
					free(name);
					if (value)
						free(value);
					break;
				}

			default:
				errs++;
				break;
		}

		if (errs)
			break;
	}

	/*
	 * Optional database name should be there only if *dbname is NULL.
	 */
	if (!errs && dbname && *dbname == NULL && argc - optind >= 1)
		*dbname = strdup(argv[optind++]);

	if (errs || argc != optind)
	{
		if (errs)
			optind--;			/* complain about the previous argument */

		/* spell the error message a bit differently depending on context */
		if (IsUnderPostmaster)
			ereport(FATAL,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid command-line argument for server process: %s", argv[optind]),
					 errhint("Try \"%s --help\" for more information.", progname)));
		else
			ereport(FATAL,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s: invalid command-line argument: %s",
							progname, argv[optind]),
					 errhint("Try \"%s --help\" for more information.", progname)));
	}

	/*
	 * Reset getopt(3) library so that it will work correctly in subprocesses
	 * or when this function is called a second time with another array.
	 */
	optind = 1;
#ifdef HAVE_INT_OPTRESET
	optreset = 1;				/* some systems need this too */
#endif
}


/* ----------------------------------------------------------------
 * PostgresMain
 *	   postgres main loop -- all backends, interactive or otherwise start here
 *
 * argc/argv are the command line arguments to be used.  (When being forked
 * by the postmaster, these are not the original argv array of the process.)
 * dbname is the name of the database to connect to, or NULL if the database
 * name should be extracted from the command line arguments or defaulted.
 * username is the PostgreSQL user name to be used for the session.
 * ----------------------------------------------------------------
 */
void
PostgresMain(int argc, char *argv[],
			 const char *dbname,
			 const char *username)
{
	int			firstchar;
	StringInfoData input_message;
	sigjmp_buf	local_sigjmp_buf;
	volatile bool send_ready_for_query = true;
	bool		disable_idle_in_transaction_timeout = false;

	/* Initialize startup process environment if necessary. */
	if (!IsUnderPostmaster)
		InitStandaloneProcess(argv[0]);

	SetProcessingMode(InitProcessing);

	/*
	 * Set default values for command-line options.
	 */
	if (!IsUnderPostmaster)
		InitializeGUCOptions();

	/*
	 * Parse command-line options.
	 */
	process_postgres_switches(argc, argv, PGC_POSTMASTER, &dbname);

	/* Must have gotten a database name, or have a default (the username) */
	if (dbname == NULL)
	{
		dbname = username;
		if (dbname == NULL)
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("%s: no database nor user name specified",
							progname)));
	}

	/* Acquire configuration parameters, unless inherited from postmaster */
	if (!IsUnderPostmaster)
	{
		if (!SelectConfigFiles(userDoption, progname))
			proc_exit(1);
	}

	/*
	 * Set up signal handlers and masks.
	 *
	 * Note that postmaster blocked all signals before forking child process,
	 * so there is no race condition whereby we might receive a signal before
	 * we have set up the handler.
	 *
	 * Also note: it's best not to use any signals that are SIG_IGNored in the
	 * postmaster.  If such a signal arrives before we are able to change the
	 * handler to non-SIG_IGN, it'll get dropped.  Instead, make a dummy
	 * handler in the postmaster to reserve the signal. (Of course, this isn't
	 * an issue for signals that are locally generated, such as SIGALRM and
	 * SIGPIPE.)
	 */
	if (am_walsender)
		WalSndSignals();
	else
	{
		pqsignal(SIGHUP, PostgresSigHupHandler);	/* set flag to read config
													 * file */
		pqsignal(SIGINT, StatementCancelHandler);	/* cancel current query */
		pqsignal(SIGTERM, die); /* cancel current query and exit */

		/*
		 * In a standalone backend, SIGQUIT can be generated from the keyboard
		 * easily, while SIGTERM cannot, so we make both signals do die()
		 * rather than quickdie().
		 */
		if (IsUnderPostmaster)
			pqsignal(SIGQUIT, quickdie);	/* hard crash time */
		else
			pqsignal(SIGQUIT, die); /* cancel current query and exit */
		InitializeTimeouts();	/* establishes SIGALRM handler */

		/*
		 * Ignore failure to write to frontend. Note: if frontend closes
		 * connection, we will notice it and exit cleanly when control next
		 * returns to outer loop.  This seems safer than forcing exit in the
		 * midst of output during who-knows-what operation...
		 */
		pqsignal(SIGPIPE, SIG_IGN);
		pqsignal(SIGUSR1, procsignal_sigusr1_handler);
		pqsignal(SIGUSR2, SIG_IGN);
		pqsignal(SIGFPE, FloatExceptionHandler);

		/*
		 * Reset some signals that are accepted by postmaster but not by
		 * backend
		 */
		pqsignal(SIGCHLD, SIG_DFL); /* system() requires this on some
									 * platforms */
	}

	pqinitmask();

	if (IsUnderPostmaster)
	{
		/* We allow SIGQUIT (quickdie) at all times */
		sigdelset(&BlockSig, SIGQUIT);
	}

	PG_SETMASK(&BlockSig);		/* block everything except SIGQUIT */

	if (!IsUnderPostmaster)
	{
		/*
		 * Validate we have been given a reasonable-looking DataDir (if under
		 * postmaster, assume postmaster did this already).
		 */
		Assert(DataDir);
		ValidatePgVersion(DataDir);

		/* Change into DataDir (if under postmaster, was done already) */
		ChangeToDataDir();

		/*
		 * Create lockfile for data directory.
		 */
		CreateDataDirLockFile(false);

		/* Initialize MaxBackends (if under postmaster, was done already) */
		InitializeMaxBackends();
	}

	/* Early initialization */
	BaseInit();

	/*
	 * Create a per-backend PGPROC struct in shared memory, except in the
	 * EXEC_BACKEND case where this was done in SubPostmasterMain. We must do
	 * this before we can use LWLocks (and in the EXEC_BACKEND case we already
	 * had to do some stuff with LWLocks).
	 */
#ifdef EXEC_BACKEND
	if (!IsUnderPostmaster)
		InitProcess();
#else
	InitProcess();
#endif

	/* We need to allow SIGINT, etc during the initial transaction */
	PG_SETMASK(&UnBlockSig);

	/*
	 * General initialization.
	 *
	 * NOTE: if you are tempted to add code in this vicinity, consider putting
	 * it inside InitPostgres() instead.  In particular, anything that
	 * involves database access should be there, not here.
	 */
	InitPostgres(dbname, InvalidOid, username, InvalidOid, NULL);

	/*
	 * If the PostmasterContext is still around, recycle the space; we don't
	 * need it anymore after InitPostgres completes.  Note this does not trash
	 * *MyProcPort, because ConnCreate() allocated that space with malloc()
	 * ... else we'd need to copy the Port data first.  Also, subsidiary data
	 * such as the username isn't lost either; see ProcessStartupPacket().
	 */
	if (PostmasterContext)
	{
		MemoryContextDelete(PostmasterContext);
		PostmasterContext = NULL;
	}

	SetProcessingMode(NormalProcessing);

	/*
	 * Now all GUC states are fully set up.  Report them to client if
	 * appropriate.
	 */
	BeginReportingGUCOptions();

	/*
	 * Also set up handler to log session end; we have to wait till now to be
	 * sure Log_disconnections has its final value.
	 */
	if (IsUnderPostmaster && Log_disconnections)
		on_proc_exit(log_disconnections, 0);

	/* Perform initialization specific to a WAL sender process. */
	if (am_walsender)
		InitWalSender();

	/*
	 * process any libraries that should be preloaded at backend start (this
	 * likewise can't be done until GUC settings are complete)
	 */
	process_session_preload_libraries();

	/*
	 * Send this backend's cancellation info to the frontend.
	 */
	if (whereToSendOutput == DestRemote)
	{
		StringInfoData buf;

		pq_beginmessage(&buf, 'K');
		pq_sendint(&buf, (int32) MyProcPid, sizeof(int32));
		pq_sendint(&buf, (int32) MyCancelKey, sizeof(int32));
		pq_endmessage(&buf);
		/* Need not flush since ReadyForQuery will do it. */
	}

	/* Welcome banner for standalone case */
	if (whereToSendOutput == DestDebug)
		printf("\nPostgreSQL stand-alone backend %s\n", PG_VERSION);

	/*
	 * Create the memory context we will use in the main loop.
	 *
	 * MessageContext is reset once per iteration of the main loop, ie, upon
	 * completion of processing of each command message from the client.
	 */
	MessageContext = AllocSetContextCreate(TopMemoryContext,
										   "MessageContext",
										   ALLOCSET_DEFAULT_SIZES);

	/*
	 * Remember stand-alone backend startup time
	 */
	if (!IsUnderPostmaster)
		PgStartTime = GetCurrentTimestamp();

	/*
	 * POSTGRES main processing loop begins here
	 *
	 * If an exception is encountered, processing resumes here so we abort the
	 * current transaction and start a new one.
	 *
	 * You might wonder why this isn't coded as an infinite loop around a
	 * PG_TRY construct.  The reason is that this is the bottom of the
	 * exception stack, and so with PG_TRY there would be no exception handler
	 * in force at all during the CATCH part.  By leaving the outermost setjmp
	 * always active, we have at least some chance of recovering from an error
	 * during error recovery.  (If we get into an infinite loop thereby, it
	 * will soon be stopped by overflow of elog.c's internal state stack.)
	 *
	 * Note that we use sigsetjmp(..., 1), so that this function's signal mask
	 * (to wit, UnBlockSig) will be restored when longjmp'ing to here.  This
	 * is essential in case we longjmp'd out of a signal handler on a platform
	 * where that leaves the signal blocked.  It's not redundant with the
	 * unblock in AbortTransaction() because the latter is only called if we
	 * were inside a transaction.
	 */

	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/*
		 * NOTE: if you are tempted to add more code in this if-block,
		 * consider the high probability that it should be in
		 * AbortTransaction() instead.  The only stuff done directly here
		 * should be stuff that is guaranteed to apply *only* for outer-level
		 * error recovery, such as adjusting the FE/BE protocol status.
		 */

		/* Since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevent interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/*
		 * Forget any pending QueryCancel request, since we're returning to
		 * the idle loop anyway, and cancel any active timeout requests.  (In
		 * future we might want to allow some timeout requests to survive, but
		 * at minimum it'd be necessary to do reschedule_timeouts(), in case
		 * we got here because of a query cancel interrupting the SIGALRM
		 * interrupt handler.)	Note in particular that we must clear the
		 * statement and lock timeout indicators, to prevent any future plain
		 * query cancels from being misreported as timeouts in case we're
		 * forgetting a timeout cancel.
		 */
		disable_all_timeouts(false);
		QueryCancelPending = false; /* second to avoid race condition */

		/* Not reading from the client anymore. */
		DoingCommandRead = false;

		/* Make sure libpq is in a good state */
		pq_comm_reset();

		/* Report the error to the client and/or server log */
		EmitErrorReport();

		/*
		 * Make sure debug_query_string gets reset before we possibly clobber
		 * the storage it points at.
		 */
		debug_query_string = NULL;

		/*
		 * Abort the current transaction in order to recover.
		 */
		AbortCurrentTransaction();

		if (am_walsender)
			WalSndErrorCleanup();

		/*
		 * We can't release replication slots inside AbortTransaction() as we
		 * need to be able to start and abort transactions while having a slot
		 * acquired. But we never need to hold them across top level errors,
		 * so releasing here is fine. There's another cleanup in ProcKill()
		 * ensuring we'll correctly cleanup on FATAL errors as well.
		 */
		if (MyReplicationSlot != NULL)
			ReplicationSlotRelease();

		/* We also want to cleanup temporary slots on error. */
		ReplicationSlotCleanup();

		/*
		 * Now return to normal top-level context and clear ErrorContext for
		 * next time.
		 */
		MemoryContextSwitchTo(TopMemoryContext);
		FlushErrorState();

		/*
		 * If we were handling an extended-query-protocol message, initiate
		 * skip till next Sync.  This also causes us not to issue
		 * ReadyForQuery (until we get Sync).
		 */
		if (doing_extended_query_message)
			ignore_till_sync = true;

		/* We don't have a transaction command open anymore */
		xact_started = false;

		/*
		 * If an error occurred while we were reading a message from the
		 * client, we have potentially lost track of where the previous
		 * message ends and the next one begins.  Even though we have
		 * otherwise recovered from the error, we cannot safely read any more
		 * messages from the client, so there isn't much we can do with the
		 * connection anymore.
		 */
		if (pq_is_reading_msg())
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("terminating connection because protocol synchronization was lost")));

		/* Now we can allow interrupts again */
		RESUME_INTERRUPTS();
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	if (!ignore_till_sync)
		send_ready_for_query = true;	/* initially, or after error */

	/*
	 * Non-error queries loop here.
	 */

	for (;;)
	{
		/*
		 * At top of loop, reset extended-query-message flag, so that any
		 * errors encountered in "idle" state don't provoke skip.
		 */
		doing_extended_query_message = false;

		/*
		 * Release storage left over from prior query cycle, and create a new
		 * query input buffer in the cleared MessageContext.
		 */
		MemoryContextSwitchTo(MessageContext);
		MemoryContextResetAndDeleteChildren(MessageContext);

		initStringInfo(&input_message);

		/*
		 * Also consider releasing our catalog snapshot if any, so that it's
		 * not preventing advance of global xmin while we wait for the client.
		 */
		InvalidateCatalogSnapshotConditionally();

		/*
		 * (1) If we've reached idle state, tell the frontend we're ready for
		 * a new query.
		 *
		 * Note: this includes fflush()'ing the last of the prior output.
		 *
		 * This is also a good time to send collected statistics to the
		 * collector, and to update the PS stats display.  We avoid doing
		 * those every time through the message loop because it'd slow down
		 * processing of batched messages, and because we don't want to report
		 * uncommitted updates (that confuses autovacuum).  The notification
		 * processor wants a call too, if we are not in a transaction block.
		 */
		if (send_ready_for_query)
		{
			if (IsAbortedTransactionBlockState())
			{
				set_ps_display("idle in transaction (aborted)", false);
				pgstat_report_activity(STATE_IDLEINTRANSACTION_ABORTED, NULL);

				/* Start the idle-in-transaction timer */
				if (IdleInTransactionSessionTimeout > 0)
				{
					disable_idle_in_transaction_timeout = true;
					enable_timeout_after(IDLE_IN_TRANSACTION_SESSION_TIMEOUT,
										 IdleInTransactionSessionTimeout);
				}
			}
			else if (IsTransactionOrTransactionBlock())
			{
				set_ps_display("idle in transaction", false);
				pgstat_report_activity(STATE_IDLEINTRANSACTION, NULL);

				/* Start the idle-in-transaction timer */
				if (IdleInTransactionSessionTimeout > 0)
				{
					disable_idle_in_transaction_timeout = true;
					enable_timeout_after(IDLE_IN_TRANSACTION_SESSION_TIMEOUT,
										 IdleInTransactionSessionTimeout);
				}
			}
			else
			{
				ProcessCompletedNotifies();
				pgstat_report_stat(false);

				set_ps_display("idle", false);
				pgstat_report_activity(STATE_IDLE, NULL);
			}

			ReadyForQuery(whereToSendOutput);
			send_ready_for_query = false;
		}

		/*
		 * (2) Allow asynchronous signals to be executed immediately if they
		 * come in while we are waiting for client input. (This must be
		 * conditional since we don't want, say, reads on behalf of COPY FROM
		 * STDIN doing the same thing.)
		 */
		DoingCommandRead = true;

		/*
		 * (3) read a command (loop blocks here)
		 */
		firstchar = ReadCommand(&input_message);

		/*
		 * (4) disable async signal conditions again.
		 *
		 * Query cancel is supposed to be a no-op when there is no query in
		 * progress, so if a query cancel arrived while we were idle, just
		 * reset QueryCancelPending. ProcessInterrupts() has that effect when
		 * it's called when DoingCommandRead is set, so check for interrupts
		 * before resetting DoingCommandRead.
		 */
		CHECK_FOR_INTERRUPTS();
		DoingCommandRead = false;

		/*
		 * (5) turn off the idle-in-transaction timeout
		 */
		if (disable_idle_in_transaction_timeout)
		{
			disable_timeout(IDLE_IN_TRANSACTION_SESSION_TIMEOUT, false);
			disable_idle_in_transaction_timeout = false;
		}

		/*
		 * (6) check for any other interesting events that happened while we
		 * slept.
		 */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/*
		 * (7) process the command.  But ignore it if we're skipping till
		 * Sync.
		 */
		if (ignore_till_sync && firstchar != EOF)
			continue;

		switch (firstchar)
		{
			case 'Q':			/* simple query */
				{
					const char *query_string;

					/* Set statement_timestamp() */
					SetCurrentStatementStartTimestamp();

					query_string = pq_getmsgstring(&input_message);
					pq_getmsgend(&input_message);

					if (am_walsender)
					{
						if (!exec_replication_command(query_string))
							exec_simple_query(query_string);
					}
					else
						exec_simple_query(query_string);

					send_ready_for_query = true;
				}
				break;

			case 'P':			/* parse */
				{
					const char *stmt_name;
					const char *query_string;
					int			numParams;
					Oid		   *paramTypes = NULL;

					forbidden_in_wal_sender(firstchar);

					/* Set statement_timestamp() */
					SetCurrentStatementStartTimestamp();

					stmt_name = pq_getmsgstring(&input_message);
					query_string = pq_getmsgstring(&input_message);
					numParams = pq_getmsgint(&input_message, 2);
					if (numParams > 0)
					{
						int			i;

						paramTypes = (Oid *) palloc(numParams * sizeof(Oid));
						for (i = 0; i < numParams; i++)
							paramTypes[i] = pq_getmsgint(&input_message, 4);
					}
					pq_getmsgend(&input_message);

					exec_parse_message(query_string, stmt_name,
									   paramTypes, numParams);
				}
				break;

			case 'B':			/* bind */
				forbidden_in_wal_sender(firstchar);

				/* Set statement_timestamp() */
				SetCurrentStatementStartTimestamp();

				/*
				 * this message is complex enough that it seems best to put
				 * the field extraction out-of-line
				 */
				exec_bind_message(&input_message);
				break;

			case 'E':			/* execute */
				{
					const char *portal_name;
					int			max_rows;

					forbidden_in_wal_sender(firstchar);

					/* Set statement_timestamp() */
					SetCurrentStatementStartTimestamp();

					portal_name = pq_getmsgstring(&input_message);
					max_rows = pq_getmsgint(&input_message, 4);
					pq_getmsgend(&input_message);

					exec_execute_message(portal_name, max_rows);
				}
				break;

			case 'F':			/* fastpath function call */
				forbidden_in_wal_sender(firstchar);

				/* Set statement_timestamp() */
				SetCurrentStatementStartTimestamp();

				/* Report query to various monitoring facilities. */
				pgstat_report_activity(STATE_FASTPATH, NULL);
				set_ps_display("<FASTPATH>", false);

				/* start an xact for this function invocation */
				start_xact_command();

				/*
				 * Note: we may at this point be inside an aborted
				 * transaction.  We can't throw error for that until we've
				 * finished reading the function-call message, so
				 * HandleFunctionRequest() must check for it after doing so.
				 * Be careful not to do anything that assumes we're inside a
				 * valid transaction here.
				 */

				/* switch back to message context */
				MemoryContextSwitchTo(MessageContext);

				HandleFunctionRequest(&input_message);

				/* commit the function-invocation transaction */
				finish_xact_command();

				send_ready_for_query = true;
				break;

			case 'C':			/* close */
				{
					int			close_type;
					const char *close_target;

					forbidden_in_wal_sender(firstchar);

					close_type = pq_getmsgbyte(&input_message);
					close_target = pq_getmsgstring(&input_message);
					pq_getmsgend(&input_message);

					switch (close_type)
					{
						case 'S':
							if (close_target[0] != '\0')
								DropPreparedStatement(close_target, false);
							else
							{
								/* special-case the unnamed statement */
								drop_unnamed_stmt();
							}
							break;
						case 'P':
							{
								Portal		portal;

								portal = GetPortalByName(close_target);
								if (PortalIsValid(portal))
									PortalDrop(portal, false);
							}
							break;
						default:
							ereport(ERROR,
									(errcode(ERRCODE_PROTOCOL_VIOLATION),
									 errmsg("invalid CLOSE message subtype %d",
											close_type)));
							break;
					}

					if (whereToSendOutput == DestRemote)
						pq_putemptymessage('3');	/* CloseComplete */
				}
				break;

			case 'D':			/* describe */
				{
					int			describe_type;
					const char *describe_target;

					forbidden_in_wal_sender(firstchar);

					/* Set statement_timestamp() (needed for xact) */
					SetCurrentStatementStartTimestamp();

					describe_type = pq_getmsgbyte(&input_message);
					describe_target = pq_getmsgstring(&input_message);
					pq_getmsgend(&input_message);

					switch (describe_type)
					{
						case 'S':
							exec_describe_statement_message(describe_target);
							break;
						case 'P':
							exec_describe_portal_message(describe_target);
							break;
						default:
							ereport(ERROR,
									(errcode(ERRCODE_PROTOCOL_VIOLATION),
									 errmsg("invalid DESCRIBE message subtype %d",
											describe_type)));
							break;
					}
				}
				break;

			case 'H':			/* flush */
				pq_getmsgend(&input_message);
				if (whereToSendOutput == DestRemote)
					pq_flush();
				break;

			case 'S':			/* sync */
				pq_getmsgend(&input_message);
				finish_xact_command();
				send_ready_for_query = true;
				break;

				/*
				 * 'X' means that the frontend is closing down the socket. EOF
				 * means unexpected loss of frontend connection. Either way,
				 * perform normal shutdown.
				 */
			case 'X':
			case EOF:

				/*
				 * Reset whereToSendOutput to prevent ereport from attempting
				 * to send any more messages to client.
				 */
				if (whereToSendOutput == DestRemote)
					whereToSendOutput = DestNone;

				/*
				 * NOTE: if you are tempted to add more code here, DON'T!
				 * Whatever you had in mind to do should be set up as an
				 * on_proc_exit or on_shmem_exit callback, instead. Otherwise
				 * it will fail to be called during other backend-shutdown
				 * scenarios.
				 */
				proc_exit(0);

			case 'd':			/* copy data */
			case 'c':			/* copy done */
			case 'f':			/* copy fail */

				/*
				 * Accept but ignore these messages, per protocol spec; we
				 * probably got here because a COPY failed, and the frontend
				 * is still sending data.
				 */
				break;

			default:
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid frontend message type %d",
								firstchar)));
		}
	}							/* end of input-reading loop */
}

/*
 * Throw an error if we're a WAL sender process.
 *
 * This is used to forbid anything else than simple query protocol messages
 * in a WAL sender process.  'firstchar' specifies what kind of a forbidden
 * message was received, and is used to construct the error message.
 */
static void
forbidden_in_wal_sender(char firstchar)
{
	if (am_walsender)
	{
		if (firstchar == 'F')
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("fastpath function calls not supported in a replication connection")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("extended query protocol not supported in a replication connection")));
	}
}


/*
 * Obtain platform stack depth limit (in bytes)
 *
 * Return -1 if unknown
 */
long
get_stack_depth_rlimit(void)
{
#if defined(HAVE_GETRLIMIT) && defined(RLIMIT_STACK)
	static long val = 0;

	/* This won't change after process launch, so check just once */
	if (val == 0)
	{
		struct rlimit rlim;

		if (getrlimit(RLIMIT_STACK, &rlim) < 0)
			val = -1;
		else if (rlim.rlim_cur == RLIM_INFINITY)
			val = LONG_MAX;
		/* rlim_cur is probably of an unsigned type, so check for overflow */
		else if (rlim.rlim_cur >= LONG_MAX)
			val = LONG_MAX;
		else
			val = rlim.rlim_cur;
	}
	return val;
#else							/* no getrlimit */
#if defined(WIN32) || defined(__CYGWIN__)
	/* On Windows we set the backend stack size in src/backend/Makefile */
	return WIN32_STACK_RLIMIT;
#else							/* not windows ... give up */
	return -1;
#endif
#endif
}


static struct rusage Save_r;
static struct timeval Save_t;

void
ResetUsage(void)
{
	getrusage(RUSAGE_SELF, &Save_r);
	gettimeofday(&Save_t, NULL);
}

void
ShowUsage(const char *title)
{
	StringInfoData str;
	struct timeval user,
				sys;
	struct timeval elapse_t;
	struct rusage r;

	getrusage(RUSAGE_SELF, &r);
	gettimeofday(&elapse_t, NULL);
	memcpy((char *) &user, (char *) &r.ru_utime, sizeof(user));
	memcpy((char *) &sys, (char *) &r.ru_stime, sizeof(sys));
	if (elapse_t.tv_usec < Save_t.tv_usec)
	{
		elapse_t.tv_sec--;
		elapse_t.tv_usec += 1000000;
	}
	if (r.ru_utime.tv_usec < Save_r.ru_utime.tv_usec)
	{
		r.ru_utime.tv_sec--;
		r.ru_utime.tv_usec += 1000000;
	}
	if (r.ru_stime.tv_usec < Save_r.ru_stime.tv_usec)
	{
		r.ru_stime.tv_sec--;
		r.ru_stime.tv_usec += 1000000;
	}

	/*
	 * the only stats we don't show here are for memory usage -- i can't
	 * figure out how to interpret the relevant fields in the rusage struct,
	 * and they change names across o/s platforms, anyway. if you can figure
	 * out what the entries mean, you can somehow extract resident set size,
	 * shared text size, and unshared data and stack sizes.
	 */
	initStringInfo(&str);

	appendStringInfoString(&str, "! system usage stats:\n");
	appendStringInfo(&str,
					 "!\t%ld.%06ld s user, %ld.%06ld s system, %ld.%06ld s elapsed\n",
					 (long) (r.ru_utime.tv_sec - Save_r.ru_utime.tv_sec),
					 (long) (r.ru_utime.tv_usec - Save_r.ru_utime.tv_usec),
					 (long) (r.ru_stime.tv_sec - Save_r.ru_stime.tv_sec),
					 (long) (r.ru_stime.tv_usec - Save_r.ru_stime.tv_usec),
					 (long) (elapse_t.tv_sec - Save_t.tv_sec),
					 (long) (elapse_t.tv_usec - Save_t.tv_usec));
	appendStringInfo(&str,
					 "!\t[%ld.%06ld s user, %ld.%06ld s system total]\n",
					 (long) user.tv_sec,
					 (long) user.tv_usec,
					 (long) sys.tv_sec,
					 (long) sys.tv_usec);
#if defined(HAVE_GETRUSAGE)
	appendStringInfo(&str,
					 "!\t%ld/%ld [%ld/%ld] filesystem blocks in/out\n",
					 r.ru_inblock - Save_r.ru_inblock,
	/* they only drink coffee at dec */
					 r.ru_oublock - Save_r.ru_oublock,
					 r.ru_inblock, r.ru_oublock);
	appendStringInfo(&str,
					 "!\t%ld/%ld [%ld/%ld] page faults/reclaims, %ld [%ld] swaps\n",
					 r.ru_majflt - Save_r.ru_majflt,
					 r.ru_minflt - Save_r.ru_minflt,
					 r.ru_majflt, r.ru_minflt,
					 r.ru_nswap - Save_r.ru_nswap,
					 r.ru_nswap);
	appendStringInfo(&str,
					 "!\t%ld [%ld] signals rcvd, %ld/%ld [%ld/%ld] messages rcvd/sent\n",
					 r.ru_nsignals - Save_r.ru_nsignals,
					 r.ru_nsignals,
					 r.ru_msgrcv - Save_r.ru_msgrcv,
					 r.ru_msgsnd - Save_r.ru_msgsnd,
					 r.ru_msgrcv, r.ru_msgsnd);
	appendStringInfo(&str,
					 "!\t%ld/%ld [%ld/%ld] voluntary/involuntary context switches\n",
					 r.ru_nvcsw - Save_r.ru_nvcsw,
					 r.ru_nivcsw - Save_r.ru_nivcsw,
					 r.ru_nvcsw, r.ru_nivcsw);
#endif							/* HAVE_GETRUSAGE */

	/* remove trailing newline */
	if (str.data[str.len - 1] == '\n')
		str.data[--str.len] = '\0';

	ereport(LOG,
			(errmsg_internal("%s", title),
			 errdetail_internal("%s", str.data)));

	pfree(str.data);
}

/*
 * on_proc_exit handler to log end of session
 */
static void
log_disconnections(int code, Datum arg)
{
	Port	   *port = MyProcPort;
	long		secs;
	int			usecs;
	int			msecs;
	int			hours,
				minutes,
				seconds;

	TimestampDifference(port->SessionStartTime,
						GetCurrentTimestamp(),
						&secs, &usecs);
	msecs = usecs / 1000;

	hours = secs / SECS_PER_HOUR;
	secs %= SECS_PER_HOUR;
	minutes = secs / SECS_PER_MINUTE;
	seconds = secs % SECS_PER_MINUTE;

	ereport(LOG,
			(errmsg("disconnection: session time: %d:%02d:%02d.%03d "
					"user=%s database=%s host=%s%s%s",
					hours, minutes, seconds, msecs,
					port->user_name, port->database_name, port->remote_host,
					port->remote_port[0] ? " port=" : "", port->remote_port)));
}
