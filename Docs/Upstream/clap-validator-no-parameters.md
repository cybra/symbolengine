# clap-validator: param-conversions divides by zero on a plugin with no parameters

A comment for [free-audio/clap-validator#40](https://github.com/free-audio/clap-validator/pull/40), the open pull request that fixes it, to confirm the fix; it is based on master and has had no review. SymbolEngine's test plugins carry two parameters meanwhile (`Tests/IdentityPlugin`), and the plugin that reproduces it is [clap-validator-test-plugin.c](clap-validator-test-plugin.c), beside this file.

## Comment

Confirming this on master (b2f1d9b), on macOS arm64. A plain-C plugin with the params extension and no parameters ([this one](https://github.com/cybra/symbolengine/blob/main/Docs/Upstream/clap-validator-test-plugin.c), built with `-DPARAMS=0`) crashes `param-conversions`, from `4000usize.div_ceil(param_info.len())`:

```
ERROR Test param-conversions crashed: attempt to divide by zero. This is a bug in the validator
44 tests run, 30 passed, 1 failed, 0 warnings, 13 skipped
```

With this pull request applied, the same plugin passes, `param-conversions` its one warning:

```
44 tests run, 30 passed, 0 failed, 1 warnings, 13 skipped
```

An empty parameter list is valid CLAP, and `param-set-events` reports a plugin without automatable parameters as skipped, so `Skipped` might suit here too. Either way, it ends the crash.
