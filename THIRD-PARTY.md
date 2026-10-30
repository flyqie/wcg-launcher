# Third-party components

Three libraries, all built from the submodules under `deps/` rather than found on the
machine.

| Component | Version | Licence |
|---|---|---|
| [LibVNCServer](https://github.com/LibVNC/libvncserver) | 0.9.15 | **GPL-2.0-or-later** |
| [zlib](https://github.com/madler/zlib) | 1.3.2 | zlib |
| [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo) | 3.2.1 | IJG + Modified BSD (3-clause) |

**LibVNCServer is GPL-2.0-or-later and is linked statically**, so the whole of
`Launcher.exe` is a derivative work under the same terms. Its full text is in
[LICENSE](LICENSE); the other two are permissive and impose nothing on the licence of the
combined work.

No other third-party code is linked — everything else `Launcher.exe` needs is a Windows
system DLL, and the build fails rather than shipping if that stops being true.
