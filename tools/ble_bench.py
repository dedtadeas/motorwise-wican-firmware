#!/usr/bin/env python3
"""Bench test of a Motorwise reader over Bluetooth LE, with the laptop's own adapter (BlueZ over D-Bus).

    tools/ble_bench.py pair   <name> <secrets-file>   pair and bond, typing the passkey from the file, then ask
    tools/ble_bench.py check  <name>                  connect on the existing bond and ask; never pairs
    tools/ble_bench.py remove <name>                  forget the bond on the laptop side

"ask" sends AT@1 and ATI on characteristic fee1 and prints the answers. `check` registers an agent that refuses
any passkey request: if the reader lost the bond, the link can't be encrypted and the check fails (it says so).
The passkey is read from the file inside this script, handed to BlueZ, never printed.

Needs only what Ubuntu ships: python3-dbus and python3-gi (BlueZ 5.x). Licence: GPL-3.0, like wican-fw.
"""
import sys
import time

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

BUS = 'org.bluez'
AGENT_PATH = '/motorwise/bench_agent'
FEE1 = '0000fee1-0000-1000-8000-00805f9b34fb'

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SystemBus()
loop = GLib.MainLoop()


def log(msg):
    print(time.strftime('%H:%M:%S'), msg, flush=True)


def read_passkey(path):
    with open(path) as f:
        for line in f:
            k, _, v = line.strip().partition('=')
            if k == 'ble_pass' and v.isdigit() and len(v) == 6:
                return int(v)
    sys.exit('no 6-digit ble_pass line in the secrets file')


class Agent(dbus.service.Object):
    def __init__(self, passkey):
        super().__init__(bus, AGENT_PATH)
        self.passkey = passkey
        self.asked = 0

    @dbus.service.method('org.bluez.Agent1', in_signature='o', out_signature='u')
    def RequestPasskey(self, device):
        self.asked += 1
        if self.passkey is None:
            log('the reader asked to pair again: refused (check mode)')
            raise dbus.DBusException('org.bluez.Error.Rejected', 'check mode never pairs')
        log('passkey requested: typed from the secrets file')
        return dbus.UInt32(self.passkey)

    @dbus.service.method('org.bluez.Agent1', in_signature='ouq', out_signature='')
    def DisplayPasskey(self, device, passkey, entered):
        pass

    @dbus.service.method('org.bluez.Agent1', in_signature='ou', out_signature='')
    def RequestConfirmation(self, device, passkey):
        self.asked += 1
        raise dbus.DBusException('org.bluez.Error.Rejected', 'passkey entry only')

    @dbus.service.method('org.bluez.Agent1', in_signature='o', out_signature='s')
    def RequestPinCode(self, device):
        raise dbus.DBusException('org.bluez.Error.Rejected', 'no PIN')

    @dbus.service.method('org.bluez.Agent1', in_signature='os', out_signature='')
    def AuthorizeService(self, device, uuid):
        pass

    @dbus.service.method('org.bluez.Agent1', in_signature='o', out_signature='')
    def RequestAuthorization(self, device):
        raise dbus.DBusException('org.bluez.Error.Rejected', 'no')

    @dbus.service.method('org.bluez.Agent1', in_signature='', out_signature='')
    def Cancel(self):
        pass

    @dbus.service.method('org.bluez.Agent1', in_signature='', out_signature='')
    def Release(self):
        pass


def objects():
    return bus.get_object(BUS, '/').GetManagedObjects(dbus_interface='org.freedesktop.DBus.ObjectManager')


def adapter_path():
    for path, ifaces in objects().items():
        if 'org.bluez.Adapter1' in ifaces:
            return path
    sys.exit('no Bluetooth adapter')


def find_devices(name):
    return [(path, ifaces['org.bluez.Device1']) for path, ifaces in objects().items()
            if 'org.bluez.Device1' in ifaces and name in (ifaces['org.bluez.Device1'].get('Name'),
                                                          ifaces['org.bluez.Device1'].get('Alias'))]


def find_device(name):
    # The reader advertises a resolvable private address that changes at each start, so BlueZ can hold several
    # objects with its name: prefer the bonded one, then one heard now (RSSI).
    found = find_devices(name)
    for want in ('Paired', 'RSSI'):
        for path, d in found:
            if d.get(want):
                return path, d
    return found[0] if found else (None, None)


def wait(cond, seconds, step=0.2):
    end = time.time() + seconds
    while time.time() < end:
        ctx = loop.get_context()
        while ctx.pending():
            ctx.iteration(False)
        r = cond()
        if r:
            return r
        time.sleep(step)
    return None


def discover(name, seconds=30):
    ad = bus.get_object(BUS, adapter_path())
    adapter = dbus.Interface(ad, 'org.bluez.Adapter1')
    adapter.SetDiscoveryFilter({'Transport': 'le'})
    try:
        adapter.StartDiscovery()
    except dbus.DBusException:
        pass
    try:
        # A device the laptop knows can sit in BlueZ's cache while the reader is off: wait until it is heard (RSSI).
        found = wait(lambda: (lambda p, d: p if p and 'RSSI' in d else None)(*find_device(name)), seconds)
    finally:
        try:
            adapter.StopDiscovery()
        except dbus.DBusException:
            pass
    if not found:
        sys.exit(f'{name} not seen advertising in {seconds} s')
    log(f'{name} advertising')
    return found


