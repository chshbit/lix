#include "primops.hh"
#include <time.h>            // for time
#include <algorithm>         // for max
#include "canon-path.hh"     // for CanonPath
#include "config.hh"         // for Setting, ExperimentalFeatureSettings
#include "eval-settings.hh"  // for EvalSettings, evalSettings
#include "eval.hh"           // for EvalState, PrimOp, Env
#include "nixexpr.hh"        // for StaticEnv
#include "ref.hh"            // for ref
#include "search-path.hh"    // for SearchPath
#include "store-api.hh"      // for Store

namespace nix {

/**
 * List of primops to be regisered explicitely
 */

extern PrimOp primop_abort;
extern PrimOp primop_add;
extern PrimOp primop_addDrvOutputDependencies;
extern PrimOp primop_addErrorContext;
extern PrimOp primop_all;
extern PrimOp primop_any;
extern PrimOp primop_appendContext;
extern PrimOp primop_attrNames;
extern PrimOp primop_attrValues;
extern PrimOp primop_baseNameOf;
extern PrimOp primop_bitAnd;
extern PrimOp primop_bitOr;
extern PrimOp primop_bitXor;
extern PrimOp primop_break;
extern PrimOp primop_catAttrs;
extern PrimOp primop_ceil;
extern PrimOp primop_compareVersions;
extern PrimOp primop_concatLists;
extern PrimOp primop_concatMap;
extern PrimOp primop_concatStringsSep;
extern PrimOp primop_deepSeq;
extern PrimOp primop_derivationStrict;
extern PrimOp primop_dirOf;
extern PrimOp primop_div;
extern PrimOp primop_elem;
extern PrimOp primop_elemAt;
extern PrimOp primop_fetchClosure;
extern PrimOp primop_fetchGit;
extern PrimOp primop_fetchMercurial;
extern PrimOp primop_fetchTarball;
extern PrimOp primop_fetchTree;
extern PrimOp primop_fetchurl;
extern PrimOp primop_filter;
extern PrimOp primop_filterSource;
extern PrimOp primop_findFile;
extern PrimOp primop_floor;
extern PrimOp primop_foldlStrict;
extern PrimOp primop_fromJSON;
extern PrimOp primop_fromTOML;
extern PrimOp primop_functionArgs;
extern PrimOp primop_genericClosure;
extern PrimOp primop_genList;
extern PrimOp primop_getAttr;
extern PrimOp primop_getContext;
extern PrimOp primop_getEnv;
extern PrimOp primop_groupBy;
extern PrimOp primop_hasAttr;
extern PrimOp primop_hasContext;
extern PrimOp primop_hashFile;
extern PrimOp primop_hashString;
extern PrimOp primop_head;
extern PrimOp primop_import;
extern PrimOp primop_intersectAttrs;
extern PrimOp primop_isAttrs;
extern PrimOp primop_isBool;
extern PrimOp primop_isFloat;
extern PrimOp primop_isFunction;
extern PrimOp primop_isInt;
extern PrimOp primop_isList;
extern PrimOp primop_isNull;
extern PrimOp primop_isPath;
extern PrimOp primop_isString;
extern PrimOp primop_length;
extern PrimOp primop_lessThan;
extern PrimOp primop_listToAttrs;
extern PrimOp primop_map;
extern PrimOp primop_mapAttrs;
extern PrimOp primop_match;
extern PrimOp primop_mul;
extern PrimOp primop_outputOf;
extern PrimOp primop_parseDrvName;
extern PrimOp primop_partition;
extern PrimOp primop_path;
extern PrimOp primop_pathExists;
extern PrimOp primop_placeholder;
extern PrimOp primop_readDir;
extern PrimOp primop_readFile;
extern PrimOp primop_readFileType;
extern PrimOp primop_removeAttrs;
extern PrimOp primop_replaceStrings;
extern PrimOp primop_scopedImport;
extern PrimOp primop_seq;
extern PrimOp primop_sort;
extern PrimOp primop_split;
extern PrimOp primop_splitVersion;
extern PrimOp primop_storePath;
extern PrimOp primop_stringLength;
extern PrimOp primop_sub;
extern PrimOp primop_substring;
extern PrimOp primop_tail;
extern PrimOp primop_throw;
extern PrimOp primop_toFile;
extern PrimOp primop_toJSON;
extern PrimOp primop_toPath;
extern PrimOp primop_toString;
extern PrimOp primop_toXML;
extern PrimOp primop_trace;
extern PrimOp primop_tryEval;
extern PrimOp primop_typeOf;
extern PrimOp primop_unsafeDiscardOutputDependency;
extern PrimOp primop_unsafeDiscardStringContext;
extern PrimOp primop_unsafeGetAttrPos;
extern PrimOp primop_zipAttrsWith;

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

