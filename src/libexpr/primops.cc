#include "archive.hh"
#include "derivations.hh"
#include "downstream-placeholder.hh"
#include "eval-inline.hh"
#include "eval.hh"
#include "eval-settings.hh"
#include "gc-small-vector.hh"
#include "globals.hh"
#include "json-to-value.hh"
#include "names.hh"
#include "path-references.hh"
#include "store-api.hh"
#include "util.hh"
#include "value-to-json.hh"
#include "value-to-xml.hh"
#include "primops.hh"
#include "fetch-to-store.hh"

#include <boost/container/small_vector.hpp>
#include <nlohmann/json.hpp>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <regex>
#include <dlfcn.h>

#include <cmath>

namespace nix {


/*************************************************************
 * Miscellaneous
 *************************************************************/

StringMap EvalState::realiseContext(const NixStringContext & context)
{
    std::vector<DerivedPath::Built> drvs;
    StringMap res;

    for (auto & c : context) {
        auto ensureValid = [&](const StorePath & p) {
            if (!store->isValidPath(p))
                error<InvalidPathError>(store->printStorePath(p)).debugThrow();
        };
        std::visit(overloaded {
            [&](const NixStringContextElem::Built & b) {
                drvs.push_back(DerivedPath::Built {
                    .drvPath = b.drvPath,
                    .outputs = OutputsSpec::Names { b.output },
                });
                ensureValid(b.drvPath->getBaseStorePath());
            },
            [&](const NixStringContextElem::Opaque & o) {
                auto ctxS = store->printStorePath(o.path);
                res.insert_or_assign(ctxS, ctxS);
                ensureValid(o.path);
            },
            [&](const NixStringContextElem::DrvDeep & d) {
                /* Treat same as Opaque */
                auto ctxS = store->printStorePath(d.drvPath);
                res.insert_or_assign(ctxS, ctxS);
                ensureValid(d.drvPath);
            },
        }, c.raw);
    }

    if (drvs.empty()) return {};

    if (!evalSettings.enableImportFromDerivation)
        error<EvalError>(
            "cannot build '%1%' during evaluation because the option 'allow-import-from-derivation' is disabled",
            drvs.begin()->to_string(*store)
        ).debugThrow();

    /* Build/substitute the context. */
    std::vector<DerivedPath> buildReqs;
    for (auto & d : drvs) buildReqs.emplace_back(DerivedPath { d });
    buildStore->buildPaths(buildReqs, bmNormal, store);

    StorePathSet outputsToCopyAndAllow;

    for (auto & drv : drvs) {
        auto outputs = resolveDerivedPath(*buildStore, drv, &*store);
        for (auto & [outputName, outputPath] : outputs) {
            outputsToCopyAndAllow.insert(outputPath);

            /* Get all the output paths corresponding to the placeholders we had */
            if (experimentalFeatureSettings.isEnabled(Xp::CaDerivations)) {
                res.insert_or_assign(
                    DownstreamPlaceholder::fromSingleDerivedPathBuilt(
                        SingleDerivedPath::Built {
                            .drvPath = drv.drvPath,
                            .output = outputName,
                        }).render(),
                    buildStore->printStorePath(outputPath)
                );
            }
        }
    }

    if (store != buildStore) copyClosure(*buildStore, *store, outputsToCopyAndAllow);
    if (allowedPaths) {
        for (auto & outputPath : outputsToCopyAndAllow) {
            /* Add the output of this derivations to the allowed
               paths. */
            allowPath(outputPath);
        }
    }

    return res;
}



/* Execute a program and parse its output */
void prim_exec(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    state.forceList(*args[0], pos, "while evaluating the first argument passed to builtins.exec");
    auto elems = args[0]->listElems();
    auto count = args[0]->listSize();
    if (count == 0)
        state.error<EvalError>("at least one argument to 'exec' required").atPos(pos).debugThrow();
    NixStringContext context;
    auto program = state.coerceToString(pos, *elems[0], context,
            "while evaluating the first element of the argument passed to builtins.exec",
            false, false).toOwned();
    Strings commandArgs;
    for (unsigned int i = 1; i < args[0]->listSize(); ++i) {
        commandArgs.push_back(
                state.coerceToString(pos, *elems[i], context,
                        "while evaluating an element of the argument passed to builtins.exec",
                        false, false).toOwned());
    }
    try {
        auto _ = state.realiseContext(context); // FIXME: Handle CA derivations
    } catch (InvalidPathError & e) {
        state.error<EvalError>("cannot execute '%1%', since path '%2%' is not valid", program, e.path).atPos(pos).debugThrow();
    }

    auto output = runProgram(program, true, commandArgs);
    Expr * parsed;
    try {
        parsed = state.parseExprFromString(std::move(output), state.rootPath(CanonPath::root));
    } catch (Error & e) {
        e.addTrace(state.positions[pos], "while parsing the output from '%1%'", program);
        throw;
    }
    try {
        state.eval(parsed, v);
    } catch (Error & e) {
        e.addTrace(state.positions[pos], "while evaluating the output from '%1%'", program);
        throw;
    }
}



template<typename Callable>
 static inline void withExceptionContext(Trace trace, Callable&& func)
{
    try
    {
        func();
    }
    catch(Error & e)
    {
        e.pushTrace(trace);
        throw;
    }
}

static void prim_genericClosure(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    state.forceAttrs(*args[0], noPos, "while evaluating the first argument passed to builtins.genericClosure");

    /* Get the start set. */
    Bindings::iterator startSet = getAttr(state, state.sStartSet, args[0]->attrs, "in the attrset passed as argument to builtins.genericClosure");

    state.forceList(*startSet->value, noPos, "while evaluating the 'startSet' attribute passed as argument to builtins.genericClosure");

    ValueList workSet;
    for (auto elem : startSet->value->listItems())
        workSet.push_back(elem);

    if (startSet->value->listSize() == 0) {
        v = *startSet->value;
        return;
    }

    /* Get the operator. */
    Bindings::iterator op = getAttr(state, state.sOperator, args[0]->attrs, "in the attrset passed as argument to builtins.genericClosure");
    state.forceFunction(*op->value, noPos, "while evaluating the 'operator' attribute passed as argument to builtins.genericClosure");

    /* Construct the closure by applying the operator to elements of
       `workSet', adding the result to `workSet', continuing until
       no new elements are found. */
    ValueList res;
    // `doneKeys' doesn't need to be a GC root, because its values are
    // reachable from res.
    auto cmp = CompareValues(state, noPos, "while comparing the `key` attributes of two genericClosure elements");
    std::set<Value *, decltype(cmp)> doneKeys(cmp);
    while (!workSet.empty()) {
        Value * e = *(workSet.begin());
        workSet.pop_front();

        state.forceAttrs(*e, noPos, "while evaluating one of the elements generated by (or initially passed to) builtins.genericClosure");

        Bindings::iterator key = getAttr(state, state.sKey, e->attrs, "in one of the attrsets generated by (or initially passed to) builtins.genericClosure");
        state.forceValue(*key->value, noPos);

        if (!doneKeys.insert(key->value).second) continue;
        res.push_back(e);

        /* Call the `operator' function with `e' as argument. */
        Value newElements;
        state.callFunction(*op->value, 1, &e, newElements, noPos);
        state.forceList(newElements, noPos, "while evaluating the return value of the `operator` passed to builtins.genericClosure");

        /* Add the values returned by the operator to the work set. */
        for (auto elem : newElements.listItems()) {
            state.forceValue(*elem, noPos); // "while evaluating one one of the elements returned by the `operator` passed to builtins.genericClosure");
            workSet.push_back(elem);
        }
    }

    /* Create the result list. */
    state.mkList(v, res.size());
    unsigned int n = 0;
    for (auto & i : res)
        v.listElems()[n++] = i;
}

static RegisterPrimOp primop_genericClosure(PrimOp {
    .name = "__genericClosure",
    .args = {"attrset"},
    .arity = 1,
    .doc = R"(
      Take an *attrset* with values named `startSet` and `operator` in order to
      return a *list of attrsets* by starting with the `startSet` and recursively
      applying the `operator` function to each `item`. The *attrsets* in the
      `startSet` and the *attrsets* produced by `operator` must contain a value
      named `key` which is comparable. The result is produced by calling `operator`
      for each `item` with a value for `key` that has not been called yet including
      newly produced `item`s. The function terminates when no new `item`s are
      produced. The resulting *list of attrsets* contains only *attrsets* with a
      unique key. For example,

      ```
      builtins.genericClosure {
        startSet = [ {key = 5;} ];
        operator = item: [{
          key = if (item.key / 2 ) * 2 == item.key
               then item.key / 2
               else 3 * item.key + 1;
        }];
      }
      ```
      evaluates to
      ```
      [ { key = 5; } { key = 16; } { key = 8; } { key = 4; } { key = 2; } { key = 1; } ]
      ```
      )",
    .fun = prim_genericClosure,
});

