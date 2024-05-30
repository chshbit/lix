#include "gc-small-vector.hh"
#include "primops.hh"

namespace nix {

static void anyOrAll(bool any, EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    const std::string fun = any ? "builtins.any" : "builtins.all";
    state.forceFunction(
        *args[0], pos, std::string("while evaluating the first argument passed to " + fun)
    );
    state.forceList(
        *args[1], pos, std::string("while evaluating the second argument passed to " + fun)
    );
    Value vTmp;
    for (auto elem : args[1]->listItems()) {
        state.callFunction(*args[0], *elem, vTmp, pos);
        bool res = state.forceBool(
            vTmp, pos, "while evaluating the return value of the function passed to " + fun
        );
        if (res == any) {
            v.mkBool(any);
            return;
        }
    }
    v.mkBool(!any);
}

static void elemAt(EvalState & state, const PosIdx pos, Value & list, int n, Value & v)
{
    state.forceList(list, pos, "while evaluating the first argument passed to builtins.elemAt");
    if (n < 0 || (unsigned int) n >= list.listSize()) {
        state.error<EvalError>("list index %1% is out of bounds", n).atPos(pos).debugThrow();
    }
    state.forceValue(*list.listElems()[n], pos);
    v = *list.listElems()[n];
}

/**
 * builtins.all
 */

static void prim_all(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    anyOrAll(false, state, pos, args, v);
}

PrimOp primop_all({
    .name = "__all",
    .args = {"pred", "list"},
    .doc = R"(
      Return `true` if the function *pred* returns `true` for all elements
      of *list*, and `false` otherwise.
    )",
    .fun = prim_all,
});

/**
 * builtins.any
 */

static void prim_any(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    anyOrAll(true, state, pos, args, v);
}

PrimOp primop_any({
    .name = "__any",
    .args = {"pred", "list"},
    .doc = R"(
      Return `true` if the function *pred* returns `true` for at least one
      element of *list*, and `false` otherwise.
    )",
    .fun = prim_any,
});

/**
 * builtins.concatLists
 */

static void prim_concatLists(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceList(
        *args[0], pos, "while evaluating the first argument passed to builtins.concatLists"
    );
    state.concatLists(
        v,
        args[0]->listSize(),
        args[0]->listElems(),
        pos,
        "while evaluating a value of the list passed to builtins.concatLists"
    );
}

PrimOp primop_concatLists({
    .name = "__concatLists",
    .args = {"lists"},
    .doc = R"(
      Concatenate a list of lists into a single list.
    )",
    .fun = prim_concatLists,
});

/**
 * builtins.concatMap
 */

static void prim_concatMap(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceFunction(
        *args[0], pos, "while evaluating the first argument passed to builtins.concatMap"
    );
    state.forceList(
        *args[1], pos, "while evaluating the second argument passed to builtins.concatMap"
    );
    auto nrLists = args[1]->listSize();

    // List of returned lists before concatenation. References to these Values must NOT be
    // persisted.
    SmallTemporaryValueVector<conservativeStackReservation> lists(nrLists);
    size_t len = 0;

    for (unsigned int n = 0; n < nrLists; ++n) {
        Value * vElem = args[1]->listElems()[n];
        state.callFunction(*args[0], *vElem, lists[n], pos);
        state.forceList(
            lists[n],
            lists[n].determinePos(args[0]->determinePos(pos)),
            "while evaluating the return value of the function passed to builtins.concatMap"
        );
        len += lists[n].listSize();
    }

    state.mkList(v, len);
    auto out = v.listElems();
    for (unsigned int n = 0, pos = 0; n < nrLists; ++n) {
        auto l = lists[n].listSize();
        if (l) {
            memcpy(out + pos, lists[n].listElems(), l * sizeof(Value *));
        }
        pos += l;
    }
}

PrimOp primop_concatMap({
    .name = "__concatMap",
    .args = {"f", "list"},
    .doc = R"(
      This function is equivalent to `builtins.concatLists (map f list)`
      but is more efficient.
    )",
    .fun = prim_concatMap,
});

/**
 * builtins.elem
 */

static void prim_elem(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    bool res = false;
    state.forceList(*args[1], pos, "while evaluating the second argument passed to builtins.elem");
    for (auto elem : args[1]->listItems()) {
        if (state.eqValues(
                *args[0],
                *elem,
                pos,
                "while searching for the presence of the given element in the list"
            ))
        {
            res = true;
            break;
        }
    }
    v.mkBool(res);
}

