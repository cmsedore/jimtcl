/* Jim Tcl ESP32 Test Extension
 *
 * Minimal test framework for running Tcl tests on ESP32 via REPL.
 * Provides the [test] command with subcommands for running tests,
 * managing constraints, and reporting results.
 *
 * Commands:
 *
 *   test run <id> <description> -body <script>
 *        ?-result <expected>? ?-returnCodes <codes>?
 *        ?-match exact|glob|regexp? ?-constraints <list>?
 *       Run a single test. Evaluates -body and compares the result
 *       against -result using the specified match mode. Prints
 *       PASS/FAIL/SKIP for each test.
 *
 *   test constraint <name> ?value?
 *       Set or query a named test constraint. Without value, returns
 *       the current value (0 if not set).
 *
 *   test report
 *       Print summary: total, passed, failed, skipped counts.
 *
 *   test reset
 *       Zero all counters and clear constraint table.
 *
 *   test suite <name>
 *       Print a suite header: "=== <name> ==="
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"

/* ---------- static state ------------------------------------------------ */

static int test_total = 0;
static int test_pass  = 0;
static int test_fail  = 0;
static int test_skip  = 0;

/* Constraints stored as a Jim dict object (refcounted) */
static Jim_Obj *constraint_dict = NULL;

/* ---------- helpers ----------------------------------------------------- */

static void ensure_constraint_dict(Jim_Interp *interp)
{
    if (constraint_dict == NULL) {
        constraint_dict = Jim_NewDictObj(interp, NULL, 0);
        Jim_IncrRefCount(constraint_dict);
    }
}

static int get_constraint(Jim_Interp *interp, const char *name)
{
    Jim_Obj *val;
    ensure_constraint_dict(interp);
    Jim_Obj *key = Jim_NewStringObj(interp, name, -1);
    Jim_IncrRefCount(key);
    int rc = Jim_DictKey(interp, constraint_dict, key, &val, 0);
    Jim_DecrRefCount(interp, key);
    if (rc != JIM_OK) {
        return 0;
    }
    long v;
    if (Jim_GetLong(interp, val, &v) != JIM_OK) {
        return 0;
    }
    return (int)v;
}

static void set_constraint(Jim_Interp *interp, const char *name, int value)
{
    ensure_constraint_dict(interp);

    /* Must replace dict -- DictAddElement works in-place if refcount == 1,
     * but our dict might be shared if someone copied it. Safest to just
     * use the high-level API. */
    Jim_Obj *key = Jim_NewStringObj(interp, name, -1);
    Jim_Obj *val = Jim_NewIntObj(interp, value);
    Jim_DictAddElement(interp, constraint_dict, key, val);
}

/* Match result against expected using the given mode.
 * Returns 1 on match, 0 on mismatch. */
static int match_result(Jim_Interp *interp, const char *mode,
                        const char *expected, const char *actual)
{
    if (strcmp(mode, "exact") == 0) {
        return strcmp(expected, actual) == 0;
    }
    if (strcmp(mode, "glob") == 0) {
        return Jim_StringMatchObj(interp,
            Jim_NewStringObj(interp, expected, -1),
            Jim_NewStringObj(interp, actual, -1), 0);
    }
    if (strcmp(mode, "regexp") == 0) {
        /* Build a regexp match script and evaluate it */
        char script[512];
        snprintf(script, sizeof(script), "regexp {%s} {%s}", expected, actual);
        int rc = Jim_Eval(interp, script);
        if (rc != JIM_OK) {
            return 0;
        }
        long v;
        if (Jim_GetLong(interp, Jim_GetResult(interp), &v) != JIM_OK) {
            return 0;
        }
        return (int)v;
    }
    return 0;
}

/* ---------- subcommand: test run --------------------------------------- */

