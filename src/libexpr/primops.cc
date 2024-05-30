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

SourcePath realisePath(EvalState & state, const PosIdx pos, Value & v, const RealisePathFlags flags)
{
    NixStringContext context;

    auto path = state.coerceToPath(noPos, v, context, "while realising the context of a path");

    try {
        StringMap rewrites = state.realiseContext(context);

        auto realPath = state.rootPath(CanonPath(state.toRealPath(rewriteStrings(path.path.abs(), rewrites), context)));

        return flags.checkForPureEval
            ? state.checkSourcePath(realPath)
            : realPath;
    } catch (Error & e) {
        e.addTrace(state.positions[pos], "while realising the context of path '%s'", path);
        throw;
    }
}

/**
 * Add and attribute to the given attribute map from the output name to
 * the output path, or a placeholder.
 *
 * Where possible the path is used, but for floating CA derivations we
 * may not know it. For sake of determinism we always assume we don't
 * and instead put in a place holder. In either case, however, the
 * string context will contain the drv path and output name, so
 * downstream derivations will have the proper dependency, and in
 * addition, before building, the placeholder will be rewritten to be
 * the actual path.
 *
 * The 'drv' and 'drvPath' outputs must correspond.
 */
static void mkOutputString(
    EvalState & state,
    BindingsBuilder & attrs,
    const StorePath & drvPath,
    const std::pair<std::string, DerivationOutput> & o)
{
    state.mkOutputString(
        attrs.alloc(o.first),
        SingleDerivedPath::Built {
            .drvPath = makeConstantStorePathRef(drvPath),
            .output = o.first,
        },
        o.second.path(*state.store, Derivation::nameFromPath(drvPath), o.first));
}

/* Load and evaluate an expression from path specified by the
   argument. */
static void import(EvalState & state, const PosIdx pos, Value & vPath, Value * vScope, Value & v)
{
    auto path = realisePath(state, pos, vPath);
    auto path2 = path.path.abs();

    // FIXME
    auto isValidDerivationInStore = [&]() -> std::optional<StorePath> {
        if (!state.store->isStorePath(path2))
            return std::nullopt;
        auto storePath = state.store->parseStorePath(path2);
        if (!(state.store->isValidPath(storePath) && isDerivation(path2)))
            return std::nullopt;
        return storePath;
    };

    if (auto storePath = isValidDerivationInStore()) {
        Derivation drv = state.store->readDerivation(*storePath);
        auto attrs = state.buildBindings(3 + drv.outputs.size());
        attrs.alloc(state.sDrvPath).mkString(path2, {
            NixStringContextElem::DrvDeep { .drvPath = *storePath },
        });
        attrs.alloc(state.sName).mkString(drv.env["name"]);
        auto & outputsVal = attrs.alloc(state.sOutputs);
        state.mkList(outputsVal, drv.outputs.size());

        for (const auto & [i, o] : enumerate(drv.outputs)) {
            mkOutputString(state, attrs, *storePath, o);
            (outputsVal.listElems()[i] = state.allocValue())->mkString(o.first);
        }

        auto w = state.allocValue();
        w->mkAttrs(attrs);

        if (!state.vImportedDrvToDerivation) {
            state.vImportedDrvToDerivation = allocRootValue(state.allocValue());
            state.eval(state.parseExprFromString(
                #include "imported-drv-to-derivation.nix.gen.hh"
                , CanonPath::root), **state.vImportedDrvToDerivation);
        }

        state.forceFunction(**state.vImportedDrvToDerivation, pos, "while evaluating imported-drv-to-derivation.nix.gen.hh");
        v.mkApp(*state.vImportedDrvToDerivation, w);
        state.forceAttrs(v, pos, "while calling imported-drv-to-derivation.nix.gen.hh");
    }

    else if (path2 == corepkgsPrefix + "fetchurl.nix") {
        state.eval(state.parseExprFromString(
            #include "fetchurl.nix.gen.hh"
            , CanonPath::root), v);
    }

    else {
        if (!vScope)
            state.evalFile(path, v);
        else {
            state.forceAttrs(*vScope, pos, "while evaluating the first argument passed to builtins.scopedImport");

            Env * env = &state.allocEnv(vScope->attrs->size());
            env->up = &state.baseEnv;

            auto staticEnv = std::make_shared<StaticEnv>(nullptr, state.staticBaseEnv.get(), vScope->attrs->size());

            unsigned int displ = 0;
            for (auto & attr : *vScope->attrs) {
                staticEnv->vars.emplace_back(attr.name, displ);
                env->values[displ++] = attr.value;
            }

            // No need to call staticEnv.sort(), because
            // args[0]->attrs is already sorted.

            debug("evaluating file '%1%'", path);
            Expr * e = state.parseExprFromFile(resolveExprPath(path), staticEnv);

            e->eval(state, *env, v);
        }
    }
}