static void prim_addErrorContext(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    try {
        state.forceValue(*args[1], pos);
        v = *args[1];
    } catch (Error & e) {
        NixStringContext context;
        auto message = state.coerceToString(pos, *args[0], context,
                "while evaluating the error message passed to builtins.addErrorContext",
                false, false).toOwned();
        e.addTrace(nullptr, HintFmt(message));
        throw;
    }
}

static RegisterPrimOp primop_addErrorContext(PrimOp {
    .name = "__addErrorContext",
    .arity = 2,
    .fun = prim_addErrorContext,
});

/* Try evaluating the argument. Success => {success=true; value=something;},
 * else => {success=false; value=false;} */

/* Return an environment variable.  Use with care. */
static void prim_getEnv(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    std::string name(state.forceStringNoCtx(*args[0], pos, "while evaluating the first argument passed to builtins.getEnv"));
    v.mkString(evalSettings.restrictEval || evalSettings.pureEval ? "" : getEnv(name).value_or(""));
}

static RegisterPrimOp primop_getEnv({
    .name = "__getEnv",
    .args = {"s"},
    .doc = R"(
      `getEnv` returns the value of the environment variable *s*, or an
      empty string if the variable doesn’t exist. This function should be
      used with care, as it can introduce all sorts of nasty environment
      dependencies in your Nix expression.

      `getEnv` is used in Nix Packages to locate the file
      `~/.nixpkgs/config.nix`, which contains user-local settings for Nix
      Packages. (That is, it does a `getEnv "HOME"` to locate the user’s
      home directory.)
    )",
    .fun = prim_getEnv,
});

/* Evaluate the first argument, then return the second argument. */
static void prim_seq(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    state.forceValue(*args[0], pos);
    state.forceValue(*args[1], pos);
    v = *args[1];
}

static RegisterPrimOp primop_seq({
    .name = "__seq",
    .args = {"e1", "e2"},
    .doc = R"(
      Evaluate *e1*, then evaluate and return *e2*. This ensures that a
      computation is strict in the value of *e1*.
    )",
    .fun = prim_seq,
});

/* Evaluate the first argument deeply (i.e. recursing into lists and
   attrsets), then return the second argument. */
