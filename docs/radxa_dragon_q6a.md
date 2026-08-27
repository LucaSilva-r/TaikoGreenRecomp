# Radxa Dragon Q6A kiosk deployment

The Radxa Dragon Q6A uses the same generic AArch64 TaikoRecomp binary and game
layout as the Raspberry Pi appliance. Live validation on 2026-08-27 used
Armbian 26.8.3 / Debian 13 with the QCS6490 kernel, Mesa Turnip on the Adreno
643, and a 1920x1080 60 Hz HDMI output.

The primary service drives DRM/KMS directly. Cage is installed only as a
disabled compositor fallback; it is not in the normal frame path.

Avahi advertises the appliance as `radxa-dragon-q6a.local` for SSH access.

## Required packages

```sh
sudo apt-get install libdrm2 libvulkan1 mesa-vulkan-drivers vulkan-tools \
  libasound2t64 alsa-utils cage wlr-randr libwayland-client0 \
  libwayland-cursor0 libwayland-egl1 libxkbcommon0 rsync \
  avahi-daemon libnss-mdns
```

Verify that `vulkaninfo --summary` reports `Turnip Adreno (TM) 643`, not only
llvmpipe.

The QCS6490 card exposes headphones on PCM 0 and HDMI/DP on PCM 1. Direct ALSA
clients do not apply the UCM route automatically. Both supplied Radxa units
enable the `DISPLAY_PORT_RX_0 ... MultiMedia2` mixer route and set SDL's ALSA
default playback device to `plughw:CARD=QCS6490RadxaDra,DEV=1`.

## Appliance layout

The runtime uses the same paths as the Pi:

```text
/var/lib/taikos/game/
/var/lib/taikos/recomp/taiko_boot
/var/lib/taikos/recomp/EBOOT.elf
/var/lib/taikos/recomp/lib/
/var/lib/taikos/recomp/vfs/
/var/lib/taikos/recomp/taiko_online.cfg
```

Install `deploy/taikos/taiko-recomp-session` as
`/usr/local/bin/taiko-recomp-session` and
`deploy/taikos/taiko-recomp-graphical-radxa.service` as
`/etc/systemd/system/taikos.service`. The unit intentionally omits the Pi's
V3DV upload-fence workarounds and VC4 zero-copy setting. It starts on the
portable atomic KMS path; any Turnip/MSM zero-copy optimization should be
enabled only after a visual and stability A/B test.

The optional Cage unit is `deploy/taikos/taiko-recomp-cage.service`. Install it
as `/etc/systemd/system/taikos-cage.service`, but do not enable both services.

```sh
# Normal direct-KMS appliance
sudo systemctl disable --now taikos-cage.service
sudo systemctl enable --now taikos.service

# Temporary Cage fallback/A-B test
sudo systemctl disable --now taikos.service
sudo systemctl enable --now taikos-cage.service
```

## Editable game-data link

The service owns the game tree, while the interactive user shares the `taikos`
group. Directories carry the setgid bit and both services use `UMask=0002`, so
files created later remain editable without `sudo`.

```sh
sudo usermod -aG taikos silvaluca
sudo chown -R taikos:taikos /var/lib/taikos/game
sudo chmod -R g+rwX /var/lib/taikos/game
sudo find /var/lib/taikos/game -type d -exec chmod g+s {} +
ln -s /var/lib/taikos/game /home/silvaluca/taikos-game
```

A fresh login is required before the added group appears in `id`. The link is
then available as `~/taikos-game`.

## Validation

```sh
systemctl status taikos.service
journalctl -u taikos.service -b --no-pager | tail -200
vulkaninfo --summary
```

The startup log must select Vulkan/Turnip and `/dev/dri/card1`, establish an
atomic 1920x1080@60 KMS output, and avoid llvmpipe. Keep the Pi intact until the
Radxa reaches attract mode, accepts drum input, and produces HDMI audio.

The initial live migration confirmed that `taiko_boot` owned a 1280x720 XB24
framebuffer scaled by the MSM display plane to 1920x1080@60, opened Turnip's
`renderD128`, and held HDMI playback PCM `/dev/snd/pcmC0D1p`. The service stayed
active with zero restarts. External drum/USB input still requires a physical
connection and an in-game input test.

For HDMI-audio diagnosis, test the wire independently of OBS. On the validated
setup, `speaker-test` through `plughw:CARD=QCS6490RadxaDra,DEV=1` produced a
clean stereo tone at the MS2109 capture card. `/proc/asound/card0/pcm1p/sub0/status`
shows its `hw_ptr` advancing near 46080 frames/s even though ALSA reports 48000
Hz, but this is not the physical HDMI sample clock: applying a matching 25/24
SDL frequency ratio audibly raised the pitch and was rejected in live testing.
Do not derive resampling compensation from the Qualcomm DSP pointer. Measure a
known recorded source at the HDMI receiver instead.

The HDMI PCM pulls 960 frames at a time. The unit retains
`TAIKO_AUDIO_PREBUFFER_BLOCKS=12` for the SDL fallback, enough SDL input for
three such periods plus scheduler jitter. The migrated configuration now has
attract audio restored; Player Entry remains a convenient repeatable live-audio
check. `TAIKO_AUDIO_DUMP` records the exact float blocks submitted by Taiko
before the host sink when deeper diagnosis is needed.

Requesting a 44100 Hz SDL device is not a workaround. SDL opens a logical
44100 Hz/882-frame stream, but `plughw` converts it back to the QCS6490's
48000 Hz/960-frame PCM; a Player Entry capture advanced at about 0.960x versus
0.966x on the normal 48000 Hz path and retained the same discontinuities.

The validated fix is `TAIKO_AUDIO_ALSA_DIRECT_DEVICE=hw:CARD=QCS6490RadxaDra,DEV=1`
in the Radxa service. Linux builds compile an opt-in direct ALSA sink alongside
the normal SDL backend; the environment variable selects it only on this
appliance. It writes 48 kHz stereo S16 directly with a 960-frame period and a
3840-frame buffer, matching the clean `aplay` control path. Live Player Entry
audio was reported clean and correctly pitched. The mixer advanced 5633
256-frame blocks in 30.02 seconds (187.64 blocks/s; target 187.5) with zero
xruns. Pi and desktop builds continue using SDL unless this variable is set.
