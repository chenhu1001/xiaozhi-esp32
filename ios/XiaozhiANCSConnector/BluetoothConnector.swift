import CoreBluetooth
import Foundation

final class BluetoothConnector: NSObject, ObservableObject {
    static let companionService = CBUUID(string: "B3C845FE-4E0C-4D75-86A6-037DC2C3D4E5")

    @Published private(set) var status = "Preparing Bluetooth"
    @Published private(set) var deviceName = ""
    @Published private(set) var isConnected = false

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?

    func start() {
        guard central == nil else {
            scan()
            return
        }
        central = CBCentralManager(delegate: self, queue: .main)
    }

    func reconnect() {
        if let peripheral {
            central.cancelPeripheralConnection(peripheral)
        }
        scan()
    }

    private func scan() {
        guard central?.state == .poweredOn else { return }
        isConnected = false
        status = "Searching for Xiaozhi ANCS"
        central.scanForPeripherals(withServices: [Self.companionService],
                                   options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
    }

    private func connect(_ peripheral: CBPeripheral) {
        self.peripheral = peripheral
        deviceName = peripheral.name ?? "Xiaozhi ANCS"
        status = "Requesting ANCS authorization"
        central.stopScan()
        central.connect(peripheral, options: [CBConnectPeripheralOptionRequiresANCS: true])
    }
}

extension BluetoothConnector: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            scan()
        case .poweredOff:
            status = "Bluetooth is turned off"
        case .unauthorized:
            status = "Bluetooth access is not allowed"
        case .unsupported:
            status = "Bluetooth LE is unavailable"
        default:
            status = "Waiting for Bluetooth"
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard self.peripheral?.identifier != peripheral.identifier else { return }
        connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        status = "Connected. ANCS authorization requested."
        isConnected = true
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral,
                        error: Error?) {
        status = "Connection failed: \(error?.localizedDescription ?? "Unknown error")"
        self.peripheral = nil
        scan()
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?) {
        isConnected = false
        status = "Disconnected. Searching again."
        self.peripheral = nil
        scan()
    }
}