static void prim_deepSeq(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    state.forceValueDeep(*args[0]);
    state.forceValue(*args[1], pos);
    v = *args[1];
}

static RegisterPrimOp primop_deepSeq({
    .name = "__deepSeq",
    .args = {"e1", "e2"},
    .doc = R"(
      This is like `seq e1 e2`, except that *e1* is evaluated *deeply*:
      if it’s a list or set, its elements or attributes are also
      evaluated recursively.
    )",
    .fun = prim_deepSeq,
});

/* Evaluate the first expression and print it on standard error.  Then
   return the second expression.  Useful for debugging. */
static void prim_trace(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    state.forceValue(*args[0], pos);
    if (args[0]->type() == nString)
        printError("trace: %1%", args[0]->string.s);
    else
        printError("trace: %1%", ValuePrinter(state, *args[0]));
    if (evalSettings.builtinsTraceDebugger && state.debugRepl && !state.debugTraces.empty()) {
        const DebugTrace & last = state.debugTraces.front();
        state.runDebugRepl(nullptr, last.env, last.expr);
    }
    state.forceValue(*args[1], pos);
    v = *args[1];
}

static RegisterPrimOp primop_trace({
    .name = "__trace",
    .args = {"e1", "e2"},
    .doc = R"(
      Evaluate *e1* and print its abstract syntax representation on
      standard error. Then return *e2*. This function is useful for
      debugging.

      If the
      [`debugger-on-trace`](@docroot@/command-ref/conf-file.md#conf-debugger-on-trace)
      option is set to `true` and the `--debugger` flag is given, the
      interactive debugger will be started when `trace` is called (like
      [`break`](@docroot@/language/builtins.md#builtins-break)).
    )",
    .fun = prim_trace,
});


/* Takes two arguments and evaluates to the second one. Used as the
 * builtins.traceVerbose implementation when --trace-verbose is not enabled
 */
static void prim_second(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    state.forceValue(*args[1], pos);
    v = *args[1];
}

/*************************************************************
 * Derivations
 *************************************************************/

static void derivationStrictInternal(EvalState & state, const std::string & name, Bindings * attrs, Value & v);

/* Construct (as a unobservable side effect) a Nix derivation
   expression that performs the derivation described by the argument
   set.  Returns the original set extended with the following
   attributes: `outPath' containing the primary output path of the
   derivation; `drvPath' containing the path of the Nix expression;
   and `type' set to `derivation' to indicate that this is a
   derivation. */
static void prim_derivationStrict(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    state.forceAttrs(*args[0], pos, "while evaluating the argument passed to builtins.derivationStrict");

    Bindings * attrs = args[0]->attrs;

    /* Figure out the name first (for stack backtraces). */
    Bindings::iterator nameAttr = getAttr(state, state.sName, attrs, "in the attrset passed as argument to builtins.derivationStrict");

    std::string drvName;
    try {
        drvName = state.forceStringNoCtx(*nameAttr->value, pos, "while evaluating the `name` attribute passed to builtins.derivationStrict");
    } catch (Error & e) {
        e.addTrace(state.positions[nameAttr->pos], "while evaluating the derivation attribute 'name'");
        throw;
    }

    try {
        derivationStrictInternal(state, drvName, attrs, v);
    } catch (Error & e) {
        Pos pos = state.positions[nameAttr->pos];
        /*
         * Here we make two abuses of the error system
         *
         * 1. We print the location as a string to avoid a code snippet being
         * printed. While the location of the name attribute is a good hint, the
         * exact code there is irrelevant.
         *
         * 2. We mark this trace as a frame trace, meaning that we stop printing
         * less important traces from now on. In particular, this prevents the
         * display of the automatic "while calling builtins.derivationStrict"
         * trace, which is of little use for the public we target here.
         *
         * Please keep in mind that error reporting is done on a best-effort
         * basis in nix. There is no accurate location for a derivation, as it
         * often results from the composition of several functions
         * (derivationStrict, derivation, mkDerivation, mkPythonModule, etc.)
         */
        e.addTrace(nullptr, HintFmt(
                "while evaluating derivation '%s'\n"
                "  whose name attribute is located at %s",
                drvName, pos));
        throw;
    }
}

static void derivationStrictInternal(EvalState & state, const std::string &
drvName, Bindings * attrs, Value & v)
{
    /* Check whether attributes should be passed as a JSON file. */
    using nlohmann::json;
    std::optional<json> jsonObject;
    auto pos = v.determinePos(noPos);
    auto attr = attrs->find(state.sStructuredAttrs);
    if (attr != attrs->end() &&
        state.forceBool(*attr->value, pos,
                        "while evaluating the `__structuredAttrs` "
                        "attribute passed to builtins.derivationStrict"))
        jsonObject = json::object();

    /* Check whether null attributes should be ignored. */
    bool ignoreNulls = false;
    attr = attrs->find(state.sIgnoreNulls);
    if (attr != attrs->end())
        ignoreNulls = state.forceBool(*attr->value, pos, "while evaluating the `__ignoreNulls` attribute " "passed to builtins.derivationStrict");

    /* Build the derivation expression by processing the attributes. */
    Derivation drv;
    drv.name = drvName;

    NixStringContext context;

    bool contentAddressed = false;
    bool isImpure = false;
    std::optional<std::string> outputHash;
    std::string outputHashAlgo;
    std::optional<ContentAddressMethod> ingestionMethod;

    StringSet outputs;
    outputs.insert("out");

    for (auto & i : attrs->lexicographicOrder(state.symbols)) {
        if (i->name == state.sIgnoreNulls) continue;
        const std::string & key = state.symbols[i->name];
        vomit("processing attribute '%1%'", key);

        auto handleHashMode = [&](const std::string_view s) {
            if (s == "recursive") ingestionMethod = FileIngestionMethod::Recursive;
            else if (s == "flat") ingestionMethod = FileIngestionMethod::Flat;
            else if (s == "text") {
                experimentalFeatureSettings.require(Xp::DynamicDerivations);
                ingestionMethod = TextIngestionMethod {};
            } else
                state.error<EvalError>(
                    "invalid value '%s' for 'outputHashMode' attribute", s
                ).atPos(v).debugThrow();
        };

        auto handleOutputs = [&](const Strings & ss) {
            outputs.clear();
            for (auto & j : ss) {
                if (outputs.find(j) != outputs.end())
                    state.error<EvalError>("duplicate derivation output '%1%'", j)
                        .atPos(v)
                        .debugThrow();
                /* !!! Check whether j is a valid attribute
                   name. */
                /* Derivations cannot be named ‘drv’, because
                   then we'd have an attribute ‘drvPath’ in
                   the resulting set. */
                if (j == "drv")
                    state.error<EvalError>("invalid derivation output name 'drv'")
                        .atPos(v)
                        .debugThrow();
                outputs.insert(j);
            }
            if (outputs.empty())
                state.error<EvalError>("derivation cannot have an empty set of outputs")
                    .atPos(v)
                    .debugThrow();
        };

        try {
            // This try-catch block adds context for most errors.
            // Use this empty error context to signify that we defer to it.
            const std::string_view context_below("");

            if (ignoreNulls) {
                state.forceValue(*i->value, pos);
                if (i->value->type() == nNull) continue;
            }

            if (i->name == state.sContentAddressed && state.forceBool(*i->value, pos, context_below)) {
                contentAddressed = true;
                experimentalFeatureSettings.require(Xp::CaDerivations);
            }

            else if (i->name == state.sImpure && state.forceBool(*i->value, pos, context_below)) {
                isImpure = true;
                experimentalFeatureSettings.require(Xp::ImpureDerivations);
            }

            /* The `args' attribute is special: it supplies the
               command-line arguments to the builder. */
            else if (i->name == state.sArgs) {
                state.forceList(*i->value, pos, context_below);
                for (auto elem : i->value->listItems()) {
                    auto s = state.coerceToString(pos, *elem, context,
                            "while evaluating an element of the argument list",
                            true).toOwned();
                    drv.args.push_back(s);
                }
            }

            /* All other attributes are passed to the builder through
               the environment. */
            else {

                if (jsonObject) {

                    if (i->name == state.sStructuredAttrs) continue;

                    (*jsonObject)[key] = printValueAsJSON(state, true, *i->value, pos, context);

                    if (i->name == state.sBuilder)
                        drv.builder = state.forceString(*i->value, context, pos, context_below);
                    else if (i->name == state.sSystem)
                        drv.platform = state.forceStringNoCtx(*i->value, pos, context_below);
                    else if (i->name == state.sOutputHash)
                        outputHash = state.forceStringNoCtx(*i->value, pos, context_below);
                    else if (i->name == state.sOutputHashAlgo)
                        outputHashAlgo = state.forceStringNoCtx(*i->value, pos, context_below);
                    else if (i->name == state.sOutputHashMode)
                        handleHashMode(state.forceStringNoCtx(*i->value, pos, context_below));
                    else if (i->name == state.sOutputs) {
                        /* Require ‘outputs’ to be a list of strings. */
                        state.forceList(*i->value, pos, context_below);
                        Strings ss;
                        for (auto elem : i->value->listItems())
                            ss.emplace_back(state.forceStringNoCtx(*elem, pos, context_below));
                        handleOutputs(ss);
                    }

                } else {
                    auto s = state.coerceToString(pos, *i->value, context, context_below, true).toOwned();
                    drv.env.emplace(key, s);
                    if (i->name == state.sBuilder) drv.builder = std::move(s);
                    else if (i->name == state.sSystem) drv.platform = std::move(s);
                    else if (i->name == state.sOutputHash) outputHash = std::move(s);
                    else if (i->name == state.sOutputHashAlgo) outputHashAlgo = std::move(s);
                    else if (i->name == state.sOutputHashMode) handleHashMode(s);
                    else if (i->name == state.sOutputs)
                        handleOutputs(tokenizeString<Strings>(s));
                }

            }

        } catch (Error & e) {
            e.addTrace(state.positions[i->pos],
                HintFmt("while evaluating attribute '%1%' of derivation '%2%'", key, drvName));
            throw;
        }
    }

    if (jsonObject) {
        drv.env.emplace("__json", jsonObject->dump());
        jsonObject.reset();
    }

    /* Everything in the context of the strings in the derivation
       attributes should be added as dependencies of the resulting
       derivation. */
    for (auto & c : context) {
        std::visit(overloaded {
            /* Since this allows the builder to gain access to every
               path in the dependency graph of the derivation (including
               all outputs), all paths in the graph must be added to
               this derivation's list of inputs to ensure that they are
               available when the builder runs. */
            [&](const NixStringContextElem::DrvDeep & d) {
                /* !!! This doesn't work if readOnlyMode is set. */
                StorePathSet refs;
                state.store->computeFSClosure(d.drvPath, refs);
                for (auto & j : refs) {
                    drv.inputSrcs.insert(j);
                    if (j.isDerivation()) {
                        drv.inputDrvs.map[j].value = state.store->readDerivation(j).outputNames();
                    }
                }
            },
            [&](const NixStringContextElem::Built & b) {
                drv.inputDrvs.ensureSlot(*b.drvPath).value.insert(b.output);
            },
            [&](const NixStringContextElem::Opaque & o) {
                drv.inputSrcs.insert(o.path);
            },
        }, c.raw);
    }

    /* Do we have all required attributes? */
    if (drv.builder == "")
        state.error<EvalError>("required attribute 'builder' missing")
            .atPos(v)
            .debugThrow();

    if (drv.platform == "")
        state.error<EvalError>("required attribute 'system' missing")
            .atPos(v)
            .debugThrow();

    /* Check whether the derivation name is valid. */
    if (isDerivation(drvName) &&
        !(ingestionMethod == ContentAddressMethod { TextIngestionMethod { } } &&
          outputs.size() == 1 &&
          *(outputs.begin()) == "out"))
    {
        state.error<EvalError>(
            "derivation names are allowed to end in '%s' only if they produce a single derivation file",
            drvExtension
        ).atPos(v).debugThrow();
    }

    if (outputHash) {
        /* Handle fixed-output derivations.

           Ignore `__contentAddressed` because fixed output derivations are
           already content addressed. */
        if (outputs.size() != 1 || *(outputs.begin()) != "out")
            state.error<EvalError>(
                "multiple outputs are not supported in fixed-output derivations"
            ).atPos(v).debugThrow();

        auto h = newHashAllowEmpty(*outputHash, parseHashTypeOpt(outputHashAlgo));

        auto method = ingestionMethod.value_or(FileIngestionMethod::Flat);

        DerivationOutput::CAFixed dof {
            .ca = ContentAddress {
                .method = std::move(method),
                .hash = std::move(h),
            },
        };

        drv.env["out"] = state.store->printStorePath(dof.path(*state.store, drvName, "out"));
        drv.outputs.insert_or_assign("out", std::move(dof));
    }

    else if (contentAddressed || isImpure) {
        if (contentAddressed && isImpure)
            state.error<EvalError>("derivation cannot be both content-addressed and impure")
                .atPos(v).debugThrow();

        auto ht = parseHashTypeOpt(outputHashAlgo).value_or(htSHA256);
        auto method = ingestionMethod.value_or(FileIngestionMethod::Recursive);

        for (auto & i : outputs) {
            drv.env[i] = hashPlaceholder(i);
            if (isImpure)
                drv.outputs.insert_or_assign(i,
                    DerivationOutput::Impure {
                        .method = method,
                        .hashType = ht,
                    });
            else
                drv.outputs.insert_or_assign(i,
                    DerivationOutput::CAFloating {
                        .method = method,
                        .hashType = ht,
                    });
        }
    }

    else {
        /* Compute a hash over the "masked" store derivation, which is
           the final one except that in the list of outputs, the
           output paths are empty strings, and the corresponding
           environment variables have an empty value.  This ensures
           that changes in the set of output names do get reflected in
           the hash. */
        for (auto & i : outputs) {
            drv.env[i] = "";
            drv.outputs.insert_or_assign(i,
                DerivationOutput::Deferred { });
        }

        auto hashModulo = hashDerivationModulo(*state.store, Derivation(drv), true);
        switch (hashModulo.kind) {
        case DrvHash::Kind::Regular:
            for (auto & i : outputs) {
                auto h = get(hashModulo.hashes, i);
                if (!h)
                    state.error<AssertionError>(
                        "derivation produced no hash for output '%s'",
                        i
                    ).atPos(v).debugThrow();
                auto outPath = state.store->makeOutputPath(i, *h, drvName);
                drv.env[i] = state.store->printStorePath(outPath);
                drv.outputs.insert_or_assign(
                    i,
                    DerivationOutput::InputAddressed {
                        .path = std::move(outPath),
                    });
            }
            break;
            ;
        case DrvHash::Kind::Deferred:
            for (auto & i : outputs) {
                drv.outputs.insert_or_assign(i, DerivationOutput::Deferred {});
            }
        }
    }

    /* Write the resulting term into the Nix store directory. */
    auto drvPath = writeDerivation(*state.store, drv, state.repair);
    auto drvPathS = state.store->printStorePath(drvPath);

    printMsg(lvlChatty, "instantiated '%1%' -> '%2%'", drvName, drvPathS);

    /* Optimisation, but required in read-only mode! because in that
       case we don't actually write store derivations, so we can't
       read them later. */
    {
        auto h = hashDerivationModulo(*state.store, drv, false);
        drvHashes.lock()->insert_or_assign(drvPath, h);
    }

    auto result = state.buildBindings(1 + drv.outputs.size());
    result.alloc(state.sDrvPath).mkString(drvPathS, {
        NixStringContextElem::DrvDeep { .drvPath = drvPath },
    });
    for (auto & i : drv.outputs)
        mkOutputString(state, result, drvPath, i);

    v.mkAttrs(result);
}

static RegisterPrimOp primop_derivationStrict(PrimOp {
    .name = "derivationStrict",
    .arity = 1,
    .fun = prim_derivationStrict,
});



/*************************************************************
 * Paths
 *************************************************************/

/*************************************************************
 * Creating files
 *************************************************************/


/* Convert the argument (which can be any Nix expression) to an XML
   representation returned in a string.  Not all Nix expressions can
   be sensibly or completely represented (e.g., functions). */
static void prim_toXML(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    std::ostringstream out;
    NixStringContext context;
    printValueAsXML(state, true, false, *args[0], out, context, pos);
    v.mkString(out.str(), context);
}

static RegisterPrimOp primop_toXML({
    .name = "__toXML",
    .args = {"e"},
    .doc = R"(
      Return a string containing an XML representation of *e*. The main
      application for `toXML` is to communicate information with the
      builder in a more structured format than plain environment
      variables.

      Here is an example where this is the case:

      ```nix
      { stdenv, fetchurl, libxslt, jira, uberwiki }:

      stdenv.mkDerivation (rec {
        name = "web-server";

        buildInputs = [ libxslt ];

        builder = builtins.toFile "builder.sh" "
          source $stdenv/setup
          mkdir $out
          echo "$servlets" | xsltproc ${stylesheet} - > $out/server-conf.xml ①
        ";

        stylesheet = builtins.toFile "stylesheet.xsl" ②
         "<?xml version='1.0' encoding='UTF-8'?>
          <xsl:stylesheet xmlns:xsl='http://www.w3.org/1999/XSL/Transform' version='1.0'>
            <xsl:template match='/'>
              <Configure>
                <xsl:for-each select='/expr/list/attrs'>
                  <Call name='addWebApplication'>
                    <Arg><xsl:value-of select=\"attr[@name = 'path']/string/@value\" /></Arg>
                    <Arg><xsl:value-of select=\"attr[@name = 'war']/path/@value\" /></Arg>
                  </Call>
                </xsl:for-each>
              </Configure>
            </xsl:template>
          </xsl:stylesheet>
        ";

        servlets = builtins.toXML [ ③
          { path = "/bugtracker"; war = jira + "/lib/atlassian-jira.war"; }
          { path = "/wiki"; war = uberwiki + "/uberwiki.war"; }
        ];
      })
      ```

      The builder is supposed to generate the configuration file for a
      [Jetty servlet container](http://jetty.mortbay.org/). A servlet
      container contains a number of servlets (`*.war` files) each
      exported under a specific URI prefix. So the servlet configuration
      is a list of sets containing the `path` and `war` of the servlet
      (①). This kind of information is difficult to communicate with the
      normal method of passing information through an environment
      variable, which just concatenates everything together into a
      string (which might just work in this case, but wouldn’t work if
      fields are optional or contain lists themselves). Instead the Nix
      expression is converted to an XML representation with `toXML`,
      which is unambiguous and can easily be processed with the
      appropriate tools. For instance, in the example an XSLT stylesheet
      (at point ②) is applied to it (at point ①) to generate the XML
      configuration file for the Jetty server. The XML representation
      produced at point ③ by `toXML` is as follows:

      ```xml
      <?xml version='1.0' encoding='utf-8'?>
      <expr>
        <list>
          <attrs>
            <attr name="path">
              <string value="/bugtracker" />
            </attr>
            <attr name="war">
              <path value="/nix/store/d1jh9pasa7k2...-jira/lib/atlassian-jira.war" />
            </attr>
          </attrs>
          <attrs>
            <attr name="path">
              <string value="/wiki" />
            </attr>
            <attr name="war">
              <path value="/nix/store/y6423b1yi4sx...-uberwiki/uberwiki.war" />
            </attr>
          </attrs>
        </list>
      </expr>
      ```

      Note that we used the `toFile` built-in to write the builder and
      the stylesheet “inline” in the Nix expression. The path of the
      stylesheet is spliced into the builder using the syntax `xsltproc
      ${stylesheet}`.
    )",
    .fun = prim_toXML,
});

/* Convert the argument (which can be any Nix expression) to a JSON
   string.  Not all Nix expressions can be sensibly or completely
   represented (e.g., functions). */
static void prim_toJSON(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    std::ostringstream out;
    NixStringContext context;
    printValueAsJSON(state, true, *args[0], pos, out, context);
    v.mkString(out.str(), context);
}

static RegisterPrimOp primop_toJSON({
    .name = "__toJSON",
    .args = {"e"},
    .doc = R"(
      Return a string containing a JSON representation of *e*. Strings,
      integers, floats, booleans, nulls and lists are mapped to their JSON
      equivalents. Sets (except derivations) are represented as objects.
      Derivations are translated to a JSON string containing the
      derivation’s output path. Paths are copied to the store and
      represented as a JSON string of the resulting store path.
    )",
    .fun = prim_toJSON,
});

/* Parse a JSON string to a value. */
static void prim_fromJSON(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    auto s = state.forceStringNoCtx(*args[0], pos, "while evaluating the first argument passed to builtins.fromJSON");
    try {
        parseJSON(state, s, v);
    } catch (JSONParseError &e) {
        e.addTrace(state.positions[pos], "while decoding a JSON string");
        throw;
    }
}

static RegisterPrimOp primop_fromJSON({
    .name = "__fromJSON",
    .args = {"e"},
    .doc = R"(
      Convert a JSON string to a Nix value. For example,

      ```nix
      builtins.fromJSON ''{"x": [1, 2, 3], "y": null}''
      ```

      returns the value `{ x = [ 1 2 3 ]; y = null; }`.
    )",
    .fun = prim_fromJSON,
});

