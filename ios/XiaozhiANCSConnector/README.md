# Xiaozhi ANCS Connector

This iPhone app discovers the Xiaozhi ANCS BLE service, connects to it, and asks iOS to authorize ANCS for that connection. It never reads, stores, or sends notification content.

1. Open `XiaozhiANCSConnector.xcodeproj` in Xcode 15 or later on macOS.
2. Select the `XiaozhiANCSConnector` target, choose your Apple Development signing team, and use a unique bundle identifier.
3. Run it on a physical iPhone with Bluetooth enabled. Keep the app open until it reports a connection.
4. The firmware log should show `iOS app connected`, followed by `ANCS link encrypted` and ANCS service discovery.

The app and firmware share the companion service UUID `B3C845FE-4E0C-4D75-86A6-037DC2C3D4E5`.
