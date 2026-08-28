# Radxa Dragon Q6A kiosk deployment

The Radxa Dragon Q6A uses the same generic AArch64 TaikoRecomp binary and game
layout as the Raspberry Pi appliance. Live validation on 2026-08-27 used
Armbian 26.8.1 / Debian 13 with the QCS6490 kernel, Mesa Turnip on the Adreno
643, and a 1920x1080 HDMI output. The initial migration used 60 Hz; the current
gameplay/profile configuration uses the display's exact 120 Hz CTA mode.

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
`/etc/systemd/system/taikos.service`. The unit enables atomic direct KMS and
dma-buf zero-copy scanout. Turnip exports the target with Qualcomm's tiled
modifier (`0x500000000000001`), so the renderer draws the status/PIN and
optional FPS overlays as GPU quads. Never write a CPU-linear badge into this
export: doing so corrupts tiles along the top edge. The backend detects the
modifier and only uses its CPU overlay path for linear exports. The unit still
omits the Pi-only V3DV separate-upload/fence-wait workarounds.

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

On the validated QCS6490 kernel, stopping or restarting a running direct-KMS
Vulkan session can reset the whole board during teardown. Use
`sudo systemctl enable taikos.service` followed by `sudo reboot` when applying
runtime or binary changes. A normal boot is reliable; this warning concerns
live service teardown.

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
atomic 1920x1080@120 KMS output, print `zero-copy KMS dma-buf path active (GPU
overlays)`, and avoid llvmpipe. Keep the Pi intact until the Radxa reaches
attract mode, accepts drum input, and produces HDMI audio.

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

## Low-overhead profiling

Install `deploy/taikos/taiko-recomp-profiling.conf` as
`/etc/systemd/system/taikos.service.d/profiling.conf`, and install
`taiko-profile-sampler` plus its service from the same directory. After the
next reboot, `[RSXFPS]` and `[PRESENTPACE]` report application timings once per
second. The binary also supports the deeper `[RSXREC]` breakdown, but for the
least intrusive steady measurement the drop-in sets `RSX_RECORDER_PROFILE=0`;
this removes its per-draw timer reads.
`[TAIKO-SYS]` independently records process CPU/RSS,
GPU target/current frequency, CPU-cluster frequencies, and CPU/GPU temperature
once per second. Correlate them with:

```sh
journalctl -b -u taikos.service -u taiko-profile-sampler.service \
  --output=short-precise
```

The default profiling drop-in does not enable `RSX_RESOURCE_TRACE`: that mode
takes timestamps inside the per-operation preparation loop and can perturb a
heavy Song Select frame. Enable it only for a short follow-up run if the first
trace shows high `prep` time. Do not read the DRM debugfs `gpu` or `perf`
nodes under `/sys/kernel/debug/dri/` while the game is running on this kernel;
a live debugfs snapshot was followed by `VK_ERROR_DEVICE_LOST` and an automatic
service restart. Linux `perf record` sampling is separate and has been safe.

The QCS6490's heterogeneous scheduler must keep the frame-critical pipeline on
the fast cluster. A live A/B in Song Select isolated the SDL renderer on CPU 4,
the frame/FIFO driver on 5, `NU::Draw::RequestManager` on 6, and the initial
guest plus its light VSync worker on prime CPU 7. Rapid category scrolling then
held 59.98--60.03 FPS; before the change, the draw manager could land on an
efficiency core and the same scene alternated between 30 and 60 FPS even while
SDL rendering used only 5--9 ms. The Radxa service carries this policy through
the `TAIKO_CPU_*_AFFINITY` variables. These are thread-role affinities applied
inside the runtime, not a process-wide `CPUAffinity=` setting (which would pin
Vulkan, audio, and every guest worker to the same small mask).

With those roles isolated, the 366--376-draw Song Select baseline spent about
5.5--5.8 ms rendering and 4.3--4.5 ms waiting for the GPU/KMS render fence. The
known worst costume's complete model outline adds 16 draws and roughly 1.77
MiB of dynamic vertices. At 120 Hz that path can phase-lock the title at 60
FPS, so the service keeps `TAIKO_GPU_CHARACTER_OUTLINE=0`. This affects the
centered/600x600 character chain; the small top-left gameplay Don-chan uses a
different sprite path and retains its authored outline.

F8 cycles this chain at runtime through normal, no outline, and no 3D
character rendering. The last mode also bypasses the native skin jobs, making
it a clean CPU/GPU A/B without a reboot. The service's outline environment
setting selects the initial no-outline baseline, so its first F8 press selects
character-off. A short status pill shows every transition.

The direct-KMS keyboard mapping uses `D/F/J/K` for P1 and `Z/X/C/V` for P2 in
left-rim/left-centre/right-centre/right-rim order. Coin is F2; service moved to
F6 because F3--F5 are the live audio-offset controls. Both players are packed
into the title's single `0x1080` USIO sensor report (P1 at bytes 32--39, P2 at
40--47); `0x1100` mirrors the same frame.

The display advertises an exact 1920x1080 at 120 Hz CTA mode. The Radxa service
requests it with `TAIKOS_OUTPUT_MODE=1920x1080@120`, ticks the guest with
`TAIKO_VBLANK_HZ=120`, and enables the title's elapsed-time animation
corrections with `TAIKO_ANIMATION_TIMING=1`. The initial 120 Hz profile showed
light gameplay at 120 FPS but dense note fields at 80--100, sometimes
phase-locked at 60, even though GPU rendering remained around 5.5--6.8 ms. A
main-guest call-graph sample traced most of the generic VM-helper cost to the
native SPURS skin job, not `sys_lwmutex` as first suspected.

The 2026-08-28 CPU pass keeps a single runtime-published fast VM base for lifted
loads/stores, validates complete skin-job source/destination ranges once,
decodes the big-endian vertex float4s with NEON loads, and skips exclusive
vblank/flip counter exchanges at HLE boundaries when the counters are already
zero. The final heavy-costume song averaged 118.49 FPS across 108 measured
seconds. Its 43 seconds at 150 or more draws averaged 118.57 FPS; 170-or-more
draw peaks averaged 118.72 FPS while GPU render time stayed at 5.82 ms. Native
skinning fell from about 5.5% to 3.0% of sampled main-thread cycles, and the
previous 6.3% atomic-exchange hot spot disappeared from the sample. Remaining
effect/animation transitions can lose several submissions for one or two
seconds (the trace still shows mixed 8.33/16.67 ms intervals), but recover
immediately; steady dense gameplay no longer locks to 60 FPS. Repeating the
same heavy song after `perf record` exited felt and performed the same, so the
99 Hz call-graph sampler was not responsible for the measured late-song drop.

Rapid Song Select scrolling is not storage-bound on the validated system. A
ten-second reproduction caused zero physical read bytes and zero major page
faults; cached reads accounted for about 24 MB. With the default governors,
however, the GPU remained at 315 MHz and CPU-cluster frequencies oscillated
while the producer alternated between 16.7 and 33.3 ms submission intervals.
Holding the GPU at its advertised 812 MHz reduced KMS render wait from about
7.6 to 4.6 ms, but neither that nor the CPU `performance` governor eliminated
the producer's 30 FPS mode; role affinity did. Keep the frequency policy as
headroom for the 366--376-draw scrolling workload. Install and enable
`taiko-performance.service` plus its helper from `deploy/taikos/`; it selects
only frequencies advertised by the kernel and does not overclock the board.
