#include "derivations.hh"
#include "primops.hh"
#include "store-api.hh"

namespace nix {

/* Want reasonable symbol names, so extern C */
/* !!! Should we pass the Pos or the file name too? */
extern "C" typedef void (*ValueInitializer)(EvalState & state, Value & v);

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

void mkOutputString(
    EvalState & state,
    BindingsBuilder & attrs,
    const StorePath & drvPath,
    const std::pair<std::string, DerivationOutput> & o
)
{
    state.mkOutputString(
        attrs.alloc(o.first),
        SingleDerivedPath::Built{
            .drvPath = makeConstantStorePathRef(drvPath),
            .output = o.first,
        },
        o.second.path(*state.store, Derivation::nameFromPath(drvPath), o.first)
    );
}

/* Load and evaluate an expression from path specified by the
   argument. */
static void import(EvalState & state, const PosIdx pos, Value & vPath, Value * vScope, Value & v)
{
    auto path = realisePath(state, pos, vPath);
    auto path2 = path.path.abs();

    // FIXME
    auto isValidDerivationInStore = [&]() -> std::optional<StorePath> {
        if (!state.store->isStorePath(path2)) {
            return std::nullopt;
        }
        auto storePath = state.store->parseStorePath(path2);
        if (!(state.store->isValidPath(storePath) && isDerivation(path2))) {
            return std::nullopt;
        }
        return storePath;
    };

    if (auto storePath = isValidDerivationInStore()) {
        Derivation drv = state.store->readDerivation(*storePath);
        auto attrs = state.buildBindings(3 + drv.outputs.size());
        attrs.alloc(state.sDrvPath)
            .mkString(
                path2,
                {
                    NixStringContextElem::DrvDeep{.drvPath = *storePath},
                }
            );
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
            state.eval(
                state.parseExprFromString(
#include "imported-drv-to-derivation.nix.gen.hh"
                    , CanonPath::root
                ),
                **state.vImportedDrvToDerivation
            );
        }

        state.forceFunction(
            **state.vImportedDrvToDerivation,
            pos,
            "while evaluating imported-drv-to-derivation.nix.gen.hh"
        );
        v.mkApp(*state.vImportedDrvToDerivation, w);
        state.forceAttrs(v, pos, "while calling imported-drv-to-derivation.nix.gen.hh");
    }

    else if (path2 == corepkgsPrefix + "fetchurl.nix")
    {
        state.eval(
            state.parseExprFromString(
#include "fetchurl.nix.gen.hh"
                , CanonPath::root
            ),
            v
        );
    }

    else
    {
        if (!vScope) {
            state.evalFile(path, v);
        } else {
            state.forceAttrs(
                *vScope, pos, "while evaluating the first argument passed to builtins.scopedImport"
            );

            Env * env = &state.allocEnv(vScope->attrs->size());
            env->up = &state.baseEnv;

            auto staticEnv = std::make_shared<StaticEnv>(
                nullptr, state.staticBaseEnv.get(), vScope->attrs->size()
            );

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

static void prim_import(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    import(state, pos, *args[0], nullptr, v);
}

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
    .fun = prim_import,
});

/* Load a ValueInitializer from a DSO and return whatever it initializes */
void prim_importNative(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto path = realisePath(state, pos, *args[0]);

    std::string sym(state.forceStringNoCtx(
        *args[1], pos, "while evaluating the second argument passed to builtins.importNative"
    ));

    void * handle = dlopen(path.path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!handle) {
        state.error<EvalError>("could not open '%1%': %2%", path, dlerror()).debugThrow();
    }

    dlerror();
    ValueInitializer func = (ValueInitializer) dlsym(handle, sym.c_str());
    if (!func) {
        char * message = dlerror();
        if (message) {
            state
                .error<EvalError>("could not load symbol '%1%' from '%2%': %3%", sym, path, message)
                .debugThrow();
        } else {
            state
                .error<EvalError>(
                    "symbol '%1%' from '%2%' resolved to NULL when a function pointer was expected",
                    sym,
                    path
                )
                .debugThrow();
        }
    }

    (func)(state, v);

    /* We don't dlclose because v may be a primop referencing a function in the shared object file
     */
}

static void prim_scopedImport(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    import(state, pos, *args[1], args[0], v);
}

static RegisterPrimOp primop_scopedImport(PrimOp{
    .name = "scopedImport",
    .arity = 2,
    .fun = prim_scopedImport,
});

}