PrimOp primop_elem({
    .name = "__elem",
    .args = {"x", "xs"},
    .doc = R"(
      Return `true` if a value equal to *x* occurs in the list *xs*, and
      `false` otherwise.
    )",
    .fun = prim_elem,
});

/**
 * builtins.elemAt
 */

static void prim_elemAt(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    elemAt(
        state,
        pos,
        *args[0],
        state.forceInt(
            *args[1], pos, "while evaluating the second argument passed to builtins.elemAt"
        ),
        v
    );
}

PrimOp primop_elemAt({
    .name = "__elemAt",
    .args = {"xs", "n"},
    .doc = R"(
      Return element *n* from the list *xs*. Elements are counted starting
      from 0. A fatal error occurs if the index is out of bounds.
    )",
    .fun = prim_elemAt,
});

/**
 * builtins.filter
 */

static void prim_filter(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceList(
        *args[1], pos, "while evaluating the second argument passed to builtins.filter"
    );

    if (args[1]->listSize() == 0) {
        v = *args[1];
        return;
    }

    state.forceFunction(
        *args[0], pos, "while evaluating the first argument passed to builtins.filter"
    );

    SmallValueVector<nonRecursiveStackReservation> vs(args[1]->listSize());
    size_t k = 0;

    bool same = true;
    for (unsigned int n = 0; n < args[1]->listSize(); ++n) {
        Value res;
        state.callFunction(*args[0], *args[1]->listElems()[n], res, noPos);
        if (state.forceBool(
                res,
                pos,
                "while evaluating the return value of the filtering function passed to "
                "builtins.filter"
            ))
        {
            vs[k++] = args[1]->listElems()[n];
        } else {
            same = false;
        }
    }

    if (same) {
        v = *args[1];
    } else {
        state.mkList(v, k);
        for (unsigned int n = 0; n < k; ++n) {
            v.listElems()[n] = vs[n];
        }
    }
}

/**
 * builtins.foldl'
 */

static void prim_foldlStrict(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceFunction(
        *args[0], pos, "while evaluating the first argument passed to builtins.foldlStrict"
    );
    state.forceList(
        *args[2], pos, "while evaluating the third argument passed to builtins.foldlStrict"
    );
    if (args[2]->listSize()) {
        Value * vCur = args[1];
        for (auto [n, elem] : enumerate(args[2]->listItems())) {
            Value * vs[]{vCur, elem};
            vCur = n == args[2]->listSize() - 1 ? &v : state.allocValue();
            state.callFunction(*args[0], 2, vs, *vCur, pos);
        }
        state.forceValue(v, pos);
    } else {
        state.forceValue(*args[1], pos);
        v = *args[1];
    }
}

PrimOp primop_foldlStrict({
    .name = "__foldl'",
    .args = {"op", "nul", "list"},
    .doc = R"(
      Reduce a list by applying a binary operator, from left to right,
      e.g. `foldl' op nul [x0 x1 x2 ...] = op (op (op nul x0) x1) x2)
      ...`. For example, `foldl' (x: y: x + y) 0 [1 2 3]` evaluates to 6.
      The return value of each application of `op` is evaluated immediately,
      even for intermediate values.
    )",
    .fun = prim_foldlStrict,
});

/**
 * builtins.genList
 */

static void prim_genList(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto len = state.forceInt(
        *args[1], pos, "while evaluating the second argument passed to builtins.genList"
    );
    if (len < 0) {
        state.error<EvalError>("cannot create list of size %1%", len).atPos(pos).debugThrow();
    }
    // More strict than striclty (!) necessary, but acceptable
    // as evaluating map without accessing any values makes little sense.
    state.forceFunction(
        *args[0], noPos, "while evaluating the first argument passed to builtins.genList"
    );
    state.mkList(v, len);
    for (unsigned int n = 0; n < (unsigned int) len; ++n) {
        auto arg = state.allocValue();
        arg->mkInt(n);
        (v.listElems()[n] = state.allocValue())->mkApp(args[0], arg);
    }
}

PrimOp primop_genList({
    .name = "__genList",
    .args = {"generator", "length"},
    .doc = R"(
      Generate list of size *length*, with each element *i* equal to the
      value returned by *generator* `i`. For example,
      ```nix
      builtins.genList (x: x * x) 5
      ```
      returns the list `[ 0 1 4 9 16 ]`.
    )",
    .fun = prim_genList,
});

/**
 * builtins.head
 */

