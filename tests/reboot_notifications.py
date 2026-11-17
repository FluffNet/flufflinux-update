#!/usr/bin/env python3
"""Run in a private mount namespace and session bus on the disposable test VM."""
import gettext, json, os, subprocess, sys, tempfile, threading, time
from pathlib import Path
if '--child' not in sys.argv:
    assert os.geteuid() == 0
    root=Path(tempfile.mkdtemp(prefix='flu-reboot-notification-'));root.chmod(0o777)
    args=['unshare','--mount','--fork',sys.executable,__file__,sys.argv[1],'--child',str(root)]
    raise SystemExit(subprocess.call(args))
root=Path(sys.argv[-1])
if os.geteuid()==0:
    subprocess.run(['mount','--make-rprivate','/'],check=True)
    target=root/'pacman';target.mkdir();target.chmod(0o777)
    boot=Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    (target/'flufflinux-update-reboot.json').write_text(json.dumps({'boot_id':boot,'recommended':True,'blocked':True}))
    (target/'flufflinux-update-state.json').write_text(json.dumps({'phase':'complete'}))
    subprocess.run(['mount','--bind',str(target),'/etc/pacman.d'],check=True)
    locale=root/'locale';locale.mkdir()
    for lang in ['he','ja']:
        catalog=locale/lang/'LC_MESSAGES';catalog.mkdir(parents=True)
        source=Path(__file__).resolve().parents[1]/'po'/lang/'kcm_fluffupdates.po'
        subprocess.run(['msgfmt',str(source),'-o',str(catalog/'kcm_fluffupdates.mo')],check=True)
    subprocess.run(['mount','--bind',str(locale),'/usr/share/locale'],check=True)
    raise SystemExit(subprocess.call(['runuser','-u','mai','--','dbus-run-session','--',sys.executable,__file__,sys.argv[1],'--child',str(root)]))
import dbus, dbus.service, dbus.mainloop.glib
from gi.repository import GLib
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus=dbus.SessionBus(); calls=[];closed=[];prompts=[]
class Notifications(dbus.service.Object):
    @dbus.service.method('org.freedesktop.Notifications',in_signature='susssasa{sv}i',out_signature='u')
    def Notify(self,app,replaces,icon,title,body,actions,hints,timeout):
        calls.append((icon,title,body,list(actions),dict(hints),int(timeout)));return len(calls)
    @dbus.service.method('org.freedesktop.Notifications',in_signature='u',out_signature='')
    def CloseNotification(self,id):
        closed.append(int(id));self.NotificationClosed(id,2)
    @dbus.service.signal('org.freedesktop.Notifications',signature='uu')
    def NotificationClosed(self,id,reason):pass
    @dbus.service.signal('org.freedesktop.Notifications',signature='us')
    def ActionInvoked(self,id,action):pass
class Logout(dbus.service.Object):
    @dbus.service.method('org.kde.LogoutPrompt',in_signature='',out_signature='')
    def promptReboot(self):prompts.append(True)
n=Notifications(dbus.service.BusName('org.freedesktop.Notifications',bus),'/org/freedesktop/Notifications')
l=Logout(dbus.service.BusName('org.kde.LogoutPrompt',bus),'/LogoutPrompt')
loop=GLib.MainLoop();errors=[]
def wait(condition):
    end=time.monotonic()+10
    while not condition():
        assert time.monotonic()<end,'notification timeout'
        time.sleep(.05)
def exercise():
    process=None
    try:
        for action in ['dismiss','restart','he','ja']:
            lang=action if action in ['he','ja'] else 'en'
            translate=gettext.translation('kcm_fluffupdates',str(root/'locale'),languages=[lang],fallback=True).gettext
            env={**os.environ,'XDG_STATE_HOME':str(root/action),'LC_ALL':{'he':'he_IL.UTF-8','ja':'ja_JP.UTF-8'}.get(lang,'en_US.UTF-8'),'LANGUAGE':lang}
            count=len(calls)
            process=subprocess.Popen([sys.argv[1]],env=env)
            wait(lambda:bus.name_has_owner('org.flufflinux.Update.Notifications'))
            panel=dbus.Interface(bus.get_object('org.flufflinux.Update.Notifications','/org/flufflinux/Update/Notifications'),'org.flufflinux.Update.Notifications')
            # Terminal updates must notify without any UI translation call.
            wait(lambda:len(calls)>count)
            panel.SetVisible(True,dbus.Dictionary({'Restart':'Restart'},signature='ss'))
            icon,title,body,actions,hints,timeout=calls[-1]
            assert icon=='system-reboot' and timeout==0 and hints['resident']
            assert title==translate('Restart recommended'),(lang,title)
            assert actions==['restart',translate('Restart')],(lang,actions)
            id=len(calls)
            if action=='restart':
                GLib.idle_add(lambda:n.ActionInvoked(id,'restart'))
                wait(lambda:id in closed and bool(prompts))
            else:GLib.idle_add(lambda:n.NotificationClosed(id,2))
            panel.SetVisible(False,dbus.Dictionary({},signature='ss'));time.sleep(1)
            panel.SetVisible(True,dbus.Dictionary({},signature='ss'));time.sleep(1)
            assert len(calls)==count+1
            process.terminate();process.wait();process=None
            assert json.loads(Path('/etc/pacman.d/flufflinux-update-reboot.json').read_text())['blocked']
            process=subprocess.Popen([sys.argv[1]],env=env)
            wait(lambda:bus.name_has_owner('org.flufflinux.Update.Notifications'))
            panel=dbus.Interface(bus.get_object('org.flufflinux.Update.Notifications','/org/flufflinux/Update/Notifications'),'org.flufflinux.Update.Notifications')
            panel.SetVisible(True,dbus.Dictionary({'Restart':'Restart'},signature='ss'));time.sleep(1)
            assert len(calls)==count+1,'dismissed notification replayed'
            process.terminate();process.wait();process=None
        print('PASS no-UI startup, English/Hebrew/Japanese catalogs, persistent timeout, foreground delivery, dismiss, restart action, panel reopen, observer restart, banner state preserved',flush=True)
    except Exception as error:errors.append(error)
    finally:
        if process:process.terminate();process.wait()
        GLib.idle_add(loop.quit)
threading.Thread(target=exercise,daemon=True).start();loop.run()
if errors:raise errors[0]