/* Store a string in the Nix store as a source file that can be used
   as an input by derivations. */



/*************************************************************
 * Sets
 *************************************************************/

/* Return position information of the specified attribute. */
static void prim_unsafeGetAttrPos(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    auto attr = state.forceStringNoCtx(*args[0], pos, "while evaluating the first argument passed to builtins.unsafeGetAttrPos");
    state.forceAttrs(*args[1], pos, "while evaluating the second argument passed to builtins.unsafeGetAttrPos");
    Bindings::iterator i = args[1]->attrs->find(state.symbols.create(attr));
    if (i == args[1]->attrs->end())
        v.mkNull();
    else
        state.mkPos(v, i->pos);
}

static RegisterPrimOp primop_unsafeGetAttrPos(PrimOp {
    .name = "__unsafeGetAttrPos",
    .arity = 2,
    .fun = prim_unsafeGetAttrPos,
});

// access to exact position information (ie, line and colum numbers) is deferred
// due to the cost associated with calculating that information and how rarely
// it is used in practice. this is achieved by creating thunks to otherwise
// inaccessible primops that are not exposed as __op or under builtins to turn
// the internal PosIdx back into a line and column number, respectively. exposing
// these primops in any way would at best be not useful and at worst create wildly
// indeterministic eval results depending on parse order of files.
//
// in a simpler world this would instead be implemented as another kind of thunk,
// but each type of thunk has an associated runtime cost in the current evaluator.
// as with black holes this cost is too high to justify another thunk type to check
// for in the very hot path that is forceValue.
static struct LazyPosAcessors {
    PrimOp primop_lineOfPos{
        .arity = 1,
        .fun = [] (EvalState & state, PosIdx pos, Value * * args, Value & v) {
            v.mkInt(state.positions[PosIdx(args[0]->integer)].line);
        }
    };
    PrimOp primop_columnOfPos{
        .arity = 1,
        .fun = [] (EvalState & state, PosIdx pos, Value * * args, Value & v) {
            v.mkInt(state.positions[PosIdx(args[0]->integer)].column);
        }
    };