static RegisterPrimOp primop_scopedImport(PrimOp {
    .name = "scopedImport",
    .arity = 2,
    .fun = [](EvalState & state, const PosIdx pos, Value * * args, Value & v)
    {
        import(state, pos, *args[1], args[0], v);
    }
});

static RegisterPrimOp primop_import({
    .name = "import",
    .args = {"path"},
    // TODO turn "normal path values" into link below
    .doc = R"(
      Load, parse and return the Nix expression in the file *path*.

      The value *path* can be a path, a string, or an attribute set with an
      `__toString` attribute or a `outPath` attribute (as derivations or flake
      inputs typically have).

      If *path* is a directory, the file `default.nix` in that directory
      is loaded.

      Evaluation aborts if the file doesn’t exist or contains
      an incorrect Nix expression. `import` implements Nix’s module
      system: you can put any Nix expression (such as a set or a
      function) in a separate file, and use it from Nix expressions in
      other files.

      > **Note**
      >
      > Unlike some languages, `import` is a regular function in Nix.
      > Paths using the angle bracket syntax (e.g., `import` *\<foo\>*)
      > are normal [path values](@docroot@/language/values.md#type-path).

      A Nix expression loaded by `import` must not contain any *free
      variables* (identifiers that are not defined in the Nix expression
      itself and are not built-in). Therefore, it cannot refer to
      variables that are in scope at the call site. For instance, if you
      have a calling expression

      ```nix
      rec {
        x = 123;
        y = import ./foo.nix;
      }
      ```

      then the following `foo.nix` will give an error:

      ```nix
      x + 456
      ```

      since `x` is not in scope in `foo.nix`. If you want `x` to be
      available in `foo.nix`, you should pass it as a function argument:

      ```nix
      rec {
        x = 123;
        y = import ./foo.nix x;
      }
      ```

      and

      ```nix
      x: x + 456
      ```

      (The function argument doesn’t have to be called `x` in `foo.nix`;
      any name would work.)
    )",
    .fun = [](EvalState & state, const PosIdx pos, Value * * args, Value & v)
    {
        import(state, pos, *args[0], nullptr, v);
    }
});

/* Want reasonable symbol names, so extern C */
/* !!! Should we pass the Pos or the file name too? */
extern "C" typedef void (*ValueInitializer)(EvalState & state, Value & v);

/* Load a ValueInitializer from a DSO and return whatever it initializes */
void prim_importNative(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    auto path = realisePath(state, pos, *args[0]);

    std::string sym(state.forceStringNoCtx(*args[1], pos, "while evaluating the second argument passed to builtins.importNative"));

    void *handle = dlopen(path.path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!handle)
        state.error<EvalError>("could not open '%1%': %2%", path, dlerror()).debugThrow();

    dlerror();
    ValueInitializer func = (ValueInitializer) dlsym(handle, sym.c_str());
    if(!func) {
        char *message = dlerror();
        if (message)
            state.error<EvalError>("could not load symbol '%1%' from '%2%': %3%", sym, path, message).debugThrow();
        else
            state.error<EvalError>("symbol '%1%' from '%2%' resolved to NULL when a function pointer was expected", sym, path).debugThrow();
    }

    (func)(state, v);

    /* We don't dlclose because v may be a primop referencing a function in the shared object file */
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

/* Return a string representing the type of the expression. */
static void prim_typeOf(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    state.forceValue(*args[0], pos);
    std::string t;
    switch (args[0]->type()) {
        case nInt: t = "int"; break;
        case nBool: t = "bool"; break;
        case nString: t = "string"; break;
        case nPath: t = "path"; break;
        case nNull: t = "null"; break;
        case nAttrs: t = "set"; break;
        case nList: t = "list"; break;
        case nFunction: t = "lambda"; break;
        case nExternal:
            t = args[0]->external->typeOf();
            break;
        case nFloat: t = "float"; break;
        case nThunk: abort();
    }
    v.mkString(t);
}

static RegisterPrimOp primop_typeOf({
    .name = "__typeOf",
    .args = {"e"},
    .doc = R"(
      Return a string representing the type of the value *e*, namely
      `"int"`, `"bool"`, `"string"`, `"path"`, `"null"`, `"set"`,
      `"list"`, `"lambda"` or `"float"`.
    )",
    .fun = prim_typeOf,
});

/* Determine whether the argument is the null value. */
static void prim_isNull(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    state.forceValue(*args[0], pos);
    v.mkBool(args[0]->type() == nNull);
}

static RegisterPrimOp primop_isNull({
    .name = "isNull",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to `null`, and `false` otherwise.

      This is equivalent to `e == null`.
    )",
    .fun = prim_isNull,
});

/* Determine whether the argument is a function. */
static void prim_isFunction(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    state.forceValue(*args[0], pos);
    v.mkBool(args[0]->type() == nFunction);
}

static RegisterPrimOp primop_isFunction({
    .name = "__isFunction",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to a function, and `false` otherwise.
    )",
    .fun = prim_isFunction,
});

/* Determine whether the argument is an integer. */
static void prim_isInt(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    state.forceValue(*args[0], pos);
    v.mkBool(args[0]->type() == nInt);
}

