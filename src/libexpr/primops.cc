#include "derivations.hh"
#include "eval.hh"
#include "eval-settings.hh"
#include "gc-small-vector.hh"
#include "json-to-value.hh"
#include "store-api.hh"
#include "primops.hh"

#include <boost/container/small_vector.hpp>
#include <nlohmann/json.hpp>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <dlfcn.h>

namespace nix {

RegisterPrimOp::PrimOps * RegisterPrimOp::primOps;

RegisterPrimOp::RegisterPrimOp(PrimOp && primOp)
{
    if (!primOps) {
        primOps = new PrimOps;
    }
    primOps->push_back(std::move(primOp));
}

void EvalState::createBaseEnv()
{
    baseEnv.up = 0;

    /* Add global constants such as `true' to the base environment. */
    Value v;

    /* `builtins' must be first! */
    v.mkAttrs(buildBindings(128).finish());
    addConstant(
        "builtins",
        v,
        {
            .type = nAttrs,
            .doc = R"(
          Contains all the [built-in functions](@docroot@/language/builtins.md) and values.

          Since built-in functions were added over time, [testing for attributes](./operators.md#has-attribute) in `builtins` can be used for graceful fallback on older Nix installations:

          ```nix
          # if hasContext is not available, we assume `s` has a context
          if builtins ? hasContext then builtins.hasContext s else true
          ```
        )",
        }
    );

    v.mkBool(true);
    addConstant(
        "true",
        v,
        {
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
        }
    );

    v.mkBool(false);
    addConstant(
        "false",
        v,
        {
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
        }
    );

    v.mkNull();
    addConstant(
        "null",
        v,
        {
            .type = nNull,
            .doc = R"(
          Primitive value.

          The name `null` is not special, and can be shadowed:

          ```nix-repl
          nix-repl> let null = 1; in null
          1
          ```
        )",
        }
    );

    if (!evalSettings.pureEval) {
        v.mkInt(time(0));
    }
    addConstant(
        "__currentTime",
        v,
        {
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
        }
    );

    if (!evalSettings.pureEval) {
        v.mkString(evalSettings.getCurrentSystem());
    }
    addConstant(
        "__currentSystem",
        v,
        {
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
        }
    );

    v.mkString("2.18.3-lix");
    addConstant(
        "__nixVersion",
        v,
        {
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
        }
    );

    v.mkString(store->storeDir);
    addConstant(
        "__storeDir",
        v,
        {
            .type = nString,
            .doc = R"(
          Logical file system location of the [Nix store](@docroot@/glossary.md#gloss-store) currently in use.

          This value is determined by the `store` parameter in [Store URLs](@docroot@/command-ref/new-cli/nix3-help-stores.md):

          ```shell-session
          $ nix-instantiate --store 'dummy://?store=/blah' --eval --expr builtins.storeDir
          "/blah"
          ```
        )",
        }
    );

    /* Legacy language version.
     * This is fixed at 6, and will never change in the future on Lix.
     * A better language versioning construct needs to be built instead. */
    v.mkInt(6);
    addConstant(
        "__langVersion",
        v,
        {
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
        }
    );

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
        .args = {"e1", "e2"},
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
    addConstant(
        "__nixPath",
        v,
        {
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
        }
    );

    if (RegisterPrimOp::primOps) {
        for (auto & primOp : *RegisterPrimOp::primOps) {
            if (experimentalFeatureSettings.isEnabled(primOp.experimentalFeature)) {
                auto primOpAdjusted = primOp;
                primOpAdjusted.arity = std::max(primOp.args.size(), primOp.arity);
                addPrimOp(std::move(primOpAdjusted));
            }
        }
    }

    /* Add a wrapper around the derivation primop that computes the
       `drvPath' and `outPath' attributes lazily.

       Null docs because it is documented separately.
       */
    auto vDerivation = allocValue();
    addConstant(
        "derivation",
        vDerivation,
        {
            .type = nFunction,
        }
    );

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
    eval(
        parse(code, sizeof(code), derivationInternal, {CanonPath::root}, staticBaseEnv),
        *vDerivation
    );
}

}
