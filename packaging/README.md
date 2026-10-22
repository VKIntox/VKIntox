# packaging

`packaging/scripts/make-source-tarball.sh VERSION [OUTDIR]` creates the vendored
`vkintox-VERSION.tar.xz` used by the Arch, RPM and Debian builds.

## release workflow

`.github/workflows/release.yml` replaces the old Flatpak-only workflow. It builds
the selected artifacts, verifies their filenames, and publishes them with
`gh release create`. A build can be skipped when its artifact comes from an
earlier run through `reuse_run`.

## flatpak

The Flatpak manifest is a `org.freedesktop.Platform.VulkanLayer` extension. The
release job exports it to a local repository and turns that into a single
runtime bundle:

```bash
flatpak install --user vkintox-VERSION-x86_64.flatpak
```

The bundle installs as an extension of `org.freedesktop.Platform` 26.08. This
means it only applies to apps that use that freedesktop runtime branch (or an
SDK built on it); apps on 25.08, GNOME or KDE runtimes will not see the layer.

## local builds

Arch and RPM builds need a non-root user for `makepkg`. Debian builds use
`dpkg-buildpackage`; Nix uses the flake at the repository root; Flatpak uses
`flatpak-builder` with the freedesktop 26.08 SDK.
