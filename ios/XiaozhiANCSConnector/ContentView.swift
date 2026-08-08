import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var bluetooth: BluetoothConnector

    var body: some View {
        VStack(spacing: 24) {
            Image(systemName: bluetooth.isConnected ? "bell.badge.fill" : "antenna.radiowaves.left.and.right")
                .font(.system(size: 54))
                .foregroundStyle(bluetooth.isConnected ? .green : .blue)

            Text("Xiaozhi ANCS")
                .font(.title2.weight(.semibold))

            Text(bluetooth.status)
                .multilineTextAlignment(.center)
                .foregroundStyle(.secondary)

            if !bluetooth.deviceName.isEmpty {
                Text(bluetooth.deviceName)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }

            Button(bluetooth.isConnected ? "Reconnect" : "Search Again") {
                bluetooth.reconnect()
            }
            .buttonStyle(.borderedProminent)
        }
        .padding(32)
    }
}
