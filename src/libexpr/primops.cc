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



/*************************************************************
 * Derivations
 *************************************************************/

static void derivationStrictInternal(EvalState & state, const std::string & name, Bindings * attrs, Value & v);




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