static RegisterPrimOp primop_isInt({
    .name = "__isInt",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to an integer, and `false` otherwise.
    )",
    .fun = prim_isInt,
});

/* Determine whether the argument is a float. */
static void prim_isFloat(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    state.forceValue(*args[0], pos);
    v.mkBool(args[0]->type() == nFloat);
}

static RegisterPrimOp primop_isFloat({
    .name = "__isFloat",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to a float, and `false` otherwise.
    )",
    .fun = prim_isFloat,
});

/* Determine whether the argument is a string. */
static void prim_isString(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    state.forceValue(*args[0], pos);
    v.mkBool(args[0]->type() == nString);
}

static RegisterPrimOp primop_isString({
    .name = "__isString",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to a string, and `false` otherwise.
    )",
    .fun = prim_isString,
});

/* Determine whether the argument is a Boolean. */
static void prim_isBool(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    state.forceValue(*args[0], pos);
    v.mkBool(args[0]->type() == nBool);
}

static RegisterPrimOp primop_isBool({
    .name = "__isBool",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to a bool, and `false` otherwise.
    )",
    .fun = prim_isBool,
});

/* Determine whether the argument is a path. */
static void prim_isPath(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    state.forceValue(*args[0], pos);
    v.mkBool(args[0]->type() == nPath);
}

static RegisterPrimOp primop_isPath({
    .name = "__isPath",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to a path, and `false` otherwise.
    )",
    .fun = prim_isPath,
});

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

/* Return a placeholder string for the specified output that will be
   substituted by the corresponding output path at build time. For
   example, 'placeholder "out"' returns the string
   /1rz4g4znpzjwh1xymhjpm42vipw92pr73vdgl6xs1hycac8kf2n9. At build
   time, any occurrence of this string in an derivation attribute will
   be replaced with the concrete path in the Nix store of the output
   ‘out’. */
static void prim_placeholder(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    v.mkString(hashPlaceholder(state.forceStringNoCtx(*args[0], pos, "while evaluating the first argument passed to builtins.placeholder")));
}

static RegisterPrimOp primop_placeholder({
    .name = "placeholder",
    .args = {"output"},
    .doc = R"(
      Return a placeholder string for the specified *output* that will be
      substituted by the corresponding output path at build time. Typical
      outputs would be `"out"`, `"bin"` or `"dev"`.
    )",
    .fun = prim_placeholder,
});


/*************************************************************
 * Paths
 *************************************************************/


/* Convert the argument to a path.  !!! obsolete? */
static void prim_toPath(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    NixStringContext context;
    auto path = state.coerceToPath(pos, *args[0], context, "while evaluating the first argument passed to builtins.toPath");
    v.mkString(path.path.abs(), context);
}

static RegisterPrimOp primop_toPath({
    .name = "__toPath",
    .args = {"s"},
    .doc = R"(
      **DEPRECATED.** Use `/. + "/path"` to convert a string into an absolute
      path. For relative paths, use `./. + "/path"`.
    )",
    .fun = prim_toPath,
});

/* Allow a valid store path to be used in an expression.  This is
   useful in some generated expressions such as in nix-push, which
   generates a call to a function with an already existing store path
   as argument.  You don't want to use `toPath' here because it copies
   the path to the Nix store, which yields a copy like
   /nix/store/newhash-oldhash-oldname.  In the past, `toPath' had
   special case behaviour for store paths, but that created weird
   corner cases. */
static void prim_storePath(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    if (evalSettings.pureEval)
        state.error<EvalError>(
            "'%s' is not allowed in pure evaluation mode",
            "builtins.storePath"
        ).atPos(pos).debugThrow();

    NixStringContext context;
    auto path = state.checkSourcePath(state.coerceToPath(pos, *args[0], context, "while evaluating the first argument passed to builtins.storePath")).path;
    /* Resolve symlinks in ‘path’, unless ‘path’ itself is a symlink
       directly in the store.  The latter condition is necessary so
       e.g. nix-push does the right thing. */
    if (!state.store->isStorePath(path.abs()))
        path = CanonPath(canonPath(path.abs(), true));
    if (!state.store->isInStore(path.abs()))
        state.error<EvalError>("path '%1%' is not in the Nix store", path)
            .atPos(pos).debugThrow();
    auto path2 = state.store->toStorePath(path.abs()).first;
    if (!settings.readOnlyMode)
        state.store->ensurePath(path2);
    context.insert(NixStringContextElem::Opaque { .path = path2 });
    v.mkString(path.abs(), context);
}