static void prim_head(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    elemAt(state, pos, *args[0], 0, v);
}

PrimOp primop_head({
    .name = "__head",
    .args = {"list"},
    .doc = R"(
      Return the first element of a list; abort evaluation if the argument
      isn’t a list or is an empty list. You can test whether a list is
      empty by comparing it with `[]`.
    )",
    .fun = prim_head,
});

/**
 * builtins.length
 */

static void prim_length(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceList(*args[0], pos, "while evaluating the first argument passed to builtins.length");
    v.mkInt(args[0]->listSize());
}
PrimOp primop_length({
    .name = "__length",
    .args = {"e"},
    .doc = R"(
      Return the length of the list *e*.
    )",
    .fun = prim_length,
});

PrimOp primop_filter({
    .name = "__filter",
    .args = {"f", "list"},
    .doc = R"(
      Return a list consisting of the elements of *list* for which the
      function *f* returns `true`.
    )",
    .fun = prim_filter,
});

/**
 * builtins.listToAttrs
 */

static void prim_listToAttrs(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceList(*args[0], pos, "while evaluating the argument passed to builtins.listToAttrs");

    auto attrs = state.buildBindings(args[0]->listSize());

    std::set<Symbol> seen;

    for (auto v2 : args[0]->listItems()) {
        state.forceAttrs(
            *v2, pos, "while evaluating an element of the list passed to builtins.listToAttrs"
        );

        Bindings::iterator j =
            getAttr(state, state.sName, v2->attrs, "in a {name=...; value=...;} pair");

        auto name = state.forceStringNoCtx(
            *j->value,
            j->pos,
            "while evaluating the `name` attribute of an element of the list passed to "
            "builtins.listToAttrs"
        );

        auto sym = state.symbols.create(name);
        if (seen.insert(sym).second) {
            Bindings::iterator j2 =
                getAttr(state, state.sValue, v2->attrs, "in a {name=...; value=...;} pair");
            attrs.insert(sym, j2->value, j2->pos);
        }
    }

    v.mkAttrs(attrs);
}

PrimOp primop_listToAttrs({
    .name = "__listToAttrs",
    .args = {"e"},
    .doc = R"(
      Construct a set from a list specifying the names and values of each
      attribute. Each element of the list should be a set consisting of a
      string-valued attribute `name` specifying the name of the attribute,
      and an attribute `value` specifying its value.

      In case of duplicate occurrences of the same name, the first
      takes precedence.

      Example:

      ```nix
      builtins.listToAttrs
        [ { name = "foo"; value = 123; }
          { name = "bar"; value = 456; }
          { name = "bar"; value = 420; }
        ]
      ```

      evaluates to

      ```nix
      { foo = 123; bar = 456; }
      ```
    )",
    .fun = prim_listToAttrs,
});

/**
 * builtins.map
 */

static void prim_map(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceList(*args[1], pos, "while evaluating the second argument passed to builtins.map");

    if (args[1]->listSize() == 0) {
        v = *args[1];
        return;
    }

    state.forceFunction(
        *args[0], pos, "while evaluating the first argument passed to builtins.map"
    );

    state.mkList(v, args[1]->listSize());
    for (unsigned int n = 0; n < v.listSize(); ++n) {
        (v.listElems()[n] = state.allocValue())->mkApp(args[0], args[1]->listElems()[n]);
    }
}

PrimOp primop_map({
    .name = "map",
    .args = {"f", "list"},
    .doc = R"(
      Apply the function *f* to each element in the list *list*. For
      example,

      ```nix
      map (x: "foo" + x) [ "bar" "bla" "abc" ]
      ```

      evaluates to `[ "foobar" "foobla" "fooabc" ]`.
    )",
    .fun = prim_map,
});

/**
 * builtins.partition
 */

static void prim_partition(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceFunction(
        *args[0], pos, "while evaluating the first argument passed to builtins.partition"
    );
    state.forceList(
        *args[1], pos, "while evaluating the second argument passed to builtins.partition"
    );

    auto len = args[1]->listSize();

    ValueVector right, wrong;

    for (unsigned int n = 0; n < len; ++n) {
        auto vElem = args[1]->listElems()[n];
        state.forceValue(*vElem, pos);
        Value res;
        state.callFunction(*args[0], *vElem, res, pos);
        if (state.forceBool(
                res,
                pos,
                "while evaluating the return value of the partition function passed to "
                "builtins.partition"
            ))
        {
            right.push_back(vElem);
        } else {
            wrong.push_back(vElem);
        }
    }

    auto attrs = state.buildBindings(2);

    auto & vRight = attrs.alloc(state.sRight);
    auto rsize = right.size();
    state.mkList(vRight, rsize);
    if (rsize) {
        memcpy(vRight.listElems(), right.data(), sizeof(Value *) * rsize);
    }

    auto & vWrong = attrs.alloc(state.sWrong);
    auto wsize = wrong.size();
    state.mkList(vWrong, wsize);
    if (wsize) {
        memcpy(vWrong.listElems(), wrong.data(), sizeof(Value *) * wsize);
    }

    v.mkAttrs(attrs);
}

