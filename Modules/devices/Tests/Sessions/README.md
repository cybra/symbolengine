# Recorded sessions

Each file is a session `symbolengine_devices_recorder` wrote on real hardware with the owner, for `FakeAudioSystem::replay` to play back in `RecordingTests.cpp`. Each was trimmed to one system, the recorder tool's echo of its own stream left out, every quiet stretch longer than half a second shortened to it, and every device's identifier and name replaced by a placeholder, so no serial number or address is kept.

- `mac-sleep-unplug-headset.jsonl`: a MacBook recording from a USB interface while AirPods connected, the Mac slept for a minute, and the hub carrying the interface and both displays was unplugged with the lid closed, which slept it again until the hub was back.
- `windows-sleep-unplug-headset.jsonl`: a Windows 11 PC recording from a USB interface while it slept for a minute, the interface was unplugged and plugged back in twice, and AirPods were paired and connected.
- `windows-headset-input.jsonl`: AirPods' microphone opened on the same PC for two seconds.

The descriptions are what SymbolEngine said at the time. In the Windows sessions the headset's microphone reads as an ordinary input, since they were recorded before hands-free endpoints were known by the bus that enumerated them.