static RegisterPrimOp primop_storePath({
    .name = "__storePath",
    .args = {"path"},
    .doc = R"(
      This function allows you to define a dependency on an already
      existing store path. For example, the derivation attribute `src
      = builtins.storePath /nix/store/f1d18v1y…-source` causes the
      derivation to depend on the specified path, which must exist or
      be substitutable. Note that this differs from a plain path
      (e.g. `src = /nix/store/f1d18v1y…-source`) in that the latter
      causes the path to be *copied* again to the Nix store, resulting
      in a new path (e.g. `/nix/store/ld01dnzc…-source-source`).

      Not available in [pure evaluation mode](@docroot@/command-ref/conf-file.md#conf-pure-eval).

      See also [`builtins.fetchClosure`](#builtins-fetchClosure).
    )",
    .fun = prim_storePath,
});

static void prim_pathExists(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    auto & arg = *args[0];

    /* We don’t check the path right now, because we don’t want to
       throw if the path isn’t allowed, but just return false (and we
       can’t just catch the exception here because we still want to
       throw if something in the evaluation of `arg` tries to
       access an unauthorized path). */
    auto path = realisePath(state, pos, arg, { .checkForPureEval = false });

    /* SourcePath doesn't know about trailing slash. */
    auto mustBeDir = arg.type() == nString
        && (arg.str().ends_with("/")
            || arg.str().ends_with("/."));

    try {
        auto checked = state
            .checkSourcePath(path)
            .resolveSymlinks(mustBeDir ? SymlinkResolution::Full : SymlinkResolution::Ancestors);

        auto st = checked.maybeLstat();
        auto exists = st && (!mustBeDir || st->type == InputAccessor::tDirectory);
        v.mkBool(exists);
    } catch (SysError & e) {
        /* Don't give away info from errors while canonicalising
           ‘path’ in restricted mode. */
        v.mkBool(false);
    } catch (RestrictedPathError & e) {
        v.mkBool(false);
    }
}

static RegisterPrimOp primop_pathExists({
    .name = "__pathExists",
    .args = {"path"},
    .doc = R"(
      Return `true` if the path *path* exists at evaluation time, and
      `false` otherwise.
    )",
    .fun = prim_pathExists,
});

/* Return the directory of the given path, i.e., everything before the
   last slash.  Return either a path or a string depending on the type
   of the argument. */
static void prim_dirOf(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    state.forceValue(*args[0], pos);
    if (args[0]->type() == nPath) {
        auto path = args[0]->path();
        v.mkPath(path.path.isRoot() ? path : path.parent());
    } else {
        NixStringContext context;
        auto path = state.coerceToString(pos, *args[0], context,
            "while evaluating the first argument passed to 'builtins.dirOf'",
            false, false);
        auto dir = dirOf(*path);
        v.mkString(dir, context);
    }
}

static RegisterPrimOp primop_dirOf({
    .name = "dirOf",
    .args = {"s"},
    .doc = R"(
      Return the directory part of the string *s*, that is, everything
      before the final slash in the string. This is similar to the GNU
      `dirname` command.
    )",
    .fun = prim_dirOf,
});

/* Return the contents of a file as a string. */
static void prim_readFile(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    auto path = realisePath(state, pos, *args[0]);
    auto s = path.readFile();
    if (s.find((char) 0) != std::string::npos)
        state.error<EvalError>(
            "the contents of the file '%1%' cannot be represented as a Nix string",
            path
        ).atPos(pos).debugThrow();
    StorePathSet refs;
    if (state.store->isInStore(path.path.abs())) {
        try {
            refs = state.store->queryPathInfo(state.store->toStorePath(path.path.abs()).first)->references;
        } catch (Error &) { // FIXME: should be InvalidPathError
        }
        // Re-scan references to filter down to just the ones that actually occur in the file.
        auto refsSink = PathRefScanSink::fromPaths(refs);
        refsSink << s;
        refs = refsSink.getResultPaths();
    }
    NixStringContext context;
    for (auto && p : std::move(refs)) {
        context.insert(NixStringContextElem::Opaque {
            .path = std::move((StorePath &&)p),
        });
    }
    v.mkString(s, context);
}

static RegisterPrimOp primop_readFile({
    .name = "__readFile",
    .args = {"path"},
    .doc = R"(
      Return the contents of the file *path* as a string.
    )",
    .fun = prim_readFile,
});

/* Find a file in the Nix search path. Used to implement <x> paths,
   which are desugared to 'findFile __nixPath "x"'. */
static void prim_findFile(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    state.forceList(*args[0], pos, "while evaluating the first argument passed to builtins.findFile");

    SearchPath searchPath;

    for (auto v2 : args[0]->listItems()) {
        state.forceAttrs(*v2, pos, "while evaluating an element of the list passed to builtins.findFile");

        std::string prefix;
        Bindings::iterator i = v2->attrs->find(state.sPrefix);
        if (i != v2->attrs->end())
            prefix = state.forceStringNoCtx(*i->value, pos, "while evaluating the `prefix` attribute of an element of the list passed to builtins.findFile");

        i = getAttr(state, state.sPath, v2->attrs, "in an element of the __nixPath");

        NixStringContext context;
        auto path = state.coerceToString(pos, *i->value, context,
                "while evaluating the `path` attribute of an element of the list passed to builtins.findFile",
                false, false).toOwned();

        try {
            auto rewrites = state.realiseContext(context);
            path = rewriteStrings(path, rewrites);
        } catch (InvalidPathError & e) {
            state.error<EvalError>(
                "cannot find '%1%', since path '%2%' is not valid",
                path,
                e.path
            ).atPos(pos).debugThrow();
        }

        searchPath.elements.emplace_back(SearchPath::Elem {
            .prefix = SearchPath::Prefix { .s = prefix },
            .path = SearchPath::Path { .s = path },
        });
    }

    auto path = state.forceStringNoCtx(*args[1], pos, "while evaluating the second argument passed to builtins.findFile");

    v.mkPath(state.checkSourcePath(state.findFile(searchPath, path, pos)));
}