def prop(path, iface, key):
    return bus.get_object(BUS, path).Get(iface, key, dbus_interface='org.freedesktop.DBus.Properties')


def ask(dev_path):
    if not wait(lambda: prop(dev_path, 'org.bluez.Device1', 'ServicesResolved'), 20):
        raise RuntimeError('services not resolved')
    char = None
    for path, ifaces in objects().items():
        c = ifaces.get('org.bluez.GattCharacteristic1')
        if c and path.startswith(dev_path) and c.get('UUID') == FEE1:
            char = path
    if not char:
        raise RuntimeError('no fee1 characteristic')
    obj = bus.get_object(BUS, char)
    ch = dbus.Interface(obj, 'org.bluez.GattCharacteristic1')
    got = bytearray()

    def changed(iface, changes, invalidated):
        if 'Value' in changes:
            got.extend(bytes(changes['Value']))

    sig = bus.add_signal_receiver(changed, 'PropertiesChanged', 'org.freedesktop.DBus.Properties', BUS, char)
    ch.StartNotify()
    answers = {}
    try:
        for cmd in ('ATE0', 'AT@1', 'ATI'):
            got.clear()
            ch.WriteValue(dbus.Array((cmd + '\r').encode(), signature='y'), {'type': 'request'})
            wait(lambda: b'>' in got, 5)
            text = got.decode('ascii', 'replace').replace('\r', ' ').replace('\n', ' ').replace('>', '').strip()
            answers[cmd] = text
            log(f'{cmd} -> {text!r}')
    finally:
        try:
            ch.StopNotify()
        except dbus.DBusException:
            pass
        sig.remove()
    return answers


def connect(dev_path):
    dev = dbus.Interface(bus.get_object(BUS, dev_path), 'org.bluez.Device1')
    for attempt in range(3):
        try:
            dev.Connect(timeout=30)
            return dev
        except dbus.DBusException as e:
            log(f'connect attempt {attempt + 1}: {e.get_dbus_name()}')
            time.sleep(2)
    raise RuntimeError('could not connect')


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ('pair', 'check', 'remove'):
        sys.exit(__doc__)
    mode, name = sys.argv[1], sys.argv[2]

    if mode == 'remove':
        found = find_devices(name)
        for path, _ in found:
            dbus.Interface(bus.get_object(BUS, adapter_path()), 'org.bluez.Adapter1').RemoveDevice(path)
        if found:
            log(f'{name} forgotten on the laptop ({len(found)} entries)')
        else:
            log(f'{name} not known on the laptop')
        return 0

    passkey = read_passkey(sys.argv[3]) if mode == 'pair' else None
    agent = Agent(passkey)
    manager = dbus.Interface(bus.get_object(BUS, '/org/bluez'), 'org.bluez.AgentManager1')
    manager.RegisterAgent(AGENT_PATH, 'KeyboardOnly')
    # The reader asks for security as soon as it is connected; BlueZ hands that to the default agent, so this one
    # must be the default for the run (the desktop's agent gets it back when this one unregisters).
    manager.RequestDefaultAgent(AGENT_PATH)
    ok = False
    adapter_props = bus.get_object(BUS, adapter_path())
    was_pairable = bool(adapter_props.Get('org.bluez.Adapter1', 'Pairable', dbus_interface='org.freedesktop.DBus.Properties'))
    if mode == 'pair' and not was_pairable:
        # Not pairable means not bondable: the reader accepts bonded pairing only. Restored at the end.
        adapter_props.Set('org.bluez.Adapter1', 'Pairable', True, dbus_interface='org.freedesktop.DBus.Properties')
        log('laptop adapter made pairable for this run')
    try:
        path = discover(name)
        dev = dbus.Interface(bus.get_object(BUS, path), 'org.bluez.Device1')
        if mode == 'pair':
            if prop(path, 'org.bluez.Device1', 'Paired'):
                log('already paired on the laptop: run remove first for a fresh pairing')
            else:
                dev.Pair(timeout=60)
                log('paired')
            bus.get_object(BUS, path).Set('org.bluez.Device1', 'Trusted', True,
                                          dbus_interface='org.freedesktop.DBus.Properties')
        else:
            if not prop(path, 'org.bluez.Device1', 'Paired'):
                sys.exit('the laptop holds no bond with this reader')
        # After bonding BlueZ may move the device to its identity address: look it up again.
        path, _ = find_device(name)
        dev = connect(path)
        log('connected')
        answers = ask(path)
        bonded = prop(path, 'org.bluez.Device1', 'Paired') and agent.asked == 0 if mode == 'check' else True
        ok = bool(answers.get('AT@1')) and bonded
        try:
            dev.Disconnect()
        except dbus.DBusException:
            pass
    except (dbus.DBusException, RuntimeError) as e:
        log(f'failed: {e}')
    finally:
        manager.UnregisterAgent(AGENT_PATH)
        if mode == 'pair' and not was_pairable:
            adapter_props.Set('org.bluez.Adapter1', 'Pairable', False, dbus_interface='org.freedesktop.DBus.Properties')
            log('laptop adapter back to not pairable')
    if mode == 'check' and agent.asked:
        log('RESULT: bond NOT kept (the reader asked to pair again)')
    else:
        log(f'RESULT: {"ok" if ok else "FAILED"}')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
