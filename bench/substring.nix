# Tests stringLength and substring performance
with builtins;
{
  bench =
    baseStrLen:
    let
      # a big string
      baseStr = concatStringsSep "" (genList (_: "x") baseStrLen);
      # a way to force the values
      sum = ns: foldl' (acc: n: acc + n) 0 ns;
      forceOpTimes =
        range: # Int
        op: # Int -> Int
        sum (genList (ix: op ix) range);

      benchOp = ix: stringLength (substring ix 1 baseStr);
    in
    forceOpTimes baseStrLen benchOp;
}
