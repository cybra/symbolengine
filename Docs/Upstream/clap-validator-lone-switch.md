# clap-validator: a plugin whose only parameter is a switch fails param-set-events

A new issue for [free-audio/clap-validator](https://github.com/free-audio/clap-validator/issues/new), which has no issue form; nothing open or closed there covers it. The title and body below go in as they are, the patch with them, so it could equally be a pull request. SymbolEngine's test plugins carry a second parameter meanwhile (`Tests/IdentityPlugin`), and the plugin that reproduces it is [clap-validator-test-plugin.c](clap-validator-test-plugin.c), beside this file.

## Title

param-set-events and param-set-no-cookies fail a plugin whose only parameter is a switch at 0

## Body

A plugin whose only parameter is stepped fails `param-set-events` and `param-set-no-cookies` whenever the value the test draws for it is the value it already has:

```
After calling 'clap_plugin_params::flush()', the parameter values did not change
```

The test draws a value for every parameter through `ParamFuzzer::randomize_params_at` with the fixed seed from `new_prng()`, and fails if the plugin's values afterwards are the same as before. A stepped parameter's draw is rounded, so it can land on the current value, and with the fixed seed it always does for a switch from 0 to 1 that starts at 0, as a bypass or an on/off switch does. A plugin with any continuous parameter passes, since that one always changes.

To reproduce, a stereo pass-through plugin whose only parameter is stepped and automatable, from 0 to 1, starting at 0: [a plain-C one](https://github.com/cybra/symbolengine/blob/main/Docs/Upstream/clap-validator-test-plugin.c), built with `-DPARAMS=1`. On master (b2f1d9b), on macOS arm64:

```
ERROR Test param-set-events failed
ERROR Test param-set-no-cookies failed
44 tests run, 31 passed, 2 failed, 0 warnings, 11 skipped
```

Built with `-DSTART=1.0`, the same plugin with its switch starting at 1 passes every test.

A fix: draw again from the same generator until at least one value differs from that parameter's current value. Both instances start from the same values and the same seed, so the flush run and the process run still send the same events, and the comparison between them is unchanged.

```diff
--- a/src/tests/plugin_instance/params.rs
+++ b/src/tests/plugin_instance/params.rs
@@ -186,6 +186,26 @@
     }
 
     Ok(TestStatus::Success { details: None })
+}
+
+/// Random values for the parameters, drawn again until at least one differs from that parameter's current value, since
+/// a stepped parameter can draw the value it already has. A plugin whose only parameter is a switch would otherwise
+/// fail for not changing. Both instances in the test start from the same values and the same seed, so they get the
+/// same events. Empty if no draw changes anything.
+fn param_events_that_change(fuzzer: &ParamFuzzer, initial_param_values: &BTreeMap<clap_id, f64>) -> Vec<Event> {
+    let mut prng = new_prng();
+    for _ in 0..100 {
+        let events = fuzzer.randomize_params_at(&mut prng, 0).collect::<Vec<_>>();
+        let changes_something = events.iter().any(|event| match event {
+            Event::ParamValue(event) => initial_param_values.get(&event.param_id) != Some(&event.value),
+            _ => false,
+        });
+        if changes_something {
+            return events;
+        }
+    }
+
+    Vec::new()
 }
 
 /// The test for `ProcessingTest::ParamChangeEvents`.
@@ -208,9 +228,8 @@
     };
 
     let param_info = params.info().context("Failure while fetching the parameters")?;
-    let mut param_events = ParamFuzzer::new(&param_info)
-        .randomize_params_at(&mut new_prng(), 0)
-        .collect::<Vec<_>>();
+    let initial_param_values = param_get_values(&params)?;
+    let mut param_events = param_events_that_change(&ParamFuzzer::new(&param_info), &initial_param_values);
 
     if param_events.is_empty() {
         return Ok(TestStatus::Skipped {
@@ -228,8 +247,6 @@
     }
 
     let flush_param_values = {
-        let initial_param_values = param_get_values(&params)?;
-
         plugin.poll_callback(|_| Ok(()))?;
 
         let input_queue = InputEventQueue::new();
@@ -270,10 +287,11 @@
 
     // we have to recreate the events because of cookies (they can be different between plugin instances)
     let param_info = params.info().context("Failure while fetching the parameters")?;
-    let mut param_events = ParamFuzzer::new(&param_info)
-        .with_no_cookies(null_cookies)
-        .randomize_params_at(&mut new_prng(), 0)
-        .collect::<Vec<_>>();
+    let initial_param_values = param_get_values(&params)?;
+    let mut param_events = param_events_that_change(
+        &ParamFuzzer::new(&param_info).with_no_cookies(null_cookies),
+        &initial_param_values,
+    );
 
     if null_cookies {
         for event in param_events.iter_mut() {
@@ -285,8 +303,6 @@
     }
 
     let process_param_values = {
-        let initial_param_values = param_get_values(&params)?;
-
         plugin.poll_callback(|_| Ok(()))?;
 
         plugin.on_audio_thread(|plugin| {
```

With it, the plugin passes (33 passed, 0 failed), and four other plugins, with one to three parameters among them continuous ones, a choice and bypass switches, get the same results as before. rustfmt and clippy have nothing to say about the file.