static RegisterPrimOp primop_findFile(PrimOp {
    .name = "__findFile",
    .args = {"search path", "lookup path"},
    .doc = R"(
      Look up the given path with the given search path.

      A search path is represented list of [attribute sets](./values.md#attribute-set) with two attributes, `prefix`, and `path`.
      `prefix` is a relative path.
      `path` denotes a file system location; the exact syntax depends on the command line interface.

      Examples of search path attribute sets:

      - ```
        {
          prefix = "nixos-config";
          path = "/etc/nixos/configuration.nix";
        }
        ```

      - ```
        {
          prefix = "";
          path = "/nix/var/nix/profiles/per-user/root/channels";
        }
        ```

      The lookup algorithm checks each entry until a match is found, returning a [path value](@docroot@/language/values.html#type-path) of the match.

      This is the process for each entry:
      If the lookup path matches `prefix`, then the remainder of the lookup path (the "suffix") is searched for within the directory denoted by `patch`.
      Note that the `path` may need to be downloaded at this point to look inside.
      If the suffix is found inside that directory, then the entry is a match;
      the combined absolute path of the directory (now downloaded if need be) and the suffix is returned.

      The syntax

      ```nix
      <nixpkgs>
      ```

      is equivalent to:

      ```nix
      builtins.findFile builtins.nixPath "nixpkgs"
      ```
    )",
    .fun = prim_findFile,
});

static std::string_view fileTypeToString(InputAccessor::Type type)
{
    return
        type == InputAccessor::Type::tRegular ? "regular" :
        type == InputAccessor::Type::tDirectory ? "directory" :
        type == InputAccessor::Type::tSymlink ? "symlink" :
        "unknown";
}

static void prim_readFileType(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    auto path = realisePath(state, pos, *args[0]);
    /* Retrieve the directory entry type and stringize it. */
    v.mkString(fileTypeToString(path.lstat().type));
}

static RegisterPrimOp primop_readFileType({
    .name = "__readFileType",
    .args = {"p"},
    .doc = R"(
      Determine the directory entry type of a filesystem node, being
      one of "directory", "regular", "symlink", or "unknown".
    )",
    .fun = prim_readFileType,
});

/* Read a directory (without . or ..) */
static void prim_readDir(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    auto path = realisePath(state, pos, *args[0]);

    // Retrieve directory entries for all nodes in a directory.
    // This is similar to `getFileType` but is optimized to reduce system calls
    // on many systems.
    auto entries = path.readDirectory();
    auto attrs = state.buildBindings(entries.size());

    // If we hit unknown directory entry types we may need to fallback to
    // using `getFileType` on some systems.
    // In order to reduce system calls we make each lookup lazy by using
    // `builtins.readFileType` application.
    Value * readFileType = nullptr;

    for (auto & [name, type] : entries) {
        auto & attr = attrs.alloc(name);
        if (!type) {
            // Some filesystems or operating systems may not be able to return
            // detailed node info quickly in this case we produce a thunk to
            // query the file type lazily.
            auto epath = state.allocValue();
            epath->mkPath(path + name);
            if (!readFileType)
                readFileType = &state.getBuiltin("readFileType");
            attr.mkApp(readFileType, epath);
        } else {
            // This branch of the conditional is much more likely.
            // Here we just stringize the directory entry type.
            attr.mkString(fileTypeToString(*type));
        }
    }

    v.mkAttrs(attrs);
}

static RegisterPrimOp primop_readDir({
    .name = "__readDir",
    .args = {"path"},
    .doc = R"(
      Return the contents of the directory *path* as a set mapping
      directory entries to the corresponding file type. For instance, if
      directory `A` contains a regular file `B` and another directory
      `C`, then `builtins.readDir ./A` will return the set

      ```nix
      { B = "regular"; C = "directory"; }
      ```

      The possible values for the file type are `"regular"`,
      `"directory"`, `"symlink"` and `"unknown"`.
    )",
    .fun = prim_readDir,
});

/* Extend single element string context with another output. */
static void prim_outputOf(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    SingleDerivedPath drvPath = state.coerceToSingleDerivedPath(pos, *args[0], "while evaluating the first argument to builtins.outputOf");

    OutputNameView outputName = state.forceStringNoCtx(*args[1], pos, "while evaluating the second argument to builtins.outputOf");

    state.mkSingleDerivedPathString(
        SingleDerivedPath::Built {
            .drvPath = make_ref<SingleDerivedPath>(drvPath),
            .output = std::string { outputName },
        },
        v);
}

