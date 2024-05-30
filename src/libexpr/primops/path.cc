#include "eval-settings.hh"
#include "fetch-to-store.hh"
#include "path-references.hh"
#include "primops.hh"
#include "store-api.hh"

namespace nix {

// TODO: Use a const map instead ?
static std::string_view fileTypeToString(InputAccessor::Type type)
{
    return type == InputAccessor::Type::tRegular  ? "regular"
        : type == InputAccessor::Type::tDirectory ? "directory"
        : type == InputAccessor::Type::tSymlink   ? "symlink"
                                                  : "unknown";
}

static void addPath(
    EvalState & state,
    const PosIdx pos,
    std::string_view name,
    Path path,
    Value * filterFun,
    FileIngestionMethod method,
    const std::optional<Hash> expectedHash,
    Value & v,
    const NixStringContext & context
)
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
                S_ISREG(st.st_mode)       ? "regular"
                    : S_ISDIR(st.st_mode) ? "directory"
                    : S_ISLNK(st.st_mode) ? "symlink"
                                          : "unknown" /* not supported, will fail! */
            );

            Value * args[]{&arg1, &arg2};
            Value res;
            state.callFunction(*filterFun, 2, args, res, pos);

            return state.forceBool(
                res, pos, "while evaluating the return value of the path filter function"
            );
        })
                                      : defaultPathFilter;

        std::optional<StorePath> expectedStorePath;
        if (expectedHash) {
            expectedStorePath = state.store->makeFixedOutputPath(
                name,
                FixedOutputInfo{
                    .method = method,
                    .hash = *expectedHash,
                    .references = {},
                }
            );
        }

        if (!expectedHash || !state.store->isValidPath(*expectedStorePath)) {
            auto dstPath = fetchToStore(
                *state.store, state.rootPath(CanonPath(path)), name, method, &filter, state.repair
            );
            if (expectedHash && expectedStorePath != dstPath) {
                state
                    .error<EvalError>(
                        "store path mismatch in (possibly filtered) path added from '%s'", path
                    )
                    .atPos(pos)
                    .debugThrow();
            }
            state.allowAndSetStorePathString(dstPath, v);
        } else {
            state.allowAndSetStorePathString(*expectedStorePath, v);
        }
    } catch (Error & e) {
        e.addTrace(state.positions[pos], "while adding path '%s'", path);
        throw;
    }
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
 * builtins.dirOf
 */

static void prim_dirOf(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceValue(*args[0], pos);
    if (args[0]->type() == nPath) {
        auto path = args[0]->path();
        v.mkPath(path.path.isRoot() ? path : path.parent());
    } else {
        NixStringContext context;
        auto path = state.coerceToString(
            pos,
            *args[0],
            context,
            "while evaluating the first argument passed to 'builtins.dirOf'",
            false,
            false
        );
        auto dir = dirOf(*path);
        v.mkString(dir, context);
    }
}

PrimOp primop_dirOf({
    .name = "dirOf",
    .args = {"s"},
    .doc = R"(
      Return the directory part of the string *s*, that is, everything
      before the final slash in the string. This is similar to the GNU
      `dirname` command.
    )",
    .fun = prim_dirOf,
});

/**
 * builtins.filterSource
 */

static void prim_filterSource(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    NixStringContext context;
    auto path = state.coerceToPath(
        pos,
        *args[1],
        context,
        "while evaluating the second argument (the path to filter) passed to builtins.filterSource"
    );
    state.forceFunction(
        *args[0], pos, "while evaluating the first argument passed to builtins.filterSource"
    );
    addPath(
        state,
        pos,
        path.baseName(),
        path.path.abs(),
        args[0],
        FileIngestionMethod::Recursive,
        std::nullopt,
        v,
        context
    );
}

