# gxfp51a0-linux

Linux support for the Goodix **GXFP51A0** (GF3658 / ST411) fingerprint sensor
found in the Huawei MateBook 13 `WRTB-WXX9`: SPI protocol, TLS-PSK session,
image capture, and a libfprint / fprintd driver.

Everything here was derived from traffic observed on this machine, not from
vendor source. Protocol facts and the reason for each magic number are recorded
in [STATUS.md](STATUS.md).

## Status

| Area | State |
|---|---|
| Reset, SPI, frame protocol, version | works (`GF_ST411SEC_APP_14108`) |
| OTP calibration + 0x90 config upload | works, DAC readback verified |
| PMK recovery via `F2` staging | works, PMK never written to disk |
| TLS 1.2 `PSK-AES128-CBC-SHA256` | works (host is the TLS server) |
| Image capture, 80×64 @ ~460 dpi | works, stable over 10 frames |
| libfprint driver, `fprintd-list` | works |
| `fprintd-enroll` | runs, but see the warning below |
| `fprintd-verify` | returns match for the wrong finger too |

**Do not use this for login or `sudo` yet.** Matching is not trustworthy on this
sensor: 80×64 is ~4.4×3.5 mm, and NBIS/MINDTCT reports no minutiae inside the
real image area at all — every point it finds sits on the border of the padding
canvas the driver adds, which makes any two frames look alike. The full analysis
and six negative results are in [SUMMARY.md](SUMMARY.md).

What is genuinely usable today: the capture path, the protocol implementation,
and the offline diagnostics tools.

## Hardware

| Item | Value |
|---|---|
| Machine | HUAWEI `WRTB-WXX9`, MateBook 13 |
| Sensor | ACPI `GXFP51A0`, Goodix GF3658 / ST411 |
| SPI | `/dev/spidev1.0`, mode 0 + `SPI_CS_HIGH`, 1 MHz |
| Reset GPIO | 264, HIGH 300 ms → LOW 600 ms |
| IRQ GPIO | 48, active high |
| Image | 10573-byte TLS record → 80×64, 12 bpp packed 6→4 |

## Build and test

```bash
make && make test          # protocol / calibration / PMK / image / session tests
```

## Probe

```bash
sudo modprobe spidev
echo spidev | sudo tee /sys/bus/spi/devices/spi-GXFP51A0:00/driver_override
echo spi-GXFP51A0:00 | sudo tee /sys/bus/spi/drivers/spidev/bind

sudo systemctl stop fprintd
sudo ./build/gx51-probe --verbose
sudo ./build/gx51-probe --tls-image --frames 5   # writes finger-NN.pgm
```

Options: `--spi PATH`, `--no-reset`, `--info`, `--configure`, `--staging`,
`--pmk`, `--tls-image`, `--frames N`, `--avg N`, `--repeat N`, `--verbose`.

## libfprint / fprintd

```bash
sudo ./tools/build-libfprint.sh            # build patched libfprint 1.94.9 + driver
sudo ./tools/build-libfprint.sh install    # /usr/local, udev rule in /etc, fprintd drop-in
fprintd-list "$USER"
```

The patch adds one thing only: a small-image exception so NBIS keeps working on
an 80×64 frame. It does not change the matcher.

## Offline diagnostics

All of these read captured `.pgm` files, no hardware needed.

| Tool | Purpose |
|---|---|
| `tools/test-minutiae` | MINDTCT on a stored capture, several upscales |
| `tools/dump-minutiae` | print minutia coordinates, write marked + binarised PGM |
| `tools/test-match` | Bozorth3 score matrix over a set of captures |
| `tools/match-corr` | ridge-pattern normalised cross-correlation |
| `tools/match-patch` | block-wise correlation with translation voting |
| `tools/align-avg.py` | align captures by translation and average |
| `tools/capture-group.sh` | capture a labelled group of frames (stops fprintd) |
| `tools/two-fingers.sh` | guided two-finger enroll / verify script |

```bash
sudo ./tools/capture-group.sh index 10
./tools/test-match captures/index/*.pgm captures/other/*.pgm
```

## Docs

- [SUMMARY.md](SUMMARY.md) — stage report: what works, what does not, why
- [STATUS.md](STATUS.md) — protocol findings, in the order they were discovered
- [PLAN.md](PLAN.md) — milestones, acceptance criteria, rollback

## Safety

- Stop `fprintd` before running the probe.
- No firmware flashing, no eFuse writes, no PMK on disk, no BIOS changes.
- The probe does pulse GPIO264, which resets the fingerprint MCU.
- Keep password login available at all times.
- Intended only for `GXFP51A0` on `WRTB-WXX9`.