static int test_cmd_run(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    /* test run <id> <description> ?-option value ...? */
    if (argc < 2) {
        Jim_WrongNumArgs(interp, 0, argv,
            "test run id description ?-body script? ?-result expected? "
            "?-returnCodes codes? ?-match mode? ?-constraints list?");
        return JIM_ERR;
    }

    const char *id   = Jim_String(argv[0]);
    const char *desc = Jim_String(argv[1]);

    /* Defaults */
    Jim_Obj *body        = NULL;
    Jim_Obj *expected    = Jim_NewStringObj(interp, "", -1);
    const char *match    = "exact";
    Jim_Obj *constraints = NULL;
    Jim_Obj *returnCodes = NULL;

    Jim_IncrRefCount(expected);

    /* Parse -option value pairs */
    int i;
    for (i = 2; i < argc - 1; i += 2) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-body") == 0) {
            body = argv[i + 1];
        } else if (strcmp(opt, "-result") == 0) {
            Jim_DecrRefCount(interp, expected);
            expected = argv[i + 1];
            Jim_IncrRefCount(expected);
        } else if (strcmp(opt, "-match") == 0) {
            match = Jim_String(argv[i + 1]);
        } else if (strcmp(opt, "-constraints") == 0) {
            constraints = argv[i + 1];
        } else if (strcmp(opt, "-returnCodes") == 0) {
            returnCodes = argv[i + 1];
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            Jim_DecrRefCount(interp, expected);
            return JIM_ERR;
        }
    }

    if (body == NULL) {
        Jim_SetResultString(interp, "test run requires -body", -1);
        Jim_DecrRefCount(interp, expected);
        return JIM_ERR;
    }

    test_total++;

    /* Check constraints */
    if (constraints != NULL) {
        int len = Jim_ListLength(interp, constraints);
        int j;
        for (j = 0; j < len; j++) {
            Jim_Obj *c = Jim_ListGetIndex(interp, constraints, j);
            const char *cname = Jim_String(c);
            if (!get_constraint(interp, cname)) {
                test_skip++;
                fprintf(stdout, "SKIP: %s (%s) - constraint '%s'\n", id, desc, cname);
                fflush(stdout);
                Jim_DecrRefCount(interp, expected);
                Jim_SetResultString(interp, "skip", -1);
                return JIM_OK;
            }
        }
    }

    /* Evaluate body */
    int rc = Jim_EvalObj(interp, body);
    Jim_Obj *result = Jim_GetResult(interp);
    Jim_IncrRefCount(result);

    /* Check return code */
    int expect_error = 0;
    if (returnCodes != NULL) {
        int rlen = Jim_ListLength(interp, returnCodes);
        int j;
        int code_ok = 0;
        for (j = 0; j < rlen; j++) {
            Jim_Obj *codeObj = Jim_ListGetIndex(interp, returnCodes, j);
            const char *codeStr = Jim_String(codeObj);
            if (strcmp(codeStr, "error") == 0) {
                expect_error = 1;
                if (rc == JIM_ERR) code_ok = 1;
            } else if (strcmp(codeStr, "ok") == 0) {
                if (rc == JIM_OK) code_ok = 1;
            } else if (strcmp(codeStr, "return") == 0) {
                if (rc == JIM_RETURN) code_ok = 1;
            } else {
                /* Numeric code */
                long v;
                if (Jim_GetLong(interp, codeObj, &v) == JIM_OK) {
                    if (rc == (int)v) code_ok = 1;
                }
            }
        }
        if (!code_ok) {
            test_fail++;
            fprintf(stdout, "FAIL: %s (%s) - expected returnCode in {%s}, got %d\n",
                    id, desc, Jim_String(returnCodes), rc);
            fflush(stdout);
            Jim_DecrRefCount(interp, result);
            Jim_DecrRefCount(interp, expected);
            Jim_SetResultString(interp, "fail", -1);
            return JIM_OK;
        }
    } else {
        /* Default: expect ok or return */
        if (rc != JIM_OK && rc != JIM_RETURN) {
            test_fail++;
            fprintf(stdout, "FAIL: %s (%s) - unexpected error: %s\n",
                    id, desc, Jim_String(result));
            fflush(stdout);
            Jim_DecrRefCount(interp, result);
            Jim_DecrRefCount(interp, expected);
            Jim_SetResultString(interp, "fail", -1);
            return JIM_OK;
        }
    }

    /* Compare result */
    const char *exp_str = Jim_String(expected);
    const char *act_str = Jim_String(result);

    if (match_result(interp, match, exp_str, act_str)) {
        test_pass++;
        fprintf(stdout, "PASS: %s (%s)\n", id, desc);
        fflush(stdout);
        Jim_DecrRefCount(interp, result);
        Jim_DecrRefCount(interp, expected);
        Jim_SetResultString(interp, "pass", -1);
        return JIM_OK;
    }

    test_fail++;
    fprintf(stdout, "FAIL: %s (%s)\n  expected: %s\n       got: %s\n",
            id, desc, exp_str, act_str);
    fflush(stdout);
    Jim_DecrRefCount(interp, result);
    Jim_DecrRefCount(interp, expected);
    Jim_SetResultString(interp, "fail", -1);
    return JIM_OK;
}

