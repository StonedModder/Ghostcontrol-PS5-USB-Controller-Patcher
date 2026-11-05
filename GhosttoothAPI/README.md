# GhosttoothAPI

Separate companion ELF for Ghost Control Toolbox 2.0. It is not built into
the native app and it does not use OpenProspero.

Load `GhosttoothAPI.elf` on the console ELF loader (port 9021). It listens
for the app on port 39072. The boilerplate app can also push the packaged
copy of this file to the loader from the Service screen.

Build:

```sh
make PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
```