    auto registerPrimOp = [this](PrimOp & p) {
        // Only register the primop if the associated experimental feature is enabled
        if (experimentalFeatureSettings.isEnabled(p.experimentalFeature)) {
            auto adjusted = p;
            adjusted.arity = std::max(p.args.size(), p.arity); // Don't trust what is defined
            addPrimOp(std::move(adjusted));
        }
    };

    /**
     * Register the primops explicitely
     */

    registerPrimOp(primop_abort);
    registerPrimOp(primop_add);
    registerPrimOp(primop_addDrvOutputDependencies);
    registerPrimOp(primop_addErrorContext);
    registerPrimOp(primop_all);
    registerPrimOp(primop_any);
    registerPrimOp(primop_appendContext);
    registerPrimOp(primop_attrNames);
    registerPrimOp(primop_attrValues);
    registerPrimOp(primop_baseNameOf);
    registerPrimOp(primop_bitAnd);
    registerPrimOp(primop_bitOr);
    registerPrimOp(primop_bitXor);
    registerPrimOp(primop_break);
    registerPrimOp(primop_catAttrs);
    registerPrimOp(primop_ceil);
    registerPrimOp(primop_compareVersions);
    registerPrimOp(primop_concatLists);
    registerPrimOp(primop_concatMap);
    registerPrimOp(primop_concatStringsSep);
    registerPrimOp(primop_deepSeq);
    registerPrimOp(primop_derivationStrict);
    registerPrimOp(primop_dirOf);
    registerPrimOp(primop_div);
    registerPrimOp(primop_elem);
    registerPrimOp(primop_elemAt);
    registerPrimOp(primop_fetchClosure);
    registerPrimOp(primop_fetchGit);
    registerPrimOp(primop_fetchMercurial);
    registerPrimOp(primop_fetchTarball);
    registerPrimOp(primop_fetchTree);
    registerPrimOp(primop_fetchurl);
    registerPrimOp(primop_filter);
    registerPrimOp(primop_filterSource);
    registerPrimOp(primop_findFile);
    registerPrimOp(primop_floor);
    registerPrimOp(primop_foldlStrict);
    registerPrimOp(primop_fromJSON);
    registerPrimOp(primop_fromTOML);
    registerPrimOp(primop_functionArgs);
    registerPrimOp(primop_genericClosure);
    registerPrimOp(primop_genList);
    registerPrimOp(primop_getAttr);
    registerPrimOp(primop_getContext);
    registerPrimOp(primop_getEnv);
    registerPrimOp(primop_groupBy);
    registerPrimOp(primop_hasAttr);
    registerPrimOp(primop_hasContext);
    registerPrimOp(primop_hashFile);
    registerPrimOp(primop_hashString);
    registerPrimOp(primop_head);
    registerPrimOp(primop_import);
    registerPrimOp(primop_intersectAttrs);
    registerPrimOp(primop_isAttrs);
    registerPrimOp(primop_isBool);
    registerPrimOp(primop_isFloat);
    registerPrimOp(primop_isFunction);
    registerPrimOp(primop_isInt);
    registerPrimOp(primop_isList);
    registerPrimOp(primop_isNull);
    registerPrimOp(primop_isPath);
    registerPrimOp(primop_isString);
    registerPrimOp(primop_length);
    registerPrimOp(primop_lessThan);
    registerPrimOp(primop_listToAttrs);
    registerPrimOp(primop_map);
    registerPrimOp(primop_mapAttrs);
    registerPrimOp(primop_match);
    registerPrimOp(primop_mul);
    registerPrimOp(primop_outputOf);
    registerPrimOp(primop_parseDrvName);
    registerPrimOp(primop_partition);
    registerPrimOp(primop_path);
    registerPrimOp(primop_pathExists);
    registerPrimOp(primop_placeholder);
    registerPrimOp(primop_readDir);
    registerPrimOp(primop_readFile);
    registerPrimOp(primop_readFileType);
    registerPrimOp(primop_removeAttrs);
    registerPrimOp(primop_replaceStrings);
    registerPrimOp(primop_scopedImport);
    registerPrimOp(primop_seq);
    registerPrimOp(primop_sort);
    registerPrimOp(primop_split);
    registerPrimOp(primop_splitVersion);
    registerPrimOp(primop_storePath);
    registerPrimOp(primop_stringLength);
    registerPrimOp(primop_sub);
    registerPrimOp(primop_substring);
    registerPrimOp(primop_tail);
    registerPrimOp(primop_throw);
    registerPrimOp(primop_toFile);
    registerPrimOp(primop_toJSON);
    registerPrimOp(primop_toPath);
    registerPrimOp(primop_toString);
    registerPrimOp(primop_toXML);
    registerPrimOp(primop_trace);
    registerPrimOp(primop_tryEval);
    registerPrimOp(primop_typeOf);
    registerPrimOp(primop_unsafeDiscardOutputDependency);
    registerPrimOp(primop_unsafeDiscardStringContext);
    registerPrimOp(primop_unsafeGetAttrPos);
    registerPrimOp(primop_zipAttrsWith);

    // Legacy way of registering primops, using c++ arcanes
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