static RegisterPrimOp primop_outputOf({
    .name = "__outputOf",
    .args = {"derivation-reference", "output-name"},
    .doc = R"(
      Return the output path of a derivation, literally or using a placeholder if needed.

      If the derivation has a statically-known output path (i.e. the derivation output is input-addressed, or fixed content-addresed), the output path will just be returned.
      But if the derivation is content-addressed or if the derivation is itself not-statically produced (i.e. is the output of another derivation), a placeholder will be returned instead.

      *`derivation reference`* must be a string that may contain a regular store path to a derivation, or may be a placeholder reference. If the derivation is produced by a derivation, you must explicitly select `drv.outPath`.
      This primop can be chained arbitrarily deeply.
      For instance,

      ```nix
      builtins.outputOf
        (builtins.outputOf myDrv "out)
        "out"
      ```

      will return a placeholder for the output of the output of `myDrv`.

      This primop corresponds to the `^` sigil for derivable paths, e.g. as part of installable syntax on the command line.
    )",
    .fun = prim_outputOf,
    .experimentalFeature = Xp::DynamicDerivations,
});

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
static void prim_toFile(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    NixStringContext context;
    std::string name(state.forceStringNoCtx(*args[0], pos, "while evaluating the first argument passed to builtins.toFile"));
    std::string contents(state.forceString(*args[1], context, pos, "while evaluating the second argument passed to builtins.toFile"));

    StorePathSet refs;

    for (auto c : context) {
        if (auto p = std::get_if<NixStringContextElem::Opaque>(&c.raw))
            refs.insert(p->path);
        else
            state.error<EvalError>(
                "files created by %1% may not reference derivations, but %2% references %3%",
                "builtins.toFile",
                name,
                c.to_string()
            ).atPos(pos).debugThrow();
    }

    auto storePath = settings.readOnlyMode
        ? state.store->computeStorePathForText(name, contents, refs)
        : state.store->addTextToStore(name, contents, refs, state.repair);

    /* Note: we don't need to add `context' to the context of the
       result, since `storePath' itself has references to the paths
       used in args[1]. */

    /* Add the output of this to the allowed paths. */
    state.allowAndSetStorePathString(storePath, v);
}

static RegisterPrimOp primop_toFile({
    .name = "__toFile",
    .args = {"name", "s"},
    .doc = R"(
      Store the string *s* in a file in the Nix store and return its
      path.  The file has suffix *name*. This file can be used as an
      input to derivations. One application is to write builders
      “inline”. For instance, the following Nix expression combines the
      Nix expression for GNU Hello and its build script into one file:

      ```nix
      { stdenv, fetchurl, perl }:

      stdenv.mkDerivation {
        name = "hello-2.1.1";

        builder = builtins.toFile "builder.sh" "
          source $stdenv/setup

          PATH=$perl/bin:$PATH

          tar xvfz $src
          cd hello-*
          ./configure --prefix=$out
          make
          make install
        ";

        src = fetchurl {
          url = "http://ftp.nluug.nl/pub/gnu/hello/hello-2.1.1.tar.gz";
          sha256 = "1md7jsfd8pa45z73bz1kszpp01yw6x5ljkjk2hx7wl800any6465";
        };
        inherit perl;
      }
      ```

      It is even possible for one file to refer to another, e.g.,

      ```nix
      builder = let
        configFile = builtins.toFile "foo.conf" "
          # This is some dummy configuration file.
          ...
        ";
      in builtins.toFile "builder.sh" "
        source $stdenv/setup
        ...
        cp ${configFile} $out/etc/foo.conf
      ";
      ```

      Note that `${configFile}` is a
      [string interpolation](@docroot@/language/values.md#type-string), so the result of the
      expression `configFile`
      (i.e., a path like `/nix/store/m7p7jfny445k...-foo.conf`) will be
      spliced into the resulting string.

      It is however *not* allowed to have files mutually referring to each
      other, like so:

      ```nix
      let
        foo = builtins.toFile "foo" "...${bar}...";
        bar = builtins.toFile "bar" "...${foo}...";
      in foo
      ```

      This is not allowed because it would cause a cyclic dependency in
      the computation of the cryptographic hashes for `foo` and `bar`.

      It is also not possible to reference the result of a derivation. If
      you are using Nixpkgs, the `writeTextFile` function is able to do
      that.
    )",
    .fun = prim_toFile,
});

