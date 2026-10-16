# KRdp-M

KRdp-M is a fork of [KRdp](https://invent.kde.org/plasma/krdp), the RDP server
of KDE Plasma, based on its Plasma 6.7 branch. It adds virtual displays that
follow the RDP client, see [Virtual displays](#virtual-displays) below,
[hardware encoding on NVIDIA GPUs](#hardware-encoding-on-nvidia-gpus) and
[AV1 encoding](#av1-encoding) for clients using FreeRDP-M.

It builds against the libraries of Plasma 6.7 and
[FreeRDP-M](https://github.com/maceeek/freerdp-m), and replaces the `krdp` package,
for Arch Linux and CachyOS see [Installing on Arch Linux and CachyOS](#installing-on-arch-linux-and-cachyos).

## Virtual displays

Instead of streaming the monitors connected to the computer, a remote session
can get its own virtual monitor that matches the client:

- It is created at the size and scale the client asks for when it connects.
- It is resized whenever the client asks for a different size, e.g. when the
  client window is resized or goes fullscreen. Use `/dynamic-resolution` with
  FreeRDP, Remmina's "Dynamic resolution update" or the default behaviour of
  the Windows Remote Desktop client. Windows stay where they are when resizing.

How remote sessions use the displays is configured in System Settings ›
Remote Desktop › Displays, with `VirtualDisplayMode` in the `[VirtualDisplay]`
group of `krdpserverrc` or with `--virtual-display`:

| Mode | Behaviour |
|------|-----------|
| `Disabled` | Stream the monitors of the computer, like upstream KRdp. |
| `Extend` | Add a virtual monitor matching the client next to the monitors. |
| `Exclusive` (default) | Only use a virtual monitor matching the client. The monitors of the computer are turned off while a client is connected. |

### Multiple monitors

Clients with several monitors get a virtual monitor for each of them,
arranged like on the client, with windows moving between them as usual.
Pointer input goes to the monitor the pointer is on.

- **KRDC**: set *Resolution* to *Use All Screens Resolution* in the
  connection settings. KRDC then opens a fullscreen window on every monitor.
  With *Current Window Size* KRDC uses a single monitor that follows the size
  of its window instead.
- **FreeRDP**: `xfreerdp3 /multimon` or `sdl-freerdp3 /multimon`, with
  `/dynamic-resolution` for a single monitor that follows the window.
- **Windows Remote Desktop**: "Use all my monitors for the remote session".

The monitors are created when the client connects. Moving or resizing them
during a session is supported for clients that report it; to use a different
number of monitors, reconnect. Multiple monitors need the Plasma screencasting
protocol; through the portal only the primary monitor is used.

### Getting the monitors back

In exclusive mode the monitors of the computer are turned off while a client is
connected. They are only turned off once the virtual monitor exists, and their
layout is saved before changing anything. They are turned back on with the
saved layout when:

- the last client disconnects;
- someone at the computer presses **Meta+Ctrl+Alt+R** (configurable in System
  Settings › Shortcuts › Return Displays to This Computer). This runs
  `krdpserver --return-displays` as a separate process, so it also works if
  krdpserver does not respond;
- someone at the computer uses the keyboard or mouse while the remote user is
  idle for a moment (`DetectLocalInput`), or plugs in or turns on a monitor;
- "Return Displays to This Computer" is picked from the tray icon;
- krdpserver exits, or crashes: KWin itself restores the layout of the monitors
  as soon as the virtual monitor disappears, and krdpserver additionally
  restores its saved layout when it starts again and in `ExecStopPost` of its
  systemd unit (`krdpserver --restore-displays`).

When the monitors are given back to someone at the computer while a remote
user is still connected, the screen is locked first (`LockOnLocalTakeover`),
so they can not see the desktop of the remote user. Remote clients continue on their virtual monitors next to the
monitors, or are disconnected if `DisconnectOnLocalTakeover` is set. The next
remote session is exclusive again.

### Configuration

All keys are in the `[VirtualDisplay]` group of `~/.config/krdpserverrc`:

| Key | Default | Description |
|-----|---------|-------------|
| `VirtualDisplayMode` | `Exclusive` | `Disabled`, `Extend` or `Exclusive`, see above. |
| `MaximumWidth`, `MaximumHeight` | `4096` | Largest virtual monitor that gets created, clients asking for more get this size. |
| `DetectLocalInput` | `true` | Give the monitors back when local input is detected. |
| `LockOnLocalTakeover` | `true` | Lock the screen before giving the monitors back while a remote user is connected. |
| `DisconnectOnLocalTakeover` | `false` | Disconnect remote clients when giving the monitors back. |

```bash
kwriteconfig6 --file krdpserverrc --group VirtualDisplay --key VirtualDisplayMode Extend
systemctl --user restart app-org.kde.krdpserver.service
```

### Requirements

- KWin 6.6 or newer, which supports custom modes on virtual outputs.
- The Plasma screencasting protocol is used for virtual monitors when
  available (it is for krdpserver started through its systemd unit or desktop
  file), the remote desktop portal otherwise.

## Hardware encoding on NVIDIA GPUs

KRdp encodes H.264 with KPipeWire, which only does hardware encoding through
VAAPI. The NVIDIA driver has no VAAPI encoding, so upstream KRdp encodes in
software on NVIDIA GPUs. KRdp-M encodes with NVENC instead, through FFmpeg:

- It is used automatically when the NVIDIA driver is loaded and FFmpeg was
  built with NVENC (it is on Arch Linux and CachyOS), for clients using H.264.
- If NVENC does not work, e.g. because the GPU has no NVENC, KPipeWire takes
  over for the rest of the session.
- The frames are converted to YUV on the CPU, with up to 4 threads, because
  NVENC would convert them to limited range colors that RDP clients show
  washed out. The encoding itself is done by the GPU.
- Set `KRDP_DISABLE_NVENC=1` in the environment of krdpserver to use KPipeWire
  anyway, e.g. with `systemctl --user edit app-org.kde.krdpserver.service`:

  ```ini
  [Service]
  Environment=KRDP_DISABLE_NVENC=1
  ```

The log of krdpserver says `Encoding monitor 0 with NVENC` when NVENC is used,
see [Debugging](#debugging).

## AV1 encoding

KRdp-M is built against [FreeRDP-M](https://github.com/maceeek/freerdp-m), a
fork of FreeRDP that encodes and decodes AV1 with the GPU: NVENC/NVDEC on
NVIDIA GPUs, VAAPI on AMD (e.g. Ryzen AI 300 with Radeon 860M) and Intel GPUs.
AV1 needs less bandwidth than H.264 for the same quality.

AV1 is an extension of FreeRDP to the RDP graphics pipeline
(`RDPGFX_CAPVERSION_FRDP_1`), so it is only used when both sides support it:

- The client uses FreeRDP-M, e.g. [KRDC-M](https://github.com/maceeek/krdc-m)
  or `xfreerdp3`/`sdl-freerdp3` of FreeRDP-M, with AV1 enabled (the default).
- A GPU of the server encodes AV1. Without one, AV1 is not offered: encoding
  it in software is too slow for a desktop.

Every other client, e.g. the Windows Remote Desktop client, Remmina or FreeRDP
itself, gets H.264 exactly as before. If AV1 stops working during a session,
the session continues with H.264 (or the progressive codec, for clients
without H.264).

Frames are encoded as a whole in 4:2:0, with the quality set in System
Settings. The log of krdpserver says `Encoding monitor 0 with AV1` when AV1 is
used. Set `KRDP_DISABLE_AV1=1` in the environment of krdpserver to always use
H.264, like `KRDP_DISABLE_NVENC` above.

## Frame pacing and diagnostics

A graphics surface waits for a key frame after it is created or reset. Resizing
only resets the stream once a frame at the new size arrives; repeated layout
requests do not recreate a decoder unnecessarily. Encoded reference frames are
kept when the connection is congested, while newer raw frames replace older
ones before encoding.

The target is 60 FPS. The acknowledgement turnaround controls a bounded window
of two to six frames, rather than dividing FPS by RTT and the number of pending
frames. Slow clients still reduce the requested rate and apply backpressure.

AV1 quality mode has a bitrate ceiling of 8 Mbit/s per megapixel, from 4 to
60 Mbit/s. Set `KRDP_AV1_BITRATE` in the server environment to override it, in
kbit/s (for example `20000`); `0` explicitly disables the ceiling. NVENC applies
the ceiling to constant quality, VAAPI uses VBR when a ceiling is requested.
This bounds encoder bursts; it does not measure or guarantee Wi-Fi capacity.

Every five seconds of activity, `Video stats` reports requested, sent and
acknowledged FPS, Mbit/s, key frames, RTT, acknowledgement turnaround, pending
frames and queued packets. `Encoder stats` reports mean encoding time and raw
frames replaced before encoding. These distinguish source/encoding starvation
from a slow network or client. Idle or unchanged desktops naturally send fewer
frames than the requested maximum.

## Debugging

```bash
QT_LOGGING_RULES="org.kde.krdp*=true" krdpserver
# or for the systemd unit
journalctl --user -u app-org.kde.krdpserver.service -f
```

## Installing on Arch Linux and CachyOS

Install [FreeRDP-M](https://github.com/maceeek/freerdp-m#installing-on-arch-linux-and-cachyos)
first, it replaces the `freerdp` package. Then:

```bash
git clone https://github.com/maceeek/krdp-m.git
cd krdp-m/packaging/archlinux
makepkg -si
```

This builds the `krdp-m-git` package from the `main` branch, which replaces
`krdp`. To update it to the latest `main`, run `makepkg -si` again in the same
directory, then restart krdpserver:

```bash
cd krdp-m/packaging/archlinux
git pull
makepkg -si
systemctl --user restart app-org.kde.krdpserver.service
```

### With paru

paru can install and update the package straight from this repository, like
a package from the AUR. Add it as a PKGBUILD repository to `/etc/paru.conf`:

```ini
[krdp-m]
Url = https://github.com/maceeek/krdp-m.git
Path = packaging/archlinux
```

Then install it once with paru, so paru knows where it comes from:

```bash
paru -Sya
paru -S krdp-m-git
```

Every commit on `main` is a new version. As it is a `-git` package, paru looks
for them with `paru -Syu --devel`, or always with plain `paru -Syu` after
adding `Devel` to the `[options]` of `/etc/paru.conf`. The package is built
locally against the installed libraries, like any package from the AUR.
After an update, restart krdpserver with
`systemctl --user restart app-org.kde.krdpserver.service`.

# KRdp

Library and examples for creating an RDP server.


# Remote Desktop KCM

![Remote Desktop Settings Window](https://cdn.kde.org/screenshots/krdp/krdp-settings.png)

Remote Desktop System Settings page (KCM) that lives in the Networking category.

Features:
- User can toggle the server (running the`krdpserver` binary) on and off using a toggle switch.
- The server can be set to auto-start at session login.
- The KCM uses systemd DBus messages to toggle the server on and off and auto-start it.
- User can easily add, modify, and remove usernames and passwords that are allowed to connect to the server.
- User can change the port of the server.
    - Do note that the address is currently set to `0.0.0.0`, which means any interface that accepts connections for `krdpserver` will work.
- Certificates can be auto-generated (this is done by default), or the user can supply their own certificates.
- Video quality can be changed between responsiveness and quality.
    - Do note that in software encoding mode, the quality slider might not necessarily do anything. This seems to be an encoder issue.
- The KCM will do some basic sanity-checking and warn the user about the following issues:
    - Password manager inaccessible (for KRDP user passwords)
    - No supported H264 encoder
    - Failures with generating certificates

# Running the example server

The example server requires a username and password to be provided on the command line, which will be used when connecting from an RDP client. They can be provided using the `-u` and `-p` command line parameters, respectively. For example:

```
krdpserver -u user -p test
```

The server will then listen on all interfaces on port 3389, and clients can connect with the username "user" and the password "pass".

# Connecting to the example server

To connect to the server, make sure to pass the username and password the server was started with. Note that the username is case-sensitive; this may be especially unexpected for those using Microsoft Windows RDP clients to connect, as system usernames on that platform are generally not case-sensitive.

Currently, the main client that has been used for testing and is confirmed to work is the FreeRDP client. Launch the FreeRDP client with the following command: `xfreerdp /u:<username> /p:<password> -clipboard /v:<ip_address>:3389`, filling in the username, password and IP address as appropriate. If testing locally, substitute `localhost` for an IP address.

# Security considerations

In addition, a valid TLS certificate and key are required to encrypt the communication between client and server. The server will look for a file called `server.crt` and `server.key` in the current working directory, but a different path can be provided using the `--certificate` and `--certificate-key` command line parameters. If no valid certificate is found using any of these methods, the server will internally generate a self-signed certificate and use that.

# Command Line Options

The following command line options are available for the example server:

<dl>
    <dt>-u, --username <username></dt>
    <dd>The username to use when a client tries to login. Required.</dd>
    <dt>-p, --password <password></dt>
    <dd>The password to require when a client tries to login. Required.</dd>
    <dt>--port <port></dt>
    <dd>The port to listen on for connections. Defaults to 3389.</dd>
    <dt>--certificate <certificate></dt>
    <dd>The path to a TLS certificate file to use. If not supplied or it cannot be found a temporary self-signed certificate will be generated.</dd>
    <dt>--certificate-key <certificate-key></dt>  
    <dd>The path to the TLS certificate key that matches the provided certificate.</dd>
    <dt>--monitor <monitor></dt>The index of the monitor to use for streaming video. If not supplied the whole workspace is used.</dd>
    <dt>--quality <quality></dt>
    <dd>Set the video quality, from 0 (lowest) to 100 (highest).</dd>
    <dt>--virtual-display <mode></dt>
    <dd>How remote sessions use the displays: <code>disabled</code>, <code>extend</code> or <code>exclusive</code>. Overrides the configuration.</dd>
    <dt>--return-displays</dt>
    <dd>Turn the monitors back on while a remote session uses them exclusively, then exit.</dd>
    <dt>--restore-displays</dt>
    <dd>Restore the monitors if a previous instance did not exit cleanly, then exit.</dd>
</dl>

# Known Working and Not-Working Clients

The following clients are known to work with the server:

- XFreeRDP and wlFreeRDP from the FreeRDP project.
- Remmina, a remote desktop client for Gnome.
- Thincast Remote Desktop Client
- Windows Remote Desktop client, at least as shipped with a recent Windows 10.

The following clients are known not to work:

- Microsoft's Remote Desktop client for Android. While it should support H.264
it seems to not enable it.

# Known Issues and Limitations

- Only video streaming and remote input is supported.
- Only the NLA security type of RDP is supported.
- Only one username and password combination is supported for login.
- Only the "Graphics Pipeline" extension of the RDP protocol is
implemented for video streaming. This extension allows using H.264 for video
streaming, but it means only clients supporting that extension are supported.
- H.264 encoding is done using hardware encoding if possible, but currently we
only support using VAAPI for this. Most notably this means hardware encoding on
NVidia hardware can not be used and software encoding will be used instead.
Additionally, on certain hardware there are limits to what size of frame can be
encoded by the hardware. In both cases, encoding will fall back to software
encoding.
- KDE's implementation of the Remote Desktop portal is rather limited as
shipped with Plasma 5.27. Most notably it does not allow selecting which screen
to stream, nor does it have an option to remember the setup and reuse it when
the same application requests a new connection. As a workaround, the server
will open a remote desktop session on startup and reuse that session for all
RDP connections. Additionally, monitor selection can be done using the
`--monitor` command line option.
- Input on a high DPI screen may be offset incorrectly. This is due to a bug in
the Remote Desktop Portal that has been fixed in the meantime. The fix will be 
released with KDE Plasma 5.27.8.

# CLI

While currently not very well supported it is possible to configure
KRdp purely from a CLI. This is particularly useful when you have access over
SSH but haven't yet configured KRdp.

```bash
# Authorize the krdpserver for remote desktop access
flatpak permission-set kde-authorized remote-desktop org.kde.krdpserver yes

# Generate a server certificate
mkdir --parents "$HOME/.local/share/krdpserver"
certificatePath="$HOME/.local/share/krdpserver/krdp.crt"
certificateKeyPath="$HOME/.local/share/krdpserver/krdp.key"
openssl req -nodes -new -x509 -keyout "$certificateKeyPath" -out "$certificatePath" -days 1 -batch

# Configure the certificate and enable system user authentication
kwriteconfig6 --file krdpserverrc --group General --key Certificate "$certificatePath"
kwriteconfig6 --file krdpserverrc --group General --key CertificateKey "$certificateKeyPath"
kwriteconfig6 --file krdpserverrc --group General --key SystemUserEnabled true

# Enable/restart the systemd service
systemctl --user enable --now app-org.kde.krdpserver.service
systemctl --user restart app-org.kde.krdpserver.service
```

## SDDM Autologin

Since SDDM currently has no RDP support, you either need to already be logged in,
or let SDDM auto log in to the user.

**Mind that your session starts physically unlocked on your PC.**
**Also when you are remote logging into your session it will be physically unlocked.**

```bash
# Configure autologin for your current user
user=$(whoami)
sudo kwriteconfig6 --file /etc/sddm.conf.d/kde_settings.conf --group Autologin --key User $user

# Enable automatic relogin on logout to avoid scenarios where the system is stuck on sddm
sudo kwriteconfig6 --file /etc/sddm.conf.d/kde_settings.conf --group Autologin --key Relogin true

# Restart sddm to force an autologin (don't run this if you are already logged in :D)
## systemctl restart sddm.service
```
