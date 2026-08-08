import SwiftUI

@main
struct XiaozhiANCSConnectorApp: App {
    @StateObject private var bluetooth = BluetoothConnector()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(bluetooth)
                .task {
                    bluetooth.start()
                }
        }
    }
}
