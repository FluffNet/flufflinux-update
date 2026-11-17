#!/usr/bin/env python3
"""Observe a real terminal transaction's marker from the unprivileged session."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import threading
import time

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--notifier', required=True)
parser.add_argument('--controller', required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--visual', action='store_true')
args = parser.parse_args()
assert os.geteuid() != 0
assert os.readlink('/proc/self/ns/mnt') != os.environ['FLU_HOST_MOUNT_NS']
args.output.mkdir(exist_ok=True)
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
received = []
real_notifications = None
real_bus = 'unix:path=/run/user/1000/bus'
if args.visual:
    connection = dbus.bus.BusConnection(real_bus)
    real_notifications = dbus.Interface(connection.get_object(
        'org.freedesktop.Notifications', '/org/freedesktop/Notifications'),
        'org.freedesktop.Notifications')


class Notifications(dbus.service.Object):
    @dbus.service.method('org.freedesktop.Notifications',
                         in_signature='susssasa{sv}i', out_signature='u')
    def Notify(self, app, replaces, icon, title, body, actions, hints, timeout):
        identifier = len(received) + 1
        if real_notifications:
            identifier = real_notifications.Notify(app, replaces, icon, title, body, actions, hints, timeout)
        received.append({'id': int(identifier), 'icon': str(icon), 'title': str(title),
                         'body': str(body), 'timeout': int(timeout)})
        return identifier

    @dbus.service.signal('org.freedesktop.Notifications', signature='us')
    def ActionInvoked(self, identifier, action):
        pass


notifications = Notifications(dbus.service.BusName('org.freedesktop.Notifications', bus),
                              '/org/freedesktop/Notifications')
loop = GLib.MainLoop()
errors = []


def exercise():
    notifier = gui = None
    try:
        env = {**os.environ, 'LC_ALL': 'en_US.UTF-8', 'LANGUAGE': 'en',
               'XDG_STATE_HOME': str(args.output / 'state')}
        # The marker was produced by Pacman, never manufactured by this test.
        model = json.loads(subprocess.check_output([args.controller, 'reconnect'], env=env, text=True))
        assert model['restartRecommended'] and model['restartRequired'], model
        notifier = subprocess.Popen([args.notifier], env=env)
        end = time.monotonic() + 15
        while not received:
            assert time.monotonic() < end, 'terminal update produced no notification'
            time.sleep(.05)
        assert received[0]['title'] == 'Restart recommended', received
        assert received[0]['icon'] == 'system-reboot' and received[0]['timeout'] == 0
        if args.visual:
            source = Path(__file__).resolve().parents[1]
            gui_env = {**env, 'XDG_RUNTIME_DIR': '/run/user/1000',
                       'WAYLAND_DISPLAY': 'wayland-1', 'QT_QPA_PLATFORM': 'wayland',
                       'QT_PLUGIN_PATH': str(source / 'build/bin') + ':/usr/lib/qt6/plugins'}
            with (args.output / 'gui.log').open('w') as log:
                gui = subprocess.Popen(['kcmshell6', 'kcm_fluffupdates'], env=gui_env,
                                       stdout=log, stderr=log)
                time.sleep(5)
                assert gui.poll() is None, (args.output / 'gui.log').read_text()
                subprocess.run(['spectacle', '-b', '-n', '-f', '-o', str(args.output / 'terminal-restart.png')],
                               env={**gui_env, 'DBUS_SESSION_BUS_ADDRESS': real_bus}, check=True)
        (args.output / 'result.json').write_text(json.dumps({
            'terminal_marker_drives_banner': True, 'terminal_marker_blocks_flu': True,
            'notification_without_opening_flu': received[0]}, indent=2))
    except Exception as error:
        errors.append(error)
    finally:
        for process in [gui, notifier]:
            if process:
                process.terminate()
                process.wait(timeout=10)
        if real_notifications:
            for item in received:
                real_notifications.CloseNotification(item['id'])
        GLib.idle_add(loop.quit)


threading.Thread(target=exercise, daemon=True).start()
loop.run()
if errors:
    raise errors[0]
print('PASS terminal Pacman -> live FLU banner/block and session restart notification')