    Value lineOfPos, columnOfPos;

    LazyPosAcessors()
    {
        lineOfPos.mkPrimOp(&primop_lineOfPos);
        columnOfPos.mkPrimOp(&primop_columnOfPos);
    }

    void operator()(EvalState & state, const PosIdx pos, Value & line, Value & column)
    {
        Value * posV = state.allocValue();
        posV->mkInt(pos.id);
        line.mkApp(&lineOfPos, posV);
        column.mkApp(&columnOfPos, posV);
    }
} makeLazyPosAccessors;

void makePositionThunks(EvalState & state, const PosIdx pos, Value & line, Value & column)
{
    makeLazyPosAccessors(state, pos, line, column);
}

/* Determine whether the argument is a set. */


/* Builds a set from a list specifying (name, value) pairs.  To be
   precise, a list [{name = "name1"; value = value1;} ... {name =
   "nameN"; value = valueN;}] is transformed to {name1 = value1;
   ... nameN = valueN;}.  In case of duplicate occurrences of the same
   name, the first takes precedence. */


/*  */



/*************************************************************
 * Lists
 *************************************************************/



/* Return the first element of a list. */


/* Apply a function to every element of a list. */

/* Return the length of a list.  This is an O(1) time operation. */






/*************************************************************
 * Integer arithmetic
 *************************************************************/