static void addPath(
    EvalState & state,
    const PosIdx pos,
    std::string_view name,
    Path path,
    Value * filterFun,
    FileIngestionMethod method,
    const std::optional<Hash> expectedHash,
    Value & v,
    const NixStringContext & context)
{
    try {
        // FIXME: handle CA derivation outputs (where path needs to
        // be rewritten to the actual output).
        auto rewrites = state.realiseContext(context);
        path = state.toRealPath(rewriteStrings(path, rewrites), context);

        StorePathSet refs;

        if (state.store->isInStore(path)) {
            try {
                auto [storePath, subPath] = state.store->toStorePath(path);
                // FIXME: we should scanForReferences on the path before adding it
                refs = state.store->queryPathInfo(storePath)->references;
                path = state.store->toRealPath(storePath) + subPath;
            } catch (Error &) { // FIXME: should be InvalidPathError
            }
        }

        path = evalSettings.pureEval && expectedHash
            ? path
            : state.checkSourcePath(CanonPath(path)).path.abs();

        PathFilter filter = filterFun ? ([&](const Path & path) {
            auto st = lstat(path);

            /* Call the filter function.  The first argument is the path,
               the second is a string indicating the type of the file. */
            Value arg1;
            arg1.mkString(path);

            Value arg2;
            arg2.mkString(
                S_ISREG(st.st_mode) ? "regular" :
                S_ISDIR(st.st_mode) ? "directory" :
                S_ISLNK(st.st_mode) ? "symlink" :
                "unknown" /* not supported, will fail! */);

            Value * args []{&arg1, &arg2};
            Value res;
            state.callFunction(*filterFun, 2, args, res, pos);

            return state.forceBool(res, pos, "while evaluating the return value of the path filter function");
        }) : defaultPathFilter;

        std::optional<StorePath> expectedStorePath;
        if (expectedHash)
            expectedStorePath = state.store->makeFixedOutputPath(name, FixedOutputInfo {
                .method = method,
                .hash = *expectedHash,
                .references = {},
            });

        if (!expectedHash || !state.store->isValidPath(*expectedStorePath)) {
            auto dstPath = fetchToStore(
                *state.store, state.rootPath(CanonPath(path)), name, method, &filter, state.repair);
            if (expectedHash && expectedStorePath != dstPath)
                state.error<EvalError>(
                    "store path mismatch in (possibly filtered) path added from '%s'",
                    path
                ).atPos(pos).debugThrow();
            state.allowAndSetStorePathString(dstPath, v);
        } else
            state.allowAndSetStorePathString(*expectedStorePath, v);
    } catch (Error & e) {
        e.addTrace(state.positions[pos], "while adding path '%s'", path);
        throw;
    }
}


static void prim_filterSource(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    NixStringContext context;
    auto path = state.coerceToPath(pos, *args[1], context,
        "while evaluating the second argument (the path to filter) passed to builtins.filterSource");
    state.forceFunction(*args[0], pos, "while evaluating the first argument passed to builtins.filterSource");
    addPath(state, pos, path.baseName(), path.path.abs(), args[0], FileIngestionMethod::Recursive, std::nullopt, v, context);
}

static RegisterPrimOp primop_filterSource({
    .name = "__filterSource",
    .args = {"e1", "e2"},
    .doc = R"(
      > **Warning**
      >
      > `filterSource` should not be used to filter store paths. Since
      > `filterSource` uses the name of the input directory while naming
      > the output directory, doing so will produce a directory name in
      > the form of `<hash2>-<hash>-<name>`, where `<hash>-<name>` is
      > the name of the input directory. Since `<hash>` depends on the
      > unfiltered directory, the name of the output directory will
      > indirectly depend on files that are filtered out by the
      > function. This will trigger a rebuild even when a filtered out
      > file is changed. Use `builtins.path` instead, which allows
      > specifying the name of the output directory.

      This function allows you to copy sources into the Nix store while
      filtering certain files. For instance, suppose that you want to use
      the directory `source-dir` as an input to a Nix expression, e.g.

      ```nix
      stdenv.mkDerivation {
        ...
        src = ./source-dir;
      }
      ```

      However, if `source-dir` is a Subversion working copy, then all
      those annoying `.svn` subdirectories will also be copied to the
      store. Worse, the contents of those directories may change a lot,
      causing lots of spurious rebuilds. With `filterSource` you can
      filter out the `.svn` directories:

      ```nix
      src = builtins.filterSource
        (path: type: type != "directory" || baseNameOf path != ".svn")
        ./source-dir;
      ```

      Thus, the first argument *e1* must be a predicate function that is
      called for each regular file, directory or symlink in the source
      tree *e2*. If the function returns `true`, the file is copied to the
      Nix store, otherwise it is omitted. The function is called with two
      arguments. The first is the full path of the file. The second is a
      string that identifies the type of the file, which is either
      `"regular"`, `"directory"`, `"symlink"` or `"unknown"` (for other
      kinds of files such as device nodes or fifos — but note that those
      cannot be copied to the Nix store, so if the predicate returns
      `true` for them, the copy will fail). If you exclude a directory,
      the entire corresponding subtree of *e2* will be excluded.
    )",
    .fun = prim_filterSource,
});

