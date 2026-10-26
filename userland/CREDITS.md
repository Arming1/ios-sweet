# Credits and licences: userland/

## HackiOS user space (MIT)

`userland/gpu/mkeagl.py` and `userland/gpu/eagl_slots.txt` come from HackiOS user space, and
`userland/gpu/eagl.c`, `gl_osmesa.c`, `redwood_gl.h` and `userland/mesa/redwood-drm/` are based on it:
<https://github.com/oosexgang/hackios-xnu-userland>

```
MIT License

Copyright (c) 2026 OOSEX GANG

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Topaz (source of the adaptations)

The Mesa patches (`userland/mesa/*.patch`), `eagl.c`, `gl_osmesa.c`, `topaz_inject.c`, `bbd_wrap.c`
and `redwood-drm/` in this tree are adapted from the Topaz tree, which is the source of the
adaptations for the Redmi Note 12 4G (SM6225, Adreno 610):
<https://github.com/MakrSas/ios-topaz>. The A660 device-name change in `redwood-drm/redwood_drm.c`
is ours, for the Mi 11 (venus, SM8350, Adreno 660).

## Mesa (MIT, downloaded at build time, not vendored)

Mesa 25.0.7 is not in this repository. The workflow `.github/workflows/gpu-userland.yml` downloads
the release archive from <https://archive.mesa3d.org/>, checks its SHA-256 and builds OSMesa with
the freedreno and softpipe drivers. Mesa is MIT licensed; its per-file licences are listed in the
COPYING and docs/license.rst files of the archive.