/*************************************************************
 * String manipulation
 *************************************************************/


/*************************************************************
 * Versions
 *************************************************************/



/*************************************************************
 * Primop registration
 *************************************************************/


RegisterPrimOp::PrimOps * RegisterPrimOp::primOps;


RegisterPrimOp::RegisterPrimOp(PrimOp && primOp)
{
    if (!primOps) primOps = new PrimOps;
    primOps->push_back(std::move(primOp));
}


void EvalState::createBaseEnv()
{
    baseEnv.up = 0;

    /* Add global constants such as `true' to the base environment. */
    Value v;

    /* `builtins' must be first! */
    v.mkAttrs(buildBindings(128).finish());
    addConstant("builtins", v, {
        .type = nAttrs,
        .doc = R"(
          Contains all the [built-in functions](@docroot@/language/builtins.md) and values.

          Since built-in functions were added over time, [testing for attributes](./operators.md#has-attribute) in `builtins` can be used for graceful fallback on older Nix installations:

          ```nix
          # if hasContext is not available, we assume `s` has a context
          if builtins ? hasContext then builtins.hasContext s else true
          ```
        )",
    });

    v.mkBool(true);
    addConstant("true", v, {
        .type = nBool,
        .doc = R"(
          Primitive value.

          It can be returned by
          [comparison operators](@docroot@/language/operators.md#Comparison)
          and used in
          [conditional expressions](@docroot@/language/constructs.md#Conditionals).

          The name `true` is not special, and can be shadowed:

          ```nix-repl
          nix-repl> let true = 1; in true
          1
          ```
        )",
    });

    v.mkBool(false);
    addConstant("false", v, {
        .type = nBool,
        .doc = R"(
          Primitive value.

          It can be returned by
          [comparison operators](@docroot@/language/operators.md#Comparison)
          and used in
          [conditional expressions](@docroot@/language/constructs.md#Conditionals).

          The name `false` is not special, and can be shadowed:

          ```nix-repl
          nix-repl> let false = 1; in false
          1
          ```
        )",
    });

    v.mkNull();
    addConstant("null", v, {
        .type = nNull,
        .doc = R"(
          Primitive value.

          The name `null` is not special, and can be shadowed:

          ```nix-repl
          nix-repl> let null = 1; in null
          1
          ```
        )",
    });

    if (!evalSettings.pureEval) {
        v.mkInt(time(0));
    }
    addConstant("__currentTime", v, {
        .type = nInt,
        .doc = R"(
          Return the [Unix time](https://en.wikipedia.org/wiki/Unix_time) at first evaluation.
          Repeated references to that name will re-use the initially obtained value.

          Example:

          ```console
          $ nix repl
          Welcome to Nix 2.15.1 Type :? for help.

          nix-repl> builtins.currentTime
          1683705525

          nix-repl> builtins.currentTime
          1683705525
          ```

          The [store path](@docroot@/glossary.md#gloss-store-path) of a derivation depending on `currentTime` will differ for each evaluation, unless both evaluate `builtins.currentTime` in the same second.
        )",
        .impureOnly = true,
    });

    if (!evalSettings.pureEval)
        v.mkString(evalSettings.getCurrentSystem());
    addConstant("__currentSystem", v, {
        .type = nString,
        .doc = R"(
          The value of the
          [`eval-system`](@docroot@/command-ref/conf-file.md#conf-eval-system)
          or else
          [`system`](@docroot@/command-ref/conf-file.md#conf-system)
          configuration option.

          It can be used to set the `system` attribute for [`builtins.derivation`](@docroot@/language/derivations.md) such that the resulting derivation can be built on the same system that evaluates the Nix expression:

          ```nix
           builtins.derivation {
             # ...
             system = builtins.currentSystem;
          }
          ```

          It can be overridden in order to create derivations for different system than the current one:

          ```console
          $ nix-instantiate --system "mips64-linux" --eval --expr 'builtins.currentSystem'
          "mips64-linux"
          ```
        )",
        .impureOnly = true,
    });

    v.mkString("2.18.3-lix");
    addConstant("__nixVersion", v, {
        .type = nString,
        .doc = R"(
          Legacy version of Nix. Always returns "2.18.3-lix" on Lix.

          Code in the Nix language should use other means of feature detection
          like detecting the presence of builtins, rather than trying to find
          the version of the Nix implementation, as there may be other Nix
          implementations with different feature combinations.

          If the feature you want to write compatibility code for cannot be
          detected by any means, please file a Lix bug.
        )",
    });

    v.mkString(store->storeDir);
    addConstant("__storeDir", v, {
        .type = nString,
        .doc = R"(
          Logical file system location of the [Nix store](@docroot@/glossary.md#gloss-store) currently in use.

          This value is determined by the `store` parameter in [Store URLs](@docroot@/command-ref/new-cli/nix3-help-stores.md):

          ```shell-session
          $ nix-instantiate --store 'dummy://?store=/blah' --eval --expr builtins.storeDir
          "/blah"
          ```
        )",
    });

    /* Legacy language version.
     * This is fixed at 6, and will never change in the future on Lix.
     * A better language versioning construct needs to be built instead. */
    v.mkInt(6);
    addConstant("__langVersion", v, {
        .type = nInt,
        .doc = R"(
          The legacy version of the Nix language. Always is `6` on Lix,
          matching Nix 2.18.

          Code in the Nix language should use other means of feature detection
          like detecting the presence of builtins, rather than trying to find
          the version of the Nix implementation, as there may be other Nix
          implementations with different feature combinations.

          If the feature you want to write compatibility code for cannot be
          detected by any means, please file a Lix bug.
        )",
    });

    // Miscellaneous
    if (evalSettings.enableNativeCode) {
        addPrimOp({
            .name = "__importNative",
            .arity = 2,
            .fun = prim_importNative,
        });
        addPrimOp({
            .name = "__exec",
            .arity = 1,
            .fun = prim_exec,
        });
    }

    addPrimOp({
        .name = "__traceVerbose",
        .args = { "e1", "e2" },
        .arity = 2,
        .doc = R"(
          Evaluate *e1* and print its abstract syntax representation on standard
          error if `--trace-verbose` is enabled. Then return *e2*. This function
          is useful for debugging.
        )",
        .fun = evalSettings.traceVerbose ? prim_trace : prim_second,
    });

    /* Add a value containing the current Nix expression search path. */
    mkList(v, searchPath.elements.size());
    int n = 0;
    for (auto & i : searchPath.elements) {
        auto attrs = buildBindings(2);
        attrs.alloc("path").mkString(i.path.s);
        attrs.alloc("prefix").mkString(i.prefix.s);
        (v.listElems()[n++] = allocValue())->mkAttrs(attrs);
    }
    addConstant("__nixPath", v, {
        .type = nList,
        .doc = R"(
          The search path used to resolve angle bracket path lookups.

          Angle bracket expressions can be
          [desugared](https://en.wikipedia.org/wiki/Syntactic_sugar)
          using this and
          [`builtins.findFile`](./builtins.html#builtins-findFile):

          ```nix
          <nixpkgs>
          ```

          is equivalent to:

          ```nix
          builtins.findFile builtins.nixPath "nixpkgs"
          ```
        )",
    });

    if (RegisterPrimOp::primOps)
        for (auto & primOp : *RegisterPrimOp::primOps)
            if (experimentalFeatureSettings.isEnabled(primOp.experimentalFeature))
            {
                auto primOpAdjusted = primOp;
                primOpAdjusted.arity = std::max(primOp.args.size(), primOp.arity);
                addPrimOp(std::move(primOpAdjusted));
            }

    /* Add a wrapper around the derivation primop that computes the
       `drvPath' and `outPath' attributes lazily.

       Null docs because it is documented separately.
       */
    auto vDerivation = allocValue();
    addConstant("derivation", vDerivation, {
        .type = nFunction,
    });

    /* Now that we've added all primops, sort the `builtins' set,
       because attribute lookups expect it to be sorted. */
    baseEnv.values[0]->attrs->sort();

    staticBaseEnv->sort();

    /* Note: we have to initialize the 'derivation' constant *after*
       building baseEnv/staticBaseEnv because it uses 'builtins'. */
    char code[] =
        #include "primops/derivation.nix.gen.hh"
        // the parser needs two NUL bytes as terminators; one of them
        // is implied by being a C string.
        "\0";
    eval(parse(code, sizeof(code), derivationInternal, {CanonPath::root}, staticBaseEnv), *vDerivation);
}


}
