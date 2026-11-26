# Read fullscreen by size, not position

A window counts as fullscreen when it is the size of its nearest monitor, within 2 px, wherever it sits (`SharedGeometry::IsMonitorSized`, used by `CoversItsMonitor` and by the companion). Apps such as Chromium go fullscreen by sizing themselves to the monitor while Windows still reports them as restored, and an app that does that almost always means fullscreen, so MultiWM doesn't second-guess it with a position check. A monitor-sized window that straddles monitors or sits partly off-screen therefore counts as fullscreen: that is accepted, not a bug.