PrimOp primop_filterSource({
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

/**
 * builtins.findFile
 */

static void prim_findFile(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceList(
        *args[0], pos, "while evaluating the first argument passed to builtins.findFile"
    );

    SearchPath searchPath;

    for (auto v2 : args[0]->listItems()) {
        state.forceAttrs(
            *v2, pos, "while evaluating an element of the list passed to builtins.findFile"
        );

        std::string prefix;
        Bindings::iterator i = v2->attrs->find(state.sPrefix);
        if (i != v2->attrs->end()) {
            prefix = state.forceStringNoCtx(
                *i->value,
                pos,
                "while evaluating the `prefix` attribute of an element of the list passed to "
                "builtins.findFile"
            );
        }

        i = getAttr(state, state.sPath, v2->attrs, "in an element of the __nixPath");

        NixStringContext context;
        auto path = state
                        .coerceToString(
                            pos,
                            *i->value,
                            context,
                            "while evaluating the `path` attribute of an element of the list "
                            "passed to builtins.findFile",
                            false,
                            false
                        )
                        .toOwned();

        try {
            auto rewrites = state.realiseContext(context);
            path = rewriteStrings(path, rewrites);
        } catch (InvalidPathError & e) {
            state.error<EvalError>("cannot find '%1%', since path '%2%' is not valid", path, e.path)
                .atPos(pos)
                .debugThrow();
        }

        searchPath.elements.emplace_back(SearchPath::Elem{
            .prefix = SearchPath::Prefix{.s = prefix},
            .path = SearchPath::Path{.s = path},
        });
    }

    auto path = state.forceStringNoCtx(
        *args[1], pos, "while evaluating the second argument passed to builtins.findFile"
    );

    v.mkPath(state.checkSourcePath(state.findFile(searchPath, path, pos)));
}

PrimOp primop_findFile(PrimOp{
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

/**
 * builtins.outputOf
 */

static void prim_outputOf(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    SingleDerivedPath drvPath = state.coerceToSingleDerivedPath(
        pos, *args[0], "while evaluating the first argument to builtins.outputOf"
    );

    OutputNameView outputName = state.forceStringNoCtx(
        *args[1], pos, "while evaluating the second argument to builtins.outputOf"
    );

    state.mkSingleDerivedPathString(
        SingleDerivedPath::Built{
            .drvPath = make_ref<SingleDerivedPath>(drvPath),
            .output = std::string{outputName},
        },
        v
    );
}

PrimOp primop_outputOf({
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

/**
 * builtins.path
 */

static void prim_path(EvalState & state, const PosIdx pos, Value ** args, Value & v)
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
        if (n == "path") {
            path.emplace(state.coerceToPath(
                attr.pos,
                *attr.value,
                context,
                "while evaluating the 'path' attribute passed to 'builtins.path'"
            ));
        } else if (attr.name == state.sName) {
            name = state.forceStringNoCtx(
                *attr.value,
                attr.pos,
                "while evaluating the `name` attribute passed to builtins.path"
            );
        } else if (n == "filter") {
            state.forceFunction(
                *(filterFun = attr.value),
                attr.pos,
                "while evaluating the `filter` parameter passed to builtins.path"
            );
        } else if (n == "recursive") {
            method = FileIngestionMethod{state.forceBool(
                *attr.value,
                attr.pos,
                "while evaluating the `recursive` attribute passed to builtins.path"
            )};
        } else if (n == "sha256") {
            expectedHash = newHashAllowEmpty(
                state.forceStringNoCtx(
                    *attr.value,
                    attr.pos,
                    "while evaluating the `sha256` attribute passed to builtins.path"
                ),
                htSHA256
            );
        } else {
            state
                .error<EvalError>(
                    "unsupported argument '%1%' to 'addPath'", state.symbols[attr.name]
                )
                .atPos(attr.pos)
                .debugThrow();
        }
    }
    if (!path) {
        state
            .error<EvalError>(
                "missing required 'path' attribute in the first argument to builtins.path"
            )
            .atPos(pos)
            .debugThrow();
    }
    if (name.empty()) {
        name = path->baseName();
    }

    addPath(state, pos, name, path->path.abs(), filterFun, method, expectedHash, v, context);
}

PrimOp primop_path({
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

/**
 * builtins.pathExists
 */

static void prim_pathExists(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto & arg = *args[0];

    /* We don’t check the path right now, because we don’t want to
       throw if the path isn’t allowed, but just return false (and we
       can’t just catch the exception here because we still want to
       throw if something in the evaluation of `arg` tries to
       access an unauthorized path). */
    auto path = realisePath(state, pos, arg, {.checkForPureEval = false});

    /* SourcePath doesn't know about trailing slash. */
    auto mustBeDir =
        arg.type() == nString && (arg.str().ends_with("/") || arg.str().ends_with("/."));

    try {
        auto checked = state.checkSourcePath(path).resolveSymlinks(
            mustBeDir ? SymlinkResolution::Full : SymlinkResolution::Ancestors
        );

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

PrimOp primop_pathExists({
    .name = "__pathExists",
    .args = {"path"},
    .doc = R"(
      Return `true` if the path *path* exists at evaluation time, and
      `false` otherwise.
    )",
    .fun = prim_pathExists,
});

/**
 * builtins.palceholder
 */

/* Return a placeholder string for the specified output that will be
   substituted by the corresponding output path at build time. For
   example, 'placeholder "out"' returns the string
   /1rz4g4znpzjwh1xymhjpm42vipw92pr73vdgl6xs1hycac8kf2n9. At build
   time, any occurrence of this string in an derivation attribute will
   be replaced with the concrete path in the Nix store of the output
   ‘out’. */
static void prim_placeholder(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    v.mkString(hashPlaceholder(state.forceStringNoCtx(
        *args[0], pos, "while evaluating the first argument passed to builtins.placeholder"
    )));
}

PrimOp primop_placeholder({
    .name = "placeholder",
    .args = {"output"},
    .doc = R"(
      Return a placeholder string for the specified *output* that will be
      substituted by the corresponding output path at build time. Typical
      outputs would be `"out"`, `"bin"` or `"dev"`.
    )",
    .fun = prim_placeholder,
});

/**
 * builtins.toPath
 * WARNING: deprecated
 */

static void prim_toPath(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    NixStringContext context;
    auto path = state.coerceToPath(
        pos, *args[0], context, "while evaluating the first argument passed to builtins.toPath"
    );
    v.mkString(path.path.abs(), context);
}

PrimOp primop_toPath({
    .name = "__toPath",
    .args = {"s"},
    .doc = R"(
      **DEPRECATED.** Use `/. + "/path"` to convert a string into an absolute
      path. For relative paths, use `./. + "/path"`.
    )",
    .fun = prim_toPath,
});

/**
 * builtins.readDir
 */

static void prim_readDir(EvalState & state, const PosIdx pos, Value ** args, Value & v)
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
            if (!readFileType) {
                readFileType = &state.getBuiltin("readFileType");
            }
            attr.mkApp(readFileType, epath);
        } else {
            // This branch of the conditional is much more likely.
            // Here we just stringize the directory entry type.
            attr.mkString(fileTypeToString(*type));
        }
    }

    v.mkAttrs(attrs);
}

PrimOp primop_readDir({
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

/**
 * builtins.readFile
 */

static void prim_readFile(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto path = realisePath(state, pos, *args[0]);
    auto s = path.readFile();
    if (s.find((char) 0) != std::string::npos) {
        state
            .error<EvalError>(
                "the contents of the file '%1%' cannot be represented as a Nix string", path
            )
            .atPos(pos)
            .debugThrow();
    }
    StorePathSet refs;
    if (state.store->isInStore(path.path.abs())) {
        try {
            refs = state.store->queryPathInfo(state.store->toStorePath(path.path.abs()).first)
                       ->references;
        } catch (Error &) { // FIXME: should be InvalidPathError
        }
        // Re-scan references to filter down to just the ones that actually occur in the file.
        auto refsSink = PathRefScanSink::fromPaths(refs);
        refsSink << s;
        refs = refsSink.getResultPaths();
    }
    NixStringContext context;
    for (auto && p : std::move(refs)) {
        context.insert(NixStringContextElem::Opaque{
            .path = std::move((StorePath &&) p),
        });
    }
    v.mkString(s, context);
}

PrimOp primop_readFile({
    .name = "__readFile",
    .args = {"path"},
    .doc = R"(
      Return the contents of the file *path* as a string.
    )",
    .fun = prim_readFile,
});

/**
 * builtins.readFileType
 */

static void prim_readFileType(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto path = realisePath(state, pos, *args[0]);
    /* Retrieve the directory entry type and stringize it. */
    v.mkString(fileTypeToString(path.lstat().type));
}

PrimOp primop_readFileType({
    .name = "__readFileType",
    .args = {"p"},
    .doc = R"(
      Determine the directory entry type of a filesystem node, being
      one of "directory", "regular", "symlink", or "unknown".
    )",
    .fun = prim_readFileType,
});

/**
 * builtins.storePath
 */

static void prim_storePath(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    if (evalSettings.pureEval) {
        state.error<EvalError>("'%s' is not allowed in pure evaluation mode", "builtins.storePath")
            .atPos(pos)
            .debugThrow();
    }

    NixStringContext context;
    auto path = state
                    .checkSourcePath(state.coerceToPath(
                        pos,
                        *args[0],
                        context,
                        "while evaluating the first argument passed to builtins.storePath"
                    ))
                    .path;
    /* Resolve symlinks in ‘path’, unless ‘path’ itself is a symlink
       directly in the store.  The latter condition is necessary so
       e.g. nix-push does the right thing. */
    if (!state.store->isStorePath(path.abs())) {
        path = CanonPath(canonPath(path.abs(), true));
    }
    if (!state.store->isInStore(path.abs())) {
        state.error<EvalError>("path '%1%' is not in the Nix store", path).atPos(pos).debugThrow();
    }
    auto path2 = state.store->toStorePath(path.abs()).first;
    if (!settings.readOnlyMode) {
        state.store->ensurePath(path2);
    }
    context.insert(NixStringContextElem::Opaque{.path = path2});
    v.mkString(path.abs(), context);
}

PrimOp primop_storePath({
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

/**
 * builtins.toFile
 */

static void prim_toFile(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    NixStringContext context;
    std::string name(state.forceStringNoCtx(
        *args[0], pos, "while evaluating the first argument passed to builtins.toFile"
    ));
    std::string contents(state.forceString(
        *args[1], context, pos, "while evaluating the second argument passed to builtins.toFile"
    ));

    StorePathSet refs;

    for (auto c : context) {
        if (auto p = std::get_if<NixStringContextElem::Opaque>(&c.raw)) {
            refs.insert(p->path);
        } else {
            state
                .error<EvalError>(
                    "files created by %1% may not reference derivations, but %2% references %3%",
                    "builtins.toFile",
                    name,
                    c.to_string()
                )
                .atPos(pos)
                .debugThrow();
        }
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

PrimOp primop_toFile({
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

}
