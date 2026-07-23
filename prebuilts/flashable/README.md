# dm3q recovery-flashable kernel package

This template installs the three images required by this kernel build:

- `boot.img`
- `vendor_boot.img`
- `vendor_dlkm.img`

Build a ZIP from an image directory:

```bash
./prebuilts/make_flashable_zip.sh \
    out/dm3q-kernel-flashable.zip \
    out/msm-kalama-kalama-gki/packaged
```

`./build.sh full` calls the same packager automatically and writes
`dm3q-kernel-recovery-flashable.zip` next to the packaged images.

The installer uses the same ARM64 Edify `update-binary` layout as the q5q
package. It writes `boot`, `vendor_boot`, and the already-mapped dynamic
`vendor_dlkm` partition directly from `updater-script`.

Before flashing, keep a matching stock/custom set of `boot`, `vendor_boot`,
and `vendor_dlkm` images available. Flashing a mismatched or corrupt image can
prevent Android from booting.