PrimOp primop_partition({
    .name = "__partition",
    .args = {"pred", "list"},
    .doc = R"(
      Given a predicate function *pred*, this function returns an
      attrset containing a list named `right`, containing the elements
      in *list* for which *pred* returned `true`, and a list named
      `wrong`, containing the elements for which it returned
      `false`. For example,

      ```nix
      builtins.partition (x: x > 10) [1 23 9 3 42]
      ```

      evaluates to

      ```nix
      { right = [ 23 42 ]; wrong = [ 1 9 3 ]; }
      ```
    )",
    .fun = prim_partition,
});

/**
 * builtins.sort
 */

static void prim_sort(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceList(*args[1], pos, "while evaluating the second argument passed to builtins.sort");

    auto len = args[1]->listSize();
    if (len == 0) {
        v = *args[1];
        return;
    }

    state.forceFunction(
        *args[0], pos, "while evaluating the first argument passed to builtins.sort"
    );

    state.mkList(v, len);
    for (unsigned int n = 0; n < len; ++n) {
        state.forceValue(*args[1]->listElems()[n], pos);
        v.listElems()[n] = args[1]->listElems()[n];
    }

    auto comparator = [&](Value * a, Value * b) {
        /* Optimization: if the comparator is lessThan, bypass
           callFunction. */
        /* TODO: (layus) this is absurd. An optimisation like this
           should be outside the lambda creation */
        if (args[0]->isPrimOp()) {
            auto ptr = args[0]->primOp->fun.target<decltype(&prim_lessThan)>();
            if (ptr && *ptr == prim_lessThan) {
                return CompareValues(state, noPos, "while evaluating the ordering function passed to builtins.sort")(a, b);
            }
        }

        Value * vs[] = {a, b};
        Value vBool;
        state.callFunction(*args[0], 2, vs, vBool, noPos);
        return state.forceBool(
            vBool,
            pos,
            "while evaluating the return value of the sorting function passed to builtins.sort"
        );
    };

    /* FIXME: std::sort can segfault if the comparator is not a strict
       weak ordering. What to do? std::stable_sort() seems more
       resilient, but no guarantees... */
    std::stable_sort(v.listElems(), v.listElems() + len, comparator);
}

PrimOp primop_sort({
    .name = "__sort",
    .args = {"comparator", "list"},
    .doc = R"(
      Return *list* in sorted order. It repeatedly calls the function
      *comparator* with two elements. The comparator should return `true`
      if the first element is less than the second, and `false` otherwise.
      For example,

      ```nix
      builtins.sort builtins.lessThan [ 483 249 526 147 42 77 ]
      ```

      produces the list `[ 42 77 147 249 483 526 ]`.

      This is a stable sort: it preserves the relative order of elements
      deemed equal by the comparator.
    )",
    .fun = prim_sort,
});

/**
 * builtins.tail
 */

static void prim_tail(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceList(*args[0], pos, "while evaluating the first argument passed to builtins.tail");
    if (args[0]->listSize() == 0) {
        state.error<EvalError>("'tail' called on an empty list").atPos(pos).debugThrow();
    }

    state.mkList(v, args[0]->listSize() - 1);
    for (unsigned int n = 0; n < v.listSize(); ++n) {
        v.listElems()[n] = args[0]->listElems()[n + 1];
    }
}

PrimOp primop_tail({
    .name = "__tail",
    .args = {"list"},
    .doc = R"(
      Return the second to last elements of a list; abort evaluation if
      the argument isn’t a list or is an empty list.

      > **Warning**
      >
      > This function should generally be avoided since it's inefficient:
      > unlike Haskell's `tail`, it takes O(n) time, so recursing over a
      > list by repeatedly calling `tail` takes O(n^2) time.
    )",
    .fun = prim_tail,
});
}
