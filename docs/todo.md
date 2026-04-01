- Fix binding expressions not updating to esp but being accepted into ui
- Telemetry packets may be sending at too high rate when pressing keyboard and motor moving etc,
- add a refresh button to target list so full browser refresh not required; same for raw inputs (tho should still auto update when controller detectd)
- add warning when attempting to browser refresh that changes may be lost. Ideally in a clean separate file so i can find and edit the warning message later manually#]
- add abs to expressions function; make list of supported expressions and examples

-schottkey diode to allow programming while battery plugged in


Okay, we really need to fix the backend now. Esp keeps disconnecting even on just rebooting.