static void prim_path(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    std::optional<SourcePath> path;
    std::string name;
    Value * filterFun = nullptr;
    auto method = FileIngestionMethod::Recursive;
    std::optional<Hash> expectedHash;
    NixStringContext context;

    state.forceAttrs(*args[0], pos, "while evaluating the argument passed to 'builtins.path'");

    for (auto & attr : *args[0]->attrs) {
        auto n = state.symbols[attr.name];
        if (n == "path")
            path.emplace(state.coerceToPath(attr.pos, *attr.value, context, "while evaluating the 'path' attribute passed to 'builtins.path'"));
        else if (attr.name == state.sName)
            name = state.forceStringNoCtx(*attr.value, attr.pos, "while evaluating the `name` attribute passed to builtins.path");
        else if (n == "filter")
            state.forceFunction(*(filterFun = attr.value), attr.pos, "while evaluating the `filter` parameter passed to builtins.path");
        else if (n == "recursive")
            method = FileIngestionMethod { state.forceBool(*attr.value, attr.pos, "while evaluating the `recursive` attribute passed to builtins.path") };
        else if (n == "sha256")
            expectedHash = newHashAllowEmpty(state.forceStringNoCtx(*attr.value, attr.pos, "while evaluating the `sha256` attribute passed to builtins.path"), htSHA256);
        else
            state.error<EvalError>(
                "unsupported argument '%1%' to 'addPath'",
                state.symbols[attr.name]
            ).atPos(attr.pos).debugThrow();
    }
    if (!path)
        state.error<EvalError>(
            "missing required 'path' attribute in the first argument to builtins.path"
        ).atPos(pos).debugThrow();
    if (name.empty())
        name = path->baseName();

    addPath(state, pos, name, path->path.abs(), filterFun, method, expectedHash, v, context);
}

static RegisterPrimOp primop_path({
    .name = "__path",
    .args = {"args"},
    .doc = R"(
      An enrichment of the built-in path type, based on the attributes
      present in *args*. All are optional except `path`:

        - path\
          The underlying path.

        - name\
          The name of the path when added to the store. This can used to
          reference paths that have nix-illegal characters in their names,
          like `@`.

        - filter\
          A function of the type expected by `builtins.filterSource`,
          with the same semantics.

        - recursive\
          When `false`, when `path` is added to the store it is with a
          flat hash, rather than a hash of the NAR serialization of the
          file. Thus, `path` must refer to a regular file, not a
          directory. This allows similar behavior to `fetchurl`. Defaults
          to `true`.

        - sha256\
          When provided, this is the expected hash of the file at the
          path. Evaluation will fail if the hash is incorrect, and
          providing a hash allows `builtins.path` to be used even when the
          `pure-eval` nix config option is on.
    )",
    .fun = prim_path,
});


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
static void prim_isAttrs(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    state.forceValue(*args[0], pos);
    v.mkBool(args[0]->type() == nAttrs);
}

static RegisterPrimOp primop_isAttrs({
    .name = "__isAttrs",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to a set, and `false` otherwise.
    )",
    .fun = prim_isAttrs,
});


/* Builds a set from a list specifying (name, value) pairs.  To be
   precise, a list [{name = "name1"; value = value1;} ... {name =
   "nameN"; value = valueN;}] is transformed to {name1 = value1;
   ... nameN = valueN;}.  In case of duplicate occurrences of the same
   name, the first takes precedence. */

static void prim_functionArgs(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    state.forceValue(*args[0], pos);
    if (args[0]->isPrimOpApp() || args[0]->isPrimOp()) {
        v.mkAttrs(&state.emptyBindings);
        return;
    }
    if (!args[0]->isLambda())
        state.error<TypeError>("'functionArgs' requires a function").atPos(pos).debugThrow();
    if (!args[0]->lambda.fun->hasFormals()) {
        v.mkAttrs(&state.emptyBindings);
        return;
    }
    auto attrs = state.buildBindings(args[0]->lambda.fun->formals->formals.size());
    for (auto & i : args[0]->lambda.fun->formals->formals)
        // !!! should optimise booleans (allocate only once)
        attrs.alloc(i.name, i.pos).mkBool(i.def);
    v.mkAttrs(attrs);
}
static RegisterPrimOp primop_functionArgs({
    .name = "__functionArgs",
    .args = {"f"},
    .doc = R"(
      Return a set containing the names of the formal arguments expected
      by the function *f*. The value of each attribute is a Boolean
      denoting whether the corresponding argument has a default value. For
      instance, `functionArgs ({ x, y ? 123}: ...) = { x = false; y =
      true; }`.
      "Formal argument" here refers to the attributes pattern-matched by
      the function. Plain lambdas are not included, e.g. `functionArgs (x:
      ...) = { }`.
    )",
    .fun = prim_functionArgs,
});

/*  */



/*************************************************************
 * Lists
 *************************************************************/


/* Determine whether the argument is a list. */
static void prim_isList(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    state.forceValue(*args[0], pos);
    v.mkBool(args[0]->type() == nList);
}

static RegisterPrimOp primop_isList({
    .name = "__isList",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to a list, and `false` otherwise.
    )",
    .fun = prim_isList,
});

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
