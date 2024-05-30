#include "primops.hh"
#include "store-api.hh"
#include "value-to-json.hh"

#include <nlohmann/json.hpp>

namespace nix {

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

static RegisterPrimOp primop_derivationStrict(PrimOp {
    .name = "derivationStrict",
    .arity = 1,
    .fun = prim_derivationStrict,
});

}
