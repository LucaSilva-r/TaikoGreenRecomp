# Radxa Dragon Q6A kiosk deployment

The Radxa Dragon Q6A uses the same generic AArch64 TaikoRecomp binary and game
layout as the Raspberry Pi appliance. The tested OS is Armbian 26.8.3 / Debian
13 with the QCS6490 kernel, Mesa Turnip on the Adreno 643, and a 1920x1080 60 Hz
HDMI output.

The primary service drives DRM/KMS directly. Cage is installed only as a
disabled compositor fallback; it is not in the normal frame path.

## Required packages

```sh
sudo apt-get install libdrm2 libvulkan1 mesa-vulkan-drivers vulkan-tools \
  libasound2t64 alsa-utils cage wlr-randr libwayland-client0 \
  libwayland-cursor0 libwayland-egl1 libxkbcommon0 rsync
```

Verify that `vulkaninfo --summary` reports `Turnip Adreno (TM) 643`, not only
llvmpipe.

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