/* ---------- subcommand: test constraint -------------------------------- */

static int test_cmd_constraint(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1 || argc > 2) {
        Jim_WrongNumArgs(interp, 0, argv, "test constraint name ?value?");
        return JIM_ERR;
    }

    const char *name = Jim_String(argv[0]);

    if (argc == 2) {
        long val;
        if (Jim_GetLong(interp, argv[1], &val) != JIM_OK) {
            return JIM_ERR;
        }
        set_constraint(interp, name, (int)val);
        Jim_SetResult(interp, argv[1]);
        return JIM_OK;
    }

    /* Query */
    int val = get_constraint(interp, name);
    Jim_SetResultInt(interp, val);
    return JIM_OK;
}

/* ---------- subcommand: test report ------------------------------------ */

static int test_cmd_report(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    fprintf(stdout,
        "\n--- Test Report ---\n"
        "Total:   %d\n"
        "Passed:  %d\n"
        "Failed:  %d\n"
        "Skipped: %d\n"
        "-------------------\n",
        test_total, test_pass, test_fail, test_skip);
    fflush(stdout);

    /* Also set result as a dict for programmatic use */
    Jim_Obj *dict = Jim_NewDictObj(interp, NULL, 0);
    Jim_DictAddElement(interp, dict,
        Jim_NewStringObj(interp, "total", -1), Jim_NewIntObj(interp, test_total));
    Jim_DictAddElement(interp, dict,
        Jim_NewStringObj(interp, "pass", -1), Jim_NewIntObj(interp, test_pass));
    Jim_DictAddElement(interp, dict,
        Jim_NewStringObj(interp, "fail", -1), Jim_NewIntObj(interp, test_fail));
    Jim_DictAddElement(interp, dict,
        Jim_NewStringObj(interp, "skip", -1), Jim_NewIntObj(interp, test_skip));
    Jim_SetResult(interp, dict);
    return JIM_OK;
}

/* ---------- subcommand: test reset ------------------------------------- */

static int test_cmd_reset(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    test_total = 0;
    test_pass  = 0;
    test_fail  = 0;
    test_skip  = 0;

    if (constraint_dict != NULL) {
        Jim_DecrRefCount(interp, constraint_dict);
        constraint_dict = NULL;
    }

    Jim_SetResultString(interp, "ok", -1);
    return JIM_OK;
}

/* ---------- subcommand: test suite ------------------------------------- */

static int test_cmd_suite(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc != 1) {
        Jim_WrongNumArgs(interp, 0, argv, "test suite name");
        return JIM_ERR;
    }

    const char *name = Jim_String(argv[0]);
    fprintf(stdout, "\n=== %s ===\n", name);
    fflush(stdout);

    Jim_SetResultString(interp, "", -1);
    return JIM_OK;
}

/* ---------- top-level dispatch via jim-subcmd -------------------------- */

static const jim_subcmd_type test_command_table[] = {
    { "run",
      "id description ?-body script? ?-result expected? ?-returnCodes codes? ?-match mode? ?-constraints list?",
      test_cmd_run,
      2, -1,       /* min 2 args (id + desc), unlimited max */
      0
    },
    { "constraint",
      "name ?value?",
      test_cmd_constraint,
      1, 2,
      0
    },
    { "report",
      NULL,
      test_cmd_report,
      0, 0,
      0
    },
    { "reset",
      NULL,
      test_cmd_reset,
      0, 0,
      0
    },
    { "suite",
      "name",
      test_cmd_suite,
      1, 1,
      0
    },
    { NULL }
};

/* ---------- extension init --------------------------------------------- */

int Jim_testInit(Jim_Interp *interp)
{
    Jim_CreateCommand(interp, "test", Jim_SubCmdProc,
                      (void *)test_command_table, NULL);

    /* Set default constraints */
    ensure_constraint_dict(interp);
    set_constraint(interp, "esp32", 1);
    set_constraint(interp, "jim", 1);

    return JIM_OK;
}
